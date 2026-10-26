// MMF Engine — lock-light instrumentation.
//
// Counters are atomics: producers (network threads) and the worker both bump
// them without contention. Wait-time samples need a real distribution, so they
// live behind a small mutex and are only ever appended by the worker.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <vector>

namespace mmf {

struct MetricsSnapshot {
  std::uint64_t joinsAccepted{0};
  std::uint64_t playersAccepted{0};
  std::uint64_t joinsRejectedDuplicate{0};
  std::uint64_t joinsRejectedInvalid{0};
  std::uint64_t joinsRejectedQueueFull{0};
  std::uint64_t joinsRejectedLatency{0};
  std::uint64_t cancellations{0};
  std::uint64_t cancellationMisses{0};
  std::uint64_t partiesDissolved{0};
  std::uint64_t matchesFormed{0};
  std::uint64_t playersMatched{0};
  std::uint64_t playersWithdrawn{0};
  std::uint64_t lobbyAttempts{0};
  std::uint64_t lobbyIncomplete{0};
  std::uint64_t lobbyRejectedBalance{0};
  std::uint64_t lobbyRejectedLatency{0};
  std::uint64_t intervalQueries{0};
  std::uint64_t intervalNodesVisited{0};
  std::uint64_t priorityRefreshes{0};
  std::uint64_t commandErrors{0};
  std::uint64_t eventsPublished{0};
  std::uint64_t eventsDelivered{0};
  std::uint64_t eventsDropped{0};
  std::uint64_t observerFailures{0};
  std::uint64_t lobbiesConfirmed{0};
  std::uint64_t lobbiesAborted{0};
  std::uint64_t lobbyLookupMisses{0};
  std::uint64_t ticketsRequeued{0};

  std::uint64_t waitSamples{0};
  double waitMeanSeconds{0.0};
  double waitP50Seconds{0.0};
  double waitP95Seconds{0.0};
  double waitP99Seconds{0.0};
  double waitMaxSeconds{0.0};
};

class Metrics {
 public:
  std::atomic<std::uint64_t> joinsAccepted{0};
  std::atomic<std::uint64_t> playersAccepted{0};
  std::atomic<std::uint64_t> joinsRejectedDuplicate{0};
  std::atomic<std::uint64_t> joinsRejectedInvalid{0};
  std::atomic<std::uint64_t> joinsRejectedQueueFull{0};
  std::atomic<std::uint64_t> joinsRejectedLatency{0};
  std::atomic<std::uint64_t> cancellations{0};
  std::atomic<std::uint64_t> cancellationMisses{0};
  std::atomic<std::uint64_t> partiesDissolved{0};
  std::atomic<std::uint64_t> matchesFormed{0};
  std::atomic<std::uint64_t> playersMatched{0};
  std::atomic<std::uint64_t> playersWithdrawn{0};
  std::atomic<std::uint64_t> lobbyAttempts{0};
  std::atomic<std::uint64_t> lobbyIncomplete{0};
  std::atomic<std::uint64_t> lobbyRejectedBalance{0};
  std::atomic<std::uint64_t> lobbyRejectedLatency{0};
  std::atomic<std::uint64_t> intervalQueries{0};
  std::atomic<std::uint64_t> intervalNodesVisited{0};
  std::atomic<std::uint64_t> priorityRefreshes{0};
  std::atomic<std::uint64_t> commandErrors{0};
  std::atomic<std::uint64_t> eventsPublished{0};
  std::atomic<std::uint64_t> eventsDelivered{0};
  std::atomic<std::uint64_t> eventsDropped{0};
  std::atomic<std::uint64_t> observerFailures{0};
  std::atomic<std::uint64_t> lobbiesConfirmed{0};
  std::atomic<std::uint64_t> lobbiesAborted{0};
  std::atomic<std::uint64_t> lobbyLookupMisses{0};
  std::atomic<std::uint64_t> ticketsRequeued{0};

  void recordWait(double seconds) {
    std::lock_guard<std::mutex> guard(waitMutex_);
    if (waits_.size() < kMaxWaitSamples) waits_.push_back(seconds);
    waitSum_ += seconds;
    waitMax_ = std::max(waitMax_, seconds);
    waitCount_ += 1;
  }

