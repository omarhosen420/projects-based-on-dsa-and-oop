// End-to-end matchmaker tests (single worker, async notification pipeline).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

#include "engine_fixture.hpp"
#include "test_support.hpp"

using namespace mmf;
using namespace mmftest;

namespace {

void checkConserved(mmftest::TestContext& ctx, const EngineSnapshot& snapshot, const char* file,
                    int line) {
  const auto metrics = snapshot.metrics;
  const auto accounted = metrics.playersMatched + snapshot.queuedPlayers + metrics.playersWithdrawn;
  if (accounted == metrics.playersAccepted) {
    ++ctx.checks;
    return;
  }
  ctx.check(false, "playersAccepted == matched + queued + withdrawn", file, line,
            "accepted=" + std::to_string(metrics.playersAccepted) + " matched=" +
                std::to_string(metrics.playersMatched) + " queued=" +
                std::to_string(snapshot.queuedPlayers) + " withdrawn=" +
                std::to_string(metrics.playersWithdrawn));
}

#define CHECK_CONSERVED(snapshot)                                                                 \
  checkConserved(ctx, (snapshot), __FILE__, __LINE__);                                             \
  CHECK_EQ((snapshot).queuedTickets, (snapshot).intervalTreeSize);                                 \
  CHECK_EQ((snapshot).intervalTreeSize, (snapshot).heapSize)

bool allSameTeam(const MatchFoundEvent& event, const std::vector<PlayerId>& ids) {
  for (const auto& team : event.teams) {
    std::size_t found = 0;
    for (const auto& player : team.players) {
      if (std::find(ids.begin(), ids.end(), player.id) != ids.end()) ++found;
    }
    if (found == ids.size()) return true;
    if (found != 0) return false;
  }
  return false;
}

}  // namespace

MMF_TEST_SUITE(engine_forms_balanced_lobby_from_solo_players) {
  EngineHarness harness(testConfig());
  std::vector<JoinSpec> specs;
  for (int i = 0; i < 10; ++i) specs.push_back(makeSolo(1500.0 + i * 5.0, 30, 0));
  for (const auto& spec : specs) CHECK(harness.submit(spec));

  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  const auto matches = harness.recorder->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    CHECK_EQ(matches[0].playerCount, static_cast<std::size_t>(10));
    CHECK_EQ(matches[0].teams.size(), static_cast<std::size_t>(2));
    CHECK_EQ(matches[0].teams[0].players.size(), static_cast<std::size_t>(5));
    CHECK_EQ(matches[0].teams[1].players.size(), static_cast<std::size_t>(5));
    const double gap =
        std::abs(matches[0].teams[0].averageMmr - matches[0].teams[1].averageMmr);
    CHECK_MSG(gap <= 600.0, "team average gap " + std::to_string(gap));
    CHECK_EQ(matches[0].pingSpreadMs, 0.0);
  }

  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.queuedTickets, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.metrics.matchesFormed, static_cast<std::uint64_t>(1));
  CHECK_EQ(snapshot.metrics.playersMatched, static_cast<std::uint64_t>(10));
  CHECK_CONSERVED(snapshot);
}

MMF_TEST_SUITE(engine_fills_lobby_with_party_and_solos) {
  EngineHarness harness(testConfig());
  JoinSpec party = makeParty({1500.0, 1502.0, 1504.0}, 30, 0);
  CHECK(harness.submit(party));
  for (int i = 0; i < 7; ++i) CHECK(harness.submit(makeSolo(1500.0 + i, 30, 0)));

  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  const auto matches = harness.recorder->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    CHECK_EQ(matches[0].playerCount, static_cast<std::size_t>(10));
    CHECK_MSG(allSameTeam(matches[0], party.ids), "party members must share one team");
  }

  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.metrics.playersMatched, static_cast<std::uint64_t>(10));
  CHECK_CONSERVED(snapshot);

  // Queue depth reflects tickets, not players: one party ticket + seven solos.
  CHECK_EQ(snapshot.queuedTickets, static_cast<std::size_t>(0));
}

MMF_TEST_SUITE(engine_enforces_ping_delta_hard_cap) {
  EngineConfig config = testConfig();
  config.maxPingDeltaMs = 20.0;
  config.maxPingMs = 90;
  EngineHarness harness(config);

  std::vector<JoinSpec> fast;
  std::vector<JoinSpec> laggy;
  for (int i = 0; i < 10; ++i) {
    fast.push_back(makeSolo(1500.0 + i, 30, 0));
    CHECK(harness.submit(fast.back()));
  }
  for (int i = 0; i < 2; ++i) {
    laggy.push_back(makeSolo(1500.0 + i, 85, 0));
    CHECK(harness.submit(laggy.back()));
  }

  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  const auto matches = harness.recorder->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    for (const auto& team : matches[0].teams) {
      for (const auto& player : team.players) {
        CHECK_MSG(player.pingMs <= 35, "laggy player slipped into a 30ms lobby");
      }
    }
  }

  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.metrics.playersMatched, static_cast<std::uint64_t>(10));
  CHECK_EQ(snapshot.queuedPlayers, static_cast<std::size_t>(2));
  CHECK_EQ(snapshot.queuedTickets, static_cast<std::size_t>(2));
  CHECK_CONSERVED(snapshot);

  // Join-time rejection for a player above the absolute ceiling.
  CHECK(harness.submit(makeSolo(1500.0, 400, 0)));
  CHECK(waitFor([&] { return harness.snapshot().metrics.joinsRejectedLatency == 1; },
                std::chrono::seconds(2)));
}

