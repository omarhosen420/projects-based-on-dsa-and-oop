// Concurrency stress test: the point of this suite is what TSan/ASan see.
//
//   * 8 producer threads push 4000 joins through the bounded command queue,
//   * 1 churn thread cancels random tickets while the worker is matching,
//   * 2 reader threads take snapshots and assert the structural invariants:
//       tree.size() == heap.size() == ticket count
//       accepted == matched + queued + withdrawn
//   * the main thread then verifies that everything was either matched or
//     explicitly withdrawn, with no lost or duplicated players.
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine_fixture.hpp"
#include "test_support.hpp"

using namespace mmf;
using namespace mmftest;

namespace {

/// Cancels every remaining ticket until the queue is empty (bounded retries).
bool drainQueueByCancelling(MatchmakerService& engine) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    const auto ids = engine.queuedPlayerIds();
    if (ids.empty()) {
      const auto snapshot = engine.snapshot();
      if (snapshot.queuedTickets == 0 && snapshot.pendingCommands == 0) return true;
    }
    for (const PlayerId id : ids) engine.submitCancel(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return engine.snapshot().queuedTickets == 0;
}

}  // namespace

MMF_TEST_SUITE(engine_handles_concurrent_producers_readers_and_churn) {
  EngineConfig config = testConfig();
  config.baseTolerance = 500.0;
  config.maxTolerance = 500.0;
  config.toleranceGrowthPerSecond = 0.0;
  config.maxPingDeltaMs = 10.0;
  config.maxLobbyRatingSpread = 1600.0;
  config.maxTeamAverageGap = 1200.0;
  config.maxAnchorAttemptsPerTick = 512;
  config.commandQueueCapacity = 1U << 14U;
  config.eventQueueCapacity = 1U << 15U;
  config.enqueueTimeout = std::chrono::milliseconds(50);

  auto engine = createMatchmaker(config, GameMode::Ranked);
  auto recorder = std::make_shared<RecordingObserver>();
  engine->attachObserver(recorder);
  engine->start();

  constexpr int kProducers = 8;
  constexpr int kPerProducer = 500;
  constexpr int kTotal = kProducers * kPerProducer;

  std::vector<JoinSpec> specs;
  specs.reserve(kTotal);
  for (int i = 0; i < kTotal; ++i) {
    specs.push_back(makeSolo(1400.0 + static_cast<double>(i % 120), 25 + (i % 5), 0));
  }

  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<int> violations{0};
  std::atomic<int> snapshots{0};
  std::atomic<bool> stopReaders{false};

  auto reader = [&] {
    while (!stopReaders.load(std::memory_order_relaxed)) {
      const auto snapshot = engine->snapshot();
      if (snapshot.queuedTickets != snapshot.intervalTreeSize ||
          snapshot.intervalTreeSize != snapshot.heapSize) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      const auto metrics = snapshot.metrics;
      if (metrics.playersAccepted !=
          metrics.playersMatched + snapshot.queuedPlayers + metrics.playersWithdrawn) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      snapshots.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  };

  std::thread readerA(reader);
  std::thread readerB(reader);

  std::thread churn([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::mt19937_64 rng(0xABCDEFULL);
    for (int i = 0; i < 250; ++i) {
      const auto index = static_cast<std::size_t>(rng() % specs.size());
      engine->submitCancel(specs[index].ids.front());
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      const int begin = producer * kPerProducer;
      const int end = begin + kPerProducer;
      for (int i = begin; i < end; ++i) {
        bool submitted = false;
        for (int attempt = 0; attempt < 5 && !submitted; ++attempt) {
          submitted = engine->submitJoin(specs[static_cast<std::size_t>(i)].request);
          if (!submitted) std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        if (submitted) {
          accepted.fetch_add(1, std::memory_order_relaxed);
        } else {
          rejected.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& producer : producers) producer.join();
  churn.join();

  CHECK_EQ(accepted.load(), kTotal);
  CHECK_EQ(rejected.load(), 0);

  CHECK(waitFor([&] { return engine->snapshot().pendingCommands == 0; },
                std::chrono::seconds(20)));
  CHECK(drainQueueByCancelling(*engine));

  stopReaders.store(true, std::memory_order_relaxed);
  readerA.join();
  readerB.join();

  engine->flushNotifications(std::chrono::milliseconds(2000));
  const auto snapshot = engine->snapshot();
  const auto metrics = snapshot.metrics;

  CHECK_EQ(violations.load(), 0);
  CHECK_MSG(snapshots.load() > 10, "reader threads observed too few snapshots");
  CHECK_EQ(snapshot.queuedTickets, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.intervalTreeSize, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.heapSize, static_cast<std::size_t>(0));
  CHECK_EQ(metrics.playersAccepted, static_cast<std::uint64_t>(kTotal));
  CHECK_EQ(metrics.playersMatched + metrics.playersWithdrawn,
           static_cast<std::uint64_t>(kTotal));
  CHECK_EQ(metrics.commandErrors, static_cast<std::uint64_t>(0));
  CHECK_EQ(metrics.eventsDropped, static_cast<std::uint64_t>(0));
  CHECK_EQ(metrics.observerFailures, static_cast<std::uint64_t>(0));

  // Every formed lobby was full, and no player was handed to more than one live
  // match. A player may appear twice only across an aborted + re-formed lobby,
  // which is bounded by the number of requeued tickets.
  std::unordered_map<PlayerId, int> appearances;
  std::uint64_t placed = 0;
  for (const auto& match : recorder->matches()) {
    CHECK_EQ(match.playerCount, static_cast<std::size_t>(10));
    placed += match.playerCount;
    for (const auto& team : match.teams) {
      for (const auto& player : team.players) appearances[player.id] += 1;
    }
  }
  CHECK(placed >= metrics.playersMatched);

  std::size_t duplicated = 0;
  for (const auto& entry : appearances) {
    if (entry.second > 1) ++duplicated;
  }
  CHECK_MSG(duplicated <= metrics.ticketsRequeued, "players reported in more than one lobby");
}
