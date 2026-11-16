# Changelog

## 0.5.0 — independently validated portfolio release

- Added a test-only vector/sort matching model with 32 seeded streams (32,000
  input commands), all-policy comparisons and quantity-conservation checks.
- Added a hand-calculated FIFO/replacement/self-trade reference scenario.
- Re-audited 75 historical experiment bundles and 7.5 million raw samples; included
  compact results, per-artifact hashes and the original measured source snapshot.
- Added portable evidence integrity checks, source-archive coverage, readable
  reviewer documentation, separate role-specific resume entries and interview exercises.
- Reorganized the README around correctness, ownership, measured scope and reproduction.
- No new trading strategy, external connectivity or hiring outcome is claimed.


## 0.4.0 — private open-order recovery

- Added bounded, owner-only TCP open-order snapshots (request 8/response 106)
  without changing existing v1 framing, journal records or matching semantics.
- Restore browser open orders before enabling submissions; added private refresh
  and native ORDERS command, including remaining quantities after offline fills.
- Preserved symbol/participant isolation and live report ordering after recovery.
- Aligned browser handling of zero order IDs with the existing native contract.
- Added maximum-frame, privacy, malformed-state, reconnect, symbol and replay checks.
- Terminal outcome history, automatic retries and crash recovery remain separate work.


## 0.3.0 — live symbol partitions

- Added 1-8 isolated C++ gateway processes with independent symbol books, risk
  state, sequences and optional journals.
- Added version-2 browser market discovery/routing and an explicit symbol selector.
- Bound trading and observer sessions to immutable symbols, with old-feed guards.
- Added per-symbol journal directories/manifests, independent replay, coordinated
  startup/shutdown and healthy-book operation after another partition exits.
- Preserved the default APEX single-book workflow and native TCP/journal v1 formats.
- Added isolation, failure-cleanup, named agent and replay tests.


## 0.2.0 — responsive local simulator

- Added browser limit/market/cancel/replace order entry, identified persistent
  sessions, real engine execution activity and actionable business rejections.
- Added adaptive participants reacting to live depth and fill-derived inventory;
  default demo no longer follows the scripted four-order sequence.
- Shared observer upstream across dashboards, with bounded fanout and snapshots.
- Kept the exact walkthrough and seeded correctness workloads as explicit tools.
- Added integration checks for browser trading, shared feed limits, adaptive
  cleanup and replay, plus a concrete scaling plan.


## 0.1.0 — local release candidate

- C++20 price-time matching with integer ticks, FIFO priority, market/limit orders,
  cancellation, replacement, partial fills and self-trade prevention.
- Participant risk limits, admission controls and trading kill switches.
- Versioned, checksummed, sealed journals and deterministic event/state replay.
- Seeded workload generation, saved inputs, core benchmarks and paired policy comparisons.
- Loopback TCP gateway, interactive clients, execution reports and sequenced market data.
- Bounded queue/output policies, connection deadlines, thread-safe metrics and stress checks.
- React/TypeScript observer dashboard, separate WebSocket bridge, gap recovery,
  stale-feed status and persistent dark/cream light themes.
- Opt-in live simulator activity, source-archive reproduction, release evidence
  packaging and checksum verification.

The candidate is local. No public release/tag or hosted CI success is claimed.
External market feeds, real broker orders, multi-symbol network routing, durable
live recovery and production latency guarantees are outside this release.
