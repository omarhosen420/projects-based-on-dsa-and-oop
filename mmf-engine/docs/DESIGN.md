# Design notes: concurrency, invariants and lifecycle

This document explains *why* the engine is shaped the way it is. The README
describes what it does; this one is about the guarantees and the reasoning behind
them.

---

## 1. Locking discipline

There are three mutexes and one condition variable in `MatchmakerService`.

| Lock | Protects | Taken by |
|---|---|---|
| `stateMutex_` | `tree_`, `heap_`, `tickets_`, `lobbies_`, `lobbyOrder_`, player repository, `lastRefresh_` | worker (writer), `snapshot()`/`findPlayer()`/`queuedPlayerIds()` (readers) |
| `commandMutex_` + `commandCv_` | the bounded command deque `commands_` | producers (push), worker (drain) |
| `waitMutex_` (inside `Metrics`) | wait-time samples | worker (append), `snapshot()` (read/sort) |

Rules:

1. **Single writer.** The four matching structures are mutated only by the worker
   thread, always with `stateMutex_` held. They need no internal synchronisation.
2. **Lock order is `stateMutex_ → commandMutex_` and `stateMutex_ → waitMutex_`.**
   The worker only ever nests in that direction (it takes the state lock, then
   swaps the command batch). Producers take `commandMutex_` alone.
   `waitUntilQuiet()` takes `stateMutex_` then `commandMutex_` — same order, so no
   cycle exists. There is no path that takes `commandMutex_` and then waits for
   `stateMutex_`, which is the only way this could deadlock.
3. **No observer is called with a lock held.** `Subject::observers()` copies the
   `shared_ptr` list under the subject's own short-lived mutex, releases it, and
   then calls each observer inside a `try/catch`. An observer may therefore call
   back into the engine (`submitLobbyConfirmed` does exactly that) without any
   re-entrancy hazard.
4. **No blocking, unbounded wait while holding the state lock.** The worker's
   condition-variable wait happens *before* acquiring `stateMutex_`; the only
   waits inside the lock are algorithmic (bounded loops).
5. **Producers never touch engine state.** They can only append a `Command` (a
   value type) to the deque. If it is full they wait up to `enqueueTimeout`
   (25 ms) and then receive `false`, which the network layer can translate to a
   429/Retry. That is a deliberate, measurable backpressure policy rather than
   an unbounded queue that hides overload until memory runs out.

### Why a snapshot API instead of returning references

`snapshot()` and `findPlayer()` return **copies** produced under the lock. A
caller can therefore never observe a ticket mid-erase, and no reader can keep a
reference alive past a mutation. This is the main reason the engine has no
"dangling reference" surface at all: the index stores `TicketId` values, and the
only owner of a `Lobby` is a `shared_ptr` map entry (plus, briefly, the call that
is aborting it — which takes it **by value** for exactly that reason).

---

## 2. Structural invariants

These hold at every quiescent point (every instant where the worker has released
`stateMutex_`, which is also every instant a reader can observe):

```
I1. tree_.size() == heap_.size() == tickets_.size()
I2. ∀ id ∈ heap_ : tickets_.count(id) == 1      (no stale heap entries persist)
I3. ∀ id ∈ tree_ : tickets_.count(id) == 1      (same for the interval index)
I4. ∀ id ∈ tickets_ : the stored interval is exactly
      [ticket.ratingLo(), ticket.ratingHi()] where ratingLo/Hi come from the
      ticket's own average rating and its enqueue-time tolerance
I5. ∀ lobby ∈ lobbies_ : lobby->state() == READY or CONFIRMED, slots filled
I6. playersAccepted == playersMatched + queuedPlayers + playersWithdrawn
I7. every player in the repository is in exactly one state, and
      QUEUED  ⇒ their ticket exists in tickets_
      MATCHED ⇒ their match id exists in lobbies_ (unless already confirmed)
```

The concurrency test samples `I1` and `I6` from two reader threads while eight
producers and a churn thread run, and fails the suite on any violation. `I1` is
also asserted by the engine tests after every cancellation and at the end of the
stress run.

`tickets_` is the *source of truth*. The tree and the heap are indexes; if a
lookup misses (which should never happen) the code discards the entry instead of
dereferencing anything. That defensive rule is what makes `I2`/`I3` cheap to
enforce: a stale entry is popped, noticed, and dropped — never trusted.

---

## 3. Match lifecycle

```
                  ┌──────────── abort / shutdown ────────────┐
                  ▼                                          │
   (none) ──► FORMING ──tryAdd...full+balanced──► READY ──confirm──► CONFIRMED
                  │                               │
                  └────── discarded (no event) ───┘
```

* `FORMING` is local to `attemptLobby` and never escapes the worker. A lobby that
  cannot be filled is simply dropped; its tickets were never removed from the
  queue, so there is nothing to roll back. **Two-phase composition** (gather →
  validate → commit) is why an unsuccessful attempt costs nothing but CPU.
* `READY` is the only state that publishes `MatchFoundEvent`. The event is built
  from a copy of the lobby's teams, so it stays valid after the lobby is untracked
  or destroyed.
