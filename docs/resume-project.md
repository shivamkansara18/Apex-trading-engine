# Resume-ready project sections

These are project entries, not an invented full resume. Add your real dates,
repository/demo link and broader experience separately. Use only bullets you can
explain and reproduce. The two versions emphasize different evidence; they do not
change what was built. There is no interview/offer guarantee.

## Quant developer / trading systems

**Apex — C++20 Exchange Simulator** | C++20, TCP, concurrency, profiling, Python

- Built a deterministic price-time matching engine with FIFO levels, indexed
  cancellation, pre-trade risk checks, bounded nonblocking TCP sessions and
  checksummed journal/replay; isolated live symbol books with a separate writer process per book.
- Profiled risk validation and removed owning-book snapshot construction;
  measured **1.95–2.19× median paired core throughput gains** across three saved
  100k-command inputs on Apple M5, with matching canonical outcomes across policies.
- Validated **32,000 seeded commands against an independent linear reference**,
  checking trades, rejections, priority, quantity conservation and settled state
  after every input; tested eight engine policy configurations and native sanitizers.

If space permits, replace the first bullet with a more networking-focused variant:

- Implemented explicit endian TCP framing, partial I/O, queue/output backpressure
  and private open-order resynchronization; tested concurrent clients, slow
  consumers, per-symbol isolation and sealed replay after disconnects.

## Quant researcher / research engineering

**Apex — Market Microstructure Simulation & Experiment Framework** | C++20, Python

- Built a deterministic limit-order-book simulator with seeded synthetic order
  flow, price/FIFO execution, participant risk controls and replayable event/state
  hashes, enabling controlled comparisons on identical inputs.
- Designed and audited **75 interleaved paired benchmark runs** across three
  100k-command workloads and five implementation policies, retaining **7.5 million
  raw latency samples**, correctness signatures, host/build metadata and source hashes.
- Compared tail behavior and paired throughput; retained direct risk traversal
  (1.95–2.19× median paired core throughput ratios) while documenting shallow-book
  limitations and rejecting smaller event buffers after large-fill tail regressions.

This version demonstrates simulation, reproducible computational experiments and
validation. It does not demonstrate alpha discovery, predictive modeling,
statistical arbitrage or profitable execution. For a pure quantitative-research
application, complement it with separately defensible probability/statistics and
real-data research work; don't relabel systems timing as investment performance.

## Compact one-line version

Built a C++20 exchange simulator with deterministic replay and an independent
32k-command correctness oracle; measured 1.95–2.19× paired core throughput gains
across three controlled workloads (Apple M5, synthetic inputs).

## Claim-to-evidence map

| Claim | Audit path |
| --- | --- |
| C++ engine / indexed book / FIFO | `src/matching`, `src/book`, `include/apex/book` |
| 32k independent inputs | `tests/reference/linear_matcher.hpp`, `tests/unit/differential_test.cpp` |
| 1.95–2.19× paired ratios | `results/summary/optimization.csv`, methodology/provenance |
| 75 runs / 7.5M samples | `results/summary/optimization.json`, retained raw bundles and exporter |
| eight symbol partitions | `ui/bridge/run.mjs`, live partition integration tests |
| replay / failures / sanitizers | full `verification.json` and logs in the local release evidence |
| private recovery | `src/network/protocol.cpp`, gateway controls, browser/native integration checks |

## Wording to avoid

Do not claim network nanosecond latency, a production matching engine, lock-free
execution, linear eight-core speedup, live exchange connectivity, profitable agents,
zero-copy behavior or statistically significant strategy returns. None is established.
The measured optimization is historical and core-only; current binaries and other
hosts need their own measurement. Do not call the synthetic agents trained models.

## Before you submit

Be ready to derive the book operations and their complexities, trace a partial
fill/replacement by hand, describe the measured timing boundary and defend why
risk traversal changed while price storage/event reservation stayed conservative.
You should be able to implement a small FIFO matcher and reason about its tests
without reading a prepared answer. The interview guide has specific exercises.
