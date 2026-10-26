# MMF — Multiplayer Matchmaking Engine (C++20)

A production-shaped, thread-safe matchmaking engine for competitive multiplayer
games: thousands of players and pre-made parties join, leave and get matched
concurrently, while a single dedicated worker forms balanced 5v5 lobbies from a
skill index and an aging-aware priority queue.

It is written as a reference implementation of the *data structures*, the
*concurrency* and the *object-oriented design* that a real matchmaker needs —
and it is verified, not just described: **29 test suites, AddressSanitizer +
UBSan clean, ThreadSanitizer clean**, with reproducible microbenchmarks.

```
producers (network)          worker thread                       notifier thread
────────────────────         ─────────────────────────────       ────────────────────────────
submitJoin / submitCancel    drain bounded command queue         SocketNotifier
   │                         refresh aging priorities            GameServerAllocator
   ▼                         pop most-urgent ticket               TelemetryLogger
bounded MPSC queue  ───────►  query interval tree (±Δ, O(logN+K))
   (mutex + condvar)         fill lobby / balance teams
                             commit atomically ──► Lobby (Subject) ──► async channel ──► observers
```

---

## 1. What it does

| Requirement | Where it lives |
|---|---|
| Interval tree over `[MMR−Δ, MMR+Δ]`, `O(log N + K)` overlap queries | [`interval_tree.hpp`](include/mmf/interval_tree.hpp) — augmented AVL, keyed by window start, augmented with `maxHi` |
| Aging-aware min-heap priority queue, starvation-free | [`weighted_min_heap.hpp`](include/mmf/weighted_min_heap.hpp) + `AgingPriorityPolicy` in [`strategy.cpp`](src/strategy.cpp) |
| Party support as composite queue nodes with average MMR | [`party.hpp`](include/mmf/party.hpp), [`ticket.hpp`](include/mmf/ticket.hpp), `Lobby::tryAdd` |
| Observer pattern, match creation decoupled from I/O | [`observer.hpp`](include/mmf/observer.hpp), [`observers.hpp`](include/mmf/observers.hpp) |
| Producer/consumer with network threads + background worker | [`matchmaker.cpp`](src/matchmaker.cpp) (`workerLoop`, `enqueueCommand`) |
| SOLID separation: repositories, strategies, latency policy | [`repository.hpp`](include/mmf/repository.hpp), [`strategy.hpp`](include/mmf/strategy.hpp) |
| Geographic latency hard cap | `HardLatencyPolicy` + `verifyLobbyLatency` (pairwise re-check before commit) |
| O(log N) purge on disconnect, no dangling references | `purgeTicket` + id-based tickets (no raw pointers anywhere) |
| Concurrency demonstration driver | [`main.cpp`](src/main.cpp) (`--players/--producers/--mode/--bench`) |

---

## 2. Architecture

**Layers** (dependencies point downwards; nothing in `data structures` knows about
networking or game modes):

```
┌──────────────────────────────────────────────────────────────────────┐
│ driver / network edge        main.cpp: producers, churn, monitor     │
├──────────────────────────────────────────────────────────────────────┤
│ application service          MatchmakerService                       │
│                              worker loop · command queue · metrics   │
├──────────────────────────────────────────────────────────────────────┤
│ domain                       Player, Party, Ticket, Lobby            │
│                              composition + balance invariants        │
├──────────────────────────────────────────────────────────────────────┤
│ policies (Strategy)          Ranked / Casual · AgingPriority ·       │
│                              HardLatency                             │
├──────────────────────────────────────────────────────────────────────┤
│ infrastructure               IntervalTree · WeightedMinHeap ·        │
│                              IPlayerRepository · AsyncNotification    │
└──────────────────────────────────────────────────────────────────────┘
```

### Matching pass (one worker iteration)

1. **Drain** the bounded command queue (`join`, `cancel`, `confirm`, `abort`).
2. **Re-age** every queued ticket — `O(N)` re-key + Floyd heapify every
   `priorityRefreshInterval` (default 100 ms), so time-dependent priorities never
   drift more than that interval.
3. **Pop the most urgent ticket** (minimum heap key = maximum priority).
4. **Query the interval tree** for windows overlapping
   `[anchorMMR − Δ(t), anchorMMR + Δ(t)]` — `O(log N + K)`.
