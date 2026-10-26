// Lobby composition invariants + Subject notification behaviour.
#include "mmf/lobby.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "engine_fixture.hpp"
#include "test_support.hpp"

using namespace mmf;
using namespace mmftest;

namespace {

std::shared_ptr<Lobby> makeLobby(std::uint32_t teamSize = 5, std::uint32_t teams = 2,
                                 double maxGap = 600.0, double maxSpread = 1200.0) {
  return std::make_shared<Lobby>(42, static_cast<GameModeId>(GameMode::Ranked), 0, teamSize, teams,
                                 maxGap, maxSpread);
}

Ticket soloTicket(double mmr) {
  return makeTicket(Party({PartyMember{nextPlayerId(), "p", toRating(mmr), 30, 0}}));
}

}  // namespace

MMF_TEST_SUITE(lobby_fills_all_slots_and_balances_teams) {
  auto lobby = makeLobby();
  auto observer = std::make_shared<RecordingObserver>();
  lobby->attach(observer);

  // Ten players, deliberately ordered strongest -> weakest.
  for (int i = 0; i < 10; ++i) {
    CHECK(lobby->tryAdd(soloTicket(1000.0 + static_cast<double>((9 - i) * 10))));
  }
  CHECK_EQ(lobby->slotsRemaining(), static_cast<std::uint32_t>(0));
  CHECK_EQ(lobby->playerCount(), static_cast<std::size_t>(10));
  CHECK(lobby->ready());

  lobby->markReady();
  CHECK(lobby->state() == LobbyState::Ready);

  const auto matches = observer->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    CHECK_EQ(matches[0].playerCount, static_cast<std::size_t>(10));
    CHECK_EQ(matches[0].teams.size(), static_cast<std::size_t>(2));
    CHECK_EQ(matches[0].teams[0].players.size(), static_cast<std::size_t>(5));
    CHECK_EQ(matches[0].teams[1].players.size(), static_cast<std::size_t>(5));
    const double gap = std::abs(matches[0].teams[0].averageMmr - matches[0].teams[1].averageMmr);
    CHECK(lobby->teamAverageGap() <= 600.0);
    CHECK(gap <= 600.0);
    CHECK_EQ(matches[0].slots, static_cast<std::uint32_t>(10));
  }

  // A full lobby rejects further tickets instead of oversubscribing.
  CHECK(!lobby->tryAdd(soloTicket(1000.0)));

  lobby->confirm();
  CHECK(lobby->state() == LobbyState::Confirmed);
  CHECK_THROWS(lobby->markReady(), DomainError);
}

MMF_TEST_SUITE(lobby_places_parties_whole) {
  auto lobby = makeLobby();
  auto observer = std::make_shared<RecordingObserver>();
  lobby->attach(observer);

  const auto partyProfiles = std::vector<PlayerProfile>{profileOf(1000.0), profileOf(1005.0),
                                                        profileOf(1010.0)};
  Party party({PartyMember{partyProfiles[0].id, "a", toRating(1000.0), 30, 0},
               PartyMember{partyProfiles[1].id, "b", toRating(1005.0), 30, 0},
               PartyMember{partyProfiles[2].id, "c", toRating(1010.0), 30, 0}});
  const Ticket partyTicket(nextTicketId(), party, static_cast<GameModeId>(GameMode::Ranked),
                           toRating(200.0), Clock::now());
  CHECK(lobby->tryAdd(partyTicket));
  for (int i = 0; i < 7; ++i) CHECK(lobby->tryAdd(soloTicket(1000.0 + i)));
  CHECK(lobby->ready());
  lobby->markReady();

  const auto matches = observer->matches();
  CHECK_EQ(matches.size(), static_cast<std::size_t>(1));
  if (!matches.empty()) {
    for (const auto& team : matches[0].teams) {
      int partyMembersInTeam = 0;
      for (const auto& player : team.players) {
        if (player.id == partyProfiles[0].id || player.id == partyProfiles[1].id ||
            player.id == partyProfiles[2].id) {
          ++partyMembersInTeam;
        }
      }
      // All three party members must land on exactly one team.
      CHECK(partyMembersInTeam == 0 || partyMembersInTeam == 3);
    }
  }
}

MMF_TEST_SUITE(lobby_rejects_party_that_cannot_fit) {
  auto lobby = makeLobby();  // 2 teams of 5
  for (int i = 0; i < 8; ++i) CHECK(lobby->tryAdd(soloTicket(1000.0 + i)));  // 4 + 4

  Party triple({PartyMember{nextPlayerId(), "a", toRating(1000.0), 30, 0},
                PartyMember{nextPlayerId(), "b", toRating(1001.0), 30, 0},
                PartyMember{nextPlayerId(), "c", toRating(1002.0), 30, 0}});
  const Ticket tripleTicket(nextTicketId(), triple, static_cast<GameModeId>(GameMode::Ranked),
                            toRating(200.0), Clock::now());
  // Two free slots exist in total, but no single team can host a party of 3.
  CHECK(!lobby->tryAdd(tripleTicket));
  CHECK_EQ(lobby->slotsRemaining(), static_cast<std::uint32_t>(2));

  // Two more solos always fit into the remaining two slots.
  CHECK(lobby->tryAdd(soloTicket(1003.0)));
  CHECK(lobby->tryAdd(soloTicket(1004.0)));
  CHECK_EQ(lobby->slotsRemaining(), static_cast<std::uint32_t>(0));
  CHECK(lobby->ready());
}

MMF_TEST_SUITE(lobby_enforces_rating_spread_and_state_machine) {
  auto lobby = makeLobby(5, 2, 600.0, 120.0);  // tight spread guard
  CHECK(lobby->tryAdd(soloTicket(1000.0)));
  CHECK(!lobby->tryAdd(soloTicket(1400.0)));  // 400 MMR spread > 120
  CHECK_EQ(lobby->playerCount(), static_cast<std::size_t>(1));

  CHECK_THROWS(lobby->markReady(), DomainError);   // not full
  CHECK_THROWS(lobby->confirm(), DomainError);     // not ready
  CHECK(lobby->state() == LobbyState::Forming);

  auto observer = std::make_shared<RecordingObserver>();
  lobby->attach(observer);
  for (int i = 0; i < 9; ++i) CHECK(lobby->tryAdd(soloTicket(1000.0 + i)));
  CHECK(lobby->ready());
  lobby->abort(AbortReason::ServerUnavailable);
  CHECK(lobby->state() == LobbyState::Aborted);
  CHECK_EQ(observer->abortCount(), static_cast<std::size_t>(1));
  CHECK_EQ(lobby->playerIds().size(), static_cast<std::size_t>(10));
}
