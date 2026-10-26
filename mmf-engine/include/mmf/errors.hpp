// MMF Engine — exception hierarchy.
//
// The engine distinguishes two failure classes:
//   * configuration / contract violations at construction time (throw),
//   * runtime rejection of a *request* (reported through return values and
//     metrics so a misbehaving network client can never kill a worker thread).
#pragma once

#include <stdexcept>
#include <string>

namespace mmf {

class MatchmakerError : public std::runtime_error {
 public:
  explicit MatchmakerError(const std::string& what) : std::runtime_error(what) {}
};

/// Thrown by EngineConfig::validate() and strategy construction.
class ConfigurationError : public MatchmakerError {
 public:
  explicit ConfigurationError(const std::string& what) : MatchmakerError("configuration: " + what) {}
};

/// Thrown when a caller violates a domain invariant (e.g. empty party).
class DomainError : public MatchmakerError {
 public:
  explicit DomainError(const std::string& what) : MatchmakerError("domain: " + what) {}
};

/// Thrown when the injected dependencies are inconsistent.
class DependencyError : public MatchmakerError {
 public:
  explicit DependencyError(const std::string& what) : MatchmakerError("dependency: " + what) {}
};

}  // namespace mmf
