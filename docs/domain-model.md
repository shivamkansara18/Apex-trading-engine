# Domain model

Phase 1 defines in-memory value types. They do not validate or execute requests,
assign sequences, or enforce book invariants yet. Invalid inputs remain
representable so the future validator can report deterministic rejection reasons.
These structures are not a binary wire format: never serialize their raw memory.

## Public headers

- `core/types.hpp`: fixed-width integer units, sides, lifecycle and reason enums.
- `core/command.hpp`: new/cancel/replace requests and the `Command` variant.
- `core/order.hpp`: order state and trades with explicit counterparties.
- `core/event.hpp`: accepted/updated/cancelled/rejected/trade payloads, wrapped in
  an `Event` carrying causal command and unique event sequences.
- `book/snapshot.hpp`: owning copies of ordered price levels and FIFO orders.

All types live in `apex`. Price is signed 64-bit ticks, quantities unsigned
64-bit units. A tick's monetary size is instrument configuration, not floating
point core state. ID aliases clarify usage but are not distinct wrapper types.
Zero IDs are allowed; zero quantities and nonpositive limit prices will be
rejected by validation. Sequence and trade counters start at one; zero denotes
unassigned/no commands processed. Counters must never wrap.

## Lifecycle and event contract

An accepted incoming order transitions from `PendingValidation` to `Accepted`
until matching determines its next state.
`OrderAccepted` reports that validated order before matching; it does not imply
that a market order was inserted in the book. Filled quantities transition to
`PartiallyFilled` or `Filled`. An unfilled resting limit remains `Resting`.
Only live limit orders appear in snapshots; market orders never rest.

`original_quantity` is the accepted quantity for that ID. `remaining_quantity`
decreases on execution; on cancellation it retains the unexecuted cancelled
quantity for audit. A partially filled resting order retains its original
priority sequence. Client timestamps stay on requests and never set priority.

Each processed command has one causal sequence, including rejects; a command
may emit several events with distinct increasing event sequences. A trade has
one quantity shared by buy and sell, both participant IDs, both order IDs, and
the resting order ID. Trade IDs are independent increasing exchange counters.

Successful replace emits cancellation of the old order with reason `Replaced`,
then acceptance of the new order, followed by matching outputs under the same
command sequence. Failed replace emits one rejection containing the whole
request and preserves the old order. Rejections are command-level; no live
`Order` is created for an invalid request. Exact fill/update ordering is defined
in [matching engine](matching-engine.md) and tested in Phase 3.

Snapshots carry the last processed command sequence, including rejected commands.
Their copied vectors give callers no access to engine-owned state. Snapshot
equality is structural. Phase 5 adds canonical hashing and serialization in
[journal and replay](journal-and-replay.md). Aggregate totals must fit `Quantity`;
mutations check overflow before changing state.
