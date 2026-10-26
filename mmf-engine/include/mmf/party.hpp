// MMF Engine — Party (pre-made group) aggregate.
#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "mmf/errors.hpp"
#include "mmf/types.hpp"

namespace mmf {

/// Snapshot of one party member as it entered the queue. Tickets deliberately
/// copy member data: a lobby must stay valid even if a player's session object
/// is dropped while the match is being announced.
struct PartyMember {
  PlayerId id{0};
  std::string name;
  Rating mmr{0};
  int pingMs{0};
  RegionId region{0};

  [[nodiscard]] double mmrValue() const noexcept { return fromRating(mmr); }
};

/// A pre-made group treated as one indivisible queue node.
///
/// Aggregates are computed once in the constructor so the matchmaker can read
/// them in O(1) during a matching pass. "Average party MMR" is the number the
/// engine matches on, while min/max member MMR feed the lobby spread guard.
class Party {
 public:
  explicit Party(std::vector<PartyMember> members) : members_(std::move(members)) {
    if (members_.empty()) throw DomainError("party must contain at least one member");
    for (const PartyMember& member : members_) {
      totalMmr_ += member.mmr;
      maxPingMs_ = firstPing_ ? member.pingMs : std::max(maxPingMs_, member.pingMs);
      minPingMs_ = firstPing_ ? member.pingMs : std::min(minPingMs_, member.pingMs);
      firstPing_ = false;
      pingSumMs_ += member.pingMs;
      if (first_) {
        minMmr_ = member.mmr;
        maxMmr_ = member.mmr;
        region_ = member.region;
        regionHomogeneous_ = true;
        first_ = false;
      } else {
        minMmr_ = std::min(minMmr_, member.mmr);
        maxMmr_ = std::max(maxMmr_, member.mmr);
        if (member.region != region_) regionHomogeneous_ = false;
      }
    }
    if (members_.size() > 1) {
      label_ = members_.front().name + "+" + std::to_string(members_.size() - 1);
    } else {
      label_ = members_.front().name;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
  [[nodiscard]] bool solo() const noexcept { return members_.size() == 1; }
  [[nodiscard]] Rating totalMmr() const noexcept { return totalMmr_; }
  [[nodiscard]] Rating minMmr() const noexcept { return minMmr_; }
  [[nodiscard]] Rating maxMmr() const noexcept { return maxMmr_; }

  /// Average member MMR rounded to the nearest rating unit.
  [[nodiscard]] Rating averageRating() const noexcept {
    const auto size = static_cast<Rating>(members_.size());
    const Rating half = size / 2;
    return totalMmr_ >= 0 ? (totalMmr_ + half) / size : (totalMmr_ - half) / size;
  }

  [[nodiscard]] double averageMmr() const noexcept { return fromRating(averageRating()); }
  [[nodiscard]] double averagePingMs() const noexcept {
    return members_.empty() ? 0.0
                            : static_cast<double>(pingSumMs_) / static_cast<double>(members_.size());
  }
  [[nodiscard]] int maxPingMs() const noexcept { return maxPingMs_; }
  [[nodiscard]] int minPingMs() const noexcept { return minPingMs_; }
  [[nodiscard]] RegionId region() const noexcept { return region_; }
  [[nodiscard]] bool regionHomogeneous() const noexcept { return regionHomogeneous_; }
  [[nodiscard]] const std::string& label() const noexcept { return label_; }
  [[nodiscard]] const std::vector<PartyMember>& members() const noexcept { return members_; }

  /// Widest intra-party rating gap; used to reject absurd pre-mades.
  [[nodiscard]] Rating ratingSpread() const noexcept { return maxMmr_ - minMmr_; }

 private:
  std::vector<PartyMember> members_;
  std::string label_;
  Rating totalMmr_{0};
  Rating minMmr_{0};
  Rating maxMmr_{0};
  std::int64_t pingSumMs_{0};
  int maxPingMs_{0};
  int minPingMs_{0};
  bool firstPing_{true};
  RegionId region_{0};
  bool regionHomogeneous_{true};
  bool first_{true};
};

}  // namespace mmf
