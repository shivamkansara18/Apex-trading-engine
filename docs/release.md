# Local release evaluation and reproduction

Apex is an educational, single-symbol exchange simulator. Local implementation
phases 0-11 are present, including the matching engine, risk controls, sealed audit
replay, reproducible workloads/benchmarks, TCP clients and an interactive dashboard.
Phase 12 provides the verification runner and this release walkthrough. A public
release/tag is not yet created: the checkout has no commits or remote, hosted CI
has not run, and browser visual verification was blocked by macOS capture access.

## Reproduce on a Mac

Requirements: Apple command-line C++ tools, CMake 3.20+, Ninja, Git, Python 3
and Node.js 24+ with npm. The C++ engine itself needs neither Node nor Python.
No venv is required. The first CMake configure can fetch pinned GoogleTest 1.17.0
from GitHub; npm installs the exact UI dependency lockfile from the npm registry.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
npm ci --prefix ui
npm --prefix ui run build
npm --prefix ui test
```

Run the local evaluation with retained evidence:

```sh
python3 scripts/verify_release.py --clean-source
```

The runner stops at the first failed step and prints its log. It creates a new
`tmp/release-TIMESTAMP-PID` directory, never overwrites a previous run, and writes
`verification.json` even on failure. A successful full run covers:

- Configure/build/test in Debug, ASAN+UBSAN, native ThreadSanitizer and unsanitized
  Release benchmark presets. On this Mac each suite contains 189 CTest cases,
  including protocol, replay, risk, randomized policy comparisons, queue stress,
  concurrent TCP producers and output overflow.
- UI typecheck/production build and 24 Node tests, including real gateway traffic,
  snapshot recovery, reconnect, journal replay and launcher cleanup. The launcher
  test also runs the live activity driver through ten native TCP commands.
- Saved seed-42, 100,000-command balanced workload; five repetitions plus warmup
  in the Release benchmark; independent CSV/JSON/sample arithmetic auditing.
- A source ZIP with deterministic content fingerprint and a separate ZIP hash.
  The archive includes source, fixtures, UI lockfile, scripts and docs, excluding
  dependencies, compiled output, runtime data and Git metadata.
- With `--clean-source`, fresh configure/build/tests from the extracted ZIP and
  a fresh lockfile-based npm install/build/test against those fresh binaries.
  The runner explicitly records when GoogleTest source is reused from the local
  pinned cache. This is local source-archive reproduction, not hosted clean-clone CI.

The manifest records the actual host, tool versions, command lines, step durations,
exit codes, source fingerprints and log locations. Benchmark metadata separately
records compiler/options, host characteristics and its own source fingerprint
format. These two source hash formats should not be compared as identical values.
Runtime folders are gitignored; retain the full evidence directory when sharing
results. `--quick` explicitly excludes ASAN/UBSAN and TSAN and cannot substitute
for full verification. Sanitizers must be available on the chosen host; a failure
is reported rather than silently skipped. No Linux machine is required on macOS.

## Demonstrate the complete flow

Terminal 1:

```sh
npm --prefix ui start -- --journal tmp/walkthrough.apex
```

Create `tmp` first if needed, and choose a new journal filename for each session.
Open http://127.0.0.1:8080. The initial empty book is expected: this is a fresh
simulator session, not an external market connection. Toggle Light mode in the
header for the cream palette; the browser remembers the preference.

Terminal 2:

```sh
npm --prefix ui run demo
```

The adaptive driver uses persistent TCP sessions and participants 9-12. It quotes
from live depth, tracks inventory from actual fills and selects actions/sizes with
a printed random seed. Use participants 1-8 in the browser Trading desk to submit
your own limit/market/cancel/replace orders alongside it. For a shorter run use
`npm --prefix ui run demo -- --steps 100 --seed 42`. Ctrl-C or normal completion
attempts to cancel only the agents' own quotes. There is no fixed expected fill
count; outcomes come from the current book and engine. The original scripted
walkthrough remains `npm --prefix ui run walkthrough -- --rounds 3`.
For a custom gateway port add `--gateway-port 9001` after `--` to both commands.
See [responsive simulation](scaling.md) for the policy and its limits.

Observe depth, the recent trade tape, feed sequences and the rolling event rate.
The rate returns to zero five seconds after traffic stops. That is a quiet market,
not a broken data connection. Turn the theme back to dark, refresh the page and
verify the selected theme persists. Disconnect/reconnect the observer and inspect
the new anchor snapshot. Observer feeds remain read-only; Trading desk connections submit commands under
a separate immutable participant identity.

Stop Terminal 1 with Ctrl-C, then:

```sh
./build/debug/apex --replay tmp/walkthrough.apex
python3 scripts/network_demo.py
```

The first command verifies all recorded commands, events and final state. The
second runs the separate scripted seven-command networking/replay demonstration.
This walkthrough can be recorded as the release demo; no video is claimed yet.

For independent live books, start with `--symbols APEX,ALPHA,BETA` and use a new
`--journal-dir` instead of `--journal`. Select each book in the dashboard, run
`npm --prefix ui run demo -- --symbol ALPHA`, then replay each symbol's journal
independently. See [multi-symbol reproduction](multi-symbol.md).

Reconnect Trading desk to restore private open orders, or use native `ORDERS`.
[Private recovery](private-orders.md) distinguishes current open state from terminal
outcome/history recovery and automatic retries, which are not implemented.

## Architecture and performance interpretation

The C++ matching engine remains synchronous and single-writer. Commands validate
before mutation; FIFO within price uses exchange priority, executions use resting
prices and all price/quantity arithmetic uses integers. Read-only risk traversal
avoids snapshot copies without maintaining a second ledger. The mutex queue owns
its inputs; bounded network outputs and observer overflow never block matching.
The Node/React layer is isolated from matching. See [architecture](architecture.md),
[matching](matching-engine.md), [risk](risk-controls.md), [journal](journal-and-replay.md),
[backpressure](backpressure.md) and [dashboard](dashboard.md) for exact contracts.

The original [baseline](performance-report.md) and [paired optimization evaluation](optimization-experiments.md)
remain the comparison evidence. The selected direct-risk policy gave 1.946..2.188x
paired throughput ratios on the three saved inputs on the captured Apple M5.
Fresh verification benchmarks exercise the current release path and are not a
new paired optimization comparison. Core-only latency/throughput excludes sockets,
journaling, bridge/browser work and network queueing; macOS scheduling and clock
quantization limit generalization. No live market or production latency claim is made.

## Portfolio review materials

The source includes an independent vector/sort matching oracle, a re-audited
historical performance summary, the measured source snapshot, a five-minute
review path and separate engineering/research resume project entries. See
[reviewer guide](reviewer-guide.md), [validation](independent-validation.md),
[resume sections](resume-project.md) and [interview preparation](portfolio.md).
Portable evidence tests validate summaries/source identity without pretending that
all raw historical latency rows are bundled. New full runs also execute three
Python evidence-guard tests in the working and extracted source trees.

## Package the verified snapshot

A source/evidence ZIP can be assembled from a successful full verification run:

```sh
python3 scripts/package_release.py tmp/release-TIMESTAMP-PID --output tmp/releases/apex-0.5.0-candidate.zip
```

See [packaging and checksum verification](packaging.md) for exact contents, checks,
overwrite behavior and publication status. Later working-tree edits are not included
in an earlier verified source archive.

## Remaining publication gates

Local verification does not create a release tag. Before publishing, commit the
reviewed source, configure a remote and verify hosted CI from a clean checkout;
review the dashboard in an ordinary browser in both themes and record the
walkthrough. Retain matching raw benchmark/source artifacts and identify the
measured host. Only then create/tag the intended release. No account, remote,
commit, push or tag is created by the local verification command.
