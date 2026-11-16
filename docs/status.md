# Project status

## Current milestone

Version 0.5.0: independent correctness validation and evidence-backed portfolio release.
Phases 0-11 local deliverables are ready; hosted CI remains unverified.

## Environment inspected

- Local host: macOS / Darwin, arm64.
- Compiler: Apple Clang 21.0.0, C++20 available.
- Git: 2.55.0.
- Native Homebrew CMake 4.5.0 and Ninja 1.13.2; no venv activation needed.
- Python 3.14; pypdf installed locally to read the supplied 17-page guide.
- Primary development, test and local benchmark target: macOS arm64, per user
  direction. Linux performance evaluation is optional later.

## Verification

- Phase 9 native macOS Debug: 145/145 CTest tests passed.
- Phase 9 native AddressSanitizer + UndefinedBehaviorSanitizer: 145/145 tests passed.
- Phase 9 native unsanitized Release benchmark build: 145/145 tests passed.
- Final protocol/network/CLI checks reverified in all three builds, including
  asynchronous maker updates while console input remains open.
- Loopback tests ran outside the execution sandbox with approval; no socket tests
  were silently skipped. No venv or Linux host was needed.
- Eight concurrent clients produced consecutive engine sequences 1..80; a large
  fill delivered 80 trades/241 response events and replayed all 401 engine events.
- Real-process demo verified seven commands, three trades, continuous feed,
  interactive maker delivery, graceful journal sealing and 22-event replay.
- Native /usr/bin/sample profile captured outside the sandbox with approval;
  profile artifacts: `results/raw/profile.ORkzGm`.
- 75 paired/interleaved experiment bundles, three saved 100,000-command inputs,
  five policies and five blocks; 7,500,000 raw samples verified.
- Measured source archive fingerprint independently checked. Result bundle:
  `results/raw/phase8-experiments`, with full interpretation in
  `optimization-experiments.md`.
- All eight policy combinations match reference events/state and byte-identical
  journals. A pre-optimization journal fixture replays under the selected default.
- Fresh-directory experiment orchestration and input fallback verified with a
  small functional smoke run (not performance evidence).
- One-command baseline script completed: 100,000 commands, five repetitions,
  one warm-up pass and 500,000 raw latency samples. CSV/JSON arithmetic and
  percentiles independently verified with the standard-library analysis tool.
- Local artifact bundle: `results/raw/phase7-macos-baseline`; interpretation
  and exact measurements are in `performance-report.md`.
- Fresh source copy under `tmp/phase0-clean`: Release configure/build/run passed,
  2/2 tests passed. Reused the downloaded GoogleTest source explicitly via
  `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`; this was not a remote clone test.
- CLI generation, repeat multi-symbol runs, optional journal/replay and invalid
  options verified separately. No compiler warnings observed in these builds.
- Local Git repository initialized. No commit or remote created.
- Linux CI workflow written, but not executed remotely. Hosted CI and clean-clone
  verification remain release tasks; local development proceeds on macOS.

Checks cover foundation, domain, storage and matching scenarios. A seeded
1,500-command stream reproduces identical events and snapshots between two engines.
A separate seeded 1,200-command risk stream verifies rejection immutability and
independent participant usage totals. Phase 5 adds file-backed exact output replay,
byte-identical seeded runs, corruption/truncation checks, and actual write-failure
injection. Phase 6 adds deterministic role-based generation, all six workload
profiles, multi-symbol isolation, lossless saved inputs and journal integration.
Phase 7 adds a separate measured baseline; prior checks remain correctness evidence.

## Phase 1 decisions

- Fixed-width integer units; tagged command/event payloads; owning snapshots.
- Separate command, event and trade counters, starting at one without wrap.
- Accepted state distinguishes validated incoming orders from resting book orders.
- Cancellation retains the unexecuted quantity for audit; replace quantity is
  desired remaining quantity. Rejected replace retains both requested IDs.
- Detailed contracts are in `domain-model.md` and `matching-rules.md`.

