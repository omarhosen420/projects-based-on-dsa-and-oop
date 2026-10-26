// Shared fixtures for lobby/engine/concurrency tests.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mmf/matchmaker.hpp"

namespace mmftest {

using namespace std::chrono_literals;

/// Captures published events so a test can assert on what an observer *saw*.
class RecordingObserver final : public mmf::IObserver {
 public:
  [[nodiscard]] std::string_view name() const override { return "recording"; }

  void onMatchReady(const mmf::MatchFoundEvent& event) override {
    std::lock_guard<std::mutex> guard(mutex_);
    matches_.push_back(event);
  }

  void onLobbyAborted(const mmf::LobbyAbortedEvent& event) override {
    std::lock_guard<std::mutex> guard(mutex_);
    aborts_.push_back(event);
  }

  [[nodiscard]] std::vector<mmf::MatchFoundEvent> matches() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return matches_;
  }

  [[nodiscard]] std::vector<mmf::LobbyAbortedEvent> aborts() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return aborts_;
  }

  [[nodiscard]] std::size_t matchCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return matches_.size();
  }

  [[nodiscard]] std::size_t abortCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return aborts_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<mmf::MatchFoundEvent> matches_;
  std::vector<mmf::LobbyAbortedEvent> aborts_;
};

inline mmf::PlayerId nextPlayerId() {
  static std::atomic<mmf::PlayerId> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

inline mmf::TicketId nextTicketId() {
  static std::atomic<mmf::TicketId> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

/// A join request plus the ids it contains, so a test can cancel/match them.
struct JoinSpec {
  mmf::JoinRequest request;
  std::vector<mmf::PlayerId> ids;
};

inline JoinSpec makeSolo(double mmr, int pingMs = 30, mmf::RegionId region = 0) {
  JoinSpec spec;
  spec.request.mode = static_cast<mmf::GameModeId>(mmf::GameMode::Ranked);
  spec.request.region = region;
  mmf::PlayerProfile profile;
  profile.id = nextPlayerId();
  profile.name = "solo-" + std::to_string(profile.id);
  profile.mmr = mmr;
  profile.pingMs = pingMs;
  profile.region = region;
  spec.ids.push_back(profile.id);
  spec.request.members.push_back(std::move(profile));
  return spec;
}

inline JoinSpec makeParty(const std::vector<double>& mmrs, int pingMs = 30,
                          mmf::RegionId region = 0) {
  JoinSpec spec;
  spec.request.mode = static_cast<mmf::GameModeId>(mmf::GameMode::Ranked);
  spec.request.region = region;
  double offset = 0.0;
  for (const double mmr : mmrs) {
    mmf::PlayerProfile profile;
    profile.id = nextPlayerId();
    profile.name = "party-" + std::to_string(profile.id);
    profile.mmr = mmr + offset;
    profile.pingMs = pingMs;
    profile.region = region;
    spec.ids.push_back(profile.id);
    spec.request.members.push_back(std::move(profile));
    offset += 1.0;
  }
  return spec;
}

inline mmf::Ticket makeTicket(const mmf::Party& party, double baseTolerance = 200.0,
                              mmf::TicketKind /*kind*/ = mmf::TicketKind::Solo) {
  return mmf::Ticket(nextTicketId(), party, static_cast<mmf::GameModeId>(mmf::GameMode::Ranked),
                     mmf::toRating(baseTolerance), mmf::Clock::now());
}

inline mmf::PlayerProfile profileOf(double mmr, int pingMs = 30, mmf::RegionId region = 0) {
  mmf::PlayerProfile profile;
  profile.id = nextPlayerId();
  profile.name = "ticket-" + std::to_string(profile.id);
  profile.mmr = mmr;
  profile.pingMs = pingMs;
  profile.region = region;
  return profile;
}

inline bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

/// Owns a started engine plus a recording observer, and shuts it down cleanly.
class EngineHarness {
 public:
  explicit EngineHarness(mmf::EngineConfig config, mmf::GameMode mode = mmf::GameMode::Ranked)
      : engine(mmf::createMatchmaker(config, mode)) {
    recorder = std::make_shared<RecordingObserver>();
    engine->attachObserver(recorder);
    engine->start();
  }

  ~EngineHarness() {
    if (engine) engine->shutdown();
  }

  EngineHarness(const EngineHarness&) = delete;
  EngineHarness& operator=(const EngineHarness&) = delete;

  bool submit(const JoinSpec& spec) { return engine->submitJoin(spec.request); }

  [[nodiscard]] mmf::EngineSnapshot snapshot() const { return engine->snapshot(); }

  std::unique_ptr<mmf::MatchmakerService> engine;
  std::shared_ptr<RecordingObserver> recorder;
};

inline mmf::EngineConfig testConfig() {
  mmf::EngineConfig config;
  config.teamSize = 5;
  config.teamsPerMatch = 2;
  config.maxPartySize = 4;
  config.baseTolerance = 200.0;
  config.toleranceGrowthPerSecond = 0.0;
  config.maxTolerance = 200.0;
  config.maxPingDeltaMs = 40.0;
  config.maxPingMs = 90;
  config.allowCrossRegion = false;
  config.weightWait = 1.0;
  config.weightScarcity = 0.30;
  config.agingExponent = 2.0;
  config.agingReferenceSeconds = 2.0;
  config.workerTick = 5ms;
  config.priorityRefreshInterval = 20ms;
  config.maxAnchorAttemptsPerTick = 128;
  config.maxCandidatesPerAnchor = 64;
  config.maxTeamAverageGap = 600.0;
  config.maxLobbyRatingSpread = 1200.0;
  config.maxPartyRatingSpread = 500.0;
  config.maxTrackedLobbies = 4096;
  config.logLevel = mmf::LogLevel::Off;
  return config;
}

}  // namespace mmftest
