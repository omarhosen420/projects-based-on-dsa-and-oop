#include "mmf/matchmaker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

#include "mmf/errors.hpp"
#include "mmf/logging.hpp"

namespace mmf {
namespace {

std::uint64_t bump(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) {
  return counter.fetch_add(by, std::memory_order_relaxed) + by;
}

}  // namespace

MatchmakerService::MatchmakerService(EngineConfig config, EngineDependencies dependencies)
    : config_(std::move(config)), dependencies_(std::move(dependencies)) {
  config_.validate();
  if (!dependencies_.strategy) throw DependencyError("a matchmaking strategy is required");
  if (!dependencies_.priority) throw DependencyError("a priority policy is required");
  if (!dependencies_.latency) throw DependencyError("a latency policy is required");
  if (!dependencies_.players) throw DependencyError("a player repository is required");
  if (!dependencies_.notifications) {
    dependencies_.notifications =
        std::make_shared<AsyncNotificationChannel>(config_.eventQueueCapacity);
  }
  Logger::instance().setLevel(config_.logLevel);
}

MatchmakerService::~MatchmakerService() { shutdown(); }

void MatchmakerService::start() {
  if (started_) return;
  dependencies_.notifications->start();
  startedAt_ = Clock::now();
  lastRefresh_ = startedAt_;
  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this] { workerLoop(); });
  started_ = true;
}

void MatchmakerService::shutdown() {
  const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
  commandCv_.notify_all();
  if (worker_.joinable()) worker_.join();

  if (wasRunning) {
    std::lock_guard<std::mutex> guard(stateMutex_);
    for (auto& entry : lobbies_) {
      if (entry.second) entry.second->abort(AbortReason::Shutdown);
    }
    lobbies_.clear();
    lobbyOrder_.clear();
  }
  dependencies_.notifications->stop();
  started_ = false;
}

// ------------------------------------------------------------- producer API ---

bool MatchmakerService::enqueueCommand(Command command) {
  if (!running_.load(std::memory_order_acquire)) return false;

  std::unique_lock<std::mutex> lock(commandMutex_);
  const bool hasRoom = commandCv_.wait_for(lock, config_.enqueueTimeout, [this] {
    return commands_.size() < config_.commandQueueCapacity ||
           !running_.load(std::memory_order_acquire);
  });
  if (!hasRoom || !running_.load(std::memory_order_acquire)) {
    bump(metrics_.joinsRejectedQueueFull);
    return false;
  }
  commands_.push_back(std::move(command));
  lock.unlock();
  commandCv_.notify_one();
  return true;
}

bool MatchmakerService::submitJoin(JoinRequest request) {
  Command command;
  command.kind = Command::Kind::Join;
  command.join = std::move(request);
  return enqueueCommand(std::move(command));
}

bool MatchmakerService::submitCancel(PlayerId playerId) {
  Command command;
  command.kind = Command::Kind::Cancel;
  command.playerId = playerId;
  return enqueueCommand(std::move(command));
}

bool MatchmakerService::submitLobbyConfirmed(MatchId matchId) {
  Command command;
  command.kind = Command::Kind::Confirm;
  command.matchId = matchId;
  return enqueueCommand(std::move(command));
}

bool MatchmakerService::submitLobbyAbort(MatchId matchId, AbortReason reason) {
  Command command;
  command.kind = Command::Kind::Abort;
  command.matchId = matchId;
  command.reason = reason;
  return enqueueCommand(std::move(command));
}

// ------------------------------------------------------------- worker thread ---

void MatchmakerService::workerLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(commandMutex_);
      commandCv_.wait_for(lock, config_.workerTick, [this] {
        return !commands_.empty() || !running_.load(std::memory_order_relaxed);
      });
      if (commands_.empty() && !running_.load(std::memory_order_relaxed)) break;
    }

    std::deque<Command> batch;
    const TimePoint now = Clock::now();
    // Everything below mutates engine state. The lock is the single-writer
    // serialisation point for tree_, heap_, tickets_, lobbies_ and players.
    std::lock_guard<std::mutex> state(stateMutex_);
    {
      // Taking the batch *after* the state lock closes the window in which
      // waitUntilQuiet() could observe "no commands, no tickets" while a batch
      // is still in flight. Lock order is always state -> command.
      std::lock_guard<std::mutex> commands(commandMutex_);
      batch.swap(commands_);
    }
    commandCv_.notify_all();  // producers parked on a full queue may retry
    for (const Command& command : batch) {
      try {
        handleCommand(command, now);
      } catch (const std::exception& ex) {
        bump(metrics_.commandErrors);
        logMessage(LogLevel::Error, "worker", std::string("command failed: ") + ex.what());
      } catch (...) {
        bump(metrics_.commandErrors);
        logMessage(LogLevel::Error, "worker", "command failed with unknown exception");
      }
    }

    if (now - lastRefresh_ >= config_.priorityRefreshInterval) {
      refreshPriorities(now);
      lastRefresh_ = now;
    }
    runMatchmakingPass(now);
  }
}