5. **Filter** each candidate: symmetric tolerance (`gap ≤ min(Δa, Δc)`), hard
   latency/region rules, and party-slot fit.
6. **Compose** the lobby: the strategy score ranks candidates, a shortlist is
   placed strongest-first so the weakest-team heuristic produces a snake draft,
   and the lobby validates slots, team-average gap and member-rating spread.
7. **Commit atomically** (single lock): erase the chosen tickets from tree +
   heap + ticket table, flip player states, publish `MatchFoundEvent`, and put
   failed anchors back with a refreshed key *after* the pass (never twice per
   tick).

---

## 3. Data structures

### 3.1 Augmented interval tree — `IntervalTree<Key>`

An **AVL tree** ordered by `(lo, key)` and augmented with `maxHi`, the largest
`hi` in each subtree.

* A subtree whose `maxHi < qlo` cannot contain an overlapping interval → pruned.
* A node whose `lo > qhi` cannot have an overlapping right subtree → right side
  pruned.

```
query(root, qlo, qhi):
  if node == null or node.maxHi < qlo: return          # no overlap possible
  query(left)                                          # may still overlap
  if node.lo <= qhi and node.hi >= qlo: report(node)
  if node.lo <= qhi: query(right)                      # lo order guarantees prune
```

AVL instead of a treap: **worst-case** `O(log N)` insert/erase, not expected
`O(log N)`, and deletions during disconnect bursts are exactly where that
matters. The visitor is a template, so the hot path pays no `std::function`
type erasure (measured 2.7× faster per query: 6.3 µs → 2.3 µs for 90 reported
intervals).

| Operation | Complexity | Notes |
|---|---|---|
| `upsert(lo, hi, key)` | `O(log N)` | erase + insert; keeps `key → node` index |
| `erase(key)` | `O(log N)` | used by cancellation/disconnect |
| `query(qlo, qhi)` | `O(log N + K)` | `K` = reported intervals |
| `queryCapped(..., cap)` | `O(log N + min(K, cap))` | scarcity estimator |
| `forEachOrdered` | `O(N)` | audit/debug traversal |

The tests assert the cost claim directly: over 20k disjoint windows, a point
query visits at most a small multiple of `log2(N)` nodes, and a 4k-operation
randomised workload is cross-checked against a brute-force model.

### 3.2 Indexed weighted min-heap — `WeightedMinHeap<Key>`

`std::priority_queue` cannot delete or re-key an arbitrary ticket, which is
exactly what a disconnect or an aging refresh needs. This heap keeps a
`key → array index` map, so every mutation is `O(log N)`:

| Operation | Complexity |
|---|---|
| `pushOrUpdate` | `O(log N)` |
| `updateKey` (both directions) | `O(log N)` |
| `erase(key)` | `O(log N)` |
| `popMin` / `peekMin` | `O(log N)` / `O(1)` |
| `rebuild(keyOf)` | `O(N)` (recompute + Floyd heapify) |

It is a **min-heap on `sortKey = −priority`**, so "pop the minimum" pops the
most urgent ticket while the documented priority score still *rises* with wait
time. A randomised test compares 20k mixed operations against a brute-force
model, including invariant checks after every step.

---

## 4. Fairness and anti-starvation

Priority is dynamic and composite:

```
priority(t) = w_wait · 1000 · (wait / T_ref)^γ  +  w_scarcity · 1000 · (scarcity / 100)
```

* `w_wait = 1.0`, `w_scarcity = 0.30`, `γ = 2.0`, `T_ref = 30 s` by default.
* `scarcity ∈ [0, 100]` is estimated **once at enqueue** from the number of
  overlapping windows in the bracket (a cheap `queryCapped`), so it is a static
  "how lonely is this skill bracket" bonus.
* `γ > 1` is essential. With a *linear* wait term the ordering of a fixed queue
  never changes (the common `w_wait·t` term cancels in every comparison), so an
  isolated bracket would never overtake a freshly queued mainstream player. The
  super-linear curve does: after 60 s at `T_ref = 30 s` the wait term is 4000
  points, more than ten times the maximum scarcity bonus. `γ < 1` is rejected by
  `EngineConfig::validate()`.

