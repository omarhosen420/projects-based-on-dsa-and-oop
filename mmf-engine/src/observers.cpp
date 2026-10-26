#include "mmf/observers.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <utility>

#include "mmf/logging.hpp"

namespace mmf {

// ------------------------------------------------- AsyncNotificationChannel ----

AsyncNotificationChannel::AsyncNotificationChannel(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

AsyncNotificationChannel::~AsyncNotificationChannel() { stop(); }

void AsyncNotificationChannel::start() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (running_) return;
    running_ = true;
  }
  thread_ = std::thread([this] { run(); });
}

void AsyncNotificationChannel::stop() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_ && !thread_.joinable()) return;
    running_ = false;
  }
  cv_.notify_all();
  idleCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool AsyncNotificationChannel::enqueue(Payload payload) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (queue_.size() >= capacity_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      logMessage(LogLevel::Warn, "notifications",
                 "event queue full (" + std::to_string(capacity_) + "), dropping newest event");
      return false;
    }
    queue_.push_back(std::move(payload));
    published_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_.notify_one();
  return true;
}

void AsyncNotificationChannel::onMatchReady(const MatchFoundEvent& event) {
  enqueue(Payload{event});
}

void AsyncNotificationChannel::onLobbyAborted(const LobbyAbortedEvent& event) {
  enqueue(Payload{event});
}

void AsyncNotificationChannel::deliver(const Payload& payload) {
  const auto observers = this->observers();
  std::visit(
      [&](const auto& event) {
        using T = std::decay_t<decltype(event)>;
        for (const auto& observer : observers) {
          try {
            if constexpr (std::is_same_v<T, MatchFoundEvent>) {
              observer->onMatchReady(event);
            } else {
              observer->onLobbyAborted(event);
            }
          } catch (const std::exception& ex) {
            observerFailures_.fetch_add(1, std::memory_order_relaxed);
            logMessage(LogLevel::Error, "notifications",
                       std::string(observer->name()) + " failed: " + ex.what());
          } catch (...) {
            observerFailures_.fetch_add(1, std::memory_order_relaxed);
            logMessage(LogLevel::Error, "notifications",
                       std::string(observer->name()) + " failed with unknown exception");
          }
        }
      },
      payload);
}

void AsyncNotificationChannel::run() {
  while (true) {
    Payload payload;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
      if (queue_.empty()) {
        if (!running_) break;
        continue;
      }
      payload = std::move(queue_.front());
      queue_.pop_front();
      inFlight_ = true;
    }

    deliver(payload);

    {
      std::lock_guard<std::mutex> guard(mutex_);
      inFlight_ = false;
      delivered_.fetch_add(1, std::memory_order_relaxed);
      if (queue_.empty()) idleCv_.notify_all();
    }
  }

  for (const auto& observer : observers()) {
    try {
      observer->onShutdown();
    } catch (...) {
      observerFailures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool AsyncNotificationChannel::flush(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idleCv_.wait_for(lock, timeout, [this] { return queue_.empty() && !inFlight_; });
}

std::size_t AsyncNotificationChannel::pending() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return queue_.size();
}

// ----------------------------------------------------------- SocketNotifier ----

void SocketNotifier::onMatchReady(const MatchFoundEvent& event) {
  // A real implementation would serialise the assignment per player and hand
  // the buffers to the network reactor; here we only account for the work.
  notificationsSent_.fetch_add(event.playerCount, std::memory_order_relaxed);
  if (!verbose_) return;
  std::ostringstream out;
  out << "match " << event.matchId << " -> pushed " << event.playerCount
      << " assignments (region " << event.region << ", team gap " << std::fixed
      << std::setprecision(1) << event.teamAverageGap << " mmr)";
  logMessage(LogLevel::Info, "socket", out.str());
}

// ------------------------------------------------------ GameServerAllocator ----

GameServerAllocator::GameServerAllocator(std::size_t poolSize,
                                         std::chrono::milliseconds leaseDuration, bool verbose)
    : poolSize_(std::max<std::size_t>(1, poolSize)), leaseDuration_(leaseDuration), verbose_(verbose) {}

void GameServerAllocator::releaseExpired(std::chrono::steady_clock::time_point now) {
  leases_.erase(std::remove_if(leases_.begin(), leases_.end(),
                               [now](const Lease& lease) { return lease.expiresAt <= now; }),
                leases_.end());
}

void GameServerAllocator::onMatchReady(const MatchFoundEvent& event) {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    releaseExpired(now);
    if (leases_.size() >= poolSize_) {
      exhausted_.fetch_add(1, std::memory_order_relaxed);
      logMessage(LogLevel::Warn, "allocator",
                 "no free game server for match " + std::to_string(event.matchId));
      return;
    }
    // Instance id is derived from the lease slot; a real allocator would return
    // an address/endpoint from its pool provider.
    leases_.push_back(Lease{event.matchId, now + leaseDuration_});
    allocations_.fetch_add(1, std::memory_order_relaxed);
    if (verbose_) {
      logMessage(LogLevel::Info, "allocator",
                 "leased instance " + std::to_string(leases_.size() - 1) + " for match " +
                     std::to_string(event.matchId));
    }
  }
}

