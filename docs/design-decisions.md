# Design decisions

1. C++20 and CMake; GCC/Clang support. macOS arm64 with Apple Clang is the primary
   development, correctness and local benchmark environment per user direction.
   Linux evaluation is optional later. Do not mix results between hosts.
2. Single-writer engine. Concurrent ingress will submit ordered commands; it will
   never mutate the book. No network or blocking file I/O inside matching.
3. Baseline book: ordered price maps, FIFO `std::list` orders, and an unordered
   ID-to-location index. Phase 8 retains this default; flat levels remain opt-in.
4. Integer ticks/quantities, resting-price executions, incoming-remainder
   self-trade cancellation, and replacement priority reset. See matching rules.
5. GoogleTest is discovered locally or fetched at the pinned v1.17.0 release.
   Build dependencies stay outside tracked source. CMake options scope sanitizer
   flags to project targets. The benchmark executable requires unsanitized Release.
6. No license chosen yet. Select a license before public distribution. No remote
   repository is created and no commits/pushes are implied by local setup.
7. Domain records are owning value types with structural equality and variant
   command/event payloads. Raw structs are not serialization formats. See
   `domain-model.md` for lifecycle, sequence and cancellation semantics.
8. Risk requires explicit participant registration. Notional is per-order in
   tick-units. Market sweeps are preflighted before any fills. Usage is derived
   from the book for correctness, using direct traversal by default after Phase 8.
   A snapshot-copy reference remains selectable. Kill
   switches preserve resting orders and allow cancels. See `risk-controls.md`.
9. Journaling wraps matching rather than adding I/O to the core. Completed files
   are explicitly sealed and replayed strictly; truncated tails are errors, not
   silently recovered prefixes. All admin inputs are recorded. See
   `journal-and-replay.md` for the versioned format and sync/failure guarantees.
10. Workloads are generated offline with stable integer sampling and saved as
    versioned text. Shadow engines keep cancel/replace targets live. Symbols are
    isolated engine instances; scheduled times never set priority. The runner
    verifies correctness and hashes, leaving measurement to Phase 7. See
    `workloads.md` for profiles, configuration and format boundaries.
11. Phase 7 uses a custom deterministic full-engine harness, with separate bulk
    throughput and per-call latency passes. Warm-up and verification are untimed;
    raw samples, repeated-run medians and host/build metadata are retained.
    Google Benchmark microbenchmarks are deferred to targeted experiments.
    See `benchmark-methodology.md` for timing scope and sample-size limitations.
12. Phase 8 promotes allocation-free risk reads after native sampling and paired
    comparisons. It preserves the reference, rejects smaller initial buffers as
    a default, and keeps flat prices opt-in because active-depth scaling is not
    established. All policies must produce identical events, state and journals.
    See `optimization-experiments.md` for the retained source/data and decisions.
13. Phase 9 uses a single nonblocking poll loop for socket ownership, ordered
    ingress and matching calls. This provides concurrent client ingress without
    sharing book memory or adding a blocking network operation to the core.
    Framing, buffers and output are bounded. The first feed uses trade events
    plus top-depth snapshots; book deltas and UDP are deferred. Session IDs are
    local simulator identity, not credentials. See `protocol.md` for disconnect,
    retry and optional journal guarantees.
