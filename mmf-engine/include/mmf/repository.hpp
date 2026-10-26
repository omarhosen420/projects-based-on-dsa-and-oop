// MMF Engine — storage abstraction (Dependency Inversion).
//
// The engine only talks to IPlayerRepository. Swapping the in-memory store for
// a sharded Redis/Postgres store is a matter of supplying another
// implementation; no matching code changes. Implementations are *not* required
// to be internally synchronized: MatchmakerService serialises access with its
// state mutex and is the only writer.
#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmf/player.hpp"

namespace mmf {

class IPlayerRepository {
 public:
  virtual ~IPlayerRepository() = default;

  virtual void add(std::shared_ptr<Player> player) = 0;
  [[nodiscard]] virtual std::shared_ptr<Player> find(PlayerId id) const = 0;
  [[nodiscard]] virtual bool contains(PlayerId id) const = 0;
  virtual bool remove(PlayerId id) = 0;
  [[nodiscard]] virtual std::size_t size() const = 0;
  virtual void clear() = 0;
};

class InMemoryPlayerRepository final : public IPlayerRepository {
 public:
  void add(std::shared_ptr<Player> player) override {
    if (!player) return;
    const PlayerId id = player->id();
    players_[id] = std::move(player);
  }

  [[nodiscard]] std::shared_ptr<Player> find(PlayerId id) const override {
    auto it = players_.find(id);
    return it == players_.end() ? nullptr : it->second;
  }

  [[nodiscard]] bool contains(PlayerId id) const override {
    return players_.find(id) != players_.end();
  }

  bool remove(PlayerId id) override { return players_.erase(id) > 0; }

  [[nodiscard]] std::size_t size() const override { return players_.size(); }

  void clear() override { players_.clear(); }

 private:
  std::unordered_map<PlayerId, std::shared_ptr<Player>> players_;
};

}  // namespace mmf