// ---------------------------------------------------------- TelemetryLogger ----

void TelemetryLogger::onMatchReady(const MatchFoundEvent& event) {
  // Arrival time, not the event's own timestamp: the report needs the span over
  // which this observer actually received matches (`createdAt` is set at the
  // instant the lobby was committed, so it would collapse the span to zero).
  const double nowSeconds = secondsSince(startedAt_);
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (matchWaits_.size() < 200'000) matchWaits_.push_back(event.oldestWaitSeconds);
    waitSum_ += event.oldestWaitSeconds;
    waitMax_ = std::max(waitMax_, event.oldestWaitSeconds);
    gapSum_ += event.teamAverageGap;
    gapMax_ = std::max(gapMax_, event.teamAverageGap);
    pingSpreadMax_ = std::max(pingSpreadMax_, event.pingSpreadMs);
    if (!seen_) {
      firstEventSeconds_ = nowSeconds;
      seen_ = true;
    }
    lastEventSeconds_ = nowSeconds;
  }
  matches_.fetch_add(1, std::memory_order_relaxed);
  playersPlaced_.fetch_add(event.playerCount, std::memory_order_relaxed);
  if (verbose_) {
    logMessage(LogLevel::Info, "telemetry",
               "match " + std::to_string(event.matchId) + " assembled in " +
                   std::to_string(static_cast<int>(event.formationMs)) + "us-ish, oldest wait " +
                   std::to_string(event.oldestWaitSeconds) + "s");
  }
}

void TelemetryLogger::onLobbyAborted(const LobbyAbortedEvent& event) {
  aborts_.fetch_add(1, std::memory_order_relaxed);
  if (verbose_) {
    logMessage(LogLevel::Info, "telemetry",
               "match " + std::to_string(event.matchId) + " aborted (" +
                   std::string(toString(event.reason)) + ")");
  }
}

void TelemetryLogger::report(std::ostream& out) const {
  std::vector<double> waits;
  double waitSum = 0.0;
  double waitMax = 0.0;
  double gapSum = 0.0;
  double gapMax = 0.0;
  double pingSpread = 0.0;
  double first = 0.0;
  double last = 0.0;
  bool seen = false;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    waits = matchWaits_;
    waitSum = waitSum_;
    waitMax = waitMax_;
    gapSum = gapSum_;
    gapMax = gapMax_;
    pingSpread = pingSpreadMax_;
    first = firstEventSeconds_;
    last = lastEventSeconds_;
    seen = seen_;
  }

  const auto observed = matches_.load(std::memory_order_relaxed);
  auto percentile = [&waits](double p) {
    if (waits.empty()) return 0.0;
    const auto index = static_cast<std::size_t>(p * static_cast<double>(waits.size() - 1));
    std::nth_element(waits.begin(), waits.begin() + static_cast<std::ptrdiff_t>(index), waits.end());
    return waits[index];
  };

  const double span = seen ? std::max(1e-9, last - first) : 0.0;
  out << "--- telemetry (observer side) ---\n";
  out << "matches observed     : " << observed << '\n';
  out << "players placed       : " << playersPlaced_.load(std::memory_order_relaxed) << '\n';
  out << "lobby aborts         : " << aborts_.load(std::memory_order_relaxed) << '\n';
  out << std::fixed << std::setprecision(3);
  out << "oldest-wait  mean    : " << (observed == 0 ? 0.0 : waitSum / static_cast<double>(observed))
      << " s   p50 " << percentile(0.50) << " s   p95 " << percentile(0.95) << " s   p99 "
      << percentile(0.99) << " s   max " << waitMax << " s\n";
  out << "team mmr gap mean    : " << (observed == 0 ? 0.0 : gapSum / static_cast<double>(observed))
      << "   max " << gapMax << '\n';
  out << "max ping spread      : " << pingSpread << " ms\n";
  out << "notification rate    : " << (span > 0.0 ? static_cast<double>(observed) / span : 0.0)
      << " matches/s over " << span << " s\n";
}

}  // namespace mmf
