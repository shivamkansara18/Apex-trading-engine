# Matching rules (design for subsequent phases)

These semantics are implemented by the Phase 3 engine. See
[matching engine](matching-engine.md) for event ordering and capacity policy.
Phase 4 adds [participant risk limits](risk-controls.md).

- One symbol initially. Prices are signed 64-bit integer ticks; valid limit prices
  are positive. Quantities and order IDs are unsigned 64-bit integers; quantities
  must be positive. Participant IDs are unsigned 32-bit integers.
- Bids sort highest first, asks lowest first. Within a price, resting orders
  execute FIFO in exchange acceptance order. Client timestamps are informational.
- Each processed command receives an increasing exchange command sequence,
  including rejections for auditability. An accepted new/replacement order uses
  that sequence for priority. Events use a separate monotonic event sequence.
  Neither sequence may wrap; exhaustion must fail explicitly.
- A limit buy crosses asks at or below its price; a limit sell crosses bids at or
  above its price. Executions use the resting order's price. Partial fills consume
  the earliest resting order before moving to the next order or price level.
- Unfilled limit quantity rests. Market orders ignore their supplied price, consume
  available opposite liquidity, and cancel any residual; they never rest.
- Self-trade prevention: cancel the incoming remainder when it reaches an eligible
  resting order owned by the same participant. Earlier fills stand. The resting
  order remains. Emit an explicit self-trade cancellation reason. Do not skip that
  resting order to trade at a worse price. This avoids a partial-fill-then-reject
  ambiguity and will be covered by scenario tests.
- Live IDs are unique exchange-wide. Reuse of a terminal ID is allowed initially;
  durable event identity uses exchange sequences. Safe network retry handling
  must be specified separately before the gateway is added.
- Cancel requires a live ID and matching owner. Unknown IDs and wrong owners are
  rejected without book mutation.
- Replace applies only to a live limit order. Validate ownership, a distinct new
  ID, price, quantity, and limits before removing the old order. A failed replace
  leaves the old order intact. Every successful replace resets time priority,
  including quantity reductions, and may immediately match. New quantity means
  desired remaining quantity, excluding previous fills.
- Rejections leave book state unchanged (audit sequences may advance). Check
  arithmetic boundaries before multiplication or accumulation.
- The settled book must have positive resting quantities, no empty levels,
  consistent totals/indexes, and best bid strictly below best ask.

Example: A buys 100 at 10,000 ticks, B buys 50 at 10,000, C sells 120 at 9,900.
C trades 100 with A and 20 with B, both at 10,000; B rests with 30.
