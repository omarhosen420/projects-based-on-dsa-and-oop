// MMF Engine — Observer pattern contracts.
//
// The matchmaker never talks to the network, the game-server pool or the
// metrics pipeline directly. It publishes immutable events to a Subject; the
// concrete delivery mechanism (synchronous for tests, queued + threaded for
// production) is somebody else's problem. That keeps the matching hot path
// free of I/O latency and makes the whole domain layer unit-testable.
#pragma once

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "mmf/events.hpp"
#include "mmf/logging.hpp"

namespace mmf {

class IObserver {
 public:
  virtual ~IObserver() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;

  /// Called when a lobby reaches READY and all slots are reserved.
  virtual void onMatchReady(const MatchFoundEvent& /*event*/) {}

  /// Called when a formed lobby is torn down before the match starts.
  virtual void onLobbyAborted(const LobbyAbortedEvent& /*event*/) {}

  /// Called once when the notification pipeline is shutting down.
  virtual void onShutdown() {}
};

/// Thread-safe observer registry with an immutable snapshot per broadcast.
///
/// Observers are copied out under the lock and invoked after it is released, so
/// an observer is free to attach/detach or call back into the engine without
/// deadlocking. A throwing observer is contained and logged: one broken
/// listener can never take down match notifications for everyone else.
class Subject {
 public:
  virtual ~Subject() = default;

  Subject() = default;
  Subject(const Subject&) = delete;
  Subject& operator=(const Subject&) = delete;

  void attach(std::shared_ptr<IObserver> observer) {
    if (!observer) return;
    std::lock_guard<std::mutex> guard(mutex_);
    observers_.push_back(std::move(observer));
  }

  bool detach(std::string_view name) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
      if ((*it)->name() == name) {
        observers_.erase(it);
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::vector<std::shared_ptr<IObserver>> observers() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return observers_;
  }

  [[nodiscard]] std::size_t observerCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return observers_.size();
  }

 protected:
  void broadcastMatchReady(const MatchFoundEvent& event) const {
    for (const auto& observer : observers()) {
      try {
        observer->onMatchReady(event);
      } catch (const std::exception& ex) {
        logMessage(LogLevel::Error, "observer",
                   std::string(observer->name()) + " threw in onMatchReady: " + ex.what());
      } catch (...) {
        logMessage(LogLevel::Error, "observer",
                   std::string(observer->name()) + " threw unknown exception");
      }
    }
  }

  void broadcastLobbyAborted(const LobbyAbortedEvent& event) const {
    for (const auto& observer : observers()) {
      try {
        observer->onLobbyAborted(event);
      } catch (const std::exception& ex) {
        logMessage(LogLevel::Error, "observer",
                   std::string(observer->name()) + " threw in onLobbyAborted: " + ex.what());
      } catch (...) {
        logMessage(LogLevel::Error, "observer",
                   std::string(observer->name()) + " threw unknown exception");
      }
    }
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<IObserver>> observers_;
};

}  // namespace mmf
