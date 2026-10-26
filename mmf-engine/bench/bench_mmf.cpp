// MMF Engine — microbenchmarks for the data structures and the full pipeline.
//
// Everything is measured on this machine at this moment; numbers are printed
// with the operation counts so they can be reproduced (`make bench`).
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

#include "mmf/interval_tree.hpp"
#include "mmf/matchmaker.hpp"
#include "mmf/weighted_min_heap.hpp"

namespace {

using namespace mmf;
using WallClock = std::chrono::steady_clock;

double secondsBetween(WallClock::time_point a, WallClock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

struct Options {
  std::size_t treeSize{200000};
  std::size_t queries{200000};
  int players{20000};
  int producers{8};
};

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::size_t fallback) {
      return i + 1 < argc ? static_cast<std::size_t>(std::stoull(argv[++i])) : fallback;
    };
    if (arg == "--tree") options.treeSize = std::max<std::size_t>(1000, next(options.treeSize));
    else if (arg == "--queries") options.queries = std::max<std::size_t>(1000, next(options.queries));
    else if (arg == "--players") options.players = static_cast<int>(std::max<std::size_t>(10, next(static_cast<std::size_t>(options.players))));
    else if (arg == "--producers") options.producers = static_cast<int>(std::max<std::size_t>(1, next(static_cast<std::size_t>(options.producers))));
  }
  return options;
}

void report(const std::string& label, std::size_t operations, double seconds, double nsPerOp) {
  std::cout << "  " << std::left << std::setw(30) << label << std::right << std::setw(9)
            << operations << " ops " << std::setw(10) << std::fixed << std::setprecision(2)
            << seconds * 1000.0 << " ms " << std::setw(9) << std::setprecision(0) << nsPerOp
            << " ns/op " << std::setw(10) << std::setprecision(2)
            << (seconds > 0.0 ? static_cast<double>(operations) / seconds / 1e6 : 0.0)
            << " M ops/s\n";
}

void benchIntervalTree(const Options& options) {
  std::mt19937_64 rng(987654321ULL);
  std::uniform_int_distribution<std::int64_t> center(0, 2000000);
  std::uniform_int_distribution<std::int64_t> halfWidth(50, 250);

  IntervalTree<std::uint64_t> tree;
  std::vector<std::pair<std::int64_t, std::int64_t>> windows;
  windows.reserve(options.treeSize);
  for (std::size_t i = 0; i < options.treeSize; ++i) {
    const auto c = center(rng);
    const auto h = halfWidth(rng);
    windows.emplace_back(c - h, c + h);
  }

  auto start = WallClock::now();
  for (std::size_t i = 0; i < options.treeSize; ++i) {
    tree.upsert({windows[i].first, windows[i].second, static_cast<std::uint64_t>(i)});
  }
  auto insertEnd = WallClock::now();
  tree.validate();
  const double insertSeconds = secondsBetween(start, insertEnd);
  report("interval tree insert", options.treeSize, insertSeconds,
         insertSeconds * 1e9 / static_cast<double>(options.treeSize));

  std::size_t totalMatches = 0;
  std::size_t totalVisited = 0;
  std::size_t worstVisited = 0;
  start = WallClock::now();
  for (std::size_t i = 0; i < options.queries; ++i) {
    const auto c = center(rng);
    const auto width = halfWidth(rng) * 2;
    const auto result = tree.query(c - width, c + width, [](const auto&) { return true; });
    totalMatches += result.matches;
    totalVisited += result.nodesVisited;
    worstVisited = std::max(worstVisited, result.nodesVisited);
  }
  const auto queryEnd = WallClock::now();
  const double querySeconds = secondsBetween(start, queryEnd);
  report("interval tree query", options.queries, querySeconds,
         querySeconds * 1e9 / static_cast<double>(options.queries));
  std::cout << "    avg matches/query " << std::setprecision(1)
            << static_cast<double>(totalMatches) / static_cast<double>(options.queries)
            << ", avg nodes/query " << static_cast<double>(totalVisited) /
                   static_cast<double>(options.queries)
            << ", worst " << worstVisited << " (log2 N = " << std::log2(static_cast<double>(
                   options.treeSize))
            << ")\n";

  start = WallClock::now();
  std::size_t erased = 0;
  for (std::size_t i = 0; i < options.treeSize; i += 2) {
    if (tree.erase(static_cast<std::uint64_t>(i))) ++erased;
  }
  const auto eraseEnd = WallClock::now();
  const double eraseSeconds = secondsBetween(start, eraseEnd);
  tree.validate();
  report("interval tree erase", erased, eraseSeconds,
         eraseSeconds * 1e9 / static_cast<double>(std::max<std::size_t>(erased, 1)));
}