## Phase 3 decisions

- Resting-price executions, FIFO by command sequence, and cancellation of incoming
  remainder on self-trade (earlier fills stand).
- Replace validates first, always resets priority, and treats new quantity as
  desired remaining quantity.
- Basic command validation is in place; Phase 4 now adds mandatory participant
  registration and configurable risk policies before matching.
- Side-total capacity uses conservative full-size admission, credited for the
  old remaining quantity on replace. Event capacity is reserved before processing.
- Exact event ordering and fatal exception policy are in `matching-engine.md`.

## Phase 4 decisions

- Explicit registration; per-order notional in tick-units, not aggregate exposure.
- Market notional/bands use executable liquidity up to self-trade prevention;
  failures reject the whole command before any fills.
- Open-order slots are conservative for limits; replace credits one slot and
  old remaining quantity. Market orders do not need a resting slot.
- Usage derives from the authoritative book; no separate cached risk ledger yet.
- Kill switches block new/replace, preserve resting orders and allow cancels.
- Admin configuration is single-writer; the Phase 5 facade now records it for replay.
- Full policies and API examples are in `risk-controls.md`.

## Phase 5 decisions

- Exclusive new-file creation, explicit big-endian versioned format, fixed-width
  fields and bounded record lengths. No raw-struct serialization.
- One input/output record per admin action or command, including rejections;
  consecutive journal record IDs and CRC-32 detect accidental damage.
- Synchronous complete writes plus fsync before returning results; journal I/O
  stays outside matching. Any processing/write/sync error stops the session.
- Explicit terminal seal detects incomplete logs even at record boundaries.
  Strict replay requires a complete sealed session; no partial-tail repair or
  resumable live-engine recovery is implemented.
- Replay compares canonical output bytes and book hashes per record. FNV-1a
  reports cover book/event state; hashes/checksums are not cryptographic.
- Format, guarantees and limitations are in `journal-and-replay.md`.

## Phase 6 decisions

- Native C++20 simulation, mt19937_64 plus explicit integer sampling, ordered live
  target selection and shadow engines. Same seed/config/version gives same input.
- Providers quote passively, takers use markets, random flow mixes types; a
  scripted profile exercises large partial fills. Six presets are available.
- Versioned text workloads include full generator config and ordered scheduled
  commands. Limits, entry count, terminal marker and ranges are checked on load.
- Multiple symbols route to isolated engines with local sequence numbers and
  registries. Journal recording currently supports one symbol only.
- Scheduling is logical metadata; the correctness runner does not sleep or
  measure latency/throughput. Generator/loader are outside future timed regions.
- Details, caveats and commands are in `workloads.md`.

## Phase 7 decisions

- Separate throughput and latency passes using the same saved workload; no
  generation, file output or final verification in the timed matching interval.
- Untimed full warm-up runs and fresh registered engines for every measured pass.
- Closed-loop synchronous engine scope; no queue, network or journal timing.
- Nearest-rank percentiles; p99.9 omitted below 100,000 samples per run. Report
  all repetitions, medians and variability, preserving raw samples.
- Release-only benchmark executable; tests cover metrics in Debug/ASan too.
- Machine/compiler/flags, source SHA-256, workload identity, clock diagnostics
  and process peak RSS are recorded. No optimization/speedup claim yet.
- See `benchmark-methodology.md` and `performance-report.md`.

## Phase 8 decisions

- Retain direct read-only risk traversal as default; remove snapshot allocation
  without adding a second risk ledger. Tree price levels and worst-case event
  reservation remain defaults.
- Keep explicit snapshot/tree/worst-case reference options. Flat levels and small
  buffers remain reproducible research policies; neither is promoted by default.
- The direct policy gave 1.946..2.188x median paired throughput ratios across the
  three captured inputs, with lower p99/p99.9. Results are host/workload-specific.
- Flat prices have O(P) level insertion/removal and the captures are shallow;
  do not generalize their combined-policy gains to deep books.