Two more mechanisms complete the anti-starvation story:

1. **Window widening.** Each ticket's tolerance grows with its own wait time
   (`Δ(t) = Δ₀ + g·t`, capped at `maxTolerance`). A lone 2600-MMR player in a
   1500-MMR population eventually gets a window wide enough to match — the
   engine test `engine_widens_window_until_outlier_bracket_matches` asserts it.
2. **Bounded staleness.** Priorities are functions of time, so the heap is
   rebuilt on a fixed cadence (`priorityRefreshInterval`, 100 ms) — orders of
   magnitude below the multi-second granularity of the aging curve. Exact
   ordering is restored by each refresh, with no per-pop bookkeeping.

Failed anchors are deferred to the end of the pass, so every anchor is attempted
at most once per tick and the loop cannot spin on one ticket.

---

## 5. Threading model

```
producers ──► submitJoin/submitCancel ──► bounded deque (mutex + condvar)
                                                │
worker ◄── drain (state lock held) ◄────────────┘
   │
   └── mutate tree / heap / tickets / lobbies / players     (single writer)
   └── Lobby::markReady() ──► AsyncNotificationChannel (queue) ──► observers
                                                      (own thread)
```

* **One writer.** `tree_`, `heap_`, `tickets_`, `lobbies_` and the player
  repository are mutated only by the worker, always under `stateMutex_`. That is
  why the structures need no internal locking and why a ticket can never be half
  removed (the "dangling reference" class of bugs is eliminated by design).
* **Producers** only touch the bounded command queue. If it is full they wait up
  to `enqueueTimeout` (25 ms) and then get a rejection rather than unbounded
  latency — explicit backpressure with a metric (`joinsRejectedQueueFull`).
* **Lock order** is `stateMutex_ → commandMutex_` and `stateMutex_ →
  waitMutex_` (metrics samples). Producers never take `stateMutex_`, so there is
  no cycle. No lock is held while calling an observer: `Subject` copies the
  observer list, releases its lock, then broadcasts; observer exceptions are
  contained and logged.
* **Notifications are asynchronous.** The worker pays only for an enqueue;
  `GameServerAllocator` (lease table, mutex) or a socket write can never stall
  match formation. A full event queue drops the newest event and counts it
  (`eventsDropped`) instead of blocking the matching hot path.
* **Shutdown** is graceful and idempotent: stop accepting, drain the command
  queue, abort live lobbies (observers hear about it), stop and join the
  notifier, join the worker.

Read side (`snapshot()`, `findPlayer()`, `queuedPlayerIds()`) returns copies
taken under the state lock, never references into mutable state.

---

## 6. Domain model, parties and lobby invariants

`Player` owns queue state; `Party` is an immutable group with pre-computed
aggregates (`totalMmr`, `averageRating`, `min/maxMmr`, `averagePingMs`,
`maxPingMs`, region homogeneity). A **`Ticket`** is the queue node: either a
solo player or a whole party, with the party's *average* MMR as its window
centre. Parties are indivisible: the tree and the heap store `TicketId`s, and a
lobby can only accept a party whole (`Lobby::tryAdd` refuses if no single team
has room for it).

`Lobby` (a `Subject`) enforces composition invariants before a match can exist:

* `slotsFilled == teamSize · teamCount` — never oversubscribed;
* teams balanced by the *weakest-team* heuristic (empty team first, then lowest
  average), which turns a strength-ordered shortlist into a snake draft;
* `|avgMMR(teamA) − avgMMR(teamB)| ≤ maxTeamAverageGap`;
* `max(member MMR) − min(member MMR) ≤ maxLobbyRatingSpread`;
* a final **pairwise** latency re-check over every member pair before commit.

Only `Lobby::markReady()` transitions `FORMING → READY` and publishes the
`MatchFoundEvent`, so a match cannot be announced twice or in a partial state.

### Hard latency cap

`HardLatencyPolicy` is deliberately *not* part of the matchmaking strategy —
geography is a safety constraint, not a preference:

