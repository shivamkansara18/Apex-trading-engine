# Participant risk controls (Phase 4)

All new, cancel and replace commands require a registered participant. Engine
construction starts with an empty registry. Register participants explicitly:

```cpp
apex::MatchingEngine engine;
apex::RiskLimits limits;
limits.max_order_quantity = 100;
limits.max_order_notional = 10'000; // ticks * quantity, not dollars
limits.max_open_orders = 5;
limits.min_price_ticks = 90;
limits.max_price_ticks = 110;
if (!engine.register_participant(1, limits)) {
    // ID was already registered; existing limits are unchanged.
}
```

Default registered limits allow the full representable range, with a positive
price band. Registration is mandatory even when using defaults. Zero quantity,
notional or open-order limits are allowed and restrict admission. Invalid price
bands throw `std::invalid_argument` without changing configuration. Registering
an existing ID returns false; configuring an unknown ID returns false.

## Admission policy

- Validate side/type, positive quantity, positive limit price, live-ID uniqueness
  and side-total capacity first. Unknown participant, disabled trading, size,
  band, arithmetic, notional and open-order checks follow for new orders.
- Maximum order quantity applies to the whole submitted size for both order
  types. It also applies to replacement desired remaining quantity.
- Maximum order notional is a **per-order** limit in integer tick-units. It is
  not a portfolio position, credit limit, currency value, or aggregate exposure.
- For limits, notional is limit price * entire requested quantity. Division
  checks for uint64 multiplication overflow before multiplication. Limit price
  must be inside the participant's inclusive configured band. Bands constrain
  limit admission prices, not the execution price improvement of limit trades.
- For markets, supplied price is ignored. A deterministic preflight walks actual
  opposite liquidity in price/FIFO order, stopping when the requested quantity
  is filled or own liquidity is reached. Sum executable price * quantity using
  checked arithmetic. Every reachable execution price must be within the taker's
  band. Excess notional, overflow or a band violation rejects the entire request
  **before any fill**, including earlier eligible levels. Unreachable liquidity
  and unavailable quantity do not count toward market notional.
- Open orders count all live orders for the participant across both sides.
  Conservatively reserve one slot for every incoming limit, even if it might
  fully execute. Market orders need no slot because they cannot rest.
- Replace validates ownership, distinct ID and all new fields/limits before
  cancelling the old order. Credit one existing open-order slot and the old
  remaining quantity for side capacity. Invalid replacements keep old priority,
  quantity and book placement. Limit changes below existing usage do not
  grandfather replacements; they must satisfy the new admission limits.
- Rejections emit one explicit `CommandRejected`. Bid/ask contents and risk
  usage stay unchanged; command/event audit counters still advance.

## Kill switches and configuration

`engine.set_trading_enabled(false)` blocks new orders and replacements globally.
Set a participant's `RiskLimits::trading_enabled` false through
`configure_participant` to block that participant. Cancels remain allowed.
Resting orders are preserved and remain executable by enabled counterparties.
A kill switch does not implicitly cancel outstanding orders or freeze them.

Configuration changes and registration are synchronous single-writer admin calls.
They do not emit trading events or advance exchange command sequences.
Reproducing a run requires the same initial configuration and admin transitions
at the same command boundaries. Phase 5's `JournaledEngine` records these inputs
and their return values; use that wrapper for recorded runs. See
[journal and replay](journal-and-replay.md) for persistence boundaries.

## Usage and implementation boundaries

`risk_usage(id)` returns open-order count and remaining buy/sell quantities.
It derives usage from the authoritative book rather than a separately maintained
risk ledger. Consequently fills, cancels, self-trade handling and replacements
cannot leave stale reservations. Phase 8's selected direct traversal scans without
copying/allocating snapshots; usage remains O(number of live orders). Market
preflight reads the opposite book in place and can stop early. A snapshot-copy
policy remains selectable for measured reference comparisons. See
[optimization experiments](optimization-experiments.md).

There is no participant deletion, account balance, short-sale policy, position
limit, margin model, authentication, rate limit or real exchange connection.
Unexpected allocation/logic failures remain fatal to the current in-memory
session; rejected commands have the documented no-book-mutation guarantee.

Tests cover exact limits, invalid configuration, overflow in individual and
summed products, market sweeps on both sides, replacement credit, usage after
partial/full fills and cancels, kill switches, and a seeded 1,200-command stream
checking all rejection snapshots and independent usage accounting.

Run `./build/debug/apex --risk-demo` for readable rejection reasons and usage.