- Small event buffers regress large-fill tails and are rejected as a default.
- See `optimization-experiments.md` for measurements, provenance and caveats.

## Phase 9 decisions

- Version-1 big-endian length framing, bounded messages and explicit payload
  codecs; no raw struct casts. Split reads, partial writes and coalescing tested.
- Loopback-only server; HELLO binds immutable simulator identity, observer 0 is
  read-only, request sequences are consecutive. This is not authentication.
- One poll/event-loop writer owns sessions, bounded inbox and matching. Network
  clients never access mutable book state. Received order is journal-replayable.
- Submitters receive correlated ACKs/events, makers receive unsolicited reports,
  subscribers receive trade increments and top-depth snapshots with feed sequences.
- Disconnect preserves resting orders. Ambiguous disconnects have no across-session
  exactly-once guarantee. Optional synchronous journal mode seals on graceful stop.
- Initial connection/inbox/output caps are implemented; detailed overload, fairness,
  liveness and concurrency stress evidence is recorded below in Phase 10.
- See `networking.md` and `protocol.md` for runnable commands and precise limits.

## Phase 10 validation

- Native macOS Debug: 150/150 CTest tests passed.
- Native AddressSanitizer + UndefinedBehaviorSanitizer: 150/150 passed.
- Native ThreadSanitizer: 150/150 passed, including producer and network stress.
- These are local results; hosted CI has not been run.

## Phase 10 decisions

- Added mutex-protected bounded queue with owned messages and thread-safe counters.
- Added handshake, idle and incomplete-frame deadlines and shutdown statistics.
- Stress covers 40,000 values from eight producers, isolated inbox overload,
  deadline eviction and output overflow during a complete 400-maker fill.
- Retain one matching/I/O writer; no speculative lock-free queue or worker threads.
- See `backpressure.md` for admission, discard, timing and fairness semantics.

## Phase 11 validation and decisions

- Fresh pre-UI verification: 150/150 CTest cases passed in each of Debug,
  Release benchmark, ASAN/UBSAN and native macOS ThreadSanitizer (600 checks).
- React + TypeScript production build passes; 10 Node tests pass, including
  real-process gateway/bridge/live trade/depth/gap recovery/reconnect/replay.
- Launcher startup, HTTP serving, graceful shutdown and owned-gateway cleanup pass.
- A three-repetition 100-command Release benchmark exported its metadata and raw
  samples as a smoke check; it is not new performance evidence.
- Browser screen-capture access was denied by macOS, so visual layout/interaction
  QA remains unverified. Automated HTTP, WebSocket and feed-state checks pass.
- Read-only observer bridge, eight browser sessions, bounded WebSocket messages
  and output; no web runtime or UI code in matching. Exact integers cross JSON as
  strings. Quiet markets, stale connections and gaps have distinct status.
- Snapshot recovery restores current depth; missed trade history is not backfilled.
- See `dashboard.md` for native run commands and precise display/feed semantics.

## Phase 12 local deliverables

- Full verification runner retains tool versions, command logs, source/ZIP hashes,
  sanitizer/Release evidence and independently audited benchmark exports.
- Optional source-archive rebuild checks fresh native binaries and fresh lockfile UI
  install/tests, with cached GoogleTest use stated explicitly.
- A terminal walkthrough and an opt-in live activity driver demonstrate real TCP
  orders, trades, remaining depth and journal replay. External markets are not connected.
- Dashboard adds a persistent cream light theme and explains the initial empty book.
- See `release.md` and the generated `verification.json` for scope and evidence.

## Release candidate packaging

- Added a source/evidence packager with a version check, required-check admission,
  source/evidence hash validation, raw samples and per-file/outer ZIP checksums.
- Five standard-library packaging checks pass. New verification runs and Debug CI
  include them; the existing full application evidence remains valid for its snapshot.
- Added the 0.1.0 changelog and `packaging.md` handoff instructions.
- Candidate bundles remain local and use the exact previously verified source ZIP,
  including all dashboard, light-theme and activity-driver application changes.

