# Implementation roadmap

Each phase must satisfy its gate before advancing. The guide's suggested ten-week
schedule is a planning aid, not a deadline or evidence of completion.

macOS arm64 is the primary development/test environment per user direction.
Hosted Linux CI and a remote clean-clone check remain release verification tasks;
they do not block local domain/book implementation. Linux performance evaluation
is optional. Every reported measurement must name its actual platform.

| Phase | Deliverable | Exit gate |
| --- | --- | --- |
| 0 | CMake skeleton, smoke tests, CI, initial docs | Clean source builds/runs/tests; remote CI passes after push |
| 1 | Domain commands, orders, trades, events, snapshots | Types compile; policies unambiguous |
| 2 | Map/list book, ID index, read queries | Insert/cancel/depth tests and invariants pass |
| 3 | Limit/market/cancel/replace matching | FIFO, price priority, partial fill scenarios pass |
| 4 | Participants, validation, limits, reject events | Invalid commands leave book unchanged |
| 5 | Versioned journal and deterministic replay | Repeated replay outputs and final hashes agree; corruption detected |
| 6 | Seeded simulator and saved workloads | Same seed gives identical command streams |
| 7 | Throughput/latency harness and metadata | One command exports reproducible machine-readable results |
| 8 | Profile and compare alternatives | At least three measured experiments with trade-offs |
| 9 | TCP framing, reports and market data | Multiple clients feed one ordered engine stream |
| 10 | Bounded queue, backpressure, stress tests | No lost/duplicated commands, races or invariant failures |
| 11 (optional) | UI via separate WebSocket bridge | Snapshots, updates, gap recovery |
| 12 | Reports, demo, sanitizer/replay/stress evidence | Another person reproduces build, tests, replay and benchmarks |

MVP is phases 0-3 plus the validation needed for safe order handling. The main
portfolio deliverable includes risk, replay and measured baselines. Networking
and concurrency follow that foundation. The UI can be omitted.