void MatchmakerService::handleCommand(const Command& command, TimePoint now) {
  switch (command.kind) {
    case Command::Kind::Join:
      handleJoin(command.join, now);
      break;
    case Command::Kind::Cancel:
      handleCancel(command.playerId, now);
      break;
    case Command::Kind::Confirm:
      handleConfirm(command.matchId);
      break;
    case Command::Kind::Abort:
      handleAbort(command.matchId, command.reason, now);
      break;
  }
}

void MatchmakerService::handleJoin(const JoinRequest& request, TimePoint now) {
  if (request.members.empty()) {
    bump(metrics_.joinsRejectedInvalid);
    return;
  }
  if (request.mode != config_.mode) {
    bump(metrics_.joinsRejectedInvalid);
    return;
  }
  if (request.members.size() > config_.maxPartySize) {
    bump(metrics_.joinsRejectedInvalid);
    return;
  }

  std::vector<PartyMember> members;
  members.reserve(request.members.size());
  for (const PlayerProfile& profile : request.members) {
    if (profile.id == 0 || !std::isfinite(profile.mmr) || profile.pingMs < 0) {
      bump(metrics_.joinsRejectedInvalid);
      return;
    }
    if (dependencies_.players->contains(profile.id)) {
      bump(metrics_.joinsRejectedDuplicate);
      return;
    }
    for (const PartyMember& existing : members) {
      if (existing.id == profile.id) {
        bump(metrics_.joinsRejectedDuplicate);
        return;
      }
    }
    members.push_back(PartyMember{profile.id, profile.name, toRating(profile.mmr), profile.pingMs,
                                  profile.region});
  }

  const Party party(std::move(members));
  if (fromRating(party.ratingSpread()) > config_.maxPartyRatingSpread) {
    bump(metrics_.joinsRejectedInvalid);
    return;
  }
  if (!config_.allowCrossRegion && !party.regionHomogeneous()) {
    bump(metrics_.joinsRejectedInvalid);
    return;
  }

  Ticket ticket(nextTicketId_.fetch_add(1, std::memory_order_relaxed), std::move(party),
                request.mode, toRating(dependencies_.strategy->baseTolerance()), now);
  if (!dependencies_.latency->admissible(ticket)) {
    bump(metrics_.joinsRejectedLatency);
    return;
  }

  for (const PlayerProfile& profile : request.members) {
    auto player = std::make_shared<Player>(profile);
    player->markQueued(ticket.id());
    dependencies_.players->add(std::move(player));
  }

  enqueueTicket(std::move(ticket), now);
  bump(metrics_.joinsAccepted);
  bump(metrics_.playersAccepted, request.members.size());
}

void MatchmakerService::handleCancel(PlayerId playerId, TimePoint now) {
  auto player = dependencies_.players->find(playerId);
  if (!player) {
    bump(metrics_.cancellationMisses);
    return;
  }

  if (player->state() == PlayerState::Matched) {
    // Disconnect after the match was announced but before it started: tear the
    // lobby down so the remaining nine players are re-queued immediately.
    auto it = lobbies_.find(player->match());
    if (it != lobbies_.end()) {
      const auto lobby = it->second;  // copy: abortLobby() untracks the map entry
      abortLobby(lobby, AbortReason::PlayerLeft, now, playerId);
    }
    player->markDisconnected();
    dependencies_.players->remove(playerId);
    bump(metrics_.cancellations);
    bump(metrics_.playersWithdrawn);
    return;
  }

  if (player->state() != PlayerState::Queued) {
    bump(metrics_.cancellationMisses);
    return;
  }

  const TicketId ticketId = player->ticket();
  auto ticketIt = tickets_.find(ticketId);
  if (ticketIt == tickets_.end()) {
    player->markDisconnected();
    dependencies_.players->remove(playerId);
    bump(metrics_.cancellationMisses);
    return;
  }

  const Ticket ticket = ticketIt->second;  // copy: purgeTicket() invalidates iterators
  purgeTicket(ticketId);

  for (const PartyMember& member : ticket.party().members()) {
    auto other = dependencies_.players->find(member.id);
    if (!other) continue;
    if (member.id == playerId) {
      other->markDisconnected();
      dependencies_.players->remove(member.id);
    } else {
      // A party queue node is indivisible: if one member drops, the group is
      // dissolved rather than silently turned into partial solos.
      other->markIdle();
    }
  }
  if (ticket.party().size() > 1) bump(metrics_.partiesDissolved);
  bump(metrics_.playersWithdrawn, ticket.party().size());
  bump(metrics_.cancellations);
}

