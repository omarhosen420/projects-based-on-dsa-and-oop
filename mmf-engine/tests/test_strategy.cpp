// Policy tests: aging curve, mode strategies, hard latency constraints.
#include "mmf/strategy.hpp"

#include <cmath>
#include <vector>

#include "engine_fixture.hpp"
#include "test_support.hpp"

using namespace mmf;
using namespace mmftest;

namespace {

Ticket ticketAt(double mmr, TimePoint enqueuedAt, double scarcity = 0.0,
                double tolerance = 200.0) {
  Party party({PartyMember{nextPlayerId(), "p", toRating(mmr), 30, 0}});
  Ticket ticket(nextTicketId(), party, static_cast<GameModeId>(GameMode::Ranked),
                toRating(tolerance), enqueuedAt);
  ticket.setScarcity(scarcity);
  return ticket;
}

}  // namespace

MMF_TEST_SUITE(aging_priority_rises_with_wait_time) {
  EngineConfig config = testConfig();
  config.agingReferenceSeconds = 10.0;
  config.agingExponent = 2.0;
  AgingPriorityPolicy policy(config);

  const TimePoint start = Clock::now();
  const Ticket fresh = ticketAt(1500.0, start);
  const double p0 = policy.priority(fresh, start);
  const double p10 = policy.priority(fresh, start + std::chrono::seconds(10));
  const double p20 = policy.priority(fresh, start + std::chrono::seconds(20));

  CHECK(p0 >= 0.0);
  CHECK(p10 > p0);
  CHECK(p20 > p10);
  // Super-linear: doubling the wait more than doubles the wait contribution.
  CHECK(p20 - p0 > 2.0 * (p10 - p0));
  // The heap stores negated keys, so a longer wait must sort first.
  CHECK(policy.sortKey(fresh, start + std::chrono::seconds(20)) <
        policy.sortKey(fresh, start + std::chrono::seconds(10)));
}

MMF_TEST_SUITE(aging_priority_lifts_an_isolated_bracket_above_fresh_players) {
  EngineConfig config = testConfig();
  config.agingReferenceSeconds = 10.0;
  config.agingExponent = 2.0;
  AgingPriorityPolicy policy(config);

  const TimePoint now = Clock::now();
  // Old isolated player: worst scarcity, waited 60 s (6 reference units).
  const Ticket oldOutlier = ticketAt(2600.0, now - std::chrono::seconds(60), 100.0);
  // Fresh well-served player: best scarcity, zero wait.
  const Ticket freshMainstream = ticketAt(1500.0, now, 0.0);

  CHECK(policy.priority(oldOutlier, now) > policy.priority(freshMainstream, now));
  // Without the aging term the isolated bracket only earns its bounded scarcity
  // bonus; the (super-linear) wait term is what lifts it above everyone else.
  const double staticOnly = policy.priority(oldOutlier, oldOutlier.enqueuedAt());
  CHECK(staticOnly <= config.weightScarcity * 1000.0 + 1e-9);
  CHECK(policy.priority(oldOutlier, now) > 10.0 * staticOnly);
}

MMF_TEST_SUITE(mode_strategies_differ_in_window_and_ranking) {
  EngineConfig config = testConfig();
  config.agingReferenceSeconds = 10.0;
  RankedStrategy ranked(config);
  CasualStrategy casual(config);

  const TimePoint now = Clock::now();
  const Ticket ticket = ticketAt(1500.0, now - std::chrono::seconds(5));

  CHECK(ranked.baseTolerance() < casual.baseTolerance());
  CHECK(ranked.toleranceFor(ticket, now) < casual.toleranceFor(ticket, now));

  const Ticket anchor = ticketAt(1500.0, now);
  const Ticket nearPeer = ticketAt(1520.0, now);
  const Ticket farPeer = ticketAt(1800.0, now);
  CHECK(ranked.candidateScore(anchor, nearPeer, now) > ranked.candidateScore(anchor, farPeer, now));
  CHECK(casual.candidateScore(anchor, nearPeer, now) > casual.candidateScore(anchor, farPeer, now));
}

MMF_TEST_SUITE(latency_policy_enforces_hard_constraints) {
  EngineConfig config = testConfig();
  config.maxPingDeltaMs = 30.0;
  config.maxPingMs = 90;
  config.allowCrossRegion = false;
  HardLatencyPolicy policy(config);
  const TimePoint now = Clock::now();

  const Ticket lowPing = ticketAt(1500.0, now);
  Ticket nearPing = ticketAt(1500.0, now);
  CHECK(policy.compatible(lowPing, nearPing));

  Party laggyParty({PartyMember{nextPlayerId(), "lag", toRating(1500.0), 80, 0}});
  const Ticket laggy(nextTicketId(), laggyParty, static_cast<GameModeId>(GameMode::Ranked),
                     toRating(200.0), now);
  CHECK(policy.admissible(laggy));
  CHECK(!policy.compatible(lowPing, laggy));  // |30-80| > 30 delta cap

  Party unplayableParty({PartyMember{nextPlayerId(), "bad", toRating(1500.0), 220, 0}});
  const Ticket unplayable(nextTicketId(), unplayableParty,
                          static_cast<GameModeId>(GameMode::Ranked), toRating(200.0), now);
  CHECK(!policy.admissible(unplayable));  // above the absolute ceiling

  Party otherRegion({PartyMember{nextPlayerId(), "eu", toRating(1500.0), 30, 7}});
  const Ticket crossRegion(nextTicketId(), otherRegion, static_cast<GameModeId>(GameMode::Ranked),
                           toRating(200.0), now);
  CHECK(!policy.compatible(lowPing, crossRegion));

  // A pre-made whose own members violate the delta rule is not admissible.
  Party splitPing({PartyMember{nextPlayerId(), "a", toRating(1500.0), 10, 0},
                   PartyMember{nextPlayerId(), "b", toRating(1500.0), 85, 0}});
  const Ticket splitTicket(nextTicketId(), splitPing, static_cast<GameModeId>(GameMode::Ranked),
                           toRating(200.0), now);
  CHECK(!policy.admissible(splitTicket));
}

MMF_TEST_SUITE(config_validation_rejects_unsafe_values) {
  EngineConfig config;
  config.validate();

  EngineConfig noTeams = config;
  noTeams.teamsPerMatch = 1;
  CHECK_THROWS(noTeams.validate(), ConfigurationError);

  EngineConfig badParty = config;
  badParty.maxPartySize = 6;
  badParty.teamSize = 5;
  CHECK_THROWS(badParty.validate(), ConfigurationError);

  EngineConfig invertedAging = config;
  invertedAging.agingExponent = 0.5;  // would let fresh players outrank starved ones
  CHECK_THROWS(invertedAging.validate(), ConfigurationError);

  EngineConfig invertedWindow = config;
  invertedWindow.maxTolerance = invertedWindow.baseTolerance / 2.0;
  CHECK_THROWS(invertedWindow.validate(), ConfigurationError);

  EngineConfig zeroTick = config;
  zeroTick.workerTick = std::chrono::milliseconds(0);
  CHECK_THROWS(zeroTick.validate(), ConfigurationError);
}