MMF_TEST_SUITE(engine_widens_window_until_outlier_bracket_matches) {
  EngineConfig config = testConfig();
  config.baseTolerance = 50.0;
  config.toleranceGrowthPerSecond = 400.0;
  config.maxTolerance = 900.0;
  config.maxLobbyRatingSpread = 1200.0;
  config.maxTeamAverageGap = 600.0;
  EngineHarness harness(config);

  for (int i = 0; i < 9; ++i) CHECK(harness.submit(makeSolo(1000.0 + i, 30, 0)));
  // Let the nine accumulate wait time and priority.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK_EQ(harness.snapshot().queuedTickets, static_cast<std::size_t>(9));
  JoinSpec outlier = makeSolo(1600.0, 30, 0);
  CHECK(harness.submit(outlier));

  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(10)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  const auto matches = harness.recorder->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    bool found = false;
    for (const auto& team : matches[0].teams) {
      for (const auto& player : team.players) {
        if (player.id == outlier.ids.front()) found = true;
      }
    }
    CHECK_MSG(found, "the 1600 MMR outlier must be part of the widened match");
    CHECK(matches[0].oldestWaitSeconds > 0.05);
  }
  CHECK_CONSERVED(harness.snapshot());
}

MMF_TEST_SUITE(engine_cancel_purges_tree_and_heap) {
  EngineConfig config = testConfig();
  // Zero ping delta + a unique ping per player: no two tickets can ever match,
  // which keeps the queue deterministic while we exercise cancellation.
  config.maxPingDeltaMs = 0.0;
  config.maxPingMs = 200;
  EngineHarness harness(config);

  std::vector<JoinSpec> specs;
  for (int i = 0; i < 100; ++i) {
    specs.push_back(makeSolo(1500.0, 10 + i, 0));
    CHECK(harness.submit(specs.back()));
  }
  CHECK(waitFor([&] { return harness.snapshot().queuedTickets == 100; }, std::chrono::seconds(5)));

  for (int i = 0; i < 37; ++i) CHECK(harness.engine->submitCancel(specs[static_cast<std::size_t>(i)].ids.front()));
  CHECK(waitFor([&] { return harness.snapshot().queuedTickets == 63; }, std::chrono::seconds(5)));

  auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.intervalTreeSize, static_cast<std::size_t>(63));
  CHECK_EQ(snapshot.heapSize, static_cast<std::size_t>(63));
  CHECK_EQ(snapshot.metrics.cancellations, static_cast<std::uint64_t>(37));
  CHECK_EQ(snapshot.metrics.playersWithdrawn, static_cast<std::uint64_t>(37));
  CHECK(!harness.engine->findPlayer(specs[0].ids.front()).has_value());
  CHECK_CONSERVED(snapshot);

  const auto remaining = harness.engine->queuedPlayerIds();
  CHECK_EQ(remaining.size(), static_cast<std::size_t>(63));
  for (const PlayerId id : remaining) CHECK(harness.engine->submitCancel(id));

  CHECK(waitFor([&] { return harness.snapshot().queuedTickets == 0; }, std::chrono::seconds(5)));
  snapshot = harness.snapshot();
  CHECK_EQ(snapshot.intervalTreeSize, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.heapSize, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.metrics.cancellations, static_cast<std::uint64_t>(100));
  CHECK_CONSERVED(snapshot);
}

MMF_TEST_SUITE(engine_rejects_duplicate_and_invalid_joins) {
  EngineConfig config = testConfig();
  config.maxPingDeltaMs = 0.0;  // a lone ticket can never match
  EngineHarness harness(config);

  const JoinSpec spec = makeSolo(1500.0, 30, 0);
  CHECK(harness.submit(spec));
  CHECK(waitFor([&] { return harness.snapshot().metrics.joinsAccepted == 1; },
                std::chrono::seconds(2)));
  CHECK(harness.submit(spec));  // same player id again
  CHECK(waitFor([&] { return harness.snapshot().metrics.joinsRejectedDuplicate == 1; },
                std::chrono::seconds(2)));

  JoinRequest empty;
  empty.mode = static_cast<GameModeId>(GameMode::Ranked);
  CHECK(harness.engine->submitJoin(empty));
  JoinRequest wrongMode = makeSolo(1500.0, 30, 0).request;
  wrongMode.mode = static_cast<GameModeId>(GameMode::Casual);
  CHECK(harness.engine->submitJoin(wrongMode));
  CHECK(waitFor([&] { return harness.snapshot().metrics.joinsRejectedInvalid == 2; },
                std::chrono::seconds(2)));

  CHECK_EQ(harness.snapshot().metrics.joinsAccepted, static_cast<std::uint64_t>(1));
  CHECK_CONSERVED(harness.snapshot());
}

