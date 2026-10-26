#include "mmf/lobby.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

#include "mmf/errors.hpp"

namespace mmf {

Lobby::Lobby(MatchId id, GameModeId mode, RegionId region, std::uint32_t teamSize,
             std::uint32_t teamCount, double maxTeamAverageGap, double maxRatingSpread)
    : id_(id),
      mode_(mode),
      region_(region),
      teamSize_(teamSize),
      slotCount_(teamSize * teamCount),
      maxTeamAverageGap_(maxTeamAverageGap),
      maxRatingSpread_(maxRatingSpread),
      createdAt_(Clock::now()) {
  if (teamSize == 0 || teamCount == 0) throw DomainError("lobby needs at least one team slot");
  teams_.resize(teamCount);
}

double Lobby::teamAverage(std::size_t index) const noexcept {
  const auto& team = teams_[index];
  if (team.players.empty()) return 0.0;
  return fromRating(team.totalMmr) / static_cast<double>(team.players.size());
}

double Lobby::teamAverageGap() const noexcept {
  double lowest = std::numeric_limits<double>::max();
  double highest = std::numeric_limits<double>::lowest();
  bool any = false;
  for (std::size_t i = 0; i < teams_.size(); ++i) {
    if (teams_[i].players.empty()) continue;
    const double average = teamAverage(i);
    lowest = std::min(lowest, average);
    highest = std::max(highest, average);
    any = true;
  }
  return any ? highest - lowest : 0.0;
}

std::size_t Lobby::weakestTeamWithRoom(std::uint32_t requiredSlots) const {
  std::size_t best = teams_.size();
  double bestAverage = 0.0;
  bool haveBest = false;
  for (std::size_t i = 0; i < teams_.size(); ++i) {
    const auto taken = static_cast<std::uint32_t>(teams_[i].players.size());
    if (taken >= teamSize_ || teamSize_ - taken < requiredSlots) continue;
    // An empty team is the weakest possible team, so balance fills teams
    // round-robin first and then always feeds the lower-rated side.
    const double average = teams_[i].players.empty()
                               ? std::numeric_limits<double>::lowest()
                               : teamAverage(i);
    if (!haveBest || average < bestAverage) {
      best = i;
      bestAverage = average;
      haveBest = true;
    }
  }
  return best;
}

bool Lobby::tryAdd(const Ticket& ticket, TimePoint now) {
  if (state_ != LobbyState::Forming) return false;
  if (ticket.slots() == 0) return false;
  if (slotsFilled_ + ticket.slots() > slotCount_) return false;

  const Rating ticketMin = ticket.party().minMmr();
  const Rating ticketMax = ticket.party().maxMmr();
  const bool firstTicket = slotsFilled_ == 0;
  const Rating prospectiveMin = firstTicket ? ticketMin : std::min(minMemberRating_, ticketMin);
  const Rating prospectiveMax = firstTicket ? ticketMax : std::max(maxMemberRating_, ticketMax);
  if (fromRating(prospectiveMax - prospectiveMin) > maxRatingSpread_) return false;

  const std::size_t team = weakestTeamWithRoom(ticket.slots());
  if (team == teams_.size()) return false;  // no single team can hold the whole party

  for (const PartyMember& member : ticket.party().members()) {
    MatchedPlayer matched;
    matched.id = member.id;
    matched.name = member.name;
    matched.mmr = member.mmr;
    matched.pingMs = member.pingMs;
    matched.region = member.region;
    matched.kind = ticket.kind();
    teams_[team].players.push_back(std::move(matched));
    teams_[team].totalMmr += member.mmr;
    ++playerCount_;
  }

  if (firstTicket) {
    minPing_ = ticket.party().minPingMs();
    maxPing_ = ticket.party().maxPingMs();
  } else {
    minPing_ = std::min(minPing_, ticket.party().minPingMs());
    maxPing_ = std::max(maxPing_, ticket.party().maxPingMs());
  }

  teams_[team].averageMmr = teamAverage(team);
  if (firstTicket) {
    minMemberRating_ = ticketMin;
    maxMemberRating_ = ticketMax;
  } else {
    minMemberRating_ = prospectiveMin;
    maxMemberRating_ = prospectiveMax;
  }
  slotsFilled_ += ticket.slots();
  updateWaitAggregate(ticket.waitSeconds(now));
  tickets_.push_back(ticket);
  return true;
}

void Lobby::updateWaitAggregate(double waitSeconds) noexcept {
  oldestWait_ = std::max(oldestWait_, waitSeconds);
}

bool Lobby::ready() const noexcept {
  if (state_ != LobbyState::Forming) return false;
  if (slotsFilled_ != slotCount_) return false;
  if (teamAverageGap() > maxTeamAverageGap_) return false;
  if (ratingSpread() > maxRatingSpread_) return false;
  return true;
}

void Lobby::buildEvent() {
  event_ = MatchFoundEvent{};
  event_.matchId = id_;
  event_.mode = mode_;
  event_.region = region_;
  event_.teams = teams_;
  event_.playerCount = playerCount_;
  event_.slots = slotCount_;
  event_.teamAverageGap = teamAverageGap();
  event_.ratingSpread = ratingSpread();
  event_.pingSpreadMs = pingSpreadMs();
  event_.oldestWaitSeconds = oldestWait_;
  event_.formationMs = secondsSince(createdAt_) * 1000.0;
  event_.createdAt = Clock::now();
}

void Lobby::markReady() {
  if (state_ != LobbyState::Forming) throw DomainError("lobby already finalized");
  if (!ready()) {
    std::ostringstream out;
    out << "lobby " << id_ << " is not ready: " << slotsFilled_ << "/" << slotCount_
        << " slots, team gap " << teamAverageGap() << "ms(mmr), spread " << ratingSpread();
    throw DomainError(out.str());
  }
  state_ = LobbyState::Ready;
  buildEvent();
  broadcastMatchReady(event_);
}

void Lobby::confirm() {
  if (state_ != LobbyState::Ready) throw DomainError("confirm requires a READY lobby");
  state_ = LobbyState::Confirmed;
}

void Lobby::abort(AbortReason reason) {
  if (state_ == LobbyState::Aborted || state_ == LobbyState::Confirmed) return;
  state_ = LobbyState::Aborted;
  LobbyAbortedEvent event;
  event.matchId = id_;
  event.reason = reason;
  event.players = playerIds();
  broadcastLobbyAborted(event);
}

std::vector<PlayerId> Lobby::playerIds() const {
  std::vector<PlayerId> ids;
  ids.reserve(playerCount_);
  for (const Ticket& ticket : tickets_) {
    for (const PartyMember& member : ticket.party().members()) ids.push_back(member.id);
  }
  return ids;
}

}  // namespace mmf
