// MMF Engine — concurrent demo driver.
//
//   * N producer threads submit players/parties from independent RNG streams,
//   * one churn thread disconnects players that are already queued,
//   * the engine's single matchmaking worker forms 5v5 lobbies,
//   * one notification thread fans results out to socket/allocator/telemetry.
//
// Run `mmf_demo --bench` for a headless throughput measurement instead.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mmf/matchmaker.hpp"
#include "mmf/observers.hpp"

namespace {

using namespace mmf;

struct Options {
  int players{4000};
  int producers{8};
  int durationMs{15000};
  int partyPercent{20};
  int disconnectPercent{2};
  bool casual{false};
  bool verbose{false};
  bool bench{false};
  std::uint64_t seed{20261026};
};

void printUsage() {
  std::cout << "usage: mmf_demo [options]\n"
               "  --players N            players to submit           (default 4000)\n"
               "  --producers N          producer threads            (default 8)\n"
               "  --duration MS          wall-clock budget           (default 15000)\n"
               "  --party-percent P      players in pre-made parties (default 20)\n"
               "  --disconnect-percent P players that disconnect     (default 2)\n"
               "  --mode casual|ranked   matchmaking strategy        (default ranked)\n"
               "  --seed N               RNG seed                    (default 20261026)\n"
               "  --bench                benchmark mode (no churn, larger default)\n"
               "  --verbose              per-match observer logging\n"
               "  --help\n";
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](int fallback) {
      return i + 1 < argc ? std::stoi(argv[++i]) : fallback;
    };
    if (arg == "--players") {
      options.players = std::max(10, next(options.players));
    } else if (arg == "--producers") {
      options.producers = std::clamp(next(options.producers), 1, 64);
    } else if (arg == "--duration") {
      options.durationMs = std::max(200, next(options.durationMs));
    } else if (arg == "--party-percent") {
      options.partyPercent = std::clamp(next(options.partyPercent), 0, 80);
    } else if (arg == "--disconnect-percent") {
      options.disconnectPercent = std::clamp(next(options.disconnectPercent), 0, 50);
    } else if (arg == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::max(1, next(static_cast<int>(options.seed))));
    } else if (arg == "--mode") {
      const std::string mode = (i + 1 < argc) ? argv[++i] : "ranked";
      options.casual = (mode == "casual");
    } else if (arg == "--bench") {
      options.bench = true;
      options.players = 20000;
      options.producers = 4;
      options.disconnectPercent = 0;
      options.durationMs = 120000;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else {
      std::cout << "unknown option: " << arg << "\n";
      printUsage();
      std::exit(2);
    }
  }
  return options;
}

EngineConfig makeConfig(const Options& options) {
  EngineConfig config;
  config.teamSize = 5;
  config.teamsPerMatch = 2;
  config.maxPartySize = 4;
  config.baseTolerance = 140.0;
  config.toleranceGrowthPerSecond = 260.0;
  config.maxTolerance = 2200.0;
  config.maxPingDeltaMs = 40.0;
  config.maxPingMs = 90;
  config.allowCrossRegion = false;
  config.weightWait = 1.0;
  config.weightScarcity = 0.30;
  config.agingExponent = 2.0;
  config.agingReferenceSeconds = 6.0;
  config.workerTick = std::chrono::milliseconds(5);
  config.priorityRefreshInterval = std::chrono::milliseconds(50);
  config.maxAnchorAttemptsPerTick = 256;
  config.maxCandidatesPerAnchor = 64;
  config.eventQueueCapacity = 1U << 15U;
  config.maxTrackedLobbies = 8192;
  config.maxTeamAverageGap = 260.0;
  config.maxLobbyRatingSpread = 900.0;
  config.maxPartyRatingSpread = 400.0;
  config.scarcityPeerCap = 32;
  config.logLevel = options.verbose ? LogLevel::Info : LogLevel::Error;
  return config;
}

/// Models the last hop of the match lifecycle: the game server accepted the
/// lease, so the lobby is confirmed and leaves the tracked-ready state. Without
/// this, every formed lobby would still be pending at shutdown.
class MatchConfirmer final : public IObserver {
 public:
  explicit MatchConfirmer(MatchmakerService* engine) : engine_(engine) {}

  [[nodiscard]] std::string_view name() const override { return "auto-confirmer"; }

