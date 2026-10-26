# projects-based-on-dsa-and-oop
High-performance projects built on core OOP principles and advanced DSA.

Each project is a self-contained, buildable system — not a snippet. The focus is
on the parts that are hard to get right: the right data structure for the
operation that actually runs hot, clean object-oriented boundaries, and
concurrency that is verified with sanitizers instead of asserted in prose.

---

## Projects

### [MMF — Multiplayer Matchmaking Engine](mmf-engine/) · C++20

A thread-safe matchmaking backend for competitive multiplayer games: thousands of
players and pre-made parties join, leave and get matched concurrently while a
dedicated worker forms balanced 5v5 lobbies.

**Data structures**
- **Augmented AVL interval tree** over each player's MMR tolerance window
  `[MMR − Δ, MMR + Δ]` — overlap queries in `O(log N + K)` via `maxHi` pruning,
  worst-case `O(log N)` insert/erase.
- **Indexed weighted min-heap** with `O(log N)` delete/re-key so cancellations and
  aging refreshes never require a scan.
- **Aging-aware priority**: `priority = w₁·(wait/T_ref)^γ + w₂·scarcity` with
  `γ > 1`, which is what actually prevents starvation for isolated skill brackets.

**Engineering**
- Observer pattern: a `Lobby` is the Subject; an async notification channel feeds
  `SocketNotifier`, `GameServerAllocator` and `TelemetryLogger` off the hot path.
- Single-writer consumer thread + bounded MPSC command queue with explicit
  backpressure; strict lock ordering documented and tested.
- Strategy pattern for Casual vs Ranked, hard latency policy with pairwise ping
  deltas, whole-party lobby placement, `O(log N)` disconnect purges, graceful
  shutdown, conservation checks.
- **29 test suites**, AddressSanitizer/UBSan clean, ThreadSanitizer clean, plus
  reproducible microbenchmarks (≈2.5k 5v5 lobbies/s and ≈25k players/s on a
  2-vCPU VM; interval-tree query 2.3 µs for 90 reported matches).

```bash
cd mmf-engine
make run     # concurrent demo: producers, churn, telemetry
make test    # 29 suites
make bench   # data-structure + end-to-end benchmarks
```

Read the full write-up: [mmf-engine/README.md](mmf-engine/README.md) ·
design notes: [mmf-engine/docs/DESIGN.md](mmf-engine/docs/DESIGN.md)

---

## Repository conventions

- `make` / `CMake` builds with no third-party dependencies.
- Tests are part of the deliverable; each project ships a `make test` target.
- Every performance claim in a project README is reproducible with its own
  benchmark target on your machine.
