# TCP protocol v1 (Phase 9)

The gateway exposes one synthetic instrument on **IPv4 loopback only**. All fields
are encoded explicitly in big-endian order; signed prices use two's-complement
64-bit representation. No untrusted bytes are cast to C++ structs. Integer
prices are ticks and notional limits use tick-units, not a currency convention.

## Framing

| Header field | Width | Meaning |
| --- | ---: | --- |
| message length | uint32 | Total bytes including the 16-byte header |
| protocol version | uint16 | Exactly 1 |
| message type | uint16 | Table below |
| request sequence | uint64 | Request correlation; zero for unsolicited output |

Length must be 16..65,536 bytes. The incremental decoder accepts split headers,
split payloads and multiple frames per read, and checks the version/length before
reading a large payload. The server reads at most 16 KiB per peer per poll cycle.
Payload lengths and trailing bytes are checked by each request decoder.

Requests on each connection must have consecutive sequences starting at 1,
including control requests. Duplicate, skipped, zero or wrapped sequences are
protocol errors and close the session; they are not silently retried/reordered.

## Requests

| Type | Name | Payload |
| ---: | --- | --- |
| 1 | HELLO | uint32 participant ID |
| 2 | NEW | uint64 ID, uint32 participant, uint8 side, uint8 order type, int64 price, uint64 quantity, uint64 informational timestamp |
| 3 | CANCEL | uint64 ID, uint32 participant |
| 4 | REPLACE | uint64 old ID, uint64 new ID, uint32 participant, int64 new price, uint64 desired remaining quantity |
| 5 | SUBSCRIBE | Empty |
| 6 | SNAPSHOT_REQUEST | Empty |
| 7 | HEARTBEAT | Empty |

HELLO is mandatory first and identity is immutable. Configured participants are
1..N (default N=16); ID 0 is a read-only observer. A participant may have one
writable session at a time. Multiple observers are allowed. Every order request's
participant must match its session; observers cannot trade. Disconnected/write-
closed clients do not cancel resting orders.

This binds identity inside a local simulator session; it is **not authentication**.
There are no credentials, TLS, remote bind option or production access controls.

Side bytes are Buy=0/Sell=1, order type Limit=0/Market=1. Unknown side/type values
are preserved through decoding and produce ordinary engine rejection reasons.
Unknown protocol versions/message types, malformed payloads and identity/sequence
violations are transport errors. Replacements inherit side from the old order;
new quantity means desired remaining quantity and always resets priority.

## Responses and feed

| Type | Name | Payload |
| ---: | --- | --- |
| 101 | HELLO_ACK | uint32 bound participant |
| 102 | COMMAND_ACK | uint64 engine command sequence, uint32 event count, uint8 accepted boolean |
| 103 | EXECUTION_REPORT | One canonical engine event |
| 104 | SUBSCRIBE_ACK | uint64 current feed sequence |
| 105 | HEARTBEAT_ACK | Empty |
| 199 | ERROR | uint32 error code, remaining bytes bounded to 256-byte message |
| 201 | TRADE_UPDATE | uint64 feed sequence, one canonical Trade event |
| 202 | BOOK_SNAPSHOT | Format below |

The submitting connection receives COMMAND_ACK followed by the advertised number
of EXECUTION_REPORT frames with the same request sequence. Rejected business
commands have accepted=false and one CommandRejected event; the session remains
usable. Acceptance includes market-remainder/self-trade cancellations after
validation; consult the events for the final order state.

Other connected makers receive relevant Trade/order-state reports with request
sequence zero. Subscribers receive trade updates and book snapshots in addition
to any personal reports. These separate streams may interleave; correlate by
type and request sequence instead of assuming the next frame is an ACK.
Personal reports are filtered from the global event stream, so gaps in their
engine event sequences are normal. Gap tracking applies to the separate public
feed sequence, not to filtered personal events.

Execution-report payloads use the same explicit field encoding as journal v1:
uint64 event count (exactly 1), uint64 event sequence, uint64 causal command
sequence, uint8 event tag and its fields. Tags are 1=accepted, 2=updated,
3=cancelled, 4=rejected, 5=trade. Fields and enum values are documented in
[journal and replay](journal-and-replay.md). Rejected reports retain their
original command, including both replacement IDs. One-event framing keeps even
large multi-fill responses below the per-frame bound.

Book payloads encode uint64 feed sequence, uint64 last engine command sequence,
then bid and ask vectors. Each vector begins with uint32 level count; each level
contains int64 price, uint64 aggregate remaining quantity, uint32 order count.
Bids descend, asks ascend. No per-order snapshot data is copied into the feed.
Default depth is 10 per side, configurable in the C++ gateway config up to 64.

