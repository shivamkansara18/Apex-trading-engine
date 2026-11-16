# Private open-order recovery (0.4.0)

The exchange preserves resting orders when a client disconnects. Version 0.4.0
adds an authoritative private snapshot so reconnecting traders can recover those
orders, including their current remaining quantities after fills while offline.
This is current-state synchronization, not replaying commands or recovering a
crashed exchange process.

## Use it

Rebuild both native binaries and UI, stop the older dashboard, then restart:

```sh
cmake --preset debug
cmake --build --preset debug
npm --prefix ui run build
npm --prefix ui start -- --symbols APEX,ALPHA,BETA
```

Select a symbol and connect participant 1-8 in Trading desk. After HELLO the bridge
requests that participant's open orders, forwards the snapshot and subsequent
live reports, then enables submissions. The UI shows Synchronizing until the
initial snapshot completes. The market selector is disabled while synchronizing
or waiting for a command. **Refresh my orders** repeats the authoritative read.

Close/disconnect the trading session, allow another trader to fill some of its
orders, then reconnect with the same participant and symbol. My open orders shows
the remaining state. Only live resting orders are returned; terminal records and command rejections
are not replayed as history.
Recovered IDs can be selected for cancel/replace, using the normal engine rules.
An ID of zero remains valid, matching the native domain contract.

From a separate native session (the participant must not already be connected):

```sh
./build/debug/apex_client --port SYMBOL_TCP_PORT --participant 1 --command "ORDERS"
```

Interactive `apex_client` also accepts `ORDERS`. The output includes participant,
command watermark, order IDs, side, price and remaining quantity. Use the symbol's
printed native port, or get it from the bridge's market discovery endpoint.

## Privacy, ordering and bounds

OpenOrdersRequest (TCP type 8) has an empty payload. The server derives ownership
only from the established HELLO identity. Observers cannot request private orders,
and including an arbitrary participant in a control payload is malformed. There
is no query for another participant's orders. Each symbol remains a separate
book/process, so the same participant ID on another symbol gets a different view.
These simulator identities are not production authentication.

OpenOrdersSnapshot (type 106) is correlated to its request sequence. Its payload is:

| Field | Encoding |
| --- | --- |
| Last processed engine command | u64 big endian |
| Bound participant | u32 big endian |
| Order count | u32 big endian, 0..1000 |
| Orders | count canonical 47-byte order records |

An order record carries ID, owner, side, type, price, original/remaining quantity,
priority and status in the same field layout as an execution report. Only live
limit orders with positive remaining quantity and Resting/PartiallyFilled status
are included. The snapshot traverses bids then asks in book order and contains no
other participants. The maximum 1000-order payload is 47,016 bytes and its framed
size is 47,032 bytes, below the existing 65,536-byte limit. Existing per-peer output
limits still apply; slow clients can disconnect rather than block matching.

The existing single writer captures the snapshot between processed commands.
It does not advance command/event/feed sequences, reset priority or mutate risk,
book or journal state. The watermark identifies the book state at capture time.
Reports for earlier activity precede the snapshot on that session's ordered TCP
output; later updates follow it. The browser replaces its map on the snapshot,
then applies live reports. It never merges a snapshot from another symbol or PID.

The Node session bounds pending requests at 16 and closes if synchronization times
out. The bridge serializes browser refresh/submission requests and waits for
initial recovery before executing even an early command from an older client.
The browser does not enable trading on HELLO alone. Discovery advertises
`capabilities.privateOrders: true`; an updated UI asks users to restart an older
bridge rather than staying indefinitely in an unsupported sync state.

This is an additive v1 TCP extension: old message IDs, framing and journal v1 are
unchanged. Existing clients need not request type 8 and receive no unsolicited
private snapshots. New recovery clients need the updated gateway; an older one
rejects an unknown request instead of returning fabricated empty state. Browser
routing schema v2 continues to identify the symbol independently of TCP framing.

## What is not recovered

- Missed execution history, terminal orders, historical balances and inventory.
- Whether an absent order filled, cancelled or was never accepted. A missing ACK
  can still leave an ambiguous outcome; snapshots are not an idempotency ledger.
- Durable exchange restart or automatic command retry. Old commands are never
  automatically resubmitted. Private trading reconnect remains an explicit user action.
- A portfolio-wide cross-symbol state. Risk and snapshots remain per book.

If the native book is restarted empty, an empty snapshot means the new process
has no open orders; it does not restore the old process. Journals still require
strict replay and are not a live crash-recovery mechanism.

## Verification

Native checks cover empty/maximum-size snapshots, all truncations of a sample,
foreign owners, duplicate IDs, malformed bounds and terminal entries. Real TCP
tests cover disconnects followed by partial and complete offline fills, owner-only
results, forbidden observer/spoof requests, control-query watermarks, cancellation
of restored orders and a 1000-order response spanning multiple write budgets.

Node integration checks restore a zero-ID order after an offline partial fill,
replace/cancel it, refresh state, hide it from a different participant, keep an
independent same-ID order on another symbol, exercise native ORDERS, and replay
both journals. Current full verification includes 189 CTest cases per native preset, 24
Node cases, packaging checks, source reproduction and raw benchmark auditing.
Browser visual capture is still unavailable on this Mac; new control layout and
manual interactions need ordinary-browser visual review.