## 0.2.0 responsive simulator

- Browser Trading desk submits real limit/market/cancel/replace requests through
  persistent identified TCP sessions and displays engine ACKs/reports/rejections.
- Default demo now uses book- and inventory-reactive participants 9-12, configurable
  sizes/timing/reference/seed and actual execution feedback. Prior exact scripted
  activity remains available under `npm run walkthrough`.
- All dashboard observers share one upstream session per bridge. Viewer output,
  connection count, pending commands, snapshots and display histories stay bounded.
- Node tests cover manual partial fills, ownership/identity, replace/cancel/risk
  rejections, shared fanout/capacity, adaptive cleanup and sealed replay.
- This remains a synthetic local exchange model. External prices, calibrated market
  realism and production/broker operation are not implemented. See `scaling.md`.
- Prior local release ZIPs are immutable historical snapshots; a new verification
  and package captures this expanded version.

## 0.3.0 live symbol partitions

- Up to eight gateway processes provide independent live books and matching writers.
- Browser routing schema v2 binds each observer/trading session to a named symbol.
  Native TCP and journal v1 stay compatible through dedicated per-book endpoints.
- Symbol selection clears public/private UI session state and ignores late old-feed
  callbacks; selection is disabled during a pending trading command.
- Per-book journals plus a session manifest support independent replay and explicit
  process availability. Failed startup cleans up owned processes; runtime partition
  failure leaves healthy books available without silent restarts.
- Node tests cover market registration, feed guards, cross-book price/identity/ID
  isolation, named agents, per-book replay, outage isolation and startup rollback.
- See `multi-symbol.md` for exact guarantees, per-book risk scope and current limits.

## 0.4.0 private open-order snapshots

- Native request 8/response 106 exposes only the HELLO participant's live orders.
  Snapshot capture is ordered by the existing writer and does not mutate sequences.
- Browser submissions wait for authoritative recovery; private refresh and native
  ORDERS recover current quantity/priority after disconnected fills.
- Snapshots cap at 1000 orders in a 47,032-byte frame. Observers/spoofed ownership,
  malformed counts, duplicates, invalid live status and identity mismatch reject.
- Five added native cases and two Node cases cover maximum-size I/O, offline
  partial/complete fills, recovery, cancel/replace, privacy, symbol isolation and replay.
- This restores open state only: no blind retries, terminal outcome ledger,
  missed execution-history reconstruction or live process crash recovery.
- See `private-orders.md` for exact wire and UI semantics.

## 0.5.0 independent validation and portfolio evidence

- Added an independent vector/sort reference matcher with no production book/risk
  helpers. Thirty-two 1000-command streams compare outcomes, state, priority,
  sequence continuity, risk usage and quantity conservation after every input.
- Full production event streams also compare default and alternate configurations,
  covering all eight policies; a hand-calculated scenario checks the reference.
- Re-audited all 75 historical paired bundles and 7.5 million raw samples. Portable
  summaries, raw-file hashes and measured source are included in `results/summary`.
- Added a five-minute review path, scoped engineering/research resume entries and
  technical exercises. Official employer materials inform positioning, not promises.
- Three Python evidence-guard checks and a portable-evidence CTest validate the
  shared summaries. Full/source verification and packaging include these checks.
- Native test count is now 189 on this Mac. The dashboard's 24 tests are unchanged.

## Remaining release work

The user selected local-only delivery. Remote CI and tagging are optional future
publication work; ordinary-browser verification remains unavailable to automation. User feedback confirms the original dashboard appearance; automated
capture was blocked by macOS permissions, so the cream theme still needs visual
review. The repository has no commits or remote; no release is tagged or published.

Run `npm --prefix ui start` after the documented UI build, then open
http://127.0.0.1:8080. In a second terminal run `npm --prefix ui run demo` to create
live simulated activity. The terminal `python3 scripts/network_demo.py` still works.
No venv is required.
