// MMF Engine — common value types, ids and time aliases.
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace mmf {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

using PlayerId = std::uint64_t;
using TicketId = std::uint64_t;
using MatchId = std::uint64_t;
using RegionId = std::uint32_t;
using GameModeId = std::uint32_t;

/// Rating is stored in centi-MMR (1 MMR == 100 units) so that the interval
/// tree can work on exact integers instead of floating point comparisons.
using Rating = std::int64_t;
inline constexpr Rating kRatingScale = 100;

[[nodiscard]] inline Rating toRating(double mmr) noexcept {
  return static_cast<Rating>(std::llround(mmr * static_cast<double>(kRatingScale)));
}

[[nodiscard]] inline double fromRating(Rating rating) noexcept {
  return static_cast<double>(rating) / static_cast<double>(kRatingScale);
}

enum class PlayerState : std::uint8_t { Idle, Queued, Matched, InMatch, Disconnected };

enum class TicketKind : std::uint8_t { Solo, Party };

enum class LobbyState : std::uint8_t { Forming, Ready, Confirmed, Aborted };

enum class GameMode : std::uint32_t { Casual = 1, Ranked = 2, Custom = 3 };

enum class AbortReason : std::uint8_t {
  PlayerLeft,        // a member disconnected or refused the match
  ServerUnavailable, // no game server instance could be allocated
  Policy,            // strategy/composer rejected the lobby
  Shutdown           // engine is going away
};

[[nodiscard]] constexpr std::string_view toString(PlayerState s) noexcept {
  switch (s) {
    case PlayerState::Idle: return "idle";
    case PlayerState::Queued: return "queued";
    case PlayerState::Matched: return "matched";
    case PlayerState::InMatch: return "in-match";
    case PlayerState::Disconnected: return "disconnected";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view toString(TicketKind k) noexcept {
  return k == TicketKind::Solo ? "solo" : "party";
}

[[nodiscard]] constexpr std::string_view toString(LobbyState s) noexcept {
  switch (s) {
    case LobbyState::Forming: return "forming";
    case LobbyState::Ready: return "ready";
    case LobbyState::Confirmed: return "confirmed";
    case LobbyState::Aborted: return "aborted";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view toString(AbortReason r) noexcept {
  switch (r) {
    case AbortReason::PlayerLeft: return "player-left";
    case AbortReason::ServerUnavailable: return "server-unavailable";
    case AbortReason::Policy: return "policy";
    case AbortReason::Shutdown: return "shutdown";
  }
  return "unknown";
}

/// Wall-clock seconds elapsed since `then`. Used by the aging curve.
[[nodiscard]] inline double secondsSince(TimePoint then, TimePoint now = Clock::now()) noexcept {
  return std::chrono::duration<double>(now - then).count();
}

}  // namespace mmf
