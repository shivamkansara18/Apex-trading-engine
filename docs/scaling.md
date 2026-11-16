# Responsive simulation and scaling

## What is computed and what is scripted?

The C++ engine has always computed matches from actual incoming commands, prices,
FIFO priority, quantities and risk state. It does not randomly decide who wins
or return prewritten fills. The original terminal demonstration was a scripted
walkthrough; saved benchmarks use seeded workload generators so comparisons can
reproduce the same inputs. Both are useful correctness tools, not market data.

Version 0.2.0 adds manual browser trading and adaptive live participants. These
submit commands through the same gateway and receive canonical engine events.
The dashboard has no fabricated trades or locally predicted fills.

## Interactive trading

Start `npm --prefix ui start`, open http://127.0.0.1:8080, and use Trading desk:

1. Connect participant 1 (IDs 1-8 are convenient for manual trading).
2. Choose Buy/Sell and Limit/Market, then enter integer price ticks and quantity.
3. Submit. Acceptance, rejection and execution activity come from C++.
4. Select an observed open order, or enter its ID, to cancel or replace it.
5. Open another browser tab with a different participant to trade against it.

Only one writable session can own each participant. Browser identity is fixed at
connect, and payloads cannot substitute another participant. Replacement uses a
new order ID and desired remaining quantity, with the existing engine priority
policy. The order list is restored from an authoritative private snapshot on reconnect
and updated by execution reports. Missed trade history is not reconstructed;
see [private recovery](private-orders.md). Never automatically retry a
command whose completion was lost: its execution outcome may be unknown.

The bridge accepts one outstanding browser command at a time per trader and caps
persistent TCP pending commands at 16. Malformed input is reported without writing
a business command; business validation/rejections still belong to C++. This is
local simulator identity, not account authentication or an exchange/broker account.

## Adaptive participants

With the stack running, use another terminal:

```sh
npm --prefix ui run demo
# Shorter repeatable-policy run:
npm --prefix ui run demo -- --steps 100 --seed 42 --interval-ms 250
```

Participants 9 and 10 provide liquidity; 11 and 12 take liquidity. Keep these IDs
free. Each provider maintains at most one tracked bid and ask, chooses whether to
replace from seeded randomness, and calculates quotes from the current best
prices and its own fill-derived inventory. Takers inspect available liquidity
before choosing a market order. Sides, sizes, spreads, repricing and fills vary
with seed and current state. A human order at a different price changes the book
input and therefore the subsequent agent decisions.

Options include `--gateway-port`, `--steps` (1..100000), `--interval-ms` (0..5000),
`--seed`, `--price` (empty-book reference), `--spread` and `--max-quantity`.
Defaults are 1000 steps, 250 ms between steps, a random seed, initial reference
10000 ticks, spread 5 and max size 50. The reference is a configurable initial
condition, not an external market price. The seed is printed. A fixed seed only
reproduces policy choices with the same observed inputs; concurrent human traffic
changes those inputs. The journal captures the actual accepted command order.

Sessions are persistent; the driver does not spawn a native process for each
order. It waits for real ACKs and reports, tracks inventory from trades, and counts
actual accepts/rejects. Ctrl-C finishes the current iteration and attempts to cancel
only its tracked quotes. Normal completion also removes agent quotes. Human orders
are preserved unless they legitimately trade. Network failure can prevent cleanup;
resting orders then persist by the normal gateway contract. Inventory skew is a
model input, not an account balance ledger or a guaranteed position limit.

This is a responsive synthetic model, not a statistically calibrated model of a
real market. Randomness alone does not establish financial realism. The separate
`npm --prefix ui run walkthrough` retains the original four-order scripted example
for exact demonstrations (`-- --rounds 3` works there).

## Live symbol partitions (0.3.0)

Live symbol routing is now implemented with separate C++ gateway processes, each
owning a single book and writer. Start `npm --prefix ui start -- --symbols APEX,ALPHA,BETA`
and use the dashboard selector. Feed state, trading identities and journals are
isolated per symbol. [Multi-symbol documentation](multi-symbol.md) defines the
versioned browser routing schema, compatible per-port TCP transport, replay,
per-symbol risk scope and failure boundaries.

## Scaling work implemented

- Dashboard observers share one upstream observer TCP session per symbol per bridge.
  Trade/book bytes decode once and fan out to subscribers. A new/recovering viewer
  requests a fresh anchor broadcast to the observer group. Independent trading
  identities keep their own TCP sessions.
- The bridge caps browser WebSocket sessions at 32 (observer and trader sockets
  both count), inbound frames at 4 KiB and pending output per viewer at 1 MiB.
  Slow viewers terminate independently. Snapshot requests coalesce globally to
  one per 250 ms. C++ matching remains single-writer with its existing bounds.
- GUI lists and trade retention remain bounded. Increasing viewer capacity does
  not introduce competing writers to a single order book.

Tests verify several observers use one upstream, capacity rejects excess viewers,
and remaining viewers still receive anchors after a disconnect. Other integration
tests verify real browser partial fills, cancel/replace, business rejections,
identity binding, adaptive cleanup and sealed journal replay. This validates
behavior at tested sizes; it is not a throughput or production-capacity claim.

## Next scaling steps, in order

1. Measure gateway command-to-ACK and feed latency under many persistent clients,
   including slow readers. Current published benchmarks cover core matching only.
   Profile queue, serialization, fanout and synchronous journal costs separately.
2. Live symbol partitioning is implemented in 0.3.0 as processes with independent
   v1 TCP endpoints, journals and writers, plus browser routing schema v2. Measure
   cross-book concurrency before claiming speedup; any future multiplexed native
   transport needs an explicit symbol-bearing protocol version.
3. Private open-order snapshots are implemented in 0.4.0. Durable idempotency,
   terminal outcome/history queries and crash recovery remain necessary before
   transparent command resubmission. Public depth alone cannot recover private state.
4. Consider batched feed serialization and paced snapshots only after measuring
   fanout cost. Decouple durable logging with explicit commit semantics if fsync is
   the bottleneck; do not silently weaken the ACK contract.
5. A public deployment needs authentication, encrypted transport, monitoring,
   crash recovery and operational limits. External exchange data needs a separate
   provider adapter and schema, with source timestamps and clear stale-data behavior.
   Neither external data nor real-money orders are part of this local-only version.

Do not add a lock-free queue or multiple writers to one book solely to claim scale.
The existing paired core measurements remain valid for their original versions;
the new networking changes require their own measurements before performance claims.
