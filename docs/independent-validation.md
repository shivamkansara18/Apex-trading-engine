# Independent model-based validation

The policy differential tests compare configurations of the same production
matching implementation. That catches storage/policy regressions, but a shared
business-logic bug could agree across every policy. The test-only linear model
adds an independently implemented check with different storage and traversal.

`tests/reference/linear_matcher.hpp` owns an unsorted vector. For every fill it
rebuilds the eligible maker set, sorts by execution price and exchange priority,
then selects the best candidate. ID lookup/erase are linear. It does not call
OrderBook, MatchingEngine, RiskManager, production codecs or workload generation.
It reuses public command/order/trade/enum types so results can be compared without
an extra serialization layer.

32 seeds each generate 1000 mixed commands (32,000 input commands). Every stream
is checked against the production default and one alternate policy; seed bits
cover all eight tree/flat, snapshot/direct risk and event-reservation combinations,
four seeds per configuration. That is 64,000 production process calls per complete
model-test pass, plus the separate hand-calculated reference check.

After each input the test compares:

- Rejection reason and retained rejected command.
- Complete trade tuples: IDs, counterparties, resting maker, price and quantity.
- Cancellation ID/reason/remaining quantity, including replacement and self-trade.
- Ordered live orders, including original/remaining size, owner, status and priority.
- Per-price aggregate quantities and per-participant open-order/buy/sell usage.
- Exchange event/command sequence continuity and the production book invariants.
- Full intermediate production event vectors between default and alternate policies.

A separate conservation equation checks each production transition:

```text
book_quantity_after = book_quantity_before + admitted_incoming_quantity
                      - 2 * executed_quantity - cancelled_remaining_quantity
```

Replacement cancellation releases the old remaining size; desired replacement
size counts as a new admission. Each trade removes one quantity from each side.
Self-trade and market remainders are cancelled rather than left in the book.
Rejected commands admit zero quantity. Generated sizes keep these independent
sums well below overflow limits.

The hand-calculated scenario verifies FIFO after replacement priority loss,
resting-price execution, partial remaining size, self-trade prevention and market
remainder cancellation without using production matching for its expectations.

## Scope and reproduction

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/apex_tests --gtest_filter='Seeds/IndependentMatcher.*:LinearReference.*'
# Reproduce one stream:
./build/debug/apex_tests --gtest_filter='*/Seed17'
```

A failed assertion prints seed, step and the input command. The PRNG is mt19937
and the generator uses modulo selection, so a seed deterministically regenerates
the stream without depending on the engine's outputs. Target selection uses the
reference model's live state. IDs come from a small range to exercise collisions,
terminal reuse and zero; generated requests include valid/invalid enums, missing
orders, wrong owners, invalid replacements and unknown participants.

This model is deliberately bounded: default/unrestrictive risk limits, four known
participants, small quantities and prices, 128 reusable IDs, no runtime admin changes.
It does not independently model notional/quantity caps, price-band changes,
kill switches, numeric overflow, journaling faults or network lifecycle. Dedicated
production unit/integration tests cover those features. The oracle compares
projected business outcomes and settled state; it does not independently predict
every intermediate Accepted/Updated report. No formal proof or exhaustive state
coverage is claimed. Independence of storage/traversal reduces, but cannot eliminate,
shared specification mistakes.
