// MMF Engine — Player aggregate.
#pragma once

#include <string>
#include <utility>

#include "mmf/types.hpp"

namespace mmf {

/// Immutable description supplied by the network layer when a session starts.
struct PlayerProfile {
  PlayerId id{0};
  std::string name;
  double mmr{1000.0};
  int pingMs{0};
  RegionId region{0};
};

/// Mutable queue state of one connected player.
///
/// Ownership: shared_ptr, held by IPlayerRepository. Mutated exclusively by the
/// matchmaking worker while it holds the engine's state mutex — readers get
/// snapshots instead of references, so no player object can be observed in a
/// half-updated state.
class Player {
 public:
  explicit Player(PlayerProfile profile)
      : profile_(std::move(profile)),
        mmr_(toRating(profile_.mmr)),
        state_(PlayerState::Idle) {}

  [[nodiscard]] PlayerId id() const noexcept { return profile_.id; }
  [[nodiscard]] const std::string& name() const noexcept { return profile_.name; }
  [[nodiscard]] double mmr() const noexcept { return profile_.mmr; }
  [[nodiscard]] Rating rating() const noexcept { return mmr_; }
  [[nodiscard]] int pingMs() const noexcept { return profile_.pingMs; }
  [[nodiscard]] RegionId region() const noexcept { return profile_.region; }

  [[nodiscard]] PlayerState state() const noexcept { return state_; }
  [[nodiscard]] TicketId ticket() const noexcept { return ticket_; }
  [[nodiscard]] MatchId match() const noexcept { return match_; }

  void markQueued(TicketId ticket) noexcept {
    state_ = PlayerState::Queued;
    ticket_ = ticket;
    match_ = 0;
  }

  void markMatched(MatchId match) noexcept {
    state_ = PlayerState::Matched;
    ticket_ = 0;
    match_ = match;
  }

  void markInMatch(MatchId match) noexcept {
    state_ = PlayerState::InMatch;
    ticket_ = 0;
    match_ = match;
  }

  void markIdle() noexcept {
    state_ = PlayerState::Idle;
    ticket_ = 0;
    match_ = 0;
  }

  void markDisconnected() noexcept {
    state_ = PlayerState::Disconnected;
    ticket_ = 0;
  }

 private:
  PlayerProfile profile_;
  Rating mmr_;
  PlayerState state_;
  TicketId ticket_{0};
  MatchId match_{0};
};

}  // namespace mmf