MMF_TEST_SUITE(engine_lobby_abort_requeues_survivors) {
  EngineHarness harness(testConfig());
  std::vector<JoinSpec> specs;
  for (int i = 0; i < 10; ++i) {
    specs.push_back(makeSolo(1500.0 + i, 30, 0));
    CHECK(harness.submit(specs.back()));
  }
  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));
  const MatchId matchId = harness.recorder->matches().front().matchId;

  CHECK(harness.engine->submitLobbyAbort(matchId, AbortReason::ServerUnavailable));
  CHECK(waitFor([&] { return harness.snapshot().metrics.lobbiesAborted == 1; },
                std::chrono::seconds(5)));

  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.metrics.ticketsRequeued, static_cast<std::uint64_t>(10));
  CHECK_EQ(snapshot.metrics.lobbiesAborted, static_cast<std::uint64_t>(1));
  // The survivors keep their accumulated wait and are free to match again in the
  // very same worker iteration, so only the conservation identity is stable here.
  CHECK_CONSERVED(snapshot);

  // The requeued players keep their original wait time and re-match immediately.
  CHECK(waitFor([&] { return harness.recorder->matchCount() >= 2; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));
  CHECK_EQ(harness.recorder->abortCount(), static_cast<std::size_t>(1));
  CHECK_CONSERVED(harness.snapshot());
}

MMF_TEST_SUITE(engine_confirm_moves_players_into_match) {
  EngineHarness harness(testConfig());
  std::vector<JoinSpec> specs;
  for (int i = 0; i < 10; ++i) {
    specs.push_back(makeSolo(1500.0 + i, 30, 0));
    CHECK(harness.submit(specs.back()));
  }
  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));
  const MatchId matchId = harness.recorder->matches().front().matchId;

  CHECK(harness.engine->submitLobbyConfirmed(matchId));
  CHECK(waitFor([&] { return harness.snapshot().metrics.lobbiesConfirmed == 1; },
                std::chrono::seconds(5)));
  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.trackedLobbies, static_cast<std::size_t>(0));
  CHECK_EQ(snapshot.metrics.playersMatched, static_cast<std::uint64_t>(10));

  const auto player = harness.engine->findPlayer(specs.front().ids.front());
  CHECK(player.has_value());
  if (player) {
    CHECK(player->state() == PlayerState::InMatch);
    CHECK_EQ(player->match(), matchId);
  }
  CHECK_CONSERVED(snapshot);
}

MMF_TEST_SUITE(engine_disconnect_of_matched_player_aborts_lobby) {
  EngineHarness harness(testConfig());
  std::vector<JoinSpec> specs;
  for (int i = 0; i < 10; ++i) {
    specs.push_back(makeSolo(1500.0 + i, 30, 0));
    CHECK(harness.submit(specs.back()));
  }
  CHECK(waitFor([&] { return harness.recorder->matchCount() == 1; }, std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  CHECK(harness.engine->submitCancel(specs[3].ids.front()));
  CHECK(waitFor([&] { return harness.snapshot().metrics.lobbiesAborted == 1; },
                std::chrono::seconds(5)));
  harness.engine->flushNotifications(std::chrono::milliseconds(500));

  const auto snapshot = harness.snapshot();
  CHECK_EQ(snapshot.metrics.playersWithdrawn, static_cast<std::uint64_t>(1));
  CHECK_EQ(snapshot.queuedPlayers, static_cast<std::size_t>(9));
  CHECK_EQ(snapshot.queuedTickets, static_cast<std::size_t>(9));
  CHECK_EQ(harness.recorder->abortCount(), static_cast<std::size_t>(1));
  CHECK(!harness.engine->findPlayer(specs[3].ids.front()).has_value());
  CHECK_CONSERVED(snapshot);

  // Nine players can never fill a 5v5 lobby on their own.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK_EQ(harness.snapshot().metrics.matchesFormed, static_cast<std::uint64_t>(1));
}

MMF_TEST_SUITE(engine_shutdown_is_graceful_and_idempotent) {
  auto engine = createMatchmaker(testConfig(), GameMode::Ranked);
  auto recorder = std::make_shared<RecordingObserver>();
  engine->attachObserver(recorder);
  engine->start();
  CHECK(engine->running());
  for (int i = 0; i < 5; ++i) CHECK(engine->submitJoin(makeSolo(1500.0, 30, 0).request));

  engine->shutdown();
  CHECK(!engine->running());
  CHECK(!engine->submitJoin(makeSolo(1500.0, 30, 0).request));
  engine->shutdown();  // idempotent
  engine.reset();      // destructor path as well
  CHECK(true);
}
