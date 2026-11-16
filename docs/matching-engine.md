# Matching engine (Phase 3)

`MatchingEngine::process(Command)` synchronously returns an owning vector of
events. The engine owns its book and command/event/trade counters. It uses no
wall clock, client timestamp priority, network or disk I/O. All calls and queries
must run on one writer thread. Public book access is const.

## Matching and lifecycle

Incoming buys consume the lowest eligible ask; sells consume the highest
eligible bid. The oldest order at that price executes first. Each execution uses
the resting price, decreases both remaining quantities, preserves the maker's
priority, and removes a filled maker from the index and level. Limit residuals
rest at the incoming limit price. Market residuals cancel and never rest.

On reaching an eligible order of the same participant, the incoming remainder
cancels with `SelfTradePrevention`. Earlier trades stand, the own resting order
is untouched, and the engine does not skip to worse prices.

Cancel checks existence and ownership and reports the cancelled remaining
quantity. Replace checks all new fields and capacity before removing the old
order. Success cancels the old ID with `Replaced`, accepts a distinct new ID,
resets priority even for quantity reductions, and immediately matches if eligible.
New quantity is desired remaining quantity, independent of previous fills.

## Exact output order

1. New order: `OrderAccepted` with status `Accepted`.
2. For each execution: `Trade`, resting `OrderUpdated`, incoming `OrderUpdated`.
3. Positive limit residual: insert, then emit `OrderUpdated` (Resting or
   PartiallyFilled). After a partial fill this can repeat the latest incoming
   quantity to explicitly mark completion of processing and placement.
4. Positive market or self-trade residual: `OrderCancelled` with its reason.
5. Filled incoming orders need no additional terminal event beyond their last
   Filled update. Cancel requests emit just `OrderCancelled`.
6. Replace emits the old `OrderCancelled` then the complete new-order sequence.
7. Invalid commands emit one `CommandRejected`, containing the original request.

Every processed command, including a rejection, advances the command counter.
Each emitted event has its own increasing sequence and its causal command
sequence. Trade IDs advance only for trades. All counters start at one. Snapshot
sequence changes on rejects, but bid/ask state does not.

## Validation and capacity boundaries

The current engine rejects invalid enums, zero quantity, nonpositive limit price,
duplicate live IDs, wrong-owner/unknown cancels and invalid replacements.
Market prices are ignored and normalized to zero on order records.

Limit admission conservatively requires room for the *entire* requested quantity
in the side's uint64 total, even if it might execute immediately. A replacement
first credits its own old remaining quantity. This explicit capacity policy
prevents overflow from causing rejection after earlier trades have occurred.
It can reject an otherwise executable order at extreme uint64 capacity.

Before mutation, the engine checks sequence capacity against worst-case events
and trades, and reserves output space (at most 3 * existing orders + 4 events).
Capacity exhaustion throws `std::overflow_error` without applying the command;
this upper-bound check may stop before the last counter value. The output reserve
is a correctness baseline, not a memory or performance optimization.

An unexpected logic error or allocation failure while processing is fatal for
this in-memory session: callers must stop using it. There is no transactional
rollback or crash recovery guarantee. Phase 5 adds strict replay of completed
journals through a separate wrapper; see [journal and replay](journal-and-replay.md).

Phase 4 now requires registered participants and applies configurable
quantity/notional/open-order limits, bands and kill switches before matching.
See [risk controls](risk-controls.md). Phase 5 adds journaling/replay and Phase 9
adds a loopback gateway outside matching. The graphical dashboard is a later phase.

## Validation evidence

Scenario tests cover the guide's FIFO example, price priority in both directions,
partial and multi-level fills, limit price boundaries, market cancellation,
self-trade handling, cancels and replacements, rejected mutations and quantity
capacity. A fixed-seed 1,500-command stream compares all events and snapshots
between two fresh engines and checks uncrossed invariants after every command.
This tests determinism; it is not an independent matching reference model or
file-backed replay. Storage tests separately cover invalid and complete fills.

Run `./build/debug/apex --trading-demo` to see orders, trades, replacement,
market cancellation and the book after each command. Prices are integer ticks.