  void onMatchReady(const MatchFoundEvent& event) override {
    if (engine_ != nullptr) engine_->submitLobbyConfirmed(event.matchId);
  }

 private:
  MatchmakerService* engine_;
};

/// Network model: skill is a gaussian around 1500 with artificial outliers, ping
/// is uniform, and regions are split in two so the latency cap actually bites.
struct PlayerGenerator {
  std::mt19937_64 rng;
  std::normal_distribution<double> skill{1500.0, 200.0};
  std::normal_distribution<double> partyDrift{0.0, 70.0};
  std::uniform_int_distribution<int> ping{12, 78};
  std::uniform_int_distribution<int> pingDrift{-14, 14};
  std::uniform_int_distribution<int> region{0, 1};
  std::uniform_int_distribution<int> partyRoll{1, 100};
  std::uniform_int_distribution<int> partySize{2, 4};
  std::bernoulli_distribution outlier{0.06};
  std::bernoulli_distribution smurf{0.06};

  explicit PlayerGenerator(std::uint64_t seed) : rng(seed) {}
};

}  // namespace

int main(int argc, char** argv) {
  const Options options = parseOptions(argc, argv);
  EngineConfig config = makeConfig(options);
  const auto mode = options.casual ? GameMode::Casual : GameMode::Ranked;
  // Requests carry the mode they were submitted for; keep the demo's copy in
  // sync with the mode the engine is built for.
  config.mode = static_cast<GameModeId>(mode);

  auto engine = createMatchmaker(config, mode);
  auto socket = std::make_shared<SocketNotifier>(options.verbose);
  auto allocator =
      std::make_shared<GameServerAllocator>(8192, std::chrono::seconds(5), options.verbose);
  auto telemetry = std::make_shared<TelemetryLogger>(options.verbose);
  auto confirmer = std::make_shared<MatchConfirmer>(engine.get());
  engine->attachObserver(socket);
  engine->attachObserver(allocator);
  engine->attachObserver(confirmer);
  engine->attachObserver(telemetry);

  std::cout << "MMF engine — " << (options.casual ? "casual" : "ranked") << " mode, "
            << config.teamSize << "v" << config.teamSize << " lobbies, " << options.producers
            << " producer threads, " << options.players << " players\n";

  const TimePoint wallStart = Clock::now();
  engine->start();

  std::atomic<PlayerId> nextPlayerId{1};
  std::atomic<int> submitted{0};
  std::atomic<int> submitFailures{0};
  std::atomic<int> cancellations{0};
  std::atomic<bool> stopProducers{false};

  const int playersPerProducer = std::max(1, options.players / options.producers);
  std::vector<std::thread> producers;
  producers.reserve(static_cast<std::size_t>(options.producers));

  for (int index = 0; index < options.producers; ++index) {
    producers.emplace_back([&, index] {
      PlayerGenerator generator(options.seed + static_cast<std::uint64_t>(index) * 7919ULL);
      const int toSubmit =
          (index == options.producers - 1)
              ? std::max(0, options.players - playersPerProducer * (options.producers - 1))
              : playersPerProducer;

      for (int i = 0; i < toSubmit && !stopProducers.load(std::memory_order_relaxed); ++i) {
        JoinRequest request;
        request.mode = config.mode;
        request.region = generator.region(generator.rng);

        const bool wantsParty =
            generator.partyRoll(generator.rng) <= options.partyPercent && toSubmit - i >= 3;
        const int memberCount = wantsParty ? generator.partySize(generator.rng) : 1;

        double baseMmr = generator.skill(generator.rng);
        if (generator.outlier(generator.rng)) baseMmr = 2600.0 + generator.skill(generator.rng) * 0.2;
        else if (generator.smurf(generator.rng)) baseMmr = 550.0 + generator.skill(generator.rng) * 0.1;
        baseMmr = std::clamp(baseMmr, 250.0, 3100.0);
        const int basePing = generator.ping(generator.rng);

        for (int member = 0; member < memberCount; ++member) {
          PlayerProfile profile;
          profile.id = nextPlayerId.fetch_add(1, std::memory_order_relaxed);
          profile.name = "player-" + std::to_string(profile.id);
          profile.mmr = std::clamp(
              member == 0 ? baseMmr : baseMmr + generator.partyDrift(generator.rng), 250.0, 3100.0);
          profile.pingMs = std::clamp(
              member == 0 ? basePing : basePing + generator.pingDrift(generator.rng), 8, 88);
          profile.region = request.region;
          request.members.push_back(std::move(profile));
        }

        // submitJoin applies bounded backpressure; retry rather than dropping a
        // player on the floor (a real gateway would answer 429 instead).
        while (!engine->submitJoin(request) && engine->running() &&
               !stopProducers.load(std::memory_order_relaxed)) {
          submitFailures.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        submitted.fetch_add(1, std::memory_order_relaxed);
        if ((i & 31) == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  // ---- churn thread: disconnect players that are already in the queue --------
  const int disconnectTarget =
      options.disconnectPercent > 0 ? (options.players * options.disconnectPercent) / 100 : 0;
  std::thread churn([&] {
    std::mt19937_64 rng(options.seed ^ 0x5DEECE66DULL);
    while (!stopProducers.load(std::memory_order_relaxed) &&
           cancellations.load(std::memory_order_relaxed) < disconnectTarget) {
      const auto queued = engine->queuedPlayerIds();
      if (queued.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      std::uniform_int_distribution<std::size_t> pick(0, queued.size() - 1);
      if (engine->submitCancel(queued[pick(rng)])) {
        cancellations.fetch_add(1, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  // ---- progress monitor -----------------------------------------------------
  std::thread monitor;
  if (!options.bench) {
    monitor = std::thread([&] {
      while (!stopProducers.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (stopProducers.load(std::memory_order_relaxed)) break;
        const auto snapshot = engine->snapshot();
        const double elapsed = secondsSince(wallStart);
        std::cout << "  t=" << std::fixed << std::setprecision(1) << std::setw(5) << elapsed
                  << "s  queued " << std::setw(5) << snapshot.queuedTickets << " tickets  matched "
                  << std::setw(6) << snapshot.metrics.playersMatched << " players  matches "
                  << std::setw(5) << snapshot.metrics.matchesFormed << "  "
                  << std::setprecision(0)
                  << (elapsed > 0 ? static_cast<double>(snapshot.metrics.playersMatched) / elapsed
                                  : 0.0)
                  << " players/s\n";
      }
    });
  }

  // ---- wait for drain or deadline ------------------------------------------
  const TimePoint deadline = Clock::now() + std::chrono::milliseconds(options.durationMs);
  EngineSnapshot finalSnapshot;
  std::uint64_t lastMatchCount = 0;
  TimePoint lastProgressAt = wallStart;
  while (Clock::now() < deadline) {
    finalSnapshot = engine->snapshot();
    if (finalSnapshot.metrics.matchesFormed > lastMatchCount) {
      lastMatchCount = finalSnapshot.metrics.matchesFormed;
      lastProgressAt = Clock::now();
    }
    const bool allSubmitted = submitted.load(std::memory_order_relaxed) >= options.players;
    if (allSubmitted && finalSnapshot.pendingCommands == 0 && finalSnapshot.queuedTickets == 0) break;
    // The remaining tickets have no legal opponent left (ping cap, region split
    // or an empty skill bracket). Report them instead of idling out the budget.
    if (allSubmitted && finalSnapshot.pendingCommands == 0 &&
        secondsSince(lastProgressAt) > 2.0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  stopProducers.store(true, std::memory_order_relaxed);
  for (std::thread& producer : producers) producer.join();
  churn.join();
  if (monitor.joinable()) monitor.join();

  engine->flushNotifications(std::chrono::milliseconds(2000));
  finalSnapshot = engine->snapshot();
  const TimePoint drainedAt = Clock::now();
  engine->shutdown();

  const double wallSeconds = std::chrono::duration<double>(drainedAt - wallStart).count();
  // Throughput is measured over the window in which the engine was actually
  // forming matches: subtracting the idle tail (leftover unmatchable tickets)
  // keeps the number honest instead of averaging against a sleep.
  const double activeSeconds = std::max(1e-9, secondsSince(wallStart, lastProgressAt));
  const MetricsSnapshot metrics = finalSnapshot.metrics;

  std::cout << "\n=== run summary =====================================================\n";
  std::cout << "mode / strategy       : " << (options.casual ? "casual" : "ranked") << " / "
            << (options.casual ? "CasualStrategy" : "RankedStrategy")
            << " + AgingPriorityPolicy + HardLatencyPolicy\n";
  std::cout << "lobby shape           : " << config.teamSize << "v" << config.teamSize << " ("
            << config.slotsPerLobby() << " slots)\n";
  std::cout << "wall clock            : " << std::fixed << std::setprecision(2) << wallSeconds
            << " s (active " << activeSeconds << " s)\n";
  std::cout << "submitted requests    : " << submitted.load() << " (retries due to full queue: "
            << submitFailures.load() << ")\n";
  std::cout << "accepted into queue   : " << metrics.joinsAccepted << " requests / "
            << metrics.playersAccepted << " players\n";
  std::cout << "  rejected duplicate  : " << metrics.joinsRejectedDuplicate << "\n";
  std::cout << "  rejected invalid    : " << metrics.joinsRejectedInvalid << "\n";
  std::cout << "  rejected queue full : " << metrics.joinsRejectedQueueFull << "\n";
  std::cout << "  rejected latency    : " << metrics.joinsRejectedLatency << "\n";
  std::cout << "matches formed        : " << metrics.matchesFormed << "\n";
  std::cout << "players matched       : " << metrics.playersMatched << "\n";
  std::cout << "still queued          : " << finalSnapshot.queuedPlayers << " players in "
            << finalSnapshot.queuedTickets << " tickets (oldest wait " << std::setprecision(2)
            << finalSnapshot.longestWaitSeconds << " s)\n";
  std::cout << "withdrawn (drops)     : " << metrics.playersWithdrawn << "   (cancels "
            << metrics.cancellations << ", parties dissolved " << metrics.partiesDissolved << ")\n";
  std::cout << "queue structures      : interval tree " << finalSnapshot.intervalTreeSize
            << " / heap " << finalSnapshot.heapSize << " / tickets " << finalSnapshot.queuedTickets
            << "  (must be equal)\n";
  std::cout << "interval queries      : " << metrics.intervalQueries << ", nodes visited "
            << metrics.intervalNodesVisited << " ("
            << (metrics.intervalQueries == 0
                    ? 0.0
                    : static_cast<double>(metrics.intervalNodesVisited) /
                          static_cast<double>(metrics.intervalQueries))
            << " per query)\n";
  std::cout << "lobby attempts        : " << metrics.lobbyAttempts << " (not fillable "
            << metrics.lobbyIncomplete << ", balance rejects " << metrics.lobbyRejectedBalance
            << ", latency rejects " << metrics.lobbyRejectedLatency << ")\n";
  if (finalSnapshot.queuedTickets > 0) {
    std::cout << "  note: the remaining tickets have no legal opponent (ping delta / region /\n"
                 "        skill window); they are aged, not starved, and stay queryable\n";
  }
  std::cout << "wait time             : mean " << std::setprecision(3) << metrics.waitMeanSeconds
            << " s  p50 " << metrics.waitP50Seconds << " s  p95 " << metrics.waitP95Seconds
            << " s  p99 " << metrics.waitP99Seconds << " s  max " << metrics.waitMaxSeconds
            << " s\n";
  std::cout << "throughput            : " << std::setprecision(0)
            << (static_cast<double>(metrics.playersMatched) / activeSeconds) << " players/s, "
            << (static_cast<double>(metrics.matchesFormed) / activeSeconds) << " matches/s\n";
  std::cout << "notifications         : published " << engine->notifications()->published()
            << ", delivered " << engine->notifications()->delivered() << ", dropped "
            << engine->notifications()->dropped() << ", observer failures "
            << engine->notifications()->observerFailures() << "\n";
  std::cout << "game servers leased   : " << allocator->allocations() << " (exhausted "
            << allocator->exhausted() << ")\n";

  const std::uint64_t accounted =
      metrics.playersMatched + finalSnapshot.queuedPlayers + metrics.playersWithdrawn;
  const bool conserved = accounted == metrics.playersAccepted;
  std::cout << "conservation check     : " << (conserved ? "OK" : "MISMATCH") << " ("
            << metrics.playersAccepted << " accepted players == " << metrics.playersMatched
            << " matched + " << finalSnapshot.queuedPlayers << " queued + "
            << metrics.playersWithdrawn << " withdrawn)\n";

  std::cout << '\n';
  telemetry->report(std::cout);
  return conserved ? 0 : 1;
}
