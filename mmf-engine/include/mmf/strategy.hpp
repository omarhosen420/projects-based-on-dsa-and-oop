// MMF Engine — pluggable policies (Strategy pattern).
//
// Three independent axes are abstracted here so that queue behaviour can be
// changed without touching the engine:
//   * IMatchmakingStrategy  — skill window growth + candidate ranking (mode)
//   * IPriorityPolicy       — aging-aware priority (anti-starvation)
//   * ILatencyPolicy        — hard network admissibility checks
//
// The latency policy is deliberately *not* part of the matchmaking strategy:
// geographical constraints are safety limits, not matchmaking preferences.
#pragma once

#include <cstdint>
#include <string_view>

#include "mmf/config.hpp"
#include "mmf/ticket.hpp"

namespace mmf {

class IMatchmakingStrategy {
 public:
  virtual ~IMatchmakingStrategy() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;

  /// Skill tolerance (in MMR) the strategy grants this ticket at `now`.
  /// Widening with wait time is what lets an isolated bracket eventually match.
  [[nodiscard]] virtual double toleranceFor(const Ticket& ticket, TimePoint now) const = 0;

  /// Ranking score in [0, 1] for a candidate seen from an anchor.
  [[nodiscard]] virtual double candidateScore(const Ticket& anchor, const Ticket& candidate,
                                              TimePoint now) const = 0;

  /// Skill window at t=0, exposed for diagnostics and tests.
  [[nodiscard]] virtual double baseTolerance() const = 0;

  [[nodiscard]] virtual std::uint32_t maxPartySize() const = 0;
};

/// Tight windows, slow widening, skill-dominated ranking.
class RankedStrategy final : public IMatchmakingStrategy {
 public:
  explicit RankedStrategy(EngineConfig config) : config_(config) {}

  [[nodiscard]] std::string_view name() const override { return "ranked"; }
  [[nodiscard]] double toleranceFor(const Ticket& ticket, TimePoint now) const override;
  [[nodiscard]] double candidateScore(const Ticket& anchor, const Ticket& candidate,
                                      TimePoint now) const override;
  [[nodiscard]] double baseTolerance() const override;
  [[nodiscard]] std::uint32_t maxPartySize() const override { return config_.maxPartySize; }

 private:
  EngineConfig config_;
};

/// Wide windows, fast widening, wait-time-dominated ranking.
class CasualStrategy final : public IMatchmakingStrategy {
 public:
  explicit CasualStrategy(EngineConfig config) : config_(config) {}

  [[nodiscard]] std::string_view name() const override { return "casual"; }
  [[nodiscard]] double toleranceFor(const Ticket& ticket, TimePoint now) const override;
  [[nodiscard]] double candidateScore(const Ticket& anchor, const Ticket& candidate,
                                      TimePoint now) const override;
  [[nodiscard]] double baseTolerance() const override;
  [[nodiscard]] std::uint32_t maxPartySize() const override { return config_.maxPartySize; }

 private:
  EngineConfig config_;
};

class IPriorityPolicy {
 public:
  virtual ~IPriorityPolicy() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;

  /// >= 0, monotonically non-decreasing in wait time.
  [[nodiscard]] virtual double priority(const Ticket& ticket, TimePoint now) const = 0;

  /// The heap is a min-heap, so urgency is stored negated.
  [[nodiscard]] double sortKey(const Ticket& ticket, TimePoint now) const {
    return -priority(ticket, now);
  }
};

/// priority(t) = weightWait * 1000 * (wait / ref)^exponent
///             + weightScarcity * 1000 * (scarcity / 100)
///
/// The super-linear wait term (exponent >= 1) is the anti-starvation guarantee:
/// with exponent 2 a ticket waiting 60 s outranks a fresh ticket by 4000 points,
/// while the scarcity term can never exceed weightScarcity * 1000. An isolated
/// skill bracket therefore climbs to the front of the queue and gets the first
/// chance at the widened skill window.
class AgingPriorityPolicy final : public IPriorityPolicy {
 public:
  explicit AgingPriorityPolicy(EngineConfig config) : config_(config) {}

  [[nodiscard]] std::string_view name() const override { return "aging"; }
  [[nodiscard]] double priority(const Ticket& ticket, TimePoint now) const override;

 private:
  EngineConfig config_;
};

class ILatencyPolicy {
 public:
  virtual ~ILatencyPolicy() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;

  /// Can this ticket ever participate in a match? (absolute ping caps and
  /// intra-party latency spread). Checked once at join time.
  [[nodiscard]] virtual bool admissible(const Ticket& ticket) const = 0;

  /// Hard constraint between two tickets: region policy, absolute ping ceiling
  /// and the maximum allowed ping delta.
  [[nodiscard]] virtual bool compatible(const Ticket& a, const Ticket& b) const = 0;

  [[nodiscard]] virtual double maxDeltaMs() const = 0;
};

class HardLatencyPolicy final : public ILatencyPolicy {
 public:
  explicit HardLatencyPolicy(EngineConfig config) : config_(config) {}

  [[nodiscard]] std::string_view name() const override { return "hard-latency"; }
  [[nodiscard]] bool admissible(const Ticket& ticket) const override;
  [[nodiscard]] bool compatible(const Ticket& a, const Ticket& b) const override;
  [[nodiscard]] double maxDeltaMs() const override { return config_.maxPingDeltaMs; }

 private:
  EngineConfig config_;
};

}  // namespace mmf
