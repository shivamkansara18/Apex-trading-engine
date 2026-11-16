# Architecture

## Implemented foundation

`apex_core` is a C++20 library with a version API. `apex` is the terminal entry
point. `apex_tests` links the same library through GoogleTest. `apex_options`
provides warnings and optional sanitizer instrumentation for project targets.
Domain headers define commands, orders, trades, events and owning snapshots;
see [domain model](domain-model.md). They have no platform-specific dependencies.
`OrderBook` owns map/list storage, an ID index and cached side quantities.
See [order-book storage](order-book.md). `MatchingEngine` owns the book and
sequence counters, processes variant commands synchronously, and returns ordered
events. See [matching engine](matching-engine.md). The terminal `--trading-demo`
shows actual executions; `--book-demo` shows storage. Phase 9 adds sockets and an
owning gateway event-loop thread outside the matching library.

Phase 4 adds `RiskManager`, owned by the engine. It keeps participant limits and
global admission state, derives usage from the authoritative book, and preflights
market risk before mutation. See [risk controls](risk-controls.md). Configuration
is single-writer; no concurrent admin changes are supported.

Phase 5 adds a `JournaledEngine` facade. It records every admin/command input and
the canonical result outside matching, syncs before returning, and seals sessions
explicitly. Strict replay rebuilds a fresh engine and verifies events and book
hashes at each record. See [journal and replay](journal-and-replay.md).

Phase 6 adds a simulator with seeded provider/taker/random agents and a scripted
large-fill profile. It saves versioned scheduled commands outside matching. A
correctness runner dispatches each symbol to its own engine and checks invariants;
optional single-symbol runs use the journal facade. Neither scheduling nor
generation adds clocks/I/O to the core. See [workloads](workloads.md).

Phase 7 adds a separate `apex_metrics` library and Release-only `apex_benchmark`
executable. It prepares engines outside timing, runs independent bulk/latency
passes, verifies outputs against the correctness runner, then exports metadata
and raw samples. Core matching remains free of benchmark clocks and file I/O.
See [benchmark methodology](benchmark-methodology.md).

Phase 8 adds physical engine policies with identical business behavior. The
selected default reads risk inputs directly on the single writer; public
snapshots still own their data. Tree/list price storage remains the default.
A flat sorted level index and smaller event reservation are opt-in research
paths, tested against the preserved reference. Journals remain format version 1
because outputs are byte-identical. See [experiments](optimization-experiments.md).

Phase 9 adds `apex_network`, `apex_gateway` and `apex_client`. One nonblocking poll
loop owns loopback sessions, the bounded FIFO inbox and engine calls; it then
queues execution reports and public trade/depth publications. Submitters,
makers and subscribers receive owning encoded bytes. No I/O consumer can access
mutable book state. Startup hands engine ownership to that writer once; stop
tokens are the only cross-thread control. Optional journal calls remain outside
matching. See [networking](networking.md) and [protocol](protocol.md).

Phase 10 replaces the ingress deque with a mutex-protected bounded owning queue;
queue metrics and atomic gateway counters can be sampled across threads. The
single poll/matching writer remains unchanged. See [backpressure](backpressure.md).

Phase 11 adds a React + TypeScript browser observer and a separate Node bridge:
`gateway TCP -> observer codec -> bounded WebSocket -> browser feed state`.
The bridge never imports the matching library or submits business commands.
Each browser receives an independent snapshot anchor and sequenced updates;
gaps request another snapshot. See [dashboard](dashboard.md).

## Ownership and flow

The gateway parses framed input and verifies session identity, then transfers
commands into a bounded ingress queue on the same event loop. One writer validates commands and
owns all mutable order-book state. The book uses price maps, FIFO order lists,
and an order index. Queries expose snapshots rather than mutable containers.

The engine produces ordered events for execution reports, journaling, and market
data. Consumers must not mutate engine state. Blocking network/disk operations
belong outside matching. Durable acknowledgement policy and journal failure
behavior is defined for the Phase 5 synchronous wrapper in the journal document.

Replay starts from an empty book using recorded configuration and command order.
Wall-clock time and client timestamps cannot influence matching decisions.
Networking adds concurrency around this core without changing single-writer
ownership. No lock-free structure is planned until a measured bottleneck exists.

## Version 0.2.0 interaction and feed scaling

A bridge-level GatewaySession owns persistent TCP framing and bounded pending
commands. Browser observers fan out from one shared observer-0 session. Browser
traders bind separate participants and receive only their session's execution
reports. Observer requests cannot submit business commands. Adaptive participants
use the same persistent adapter and engine events; book/inventory affect decisions.
No JavaScript code mutates C++ engine state directly. Matching and risk ownership
remain unchanged. See [responsive simulation and scaling](scaling.md).

## Version 0.3.0 symbol ownership

The launcher maps each configured symbol to a separate C++ gateway process. One
process owns one engine, registry, queue, sequence space and optional journal.
The bridge keeps per-symbol observer hubs and binds each trading WebSocket to a
fixed native destination. Browser routing schema v2 includes symbol identity;
existing TCP v1 remains one instrument per endpoint. Partitions can match in
parallel; no global cross-symbol order or risk ledger is introduced. See
[multi-symbol lifecycle and replay](multi-symbol.md).

## Version 0.4.0 private synchronization

The matching writer captures a participant-filtered owning order vector between
commands. Native type 106 carries a command watermark and at most 1000 live orders.
The bridge orders initial recovery before new submissions; TCP report ordering
ensures later events update the restored state. No cached browser state becomes
exchange authority, and no query mutates matching or risk state. See
[private recovery semantics](private-orders.md).