void benchHeap(const Options& options) {
  WeightedMinHeap<std::uint64_t> heap;
  std::mt19937_64 rng(13579ULL);
  std::uniform_real_distribution<double> key(-1e6, 1e6);

  auto start = WallClock::now();
  for (std::size_t i = 0; i < options.treeSize; ++i) {
    heap.pushOrUpdate(i, key(rng));
  }
  auto end = WallClock::now();
  heap.validate();
  double seconds = secondsBetween(start, end);
  report("indexed min-heap push", options.treeSize, seconds,
         seconds * 1e9 / static_cast<double>(options.treeSize));

  start = WallClock::now();
  for (std::size_t i = 0; i < options.treeSize; i += 2) {
    heap.updateKey(i, key(rng));
  }
  end = WallClock::now();
  heap.validate();
  seconds = secondsBetween(start, end);
  report("indexed min-heap updateKey", options.treeSize / 2, seconds,
         seconds * 1e9 / static_cast<double>(options.treeSize / 2));

  start = WallClock::now();
  WeightedMinHeap<std::uint64_t>::Item item;
  std::size_t popped = 0;
  while (heap.popMin(item)) ++popped;
  end = WallClock::now();
  seconds = secondsBetween(start, end);
  heap.validate();
  report("indexed min-heap popMin", popped, seconds,
         seconds * 1e9 / static_cast<double>(std::max<std::size_t>(popped, 1)));
}