* `CONFIRMED` means the game server accepted the allocation; players move to
  `IN_MATCH` and the lobby leaves the tracking map (the `MatchFoundEvent` is the
  permanent record).
* `ABORTED` publishes `LobbyAbortedEvent`; live tickets are re-queued **with their
  original enqueue timestamp**, and the ticket containing the disconnecting
  player is dropped (its teammates go back to `IDLE`).

### Player state machine

```
IDLE ──join──► QUEUED ──commit──► MATCHED ──confirm──► IN_MATCH
  ▲              │                   │
  │              │ cancel            │ cancel (disconnect)
  │              ▼                   ▼
  └── party dissolved ── IDLE     DISCONNECTED (removed from repository)
```

Every transition happens inside the worker under `stateMutex_`, so a player can
never be seen in `QUEUED` while their ticket is already gone (the ordering inside
`handleCancel`/`commitLobby` is: purge index → update states → publish).

---

## 4. Fairness argument

Define a ticket's priority at time `t`:

```
P(t) = w_w · 1000 · (t − t₀)^γ / T_ref^γ  +  w_s · 1000 · scarcity / 100
```

with `γ ≥ 1`, `scarcity ∈ [0, 100]`, `w_s = 0.3` default. Then:

1. `P(t)` is non-decreasing in wait time for every ticket, and strictly increasing
   for `w_w > 0`, `t > t₀`.
2. For `γ = 1` the *relative order* of a fixed set of tickets is invariant
   (the `w_w·t` term is common), so a linear term cannot rescue an isolated
   bracket. `γ > 1` makes old waiters overtake fresh ones; `validate()` rejects
   `γ < 1`.
3. Because the heap re-keys in bulk every `priorityRefreshInterval`, the pending
   ticket set is re-ordered exactly, with a bounded staleness of one interval.
4. Every anchor is attempted at most once per tick, and a ticket that cannot be
   matched is re-inserted with an increased key. It therefore climbs the heap and
   gets the first attempts as its window widens. Under a bounded number of
   attempts per tick, no ticket can be starved by a stream of fresh arrivals:
   fresh arrivals start at `P ≈ 0..300`, an old ticket at `P = 1000·k^γ`.
5. Tickets that *can never* legally match (ping/region rules) are not starved —
   they are unmatchable. The engine reports them (`queuedPlayers`,
   `longestWaitSeconds`) and the demo prints an explicit note, instead of silently
   violating the hard latency cap to clear the queue.

---

## 5. Complexity summary

| Path | Cost |
|---|---|
| `submitJoin` (producer) | `O(1)` amortised + bounded wait |
| join handling | `O(log N)` insert into tree & heap + `O(log N + cap)` scarcity estimate |
| one anchor attempt | `O(log N + K)` query, `K ≤ maxCandidatesPerAnchor`, plus `O(K log K)` shortlist sort |
| commit a lobby | `O(slots · (log N + 1))` erases + one event enqueue |
| cancel/disconnect purge | `O(log N)` (tree) + `O(log N)` (heap) + `O(1)` average (hash) |
| priority refresh | `O(N)` every `priorityRefreshInterval` |
| `snapshot()` | `O(N + S log S)` worst case (waiting tickets + percentile sort); use sparingly |

The `O(N)` refresh is the only super-logarithmic periodic cost, and it is what
buys exact time-dependent ordering without per-element timers.

---

## 6. Failure handling

* **Invalid requests** (empty party, wrong mode, duplicate player id, non-finite
  MMR, ping above the ceiling, over-sized party, intra-party MMR/ping spread) are
  rejected with a metric and never enter the queue.
* **Exceptions inside a command handler** are caught per command, counted
  (`commandErrors`) and logged; one malformed request cannot kill the worker.
* **Observer exceptions** are caught at the subject boundary and at the async
  channel; `observerFailures` counts them. A broken socket observer does not stop
  matchmaking.
* **Overflow policy**: the command queue applies backpressure then rejects; the
  event queue drops the newest event and counts it. Both are visible in metrics,
  because silent loss is worse than a number.
* **Shutdown** aborts live lobbies with reason `SHUTDOWN`, stops the notifier
  thread after draining its queue, and joins both threads. It is idempotent and
  safe to call from a destructor.

---

## 7. Known trade-offs

| Decision | Benefit | Cost |
|---|---|---|
| Single worker per instance | No races, simple reasoning, cache-friendly | Vertical limit; scale by sharding regions/brackets |
| Full priority rebuild `O(N)` every 100 ms | Exact ordering, no per-ticket timers | `O(N)` periodic work (fine for 10²–10⁵ tickets) |
| Candidate scan capped at 64 | Bounded per-attempt latency | May miss a better candidate deeper in the window; mitigated by score-ranked shortlist and by re-attempts |
| Ids instead of pointers in indexes | No dangling references, easy serialisation | One hash lookup per candidate/commit step |
| `snapshot()` copies | Race-free readers | `O(N)` read cost; not for per-request hot paths |
| Greedy snake-draft balancing | Simple, deterministic, fast | Not an optimal partition; guarded by a hard average-gap check |
