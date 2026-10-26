// MMF Engine — MatchmakerService: the thread-safe core.
//
// Threading model (single-writer, many-readers, zero shared mutable state on
// the producer side):
//
//   network/producer threads                worker thread              notifier thread
//   ------------------------                -------------              ---------------
//   submitJoin/Cancel  --(command queue)-->  drain + mutate queue  --> AsyncNotificationChannel
//                                             (stateMutex_ held)        (own queue + thread)
//   snapshot() (shared read of a copy) <---- stateMutex_                SocketNotifier
//                                                                       GameServerAllocator
//                                                                       TelemetryLogger
//
// The interval tree, the heap and the ticket table have exactly one mutating
// thread. That removes every data race by construction instead of relying on
// per-structure locks; producers only ever touch the bounded command queue,
// whose mutex is never held while the state mutex is acquired (documented lock
// order: stateMutex_ -> commandMutex_ nowhere; commandMutex_ is always released
// before the worker takes stateMutex_).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mmf/config.hpp"
#include "mmf/interval_tree.hpp"
#include "mmf/lobby.hpp"
#include "mmf/metrics.hpp"
#include "mmf/observers.hpp"
#include "mmf/repository.hpp"
#include "mmf/strategy.hpp"
#include "mmf/ticket.hpp"
#include "mmf/weighted_min_heap.hpp"

namespace mmf {

/// A player (or pre-made group) asking to enter the queue.
/// Player ids are assigned by the network/session layer so the submission path
/// stays fire-and-forget: no futures, no back-pressure on the caller's stack.
struct JoinRequest {
  GameModeId mode{0};
  RegionId region{0};
  std::vector<PlayerProfile> members;  // size 1 == solo, > 1 == party
};

struct EngineDependencies {
  std::unique_ptr<IMatchmakingStrategy> strategy;
  std::unique_ptr<IPriorityPolicy> priority;
  std::unique_ptr<ILatencyPolicy> latency;
  std::shared_ptr<IPlayerRepository> players;
  std::shared_ptr<AsyncNotificationChannel> notifications;
};

/// Consistent read-only view of the engine, safe to call from any thread.
struct EngineSnapshot {
  std::size_t queuedTickets{0};
  std::size_t queuedPlayers{0};
  std::size_t intervalTreeSize{0};
  std::size_t heapSize{0};
  std::size_t pendingCommands{0};
  std::size_t trackedLobbies{0};
  std::size_t trackedPlayers{0};
  double longestWaitSeconds{0.0};
  MetricsSnapshot metrics;
};

class MatchmakerService {
 public:
  MatchmakerService(EngineConfig config, EngineDependencies dependencies);
  ~MatchmakerService();

  MatchmakerService(const MatchmakerService&) = delete;
  MatchmakerService& operator=(const MatchmakerService&) = delete;

  void start();
  /// Graceful stop: drain pending commands, abort live lobbies (observers are
  /// told about it), flush the notification pipeline, join both threads.
  void shutdown();

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  // ---- producer API (thread-safe, non-blocking except for bounded backpressure)

  /// Enqueues a join. Returns false when the engine is stopping or the bounded
  /// command queue stayed full for config.enqueueTimeout.
  bool submitJoin(JoinRequest request);

  /// Player disconnected / left the queue. Purges the ticket from the interval
  /// tree and the heap in O(log N); a party queue node is dissolved.
  bool submitCancel(PlayerId playerId);

  /// The game server accepted the ready lobby.
  bool submitLobbyConfirmed(MatchId matchId);

  /// The ready lobby must be torn down (client refused, allocation failed...).
  bool submitLobbyAbort(MatchId matchId, AbortReason reason);

  // ---- read side -----------------------------------------------------------

  [[nodiscard]] EngineSnapshot snapshot() const;
  [[nodiscard]] std::vector<PlayerId> queuedPlayerIds() const;
  [[nodiscard]] std::optional<Player> findPlayer(PlayerId playerId) const;
  [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }
  [[nodiscard]] std::shared_ptr<AsyncNotificationChannel> notifications() const {
    return dependencies_.notifications;
  }

  void attachObserver(std::shared_ptr<IObserver> observer);
  bool flushNotifications(std::chrono::milliseconds timeout);

  /// True once the queue and the command pipeline are empty (tests, drain-on-exit).
  bool waitUntilQuiet(std::chrono::milliseconds timeout) const;

 private:
  struct Command {
    enum class Kind : std::uint8_t { Join, Cancel, Confirm, Abort } kind{Kind::Join};
    JoinRequest join;
    PlayerId playerId{0};
    MatchId matchId{0};
    AbortReason reason{AbortReason::Policy};
  };

  // worker
  void workerLoop();
  void handleCommand(const Command& command, TimePoint now);
  void handleJoin(const JoinRequest& request, TimePoint now);
  void handleCancel(PlayerId playerId, TimePoint now);
  void handleConfirm(MatchId matchId);
  void handleAbort(MatchId matchId, AbortReason reason, TimePoint now);

  void enqueueTicket(Ticket ticket, TimePoint now);
  void purgeTicket(TicketId ticketId);
  [[nodiscard]] double estimateScarcity(const Ticket& ticket) const;
  void refreshPriorities(TimePoint now);
  void runMatchmakingPass(TimePoint now);
  [[nodiscard]] std::shared_ptr<Lobby> attemptLobby(const Ticket& anchor, TimePoint now);
  [[nodiscard]] bool verifyLobbyLatency(const Lobby& lobby) const;
  void commitLobby(const std::shared_ptr<Lobby>& lobby, TimePoint now);
  /// Takes the lobby by value on purpose: aborting untracks (and therefore
  /// destroys the map's sole reference to) the lobby, so the callee must own a
  /// reference for the whole call. A const& here was a use-after-free in an
  /// earlier revision — caught by the ASan build.
  void abortLobby(std::shared_ptr<Lobby> lobby, AbortReason reason, TimePoint now,
                  std::optional<PlayerId> droppedPlayer);
  void untrackLobby(MatchId matchId);
  void evictLobbiesIfNeeded();

  bool enqueueCommand(Command command);

  EngineConfig config_;
  EngineDependencies dependencies_;
  mutable Metrics metrics_;

  // State owned exclusively by the worker thread (guarded by stateMutex_).
  mutable std::mutex stateMutex_;
  IntervalTree<TicketId> tree_;
  WeightedMinHeap<TicketId> heap_;
  std::unordered_map<TicketId, Ticket> tickets_;
  std::unordered_map<MatchId, std::shared_ptr<Lobby>> lobbies_;
  std::deque<MatchId> lobbyOrder_;
  TimePoint lastRefresh_{};
  TimePoint startedAt_{};

  std::atomic<TicketId> nextTicketId_{1};
  std::atomic<MatchId> nextMatchId_{1};

  // Command queue: touched by producers and drained by the worker.
  mutable std::mutex commandMutex_;
  std::condition_variable commandCv_;
  std::deque<Command> commands_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  bool started_{false};
};

/// Builds the standard dependency set for a mode (Ranked or Casual).
/// Observers are attached by the caller, not here.
std::unique_ptr<MatchmakerService> createMatchmaker(EngineConfig config, GameMode mode);

}  // namespace mmf