The publisher increments one global feed sequence for each trade, then once for
the top-depth snapshot after every processed command, including rejections.
It sends the same publication sequence to all current subscribers. Full top-depth
snapshots are the baseline book-update strategy; book deltas/UDP are deferred.

SUBSCRIBE returns an ACK and correlated snapshot at the **current** sequence.
Explicit SNAPSHOT_REQUEST also returns an anchor at the current sequence without
incrementing publication order. The client resets gap tracking on these anchors;
subsequent unsolicited updates should increment by one. `--watch` detects gaps,
requests a new snapshot, and checks heartbeats during idle periods. TCP preserves
bytes while connected; reconnection requires a new HELLO/subscription/anchor.
Feed counters restart with a new server process and are not journal-recovered.

Error codes: 1 malformed, 2 sequence, 3 identity, 4 unknown message, 5 inbox
overload, 6 participant session in use. Protocol errors flush a bounded error
response then close. Connection-limit and slow-output disconnects may occur
without a complete ERROR frame. Never interpret disconnect alone as rejection.

## Ownership, ordering and bounds

A single poll/event-loop writer owns every session, the bounded FIFO inbox and
the matching engine. The constructor initializes participants before handing
ownership to that writer. Concurrent clients feed OS socket buffers; no socket
worker, subscriber or client mutates book memory.

Within each TCP connection, decoded frames retain order. Across peers, the poll
loop reads ready connections in monotonically assigned connection-ID order and
enqueues frames; arrival/read interleaving determines the global processing
order. The engine's command sequence is authoritative, not client time. Replaying
the recorded accepted order is deterministic; rerunning concurrent clients is
not promised to reproduce the same arrival order.

Defaults: 64 connections, 256 queued requests globally, 1 MiB pending output per
connection, 16 KiB read/write budgets, and at most 64 inbox items processed per
iteration. Limits are configurable through GatewayConfig and validated before
bind. There is no unbounded network queue. A full inbox produces overload and
closes the submitting connection, discarding its unprocessed requests. A slow
peer whose output exceeds its cap is disconnected while others continue. The
caps count payload/wire bytes, not all allocator/container overhead.

Default participant risk limits are maximum quantity 1,000,000, per-order
notional 1,000,000,000,000 tick-units, 1,000 open orders, and price band
1..1,000,000,000. The configured registry is limited to 32 participants, bounding
live book size and response event counts. Business risk checks remain in the
engine and rejected commands do not mutate its book.

The event loop uses nonblocking sockets, poll, partial-send offsets, short-read
decoding and per-socket SIGPIPE protection. TCP_NODELAY is enabled. A complete
write-half-close drains already queued requests and responses. An incomplete
frame at EOF is a protocol error. Phase 10 adds handshake (5s), idle (60s) and
incomplete-frame (5s) deadlines; see [backpressure](backpressure.md).

## Commit, disconnect and persistence semantics

A queued frame is not an accepted engine command. Requests discarded before
processing do not receive engine sequences. Once processed, resting orders
persist after disconnect. Output overflow/send failure can disconnect a client
**after** its command executed but before it received an ACK. There is no
cross-session exactly-once retry cache; do not blindly resubmit after an ambiguous
disconnect. Live duplicate IDs are rejected, but terminal IDs may be reused by
the engine's existing policy.

Optional `--journal NEW_FILE` routes startup registrations and all engine commands
through JournaledEngine. It synchronously writes/syncs before an ACK is queued;
disk errors stop the session/server. Control traffic and protocol errors are not
engine commands and are not recorded there. Ctrl-C/SIGTERM stops reads, drains the
bounded inbox, attempts output flushing for 250 ms, then seals the journal.
Slow peers may still lose output at shutdown. A crash does not create a seal:
strict Phase 5 replay rejects incomplete journals, and live resume/crash recovery
is not implemented. Nonjournal mode has no persistence guarantee.

No networking throughput/latency result is claimed. Phase 7/8 measurements retain
their synchronous engine-only scope. Multiple symbols, remote service exposure,
authentication and durable session recovery are later work. The separate local
WebSocket observer bridge is described in [dashboard](dashboard.md).

## Private open-order extension (0.4.0)

Request 8 (empty payload) and response 106 add owner-only open-order snapshots.
The maximum framed response is 47,032 bytes; no engine sequence or book mutation
is introduced. See [wire layout, privacy and ordering](private-orders.md). Existing
v1 frames and old clients remain compatible. Observers are denied this request.
