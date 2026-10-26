#include "mmf/config.hpp"

#include <cmath>
#include <sstream>

namespace mmf {
namespace {

[[noreturn]] void fail(const std::string& message) { throw ConfigurationError(message); }

void require(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

}  // namespace

void EngineConfig::validate() const {
  require(teamSize >= 1, "teamSize must be >= 1");
  require(teamsPerMatch >= 2, "teamsPerMatch must be >= 2");
  require(maxPartySize >= 1, "maxPartySize must be >= 1");
  require(maxPartySize <= teamSize, "maxPartySize must not exceed teamSize");
  require(baseTolerance > 0.0, "baseTolerance must be > 0");
  require(toleranceGrowthPerSecond >= 0.0, "toleranceGrowthPerSecond must be >= 0");
  require(maxTolerance >= baseTolerance, "maxTolerance must be >= baseTolerance");
  require(maxPingDeltaMs >= 0.0, "maxPingDeltaMs must be >= 0");
  require(maxPingMs >= 0, "maxPingMs must be >= 0");
  require(weightWait >= 0.0, "weightWait must be >= 0");
  require(weightScarcity >= 0.0, "weightScarcity must be >= 0");
  require(weightWait > 0.0 || weightScarcity > 0.0,
          "at least one priority weight must be positive");
  require(agingExponent >= 1.0, "agingExponent must be >= 1 (otherwise aging is not monotone)");
  require(agingReferenceSeconds > 0.0, "agingReferenceSeconds must be > 0");
  require(workerTick.count() > 0, "workerTick must be positive");
  require(priorityRefreshInterval.count() > 0, "priorityRefreshInterval must be positive");
  require(maxAnchorAttemptsPerTick >= 1, "maxAnchorAttemptsPerTick must be >= 1");
  require(maxCandidatesPerAnchor >= 1, "maxCandidatesPerAnchor must be >= 1");
  require(commandQueueCapacity >= 1, "commandQueueCapacity must be >= 1");
  require(eventQueueCapacity >= 1, "eventQueueCapacity must be >= 1");
  require(enqueueTimeout.count() >= 0, "enqueueTimeout must be >= 0");
  require(scarcityPeerCap >= 1, "scarcityPeerCap must be >= 1");
  require(maxTeamAverageGap >= 0.0, "maxTeamAverageGap must be >= 0");
  require(maxLobbyRatingSpread >= 0.0, "maxLobbyRatingSpread must be >= 0");
  require(maxPartyRatingSpread >= 0.0, "maxPartyRatingSpread must be >= 0");
  require(maxTrackedLobbies >= 1, "maxTrackedLobbies must be >= 1");
  require(maxTolerance <= 100000.0, "maxTolerance is implausibly large");
  require(std::isfinite(baseTolerance) && std::isfinite(maxTolerance) &&
              std::isfinite(toleranceGrowthPerSecond) && std::isfinite(maxPingDeltaMs) &&
              std::isfinite(weightWait) && std::isfinite(weightScarcity) &&
              std::isfinite(agingExponent) && std::isfinite(agingReferenceSeconds),
          "numeric configuration values must be finite");
}

}  // namespace mmf
