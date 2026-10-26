// MMF Engine — Ticket: the unit the matchmaker indexes, orders and consumes.
#pragma once

#include <cstddef>
#include <utility>

#include "mmf/party.hpp"
#include "mmf/types.hpp"

namespace mmf {

/// A ticket is either a solo player (party of one) or a whole pre-made party.
/// It is the composite node of the queue: the interval tree and the heap both
/// store TicketIds, never player pointers, so a party always occupies exactly
/// one window and is never split across lobbies.
class Ticket {
 public:
  Ticket(TicketId id, Party party, GameModeId mode, Rating baseTolerance, TimePoint enqueuedAt)
      : id_(id),
        party_(std::move(party)),
        mode_(mode),
        baseTolerance_(baseTolerance),
        enqueuedAt_(enqueuedAt) {}

  [[nodiscard]] TicketId id() const noexcept { return id_; }
  [[nodiscard]] const Party& party() const noexcept { return party_; }
  [[nodiscard]] std::uint32_t slots() const noexcept { return static_cast<std::uint32_t>(party_.size()); }
  [[nodiscard]] TicketKind kind() const noexcept {
    return party_.solo() ? TicketKind::Solo : TicketKind::Party;
  }
  [[nodiscard]] GameModeId mode() const noexcept { return mode_; }

  [[nodiscard]] Rating averageRating() const noexcept { return party_.averageRating(); }
  [[nodiscard]] double averageMmr() const noexcept { return party_.averageMmr(); }
  [[nodiscard]] Rating ratingLo() const noexcept { return averageRating() - baseTolerance_; }
  [[nodiscard]] Rating ratingHi() const noexcept { return averageRating() + baseTolerance_; }
  [[nodiscard]] Rating baseTolerance() const noexcept { return baseTolerance_; }

  [[nodiscard]] RegionId region() const noexcept { return party_.region(); }
  [[nodiscard]] int maxPingMs() const noexcept { return party_.maxPingMs(); }
  [[nodiscard]] double averagePingMs() const noexcept { return party_.averagePingMs(); }

  [[nodiscard]] TimePoint enqueuedAt() const noexcept { return enqueuedAt_; }
  void setEnqueuedAt(TimePoint when) noexcept { enqueuedAt_ = when; }

  [[nodiscard]] double scarcity() const noexcept { return scarcity_; }
  void setScarcity(double value) noexcept { scarcity_ = value; }

  [[nodiscard]] double waitSeconds(TimePoint now) const noexcept {
    return secondsSince(enqueuedAt_, now);
  }

 private:
  TicketId id_{0};
  Party party_;
  GameModeId mode_{0};
  Rating baseTolerance_{0};
  TimePoint enqueuedAt_{};
  double scarcity_{0.0};  // static skill-scarcity estimator in [0, 100]
};

}  // namespace mmf
