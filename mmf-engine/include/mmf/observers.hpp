// MMF Engine — asynchronous notification pipeline and sample listeners.
//
//   Lobby (Subject)  --->  AsyncNotificationChannel  --->  SocketNotifier
//                                   (queue + thread)  --->  GameServerAllocator
//                                                     --->  TelemetryLogger
//
// The channel is the Observer that decouples match formation from I/O: the
// matchmaking worker only pays for an enqueue (a few hundred nanoseconds), and
// slow observers (allocation, socket writes) never sit inside the state lock.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "mmf/events.hpp"
#include "mmf/observer.hpp"

namespace mmf {

/// Bounded MPSC queue plus one delivery thread.
///
/// Overflow policy: drop newest and count it. Blocking the producer would stall
/// matchmaking for everybody because notifications are downstream of matches;
/// a dropped notification is visible in metrics and never corrupts queue state.
class AsyncNotificationChannel final : public IObserver, public Subject {
 public:
  using Payload = std::variant<MatchFoundEvent, LobbyAbortedEvent>;

  explicit AsyncNotificationChannel(std::size_t capacity);
  ~AsyncNotificationChannel() override;

  AsyncNotificationChannel(const AsyncNotificationChannel&) = delete;
  AsyncNotificationChannel& operator=(const AsyncNotificationChannel&) = delete;

  [[nodiscard]] std::string_view name() const override { return "async-notification-channel"; }

  void onMatchReady(const MatchFoundEvent& event) override;
  void onLobbyAborted(const LobbyAbortedEvent& event) override;

  void start();
  void stop();

  /// Blocks until every published payload has been delivered (or timeout).
  bool flush(std::chrono::milliseconds timeout);

  [[nodiscard]] std::size_t pending() const;
  [[nodiscard]] std::uint64_t published() const { return published_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t delivered() const { return delivered_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t observerFailures() const {
    return observerFailures_.load(std::memory_order_relaxed);
  }

 private:
  void run();
  void deliver(const Payload& payload);
  bool enqueue(Payload payload);

  std::deque<Payload> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable idleCv_;
  std::thread thread_;
  bool running_{false};
  bool inFlight_{false};
  std::size_t capacity_;

  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> delivered_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> observerFailures_{0};
};

/// Sample listener 1: writes the match assignment to each player's socket.
/// Stands in for a real WebSocket/gRPC push (no I/O in this demo).
class SocketNotifier final : public IObserver {
 public:
  explicit SocketNotifier(bool verbose = false) : verbose_(verbose) {}

  [[nodiscard]] std::string_view name() const override { return "socket-notifier"; }
  void onMatchReady(const MatchFoundEvent& event) override;

  [[nodiscard]] std::uint64_t notificationsSent() const {
    return notificationsSent_.load(std::memory_order_relaxed);
  }

 private:
  bool verbose_;
  std::atomic<std::uint64_t> notificationsSent_{0};
};

/// Sample listener 2: leases a game server instance for a ready match.
/// Models the real flow (finite pool, lease expiry) through a mutex-guarded
/// lease table; a production version would call the Agones/Pilot API here.
class GameServerAllocator final : public IObserver {
 public:
  explicit GameServerAllocator(std::size_t poolSize = 2048,
                               std::chrono::milliseconds leaseDuration = std::chrono::seconds(20),
                               bool verbose = false);

  [[nodiscard]] std::string_view name() const override { return "game-server-allocator"; }
  void onMatchReady(const MatchFoundEvent& event) override;

  [[nodiscard]] std::uint64_t allocations() const {
    return allocations_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

 private:
  struct Lease {
    MatchId matchId;
    std::chrono::steady_clock::time_point expiresAt;
  };

  void releaseExpired(std::chrono::steady_clock::time_point now);

  std::mutex mutex_;
  std::vector<Lease> leases_;
  std::size_t poolSize_;
  std::chrono::milliseconds leaseDuration_;
  bool verbose_;
  std::atomic<std::uint64_t> allocations_{0};
  std::atomic<std::uint64_t> exhausted_{0};
};

/// Sample listener 3: aggregation for dashboards and end-of-run reports.
class TelemetryLogger final : public IObserver {
 public:
  explicit TelemetryLogger(bool verbose = false) : verbose_(verbose) {}

  [[nodiscard]] std::string_view name() const override { return "telemetry"; }
  void onMatchReady(const MatchFoundEvent& event) override;
  void onLobbyAborted(const LobbyAbortedEvent& event) override;

  void report(std::ostream& out) const;

  [[nodiscard]] std::uint64_t matches() const { return matches_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t playersPlaced() const {
    return playersPlaced_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t aborts() const { return aborts_.load(std::memory_order_relaxed); }

 private:
  bool verbose_;
  std::atomic<std::uint64_t> matches_{0};
  std::atomic<std::uint64_t> playersPlaced_{0};
  std::atomic<std::uint64_t> aborts_{0};

  mutable std::mutex mutex_;
  std::vector<double> matchWaits_;
  double waitSum_{0.0};
  double waitMax_{0.0};
  double gapSum_{0.0};
  double gapMax_{0.0};
  double pingSpreadMax_{0.0};
  double firstEventSeconds_{0.0};
  double lastEventSeconds_{0.0};
  bool seen_{false};
  TimePoint startedAt_{Clock::now()};
};

}  // namespace mmf