void MatchmakerService::handleConfirm(MatchId matchId) {
  auto it = lobbies_.find(matchId);
  if (it == lobbies_.end()) {
    bump(metrics_.lobbyLookupMisses);
    return;
  }
  const auto lobby = it->second;
  lobby->confirm();
  for (const Ticket& ticket : lobby->tickets()) {
    for (const PartyMember& member : ticket.party().members()) {
      if (auto player = dependencies_.players->find(member.id)) player->markInMatch(matchId);
    }
  }
  untrackLobby(matchId);
  bump(metrics_.lobbiesConfirmed);
}

void MatchmakerService::handleAbort(MatchId matchId, AbortReason reason, TimePoint now) {
  auto it = lobbies_.find(matchId);
  if (it == lobbies_.end()) {
    bump(metrics_.lobbyLookupMisses);
    return;
  }
  const auto lobby = it->second;  // copy: abortLobby() untracks the map entry
  abortLobby(lobby, reason, now, std::nullopt);
}

// ------------------------------------------------------------- queue plumbing ---

void MatchmakerService::enqueueTicket(Ticket ticket, TimePoint now) {
  ticket.setScarcity(estimateScarcity(ticket));
  const TicketId id = ticket.id();
  tree_.upsert(IntervalTree<TicketId>::Interval{ticket.ratingLo(), ticket.ratingHi(), id});
  tickets_.insert_or_assign(id, std::move(ticket));
  heap_.pushOrUpdate(id, dependencies_.priority->sortKey(tickets_.at(id), now));
}

void MatchmakerService::purgeTicket(TicketId ticketId) {
  tree_.erase(ticketId);   // O(log N)
  heap_.erase(ticketId);   // O(log N)
  tickets_.erase(ticketId);
}

double MatchmakerService::estimateScarcity(const Ticket& ticket) const {
  const std::size_t cap = std::max<std::size_t>(1, config_.scarcityPeerCap);
  const auto result = tree_.queryCapped(ticket.ratingLo(), ticket.ratingHi(), cap,
                                        [](const IntervalTree<TicketId>::Interval&) { return true; });
  bump(metrics_.intervalQueries);
  bump(metrics_.intervalNodesVisited, result.nodesVisited);
  const double crowded = static_cast<double>(result.matches) / static_cast<double>(cap);
  return 100.0 * (1.0 - std::min(1.0, crowded));
}

void MatchmakerService::refreshPriorities(TimePoint now) {
  // O(N) re-key + Floyd heapify. Time-dependent priorities must be refreshed or
  // the heap ordering would drift; the interval (100 ms) bounds that drift.
  heap_.rebuild([this, now](TicketId id) {
    auto it = tickets_.find(id);
    if (it == tickets_.end()) return std::numeric_limits<double>::max();
    return dependencies_.priority->sortKey(it->second, now);
  });
  bump(metrics_.priorityRefreshes);
}

// ------------------------------------------------------------------ matching ---

void MatchmakerService::runMatchmakingPass(TimePoint now) {
  if (tickets_.empty() || heap_.empty()) return;

  std::vector<TicketId> deferred;
  deferred.reserve(config_.maxAnchorAttemptsPerTick);
  const std::uint32_t attempts = std::max<std::uint32_t>(1, config_.maxAnchorAttemptsPerTick);

  for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
    WeightedMinHeap<TicketId>::Item item;
    if (!heap_.popMin(item)) break;
    auto it = tickets_.find(item.key);
    if (it == tickets_.end()) continue;  // stale heap entry: already consumed
    const Ticket anchor = it->second;    // copy: commitLobby() may erase the entry
    auto lobby = attemptLobby(anchor, now);
    if (!lobby) {
      deferred.push_back(anchor.id());
      continue;
    }
    commitLobby(lobby, now);
  }

  // Anchors that could not be matched go back with a refreshed key, *after* the
  // pass, so each anchor is attempted at most once per tick (no starvation and
  // no hot loop on the same ticket).
  for (const TicketId id : deferred) {
    auto it = tickets_.find(id);
    if (it == tickets_.end()) continue;  // consumed as somebody else's candidate
    heap_.pushOrUpdate(id, dependencies_.priority->sortKey(it->second, now));
  }
}

