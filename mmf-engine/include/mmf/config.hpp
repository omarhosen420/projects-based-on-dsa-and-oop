// MMF Engine — tuning knobs, validated once at construction.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mmf/errors.hpp"
#include "mmf/logging.hpp"
#include "mmf/types.hpp"

namespace mmf {

/// Every mutable behaviour of the engine is a field here so that a deployment
/// can be tuned (and unit-tested) without touching code. Values are validated
/// in EngineConfig::validate(); an invalid configuration throws instead of
/// producing a silently broken queue.
struct EngineConfig {
  // ---- lobby shape --------------------------------------------------------
  std::uint32_t teamSize{5};
  std::uint32_t teamsPerMatch{2};
  std::uint32_t maxPartySize{5};
  /// Max rating gap allowed inside a single pre-made party.
  double maxPartyRatingSpread{900.0};

  // ---- skill window -------------------------------------------------------
  /// Skill tolerance (MMR) granted at t=0 by the strategy baseline.
  double baseTolerance{150.0};
  /// How fast the tolerance widens while a ticket waits (MMR per second).
  double toleranceGrowthPerSecond{30.0};
  /// Absolute ceiling for the widening window.
  double maxTolerance{1500.0};

  // ---- hard network constraints ------------------------------------------
  /// Maximum allowed |ping(a) - ping(b)| between two tickets in one match.
  double maxPingDeltaMs{55.0};
  /// Absolute ping ceiling for any participant.
  int maxPingMs{95};
  /// When false, tickets from different regions never share a match.
  bool allowCrossRegion{false};

  // ---- aging-aware priority ----------------------------------------------
  /// Weight of the (super-linear) wait term.
  double weightWait{1.0};
  /// Weight of the static skill-scarcity term.
  double weightScarcity{0.30};
  /// Aging exponent; must be >= 1. Values > 1 make old outliers overtake
  /// freshly queued players, which is the actual anti-starvation mechanism.
  double agingExponent{2.0};
  /// Wait time at which the wait term reaches its documented reference scale.
  double agingReferenceSeconds{30.0};

  // ---- scheduler ----------------------------------------------------------
  std::chrono::milliseconds workerTick{20};
  std::chrono::milliseconds priorityRefreshInterval{100};
  std::uint32_t maxAnchorAttemptsPerTick{96};
  std::uint32_t maxCandidatesPerAnchor{64};
  std::size_t commandQueueCapacity{1U << 16U};
  std::size_t eventQueueCapacity{1U << 15U};
  /// How long a producer applies backpressure before rejecting a join.
  std::chrono::milliseconds enqueueTimeout{25};
  /// Peer count above which a bracket is considered "well served" (scarcity 0).
  std::uint32_t scarcityPeerCap{32};

  // ---- lobby validation ---------------------------------------------------
  /// |avgMMR(team A) - avgMMR(team B)| must not exceed this.
  double maxTeamAverageGap{220.0};
  /// (max member MMR - min member MMR) inside one lobby.
  double maxLobbyRatingSpread{700.0};

  // ---- bookkeeping --------------------------------------------------------
  std::size_t maxTrackedLobbies{4096};
  GameModeId mode{static_cast<GameModeId>(GameMode::Ranked)};
  LogLevel logLevel{LogLevel::Warn};

  [[nodiscard]] std::size_t slotsPerLobby() const {
    return static_cast<std::size_t>(teamSize) * teamsPerMatch;
  }

  /// Throws ConfigurationError describing the first violated invariant.
  void validate() const;
};

}  // namespace mmf