  [[nodiscard]] MetricsSnapshot snapshot() const {
    MetricsSnapshot out;
    out.joinsAccepted = joinsAccepted.load(std::memory_order_relaxed);
    out.playersAccepted = playersAccepted.load(std::memory_order_relaxed);
    out.joinsRejectedDuplicate = joinsRejectedDuplicate.load(std::memory_order_relaxed);
    out.joinsRejectedInvalid = joinsRejectedInvalid.load(std::memory_order_relaxed);
    out.joinsRejectedQueueFull = joinsRejectedQueueFull.load(std::memory_order_relaxed);
    out.joinsRejectedLatency = joinsRejectedLatency.load(std::memory_order_relaxed);
    out.cancellations = cancellations.load(std::memory_order_relaxed);
    out.cancellationMisses = cancellationMisses.load(std::memory_order_relaxed);
    out.partiesDissolved = partiesDissolved.load(std::memory_order_relaxed);
    out.matchesFormed = matchesFormed.load(std::memory_order_relaxed);
    out.playersMatched = playersMatched.load(std::memory_order_relaxed);
    out.playersWithdrawn = playersWithdrawn.load(std::memory_order_relaxed);
    out.lobbyAttempts = lobbyAttempts.load(std::memory_order_relaxed);
    out.lobbyIncomplete = lobbyIncomplete.load(std::memory_order_relaxed);
    out.lobbyRejectedBalance = lobbyRejectedBalance.load(std::memory_order_relaxed);
    out.lobbyRejectedLatency = lobbyRejectedLatency.load(std::memory_order_relaxed);
    out.intervalQueries = intervalQueries.load(std::memory_order_relaxed);
    out.intervalNodesVisited = intervalNodesVisited.load(std::memory_order_relaxed);
    out.priorityRefreshes = priorityRefreshes.load(std::memory_order_relaxed);
    out.commandErrors = commandErrors.load(std::memory_order_relaxed);
    out.eventsPublished = eventsPublished.load(std::memory_order_relaxed);
    out.eventsDelivered = eventsDelivered.load(std::memory_order_relaxed);
    out.eventsDropped = eventsDropped.load(std::memory_order_relaxed);
    out.observerFailures = observerFailures.load(std::memory_order_relaxed);
    out.lobbiesConfirmed = lobbiesConfirmed.load(std::memory_order_relaxed);
    out.lobbiesAborted = lobbiesAborted.load(std::memory_order_relaxed);
    out.lobbyLookupMisses = lobbyLookupMisses.load(std::memory_order_relaxed);
    out.ticketsRequeued = ticketsRequeued.load(std::memory_order_relaxed);

    std::vector<double> samples;
    {
      std::lock_guard<std::mutex> guard(waitMutex_);
      samples = waits_;
      out.waitSamples = waitCount_;
      out.waitMeanSeconds = waitCount_ == 0 ? 0.0 : waitSum_ / static_cast<double>(waitCount_);
      out.waitMaxSeconds = waitMax_;
    }
    if (!samples.empty()) {
      auto percentile = [&samples](double p) {
        const auto index = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index),
                         samples.end());
        return samples[index];
      };
      out.waitP50Seconds = percentile(0.50);
      out.waitP95Seconds = percentile(0.95);
      out.waitP99Seconds = percentile(0.99);
    }
    return out;
  }

  void reset() {
    joinsAccepted = 0;
    playersAccepted = 0;
    joinsRejectedDuplicate = 0;
    joinsRejectedInvalid = 0;
    joinsRejectedQueueFull = 0;
    joinsRejectedLatency = 0;
    cancellations = 0;
    cancellationMisses = 0;
    partiesDissolved = 0;
    matchesFormed = 0;
    playersMatched = 0;
    playersWithdrawn = 0;
    lobbyAttempts = 0;
    lobbyIncomplete = 0;
    lobbyRejectedBalance = 0;
    lobbyRejectedLatency = 0;
    intervalQueries = 0;
    intervalNodesVisited = 0;
    priorityRefreshes = 0;
    commandErrors = 0;
    eventsPublished = 0;
    eventsDelivered = 0;
    eventsDropped = 0;
    observerFailures = 0;
    lobbiesConfirmed = 0;
    lobbiesAborted = 0;
    lobbyLookupMisses = 0;
    ticketsRequeued = 0;
    std::lock_guard<std::mutex> guard(waitMutex_);
    waits_.clear();
    waitSum_ = 0.0;
    waitMax_ = 0.0;
    waitCount_ = 0;
  }

 private:
  static constexpr std::size_t kMaxWaitSamples = 500'000;

  mutable std::mutex waitMutex_;
  std::vector<double> waits_;
  double waitSum_{0.0};
  double waitMax_{0.0};
  std::uint64_t waitCount_{0};
};

}  // namespace mmf
