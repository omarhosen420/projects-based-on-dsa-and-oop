#include "mmf/strategy.hpp"

#include <algorithm>
#include <cmath>

namespace mmf {
namespace {

/// Shared helper: how far a ticket's window has widened at `now`.
double widenedTolerance(double base, double growthPerSecond, double ceiling, const Ticket& ticket,
                        TimePoint now) {
  const double waited = std::max(0.0, ticket.waitSeconds(now));
  return std::min(ceiling, base + growthPerSecond * waited);
}

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

}  // namespace

// ---------------------------------------------------------------- Ranked ----

double RankedStrategy::baseTolerance() const { return config_.baseTolerance * 0.75; }

double RankedStrategy::toleranceFor(const Ticket& ticket, TimePoint now) const {
  return widenedTolerance(baseTolerance(), config_.toleranceGrowthPerSecond * 0.75,
                          config_.maxTolerance * 0.85, ticket, now);
}

double RankedStrategy::candidateScore(const Ticket& anchor, const Ticket& candidate,
                                      TimePoint now) const {
  const double skillGap =
      std::abs(fromRating(anchor.averageRating() - candidate.averageRating())) /
      std::max(1.0, config_.maxTolerance);
  const double latencyGap =
      std::abs(anchor.averagePingMs() - candidate.averagePingMs()) /
      std::max(1.0, config_.maxPingDeltaMs);
  const double waited = clamp01(candidate.waitSeconds(now) / config_.agingReferenceSeconds);
  // Ranked cares about skill first; waiting still breaks ties.
  return 0.70 * (1.0 - clamp01(skillGap)) + 0.15 * (1.0 - clamp01(latencyGap)) + 0.15 * waited;
}

// ---------------------------------------------------------------- Casual ----

double CasualStrategy::baseTolerance() const { return config_.baseTolerance * 1.40; }

double CasualStrategy::toleranceFor(const Ticket& ticket, TimePoint now) const {
  return widenedTolerance(baseTolerance(), config_.toleranceGrowthPerSecond * 2.0,
                          config_.maxTolerance, ticket, now);
}

double CasualStrategy::candidateScore(const Ticket& anchor, const Ticket& candidate,
                                      TimePoint now) const {
  const double skillGap =
      std::abs(fromRating(anchor.averageRating() - candidate.averageRating())) /
      std::max(1.0, config_.maxTolerance);
  const double latencyGap =
      std::abs(anchor.averagePingMs() - candidate.averagePingMs()) /
      std::max(1.0, config_.maxPingDeltaMs);
  const double waited = clamp01(candidate.waitSeconds(now) / config_.agingReferenceSeconds);
  // Casual trades match quality for queue speed.
  return 0.40 * (1.0 - clamp01(skillGap)) + 0.15 * (1.0 - clamp01(latencyGap)) + 0.45 * waited;
}

// -------------------------------------------------------------- Priority ----

double AgingPriorityPolicy::priority(const Ticket& ticket, TimePoint now) const {
  const double waited = std::max(0.0, ticket.waitSeconds(now));
  const double normalized = waited / config_.agingReferenceSeconds;
  const double waitTerm =
      config_.weightWait * 1000.0 * std::pow(normalized, config_.agingExponent);
  const double scarcityTerm =
      config_.weightScarcity * 1000.0 * (std::clamp(ticket.scarcity(), 0.0, 100.0) / 100.0);
  return waitTerm + scarcityTerm;
}

// --------------------------------------------------------------- Latency ----

bool HardLatencyPolicy::admissible(const Ticket& ticket) const {
  // Absolute ceiling: a player above it is unplayable regardless of the lobby.
  if (ticket.maxPingMs() > config_.maxPingMs) return false;
  if (!ticket.party().regionHomogeneous() && !config_.allowCrossRegion) return false;
  // A pre-made party must also satisfy the delta rule between its own members,
  // otherwise adding it would silently violate the lobby-wide constraint.
  const auto& members = ticket.party().members();
  for (const PartyMember& a : members) {
    for (const PartyMember& b : members) {
      if (std::abs(a.pingMs - b.pingMs) > config_.maxPingDeltaMs) return false;
    }
  }
  return true;
}

bool HardLatencyPolicy::compatible(const Ticket& a, const Ticket& b) const {
  if (!admissible(a) || !admissible(b)) return false;
  if (!config_.allowCrossRegion && a.region() != b.region()) return false;
  if (std::abs(a.averagePingMs() - b.averagePingMs()) > config_.maxPingDeltaMs) return false;

  // Worst case between any two members, not just the averages: a party with a
  // 20 ms and a 70 ms member must not slip through an average of 45 ms.
  for (const PartyMember& memberA : a.party().members()) {
    if (memberA.pingMs > config_.maxPingMs) return false;
    for (const PartyMember& memberB : b.party().members()) {
      if (memberB.pingMs > config_.maxPingMs) return false;
      if (std::abs(memberA.pingMs - memberB.pingMs) > config_.maxPingDeltaMs) return false;
    }
  }
  return true;
}

}  // namespace mmf
