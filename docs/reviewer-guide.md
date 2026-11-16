# Five-minute technical review

A reviewer should be able to establish what the system does, what was measured,
and how to reproduce it without reading the development chronology.

## First minute: design and scope

Read the README's design and results table. Inspect `src/matching/matching_engine.cpp`
and `include/apex/book/order_book.hpp`: price/FIFO matching, list-node ownership,
ID index, explicit event sequences and validation-before-mutation. This is a local
simulator. No market-data provider, broker or production trading claim is made.

## Second minute: correctness independent of optimization

Read `tests/reference/linear_matcher.hpp` and `tests/unit/differential_test.cpp`.
The independent vector/sort model avoids production book/risk helpers. Thirty-two
seeds exercise 32,000 mixed inputs; each transition checks projected outcomes,
priority, settled state and quantity conservation. Full intermediate events are
also compared across two production policy configurations. See the validation
scope: capped-risk/overflow/admin behaviors are separately tested, not modeled here.

## Third minute: measured decision

Open `results/summary/optimization.csv` and `PROVENANCE.md`. Compare direct risk
traversal with the same-binary snapshot reference. Inspect the paired ratios,
not only the best throughput number. Then read the negative conclusions on flat
price levels and small event buffers in `optimization-experiments.md`.
Core-only scope, synthetic inputs, historical source, clock quantization and host
scheduling limits are explicit. Full local raw data is retained separately.

## Fourth minute: lifecycle boundaries

Read `private-orders.md` and `journal-and-replay.md`. Open orders survive session
disconnect, while process restart does not restore a live book. Snapshots are not
an idempotency/terminal-outcome ledger. Synchronous journal ACKs wait for writes
and sync; failed journaling poisons the session rather than inventing success.
Per-symbol writers are isolated processes with separate histories and risk limits.

## Fifth minute: run it

After prerequisites/dependencies are installed:

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
python3 scripts/network_demo.py
```

The script launches real gateway, observer and trading processes, checks fills,
private maker delivery, graceful stop and journal replay. It uses ephemeral ports.
The full matrix/reproduction runner is:

```sh
python3 scripts/verify_release.py --clean-source
```

For the optional interface, build `ui`, run `npm --prefix ui start -- --symbols
APEX,ALPHA,BETA`, then use Trading desk. Adaptive agents are separate TCP clients.
The UI is a demonstration surface; it is not evidence of engine latency.

## Suggested review questions

- Which state changes before a command is admitted, and which can fail afterward?
- Why does replacement lose priority, and how is rejection atomicity checked?
- Where is queue capacity measured, and what may be discarded on disconnect?
- What does an ACK prove in journal and non-journal modes?
- Why keep a mutex queue and single writer? Which measurement would change that?
- How would private snapshots differ from durable retry deduplication?
- Which conclusions can be drawn from five paired timing blocks—and which cannot?