std::shared_ptr<Lobby> MatchmakerService::attemptLobby(const Ticket& anchor, TimePoint now) {
  bump(metrics_.lobbyAttempts);

  const std::size_t slotsTotal = config_.slotsPerLobby();
  if (anchor.slots() > slotsTotal) {
    bump(metrics_.lobbyIncomplete);
    return nullptr;
  }

  // Strategy tolerances are expressed in MMR; the index stores centi-MMR. The
  // conversion happens exactly once, here, so the query window can never be off
  // by the kRatingScale factor.
  const double anchorTolerance = dependencies_.strategy->toleranceFor(anchor, now);
  const Rating anchorToleranceUnits = toRating(anchorTolerance);
  const Rating center = anchor.averageRating();
  const Rating qlo = center - anchorToleranceUnits;
  const Rating qhi = center + anchorToleranceUnits;

  // Candidates are stored as lightweight handles. Copying whole Tickets here
  // (each owning a vector<PartyMember> with std::string names) dominated the
  // matching hot path in the first benchmark run; the ticket stays in
  // tickets_ and is looked up only when it is actually placed in the lobby.
  struct Candidate {
    TicketId id;
    Rating rating;
    std::uint32_t slots;
    double score;
  };
  std::vector<Candidate> candidates;
  const std::size_t candidateCap = std::max<std::size_t>(config_.maxCandidatesPerAnchor, slotsTotal);
  candidates.reserve(std::min<std::size_t>(candidateCap, 64));
  std::size_t availableSlots = anchor.slots();

  // ---- O(log N + K) overlap query over the augmented interval tree ----------
  const auto query = tree_.query(qlo, qhi, [&](const IntervalTree<TicketId>::Interval& interval) {
    if (candidates.size() >= candidateCap) return false;

    auto it = tickets_.find(interval.key);
    if (it == tickets_.end()) return true;
    const Ticket& candidate = it->second;
    if (candidate.id() == anchor.id()) return true;
    if (candidate.slots() > slotsTotal - anchor.slots()) return true;  // can never fit

    // Symmetric tolerance: both sides must accept each other's skill. Compared
    // in integer rating units, so there is no floating-point epsilon debate at
    // the boundary of the window.
    const double candidateTolerance = dependencies_.strategy->toleranceFor(candidate, now);
    const Rating allowedUnits = toRating(std::min(anchorTolerance, candidateTolerance));
    const Rating gapUnits = std::llabs(center - candidate.averageRating());
    if (gapUnits > allowedUnits) return true;

    // Hard latency filter. Skips are *expected* during candidate scanning, so
    // they are not counted as lobby rejections (that counter belongs to
    // verifyLobbyLatency, which runs on a fully composed lobby).
    if (!dependencies_.latency->compatible(anchor, candidate)) return true;

    candidates.push_back(Candidate{candidate.id(), candidate.averageRating(), candidate.slots(),
                                   dependencies_.strategy->candidateScore(anchor, candidate, now)});
    availableSlots += candidate.slots();
    return true;
  });
  bump(metrics_.intervalQueries);
  bump(metrics_.intervalNodesVisited, query.nodesVisited);

  if (candidates.empty() || availableSlots < slotsTotal) {
    bump(metrics_.lobbyIncomplete);
    return nullptr;
  }

  auto lobby = std::make_shared<Lobby>(nextMatchId_.fetch_add(1, std::memory_order_relaxed),
                                       config_.mode, anchor.region(), config_.teamSize,
                                       config_.teamsPerMatch, config_.maxTeamAverageGap,
                                       config_.maxLobbyRatingSpread);
  lobby->attach(dependencies_.notifications);

  if (!lobby->tryAdd(anchor, now)) {
    bump(metrics_.lobbyIncomplete);
    return nullptr;
  }

  // Step 1: the strategy score decides *who* is worth trying (skill closeness,
  // latency and wait). Keep a shortlist of at most two lobbies' worth.
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
  });
  const std::size_t shortlistSize = std::min<std::size_t>(candidates.size(), slotsTotal * 2);

  // Step 2: placement is strength-ordered, so the weakest-team heuristic turns
  // into a snake draft and the two teams end up within maxTeamAverageGap.
  std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(shortlistSize),
            [](const Candidate& a, const Candidate& b) {
              if (a.rating != b.rating) return a.rating > b.rating;
              return a.id < b.id;
            });

  for (std::size_t i = 0; i < shortlistSize; ++i) {
    if (lobby->slotsRemaining() == 0) break;
    const Candidate& candidate = candidates[i];
    if (candidate.slots > lobby->slotsRemaining()) continue;
    auto ticketIt = tickets_.find(candidate.id);
    if (ticketIt == tickets_.end()) continue;  // defensive: cannot happen while locked
    lobby->tryAdd(ticketIt->second, now);
  }

  if (config_.logLevel <= LogLevel::Trace) {
    std::ostringstream trace;
    trace << "anchor " << anchor.id() << " avg " << anchor.averageMmr() << " tol "
          << anchorTolerance << " candidates " << candidates.size() << " query "
          << query.matches << "/" << query.nodesVisited << " filled "
          << (lobby->slotCount() - lobby->slotsRemaining()) << "/" << lobby->slotCount()
          << " gap " << lobby->teamAverageGap() << " spread " << lobby->ratingSpread();
    logMessage(LogLevel::Trace, "match", trace.str());
  }
  if (!lobby->ready()) {
    bump(metrics_.lobbyRejectedBalance);
    return nullptr;
  }
  if (!verifyLobbyLatency(*lobby)) {
    bump(metrics_.lobbyRejectedLatency);
    bump(metrics_.lobbyIncomplete);
    return nullptr;
  }
  return lobby;
}

