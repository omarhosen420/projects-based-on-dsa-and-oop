// MMF Engine — immutable event payloads crossing the observer boundary.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mmf/party.hpp"
#include "mmf/types.hpp"

namespace mmf {

struct MatchedPlayer {
  PlayerId id{0};
  std::string name;
  Rating mmr{0};
  int pingMs{0};
  RegionId region{0};
  TicketKind kind{TicketKind::Solo};
};

struct TeamSnapshot {
  std::vector<MatchedPlayer> players;
  Rating totalMmr{0};
  double averageMmr{0.0};
};

struct MatchFoundEvent {
  MatchId matchId{0};
  GameModeId mode{0};
  RegionId region{0};
  std::vector<TeamSnapshot> teams;
  std::size_t playerCount{0};
  std::uint32_t slots{0};
  double teamAverageGap{0.0};
  double ratingSpread{0.0};
  double pingSpreadMs{0.0};
  double oldestWaitSeconds{0.0};
  double slowestWaitSeconds{0.0};
  double formationMs{0.0};
  TimePoint createdAt{};
};

struct LobbyAbortedEvent {
  MatchId matchId{0};
  AbortReason reason{AbortReason::Policy};
  std::vector<PlayerId> players;
};

}  // namespace mmf
