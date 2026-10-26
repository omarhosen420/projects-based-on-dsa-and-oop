// MMF Engine — Lobby aggregate (the Subject of match-ready notifications).
//
// A Lobby owns the composition invariants:
//   * exactly `teamSize * teamCount` reserved slots, never oversubscribed,
//   * parties are placed whole (a party can never straddle two teams),
//   * teams stay balanced (empirical average MMR gap),
//   * no lobby exceeds the configured rating spread.
//
// It is also the only place where a match becomes READY, which is why it is the
// Subject that publishes MatchFoundEvent to its observers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mmf/events.hpp"
#include "mmf/observer.hpp"
#include "mmf/ticket.hpp"
#include "mmf/types.hpp"

namespace mmf {

class Lobby : public Subject {
 public:
  Lobby(MatchId id, GameModeId mode, RegionId region, std::uint32_t teamSize,
        std::uint32_t teamCount, double maxTeamAverageGap, double maxRatingSpread);

  /// Reserves the whole ticket inside the weakest team that can hold it.
  /// Returns false when the lobby cannot legally accept the ticket.
  bool tryAdd(const Ticket& ticket, TimePoint now = Clock::now());

  /// True when every slot is reserved and both balance guards hold.
  [[nodiscard]] bool ready() const noexcept;

  /// Transitions Forming -> Ready and publishes MatchFoundEvent to observers.
  void markReady();

  /// Ready/Forming -> Confirmed (the game server accepted the allocation).
  void confirm();

  /// Any live state -> Aborted; publishes LobbyAbortedEvent.
  void abort(AbortReason reason);

  [[nodiscard]] MatchId id() const noexcept { return id_; }
  [[nodiscard]] GameModeId mode() const noexcept { return mode_; }
  [[nodiscard]] RegionId region() const noexcept { return region_; }
  [[nodiscard]] LobbyState state() const noexcept { return state_; }

  [[nodiscard]] std::uint32_t slotsRemaining() const noexcept { return slotCount_ - slotsFilled_; }
  [[nodiscard]] std::uint32_t slotCount() const noexcept { return slotCount_; }
  [[nodiscard]] std::size_t playerCount() const noexcept { return playerCount_; }
  [[nodiscard]] std::size_t ticketCount() const noexcept { return tickets_.size(); }
  [[nodiscard]] bool empty() const noexcept { return tickets_.empty(); }

  [[nodiscard]] const std::vector<Ticket>& tickets() const noexcept { return tickets_; }
  [[nodiscard]] const std::vector<TeamSnapshot>& teams() const noexcept { return teams_; }
  [[nodiscard]] const MatchFoundEvent& event() const noexcept { return event_; }

  /// max(member MMR) - min(member MMR) across the whole lobby.
  [[nodiscard]] double ratingSpread() const noexcept {
    return slotsFilled_ == 0 ? 0.0 : fromRating(maxMemberRating_ - minMemberRating_);
  }

  /// Largest team-vs-team average MMR gap (0 for a single team).
  [[nodiscard]] double teamAverageGap() const noexcept;

  /// Largest member ping minus smallest member ping.
  [[nodiscard]] double pingSpreadMs() const noexcept {
    return slotsFilled_ == 0 ? 0.0 : static_cast<double>(maxPing_ - minPing_);
  }

  /// Wait (seconds) of the longest-waiting ticket that made it into the match.
  [[nodiscard]] double oldestWaitSeconds() const noexcept { return oldestWait_; }

  [[nodiscard]] std::vector<PlayerId> playerIds() const;

 private:
  [[nodiscard]] double teamAverage(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t weakestTeamWithRoom(std::uint32_t requiredSlots) const;
  void buildEvent();
  void updateWaitAggregate(double waitSeconds) noexcept;

  MatchId id_{0};
  GameModeId mode_{0};
  RegionId region_{0};
  std::uint32_t teamSize_{1};
  std::uint32_t slotCount_{1};
  std::uint32_t slotsFilled_{0};
  std::size_t playerCount_{0};
  double maxTeamAverageGap_{0.0};
  double maxRatingSpread_{0.0};
  LobbyState state_{LobbyState::Forming};

  std::vector<Ticket> tickets_;
  std::vector<TeamSnapshot> teams_;
  Rating minMemberRating_{0};
  Rating maxMemberRating_{0};
  int minPing_{0};
  int maxPing_{0};
  double oldestWait_{0.0};
  TimePoint createdAt_{};
  MatchFoundEvent event_{};
};

}  // namespace mmf