bool MatchmakerService::verifyLobbyLatency(const Lobby& lobby) const {
  const auto& tickets = lobby.tickets();
  for (std::size_t i = 0; i < tickets.size(); ++i) {
    if (!dependencies_.latency->admissible(tickets[i])) return false;
    for (std::size_t j = i + 1; j < tickets.size(); ++j) {
      if (!dependencies_.latency->compatible(tickets[i], tickets[j])) return false;
    }
  }
  for (const TeamSnapshot& team : lobby.teams()) {
    for (const MatchedPlayer& player : team.players) {
      if (player.pingMs > config_.maxPingMs) return false;
    }
  }
  return true;
}

void MatchmakerService::commitLobby(const std::shared_ptr<Lobby>& lobby, TimePoint now) {
  for (const Ticket& ticket : lobby->tickets()) {
    purgeTicket(ticket.id());
    metrics_.recordWait(ticket.waitSeconds(now));
    for (const PartyMember& member : ticket.party().members()) {
      auto player = dependencies_.players->find(member.id);
      if (player) player->markMatched(lobby->id());
    }
  }

  bump(metrics_.matchesFormed);
  bump(metrics_.playersMatched, lobby->playerCount());
  lobbies_.emplace(lobby->id(), lobby);
  lobbyOrder_.push_back(lobby->id());
  evictLobbiesIfNeeded();

  // Lobby is the Subject: this publishes MatchFoundEvent to its observers, which
  // in production is the async channel (enqueue only, never blocking).
  lobby->markReady();
  bump(metrics_.eventsPublished);
}

void MatchmakerService::abortLobby(std::shared_ptr<Lobby> lobby, AbortReason reason, TimePoint now,
                                   std::optional<PlayerId> droppedPlayer) {
  if (!lobby) return;
  const std::vector<Ticket> tickets = lobby->tickets();
  untrackLobby(lobby->id());
  lobby->abort(reason);
  bump(metrics_.lobbiesAborted);

  for (const Ticket& ticket : tickets) {
    // These players are no longer in a live lobby: they either go back to the
    // queue (counted by the queued snapshot) or are withdrawn. Keeping
    // playersMatched decremented keeps the conservation identity exact:
    //   accepted == matched + queued + withdrawn
    metrics_.playersMatched.fetch_sub(ticket.slots(), std::memory_order_relaxed);
    bool cutByDrop = false;
    for (const PartyMember& member : ticket.party().members()) {
      if (droppedPlayer && member.id == *droppedPlayer) cutByDrop = true;
    }
    if (cutByDrop) {
      // The ticket containing the disconnector is gone for good; friends keep
      // their session and can re-queue.
      for (const PartyMember& member : ticket.party().members()) {
        if (droppedPlayer && member.id == *droppedPlayer) continue;
        if (auto player = dependencies_.players->find(member.id)) player->markIdle();
      }
      if (ticket.party().size() > 1) {
        bump(metrics_.partiesDissolved);
        bump(metrics_.playersWithdrawn, ticket.party().size() - 1);
      }
      continue;
    }

    // Re-queue with the *original* enqueue timestamp: a player must not lose
    // their accumulated wait because somebody else's client crashed.
    enqueueTicket(ticket, now);
    for (const PartyMember& member : ticket.party().members()) {
      if (auto player = dependencies_.players->find(member.id)) player->markQueued(ticket.id());
    }
    bump(metrics_.ticketsRequeued);
  }
}