* absolute ceiling (`maxPingMs`) for every member, checked at join time;
* `|ping(a) − ping(b)| ≤ maxPingDeltaMs` **for every member pair**, not just
  the party averages;
* region equality unless `allowCrossRegion` is enabled;
* intra-party spread is validated too, so a pre-made cannot smuggle a laggy
  member into a match.

---

## 7. Cancellation, disconnects and lifecycle

A ticket is purged from all three structures in `O(log N)`:

```cpp
void purgeTicket(TicketId id) {   // O(log N) + O(log N) + O(1) average
  tree_.erase(id);
  heap_.erase(id);
  tickets_.erase(id);
}
```

* **Queued player disconnects** → whole ticket purged; a party ticket is
  dissolved (teammates return to idle) rather than silently converted to solos;
  the disconnected player is removed from the repository (no leak).
* **Matched player disconnects** (after READY, before the game starts) → the
  lobby is aborted, every *other* ticket is re-queued **with its original
  enqueue timestamp** (a crash must not reset somebody's wait), and observers
  are notified.
* **Server/policy abort** → same re-queue path; aborting twice is a no-op.
* **Confirm** → players move to `IN_MATCH`, the lobby leaves the tracked set.

Conservation is enforced by the tests and printed by the demo:

```
playersAccepted == playersMatched + queuedPlayers + playersWithdrawn
```

`playersMatched` is decremented when a ready lobby is torn down, so the identity
holds at every instant, not just at the end.

---

## 8. Build and run

```bash
cd mmf-engine
make            # demo + tests + benchmark (needs only g++ ≥ 12 / C++20)
make run        # concurrent demo: 4000 requests / 8 producer threads / churn
make test       # 29 test suites, no external dependencies
make bench      # microbenchmarks + end-to-end throughput
make asan       # AddressSanitizer + UBSan test build and run
make tsan       # ThreadSanitizer test build and run
```

A `CMakeLists.txt` is included as an alternative build path (`cmake -B build &&
cmake --build build -j`); the Makefile is what every number in this document was
produced with, since CMake was not available in the verification environment.

Demo options: `--players N`, `--producers T`, `--duration MS`, `--party-percent P`,
`--disconnect-percent P`, `--mode casual|ranked`, `--seed N`, `--verbose`,
`--bench`.

---

## 9. Demo output (real run, 2-vCPU cloud VM)

```
=== run summary =====================================================
mode / strategy       : ranked / RankedStrategy + AgingPriorityPolicy + HardLatencyPolicy
lobby shape           : 5v5 (10 slots)
wall clock            : 1.00 s (active 0.03 s)
submitted requests    : 4000 (retries due to full queue: 0)
accepted into queue   : 4000 requests / 5502 players
matches formed        : 534
players matched       : 5340
still queued          : 0 players in 0 tickets (oldest wait 0.00 s)
withdrawn (drops)     : 162   (cancels 76, parties dissolved 37)
queue structures      : interval tree 0 / heap 0 / tickets 0  (must be equal)
interval queries      : 7818, nodes visited 368021 (47.07 per query)
lobby attempts        : 3818 (not fillable 2799, balance rejects 485, latency rejects 281)
wait time             : mean 0.006 s  p50 0.000 s  p95 0.016 s  p99 0.023 s  max 0.027 s
throughput            : 168465 players/s, 16847 matches/s
notifications         : published 534, delivered 534, dropped 0, observer failures 0
game servers leased   : 534 (exhausted 0)
conservation check     : OK (5502 accepted players == 5340 matched + 0 queued + 162 withdrawn)
```

8 producer threads, a churn thread cancelling ~2 % of the queue, 20 % of players
arriving as 2–4 player parties, 5 % artificial MMR outliers — everything is
accounted for, and the tree, the heap and the ticket table agree at the end.

---

## 10. Benchmarks

Measured with `make bench` on a **2-vCPU Ubuntu 24.04 cloud VM, g++ 13.3,
`-O2`**. These are single-run numbers on shared hardware: treat them as orders
of magnitude, and reproduce them on your machine.

**Data structures** (200k intervals / 200k queries)

| Operation | Throughput | Per op |
|---|---|---|
| interval tree insert | 2.09 M ops/s | 479 ns |
| interval tree overlap query (90 matches, 113 nodes) | 0.43 M ops/s | 2.3 µs |
| interval tree erase | 1.33 M ops/s | 754 ns |
| indexed min-heap push | 9.15 M ops/s | 109 ns |
| indexed min-heap `updateKey` | 16.5 M ops/s | 61 ns |
| indexed min-heap `popMin` | 2.27 M ops/s | 441 ns |

**End-to-end** (20,000 players, 8 producers, one worker, 5v5)

| Metric | Value |
|---|---|
| joins accepted into the queue | 20,000 in 0.013 s (1.59 M joins/s submission) |
| drain time after the last submit | 0.79 s |
| matches formed | 1,999 (10.0 players per match) |
| throughput | **≈24,900 players/s / 2,489 matches/s** |
| interval queries | 36,321 (88 nodes per query, deep queue) |
| wait p95 | 0.17 s |
| notifications dropped | 0 |

The demo runs faster (~168k players/s) because its queue is shallow: fewer
overlapping windows per query. Queue depth, not player count alone, drives
query cost — the interval tree keeps that cost at `O(log N + K)`.

---

## 11. Tests and sanitizers

```
$ ./build/mmf_tests
...
PASSED: 29/29 test suites
```

Coverage highlights:

* **Data structures** — randomised differential tests against brute-force
  models (interval tree 4k mutations × 40 probes, heap 20k mixed operations),
  invariant validators (AVL balance, `maxHi` freshness, BST order, heap property,
  index coherence), and an explicit `O(log N)` node-visit assertion.
* **Domain** — lobby slot accounting, party indivisibility, spread/gap guards,
  state machine (double-ready, confirm-before-ready, abort notifications).
* **Policies** — aging monotonicity and super-linearity, "isolated bracket
  overtakes fresh players", ranked vs casual window/scoring, latency matrix
  (delta, absolute ceiling, region, intra-party spread), config validation.
* **Engine** — full match formation, party + solo lobbies, ping cap keeps laggy
  players out of the match, window widening for an outlier bracket, O(log N)
  cancellation purge, duplicate/invalid join rejection, abort → re-queue with
  preserved wait time, confirm → `IN_MATCH`, disconnect of a matched player,
  graceful idempotent shutdown.
* **Concurrency** — 8 producers × 500 joins, 2 snapshot readers asserting
  `tree.size() == heap.size() == tickets.size()` and the conservation identity,
  plus a churn thread cancelling random tickets; final checks that every lobby
  was full, no player appeared in two live lobbies, and nothing was dropped.

Sanitizers (both clean):

```bash
make asan   # 29/29, no leaks, no UB
make tsan   # 29/29, no data races
```

Two real bugs were caught this way and are worth mentioning because they are
classic shapes:

1. **Use-after-free** — `abortLobby` took `const shared_ptr<Lobby>&` and the
   first thing it did was erase that lobby from the tracking map, destroying the
   `shared_ptr` the reference pointed at. ASan reported the read immediately;
   the fix is to take the lobby **by value**, so the callee owns a reference for
   the whole call.
2. **Unit mismatch** — the query window was `center − Δ` where `center` is
   centi-MMR and `Δ` was MMR, i.e. a window ~100× too narrow. It only showed up
   for far-away brackets (the outlier test), because overlapping *stored*
   windows hid it for near players. All window arithmetic is now integer
   centi-MMR.

---

## 12. Configuration

Everything is a field on `EngineConfig` and validated at construction
(`ConfigurationError` instead of silently broken behaviour).

| Field | Default | Meaning |
|---|---|---|
| `teamSize` / `teamsPerMatch` | 5 / 2 | lobby shape (10 slots) |
| `maxPartySize` | 5 | largest accepted pre-made (≤ `teamSize`) |
| `baseTolerance` | 150 | skill window at `t = 0` (MMR) |
| `toleranceGrowthPerSecond` | 30 | widening per second of wait |
| `maxTolerance` | 1500 | absolute window ceiling |
| `maxPingDeltaMs` | 55 | hard `|ping(a) − ping(b)|` cap |
| `maxPingMs` | 95 | absolute ping ceiling per member |
| `allowCrossRegion` | false | region equality required when false |
| `weightWait` / `weightScarcity` | 1.0 / 0.30 | priority weights |
| `agingExponent` | 2.0 | must be ≥ 1 (super-linear aging) |
| `agingReferenceSeconds` | 30 | `T_ref` in the aging curve |
| `workerTick` | 20 ms | max sleep between worker iterations |
| `priorityRefreshInterval` | 100 ms | full re-key cadence |
| `maxAnchorAttemptsPerTick` | 96 | bounded work per tick |
| `maxCandidatesPerAnchor` | 64 | candidate scan cap |
| `commandQueueCapacity` | 65536 | bounded producer queue |
| `enqueueTimeout` | 25 ms | backpressure before rejecting a join |
| `maxTeamAverageGap` | 220 | team balance guard (MMR) |
| `maxLobbyRatingSpread` | 700 | member spread guard (MMR) |
| `maxTrackedLobbies` | 4096 | ready-lobby retention for confirm/abort |

---

## 13. Design decisions worth calling out

* **Single-writer consumer, not per-structure locks.** Fewer locks, no lock
  ordering puzzles inside the matching pass, and a consistent view for every
  read. The cost is that matching and command processing share one lock; at
  these work sizes one worker already forms thousands of lobbies per second, and
  the queue is explicitly bounded so producers feel backpressure instead of
  latency spikes.
* **Ids, not pointers, in the index.** A ticket is a value in `tickets_`; the
  tree and the heap store `TicketId`. A stale entry is detected with a map
  lookup and discarded, so no cancellation order can dangle.
* **Lobby as the Subject.** Match-ready notification originates from the domain
  object that owns the composition invariants, and an async channel observer is
  what makes delivery non-blocking. Testing gets a synchronous recorder instead.
* **Late-bound strategies.** Ranked vs Casual differ in window size, growth rate
  and score weights, not in engine code. A different game (1v1, 3v3, battle
  royale with 60 slots) is a config + strategy change.
* **Aging as a curve, refreshed in bulk.** Cheaper and easier to reason about
  than per-element timers or a timing wheel, with a documented staleness bound.

## 14. Limitations / next steps

* A single worker thread per engine instance. Sharding by region or MMR bucket
  (with a cross-shard pass for widened windows) is the obvious scale-out path;
  the service is already single-writer, so it shards cleanly.
* The repository is in-memory. `IPlayerRepository` exists precisely so a Redis
  or Postgres implementation can be injected; it must preserve the
  single-writer contract or provide its own consistency.
* The event queue drops on overflow (counted). A production deployment would
  spill to a durable queue or apply upstream backpressure.
* Team balancing is a greedy snake draft with a hard gap guard; an optimal
  partition (or role-aware balancing) would plug in as another `Lobby` policy.
* The sample observers simulate sockets/allocation. They demonstrate the
  pattern, not a protocol implementation.

## 15. Project layout

```
mmf-engine/
├── include/mmf/
│   ├── types.hpp             ids, rating units (centi-MMR), enums
│   ├── config.hpp            EngineConfig + validation entry point
│   ├── errors.hpp            exception hierarchy
│   ├── logging.hpp           tiny leveled, thread-safe logger
│   ├── interval_tree.hpp     augmented AVL interval tree (header-only)
│   ├── weighted_min_heap.hpp indexed min-heap (header-only)
│   ├── player.hpp party.hpp ticket.hpp lobby.hpp    domain model
│   ├── events.hpp observer.hpp observers.hpp        observer layer
│   ├── strategy.hpp          mode / priority / latency policies
│   ├── repository.hpp        IPlayerRepository + in-memory implementation
│   ├── metrics.hpp           counters + wait-time distribution
│   └── matchmaker.hpp        MatchmakerService + factory
├── src/                      implementations, worker loop, demo driver
├── tests/                    test harness, fixtures, 29 suites
├── bench/                    microbenchmarks + end-to-end benchmark
├── docs/DESIGN.md            locking discipline, invariants, lifecycle
├── CMakeLists.txt · Makefile · .gitignore
```

---

*Part of [projects-based-on-dsa-and-oop](../README.md): systems built on core
data structures, algorithms and object-oriented design — verified with tests and
sanitizers rather than asserted in prose.*