int benchEngine(const Options& options) {
  EngineConfig config;
  config.teamSize = 5;
  config.teamsPerMatch = 2;
  config.maxPartySize = 4;
  config.baseTolerance = 250.0;
  config.toleranceGrowthPerSecond = 700.0;
  config.maxTolerance = 2400.0;
  config.maxPingDeltaMs = 60.0;
  config.maxPingMs = 90;
  config.agingReferenceSeconds = 5.0;
  config.workerTick = std::chrono::milliseconds(2);
  config.priorityRefreshInterval = std::chrono::milliseconds(50);
  config.maxAnchorAttemptsPerTick = 1024;
  config.maxCandidatesPerAnchor = 64;
  config.maxTeamAverageGap = 400.0;
  config.maxLobbyRatingSpread = 1200.0;
  config.maxPartyRatingSpread = 400.0;
  config.commandQueueCapacity = 1U << 16U;
  config.eventQueueCapacity = 1U << 16U;
  config.logLevel = LogLevel::Error;

  auto engine = createMatchmaker(config, GameMode::Ranked);
  struct Sink final : IObserver {
    std::atomic<std::uint64_t> matches{0};
    std::atomic<std::uint64_t> players{0};
    [[nodiscard]] std::string_view name() const override { return "bench-sink"; }
    void onMatchReady(const MatchFoundEvent& event) override {
      matches.fetch_add(1, std::memory_order_relaxed);
      players.fetch_add(event.playerCount, std::memory_order_relaxed);
    }
  };
  auto sink = std::make_shared<Sink>();
  engine->attachObserver(sink);
  engine->start();

  const int players = options.players;
  const int producers = options.producers;
  std::atomic<PlayerId> nextId{1};
  std::atomic<int> submitted{0};
  std::atomic<int> failed{0};

  const auto start = WallClock::now();
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(producers));
  for (int producer = 0; producer < producers; ++producer) {
    threads.emplace_back([&, producer] {
      std::mt19937_64 rng(0x5151ULL + static_cast<std::uint64_t>(producer));
      std::normal_distribution<double> skill(1500.0, 180.0);
      std::uniform_int_distribution<int> ping(15, 85);
      std::uniform_int_distribution<int> region(0, 1);
      const int perProducer = players / producers + (producer < players % producers ? 1 : 0);
      for (int i = 0; i < perProducer; ++i) {
        PlayerProfile profile;
        profile.id = nextId.fetch_add(1, std::memory_order_relaxed);
        profile.name = "bench-" + std::to_string(profile.id);
        profile.mmr = std::clamp(skill(rng), 300.0, 3000.0);
        profile.pingMs = ping(rng);
        profile.region = static_cast<RegionId>(region(rng));
        JoinRequest request;
        request.mode = config.mode;
        request.region = profile.region;
        request.members.push_back(std::move(profile));
        bool ok = false;
        for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
          ok = engine->submitJoin(request);
          if (!ok) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (ok) submitted.fetch_add(1, std::memory_order_relaxed);
        else failed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const auto submittedAt = WallClock::now();

  // Stop at the point matching actually stops making progress: the tail of a
  // queue is usually a handful of players that the hard latency/region rules
  // can never pair, and waiting for them would dwarf the real throughput.
  auto drainedAt = submittedAt;
  std::uint64_t lastMatches = 0;
  auto lastProgress = submittedAt;
  while (true) {
    const auto snapshot = engine->snapshot();
    if (snapshot.metrics.matchesFormed > lastMatches) {
      lastMatches = snapshot.metrics.matchesFormed;
      lastProgress = WallClock::now();
    }
    if (snapshot.queuedTickets == 0) {
      drainedAt = WallClock::now();
      break;
    }
    if (secondsBetween(lastProgress, WallClock::now()) > 2.0) {
      drainedAt = lastProgress;
      break;
    }
    if (secondsBetween(start, WallClock::now()) > 120.0) {
      drainedAt = WallClock::now();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  engine->flushNotifications(std::chrono::seconds(5));
  const auto snapshot = engine->snapshot();
  engine->shutdown();

  const double submitSeconds = std::max(1e-9, secondsBetween(start, submittedAt));
  const double drainSeconds = std::max(1e-9, secondsBetween(submittedAt, drainedAt));
  const auto metrics = snapshot.metrics;
  const double avgNodes =
      metrics.intervalQueries == 0
          ? 0.0
          : static_cast<double>(metrics.intervalNodesVisited) /
                static_cast<double>(metrics.intervalQueries);

  std::cout << "\n[end-to-end: " << players << " players, " << producers
            << " producers, single worker]\n";
  std::cout << "  join submission               : " << submitted.load() << " accepted ("
            << failed.load() << " failed) in " << std::fixed << std::setprecision(3)
            << submitSeconds << " s -> " << std::setprecision(0)
            << (static_cast<double>(submitted.load()) / submitSeconds)
            << " joins/s\n";
  std::cout << "  drain after last submit       : " << std::setprecision(3) << drainSeconds
            << " s\n";
  std::cout << "  matches formed                : " << metrics.matchesFormed << " ("
            << metrics.playersMatched << " players, " << std::setprecision(1)
            << (metrics.matchesFormed == 0
                    ? 0.0
                    : static_cast<double>(metrics.playersMatched) /
                          static_cast<double>(metrics.matchesFormed))
            << " players/match)\n";
  std::cout << "  end-to-end throughput         : " << std::setprecision(0)
            << (static_cast<double>(metrics.playersMatched) /
                std::max(1e-9, secondsBetween(start, drainedAt)))
            << " players/s, "
            << (static_cast<double>(metrics.matchesFormed) /
                std::max(1e-9, secondsBetween(start, drainedAt)))
            << " matches/s\n";
  std::cout << "  interval queries              : " << metrics.intervalQueries << " ("
            << std::setprecision(1) << avgNodes << " nodes/query avg)\n";
  std::cout << "  lobby attempts / rejects      : " << metrics.lobbyAttempts << " / "
            << metrics.lobbyRejectedBalance << " balance, " << metrics.lobbyRejectedLatency
            << " latency, " << metrics.lobbyIncomplete << " incomplete\n";
  std::cout << "  queue metrics                 : " << snapshot.queuedPlayers
            << " players left, longest wait " << std::setprecision(3)
            << snapshot.longestWaitSeconds << " s\n";
  std::cout << "  wait distribution             : p50 " << std::setprecision(3)
            << metrics.waitP50Seconds << " s, p95 " << metrics.waitP95Seconds << " s, p99 "
            << metrics.waitP99Seconds << " s\n";
  std::cout << "  notifications                 : " << engine->notifications()->delivered()
            << " delivered, " << engine->notifications()->dropped() << " dropped\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parseOptions(argc, argv);
  std::cout << "MMF engine benchmark\n-------------------\n[data structures: "
            << options.treeSize << " intervals, " << options.queries << " queries]\n";
  benchIntervalTree(options);
  std::cout << '\n';
  benchHeap(options);
  return benchEngine(options);
}