void MatchmakerService::untrackLobby(MatchId matchId) {
  lobbies_.erase(matchId);
  const auto it = std::find(lobbyOrder_.begin(), lobbyOrder_.end(), matchId);
  if (it != lobbyOrder_.end()) lobbyOrder_.erase(it);
}

void MatchmakerService::evictLobbiesIfNeeded() {
  while (lobbies_.size() > config_.maxTrackedLobbies && !lobbyOrder_.empty()) {
    untrackLobby(lobbyOrder_.front());
  }
}

// ------------------------------------------------------------------ read side ---

EngineSnapshot MatchmakerService::snapshot() const {
  EngineSnapshot out;
  {
    std::lock_guard<std::mutex> guard(stateMutex_);
    out.queuedTickets = tickets_.size();
    out.intervalTreeSize = tree_.size();
    out.heapSize = heap_.size();
    out.trackedLobbies = lobbies_.size();
    out.trackedPlayers = dependencies_.players->size();
    const TimePoint now = Clock::now();
    for (const auto& entry : tickets_) {
      out.queuedPlayers += entry.second.slots();
      out.longestWaitSeconds = std::max(out.longestWaitSeconds, entry.second.waitSeconds(now));
    }
    out.metrics = metrics_.snapshot();
  }
  {
    std::lock_guard<std::mutex> guard(commandMutex_);
    out.pendingCommands = commands_.size();
  }
  return out;
}

std::vector<PlayerId> MatchmakerService::queuedPlayerIds() const {
  std::lock_guard<std::mutex> guard(stateMutex_);
  std::vector<PlayerId> ids;
  ids.reserve(tickets_.size());
  for (const auto& entry : tickets_) {
    for (const PartyMember& member : entry.second.party().members()) ids.push_back(member.id);
  }
  return ids;
}

std::optional<Player> MatchmakerService::findPlayer(PlayerId playerId) const {
  std::lock_guard<std::mutex> guard(stateMutex_);
  const auto player = dependencies_.players->find(playerId);
  if (!player) return std::nullopt;
  return *player;
}

void MatchmakerService::attachObserver(std::shared_ptr<IObserver> observer) {
  dependencies_.notifications->attach(std::move(observer));
}

bool MatchmakerService::flushNotifications(std::chrono::milliseconds timeout) {
  return dependencies_.notifications->flush(timeout);
}

bool MatchmakerService::waitUntilQuiet(std::chrono::milliseconds timeout) const {
  const TimePoint deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> state(stateMutex_);
      std::lock_guard<std::mutex> commands(commandMutex_);
      if (tickets_.empty() && commands_.empty()) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// --------------------------------------------------------------------- factory ---

std::unique_ptr<MatchmakerService> createMatchmaker(EngineConfig config, GameMode mode) {
  config.mode = static_cast<GameModeId>(mode);
  config.validate();

  EngineDependencies dependencies;
  if (mode == GameMode::Casual) {
    dependencies.strategy = std::make_unique<CasualStrategy>(config);
  } else {
    dependencies.strategy = std::make_unique<RankedStrategy>(config);
  }
  dependencies.priority = std::make_unique<AgingPriorityPolicy>(config);
  dependencies.latency = std::make_unique<HardLatencyPolicy>(config);
  dependencies.players = std::make_shared<InMemoryPlayerRepository>();
  dependencies.notifications =
      std::make_shared<AsyncNotificationChannel>(config.eventQueueCapacity);
  return std::make_unique<MatchmakerService>(config, std::move(dependencies));
}

}  // namespace mmf
