# Order-book storage (Phase 2)

`apex::OrderBook` stores one symbol in ascending `std::map` price levels for each
side. Best bid uses the reverse begin iterator; best ask uses begin. Each level
owns a FIFO `std::list<Order>`. An unordered ID index stores side, price and list
iterator, permitting removal without scanning the price-level queue.

Insert checks storage prerequisites: positive limit price and remaining quantity,
valid side, live status, remaining <= original quantity, nonzero priority,
unique live ID, increasing FIFO priority at that level, and no side-total
overflow. It does not perform participant risk checks or match crossing prices.
Invalid operations return `BookError` without mutation; allocation failures throw
after rollback. Full validation belongs to the future engine/risk layer.

Cancel verifies ownership, updates cached totals, removes the indexed list entry,
and erases an empty price level. Terminal IDs may then be reused. This storage
operation emits no lifecycle events; the matching layer will emit them.

`find`, `snapshot`, and `top_levels` return owning copies. Best price queries are
optional on an empty book. Depth and totals are per side; invalid enum query
values return zero/empty results. Snapshots sort bids descending and asks
ascending, keeping FIFO order within each level. Their command sequence is
supplied by the future engine, rather than invented by storage.

`check_invariants()` checks positive quantities, price/side membership, FIFO
sequences, index/list consistency, uniqueness, cached totals and no empty levels.
`check_invariants(true)` additionally rejects a locked or crossed book; storage
alone allows crossing because matching will own execution decisions. Invariant
scans are explicit debug/test checks, not inserted into the hot path.

The object cannot be copied or moved because its index contains owned-list
iterators. It has no thread synchronization: one writer must own it. Queries are
also confined to that writer until snapshots are transferred safely to consumers.

Phase 3 adds `best_order(side)` (copy of the FIFO head at the best price) and
`fill(id, quantity)`. Fill rejects zero/oversized/unknown executions without
mutation, decreases totals, preserves partial order priority, and removes filled
orders and empty levels. It returns the post-fill order, even when removed.

Insert is O(log P) plus average O(1) index insertion. Cancellation includes
O(log P) price-level lookup and O(1) list removal. Best price and cached side
totals are O(1). Snapshots copy every order in the selected levels.

Tests cover ordered queries/FIFO, middle cancellation, empty levels, rejected
mutations, overflow, owning snapshots, crossed detection, and a fixed-seed
2,000-command insert/cancel stream with independent order and total checks.

Run a visible storage demonstration with `./build/debug/apex --book-demo`.

Phase 8 keeps tree/list storage as the default and adds an opt-in flat sorted
price-level index with stable FIFO list nodes. `visit_orders(side, callback)`
performs allocation-free const traversal in price/FIFO order on the owning
thread; false stops the scan. Never retain callback references. Public snapshots
remain copies. See [experiments](optimization-experiments.md) for complexity,
measurements and why the flat index is not the general-purpose default.
