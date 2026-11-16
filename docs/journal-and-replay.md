# Journal and deterministic replay (Phase 5)

`JournaledEngine` wraps a fresh `MatchingEngine`. Route every participant setup,
limit change, global switch and trading command through this wrapper. Its engine
accessor is const, so callers cannot bypass the recorded input path.

```cpp
apex::JournaledEngine session("new-session.apex"); // Must not already exist.
if (!session.register_participant(1)) { /* duplicate ID */ }
auto events = session.process(apex::NewOrder{
    1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0});
session.finish(); // Seal, sync and close; required before strict replay.
const auto result = apex::replay("new-session.apex");
```

Headers: `apex/journal/journal.hpp` and `apex/replay/replay.hpp`. Rejections are
recorded too. Duplicate registration and unknown-participant configuration
return false and that result is recorded. Invalid configuration exceptions stop
the session rather than being recorded as successful admin operations.

## File format, version 1

All multibyte integers are explicitly big-endian. Prices use the 64-bit signed
two's-complement bit representation. No raw C++ structs, pointer values, wall
clock timestamps, padding or host byte order are serialized.

| Region | Fields |
| --- | --- |
| Header (12 bytes) | 8-byte `APEXJNL\0` magic, uint32 version = 1 |
| Record prefix | uint32 payload byte count, uint64 record sequence |
| Record payload | Input + expected result, or one-byte terminal seal |
| Record suffix | uint32 CRC-32 of the sequence bytes followed by payload |

Payload length must be 1..16,777,216 bytes and is validated before allocation.
Record sequences start at one, include admin operations and the seal, and must
be consecutive. They are separate from exchange command/event/trade sequences.
CRC uses the standard reflected IEEE polynomial `0xedb88320`, initial/final XOR
`0xffffffff`; `123456789` gives `cbf43926`.

Input tags:

| uint8 tag | Fields |
| --- | --- |
| 1: register | uint32 participant ID, RiskLimits |
| 2: configure | uint32 participant ID, RiskLimits |
| 3: global enabled | uint8 boolean |
| 4: trading command | uint8 command tag, command fields |
| 255: seal | No further payload bytes |

RiskLimits encode uint64 max quantity, max notional, max open orders, int64
minimum/maximum prices, uint8 trading-enabled boolean. Booleans must be 0 or 1.

Trading command tags: 1=new, 2=cancel, 3=replace. Fields follow their public
records in declaration order: ID uint64, participant uint32, side/type uint8,
prices int64, quantities and client timestamps uint64. Invalid side/type bytes
are preserved as inputs so malformed-request rejections replay identically.
Unknown input/command tags are format errors.

Expected results encode uint8 admin-result (true for trading commands), uint64
event count, events, then uint64 canonical book hash. Each event begins with
uint64 event sequence and command sequence, then uint8 payload tag:
1=accepted, 2=updated, 3=cancelled, 4=rejected, 5=trade.
Accepted/updated encode Order; cancelled adds uint8 cancel reason; rejected
encodes the original tagged command plus uint8 reject reason. Trade fields follow
the public Trade declaration. Order fields follow its declaration, with enums
uint8 and all numeric fields fixed width. Enum values are the current domain
declaration values starting at zero; changing them requires a format version bump.

Canonical book bytes encode last command sequence, bid level count, bid levels,
ask level count and ask levels. Each level encodes int64 price, uint64 total,
uint64 order count and FIFO Order records. Counts and sequences are uint64.
Bid prices descend; asks ascend. The hash is FNV-1a 64-bit (offset basis
14695981039346656037, prime 1099511628211). It covers book state and priority,
including last processed command sequence, **not** participant configuration.
The event hash incrementally hashes each command's canonical event vector,
including its event count. CRC and FNV are diagnostics, not authentication or
cryptographic tamper protection.

## Replay verification

Replay starts from an empty engine, applies all admin and trading inputs in
record order, and regenerates outputs. It compares the full canonical result
bytes, not just a summary hash, and checks uncrossed book invariants after each
input. A changed event, rejection reason, admin return value or book hash fails
verification. The returned report includes input/command/event counts, event
hash, book hash and the final owning snapshot. Repeated replay must match exactly.

A terminal seal is mandatory; trailing bytes are forbidden. This catches a file
cut off at a complete-record boundary as well as mid-record truncation. This
version deliberately rejects unfinished/crashed sessions. It does not repair
partial tails, recover only a prefix, resume a live engine, or append to an
existing session. Snapshot acceleration is deferred until needed.

## Write and failure guarantees

Files are created exclusively with POSIX `O_EXCL` and owner read/write mode;
existing files are never truncated. This works natively on macOS and Linux.
The wrapper processes each input in memory, serializes inputs and outputs into
one record, writes it completely (handling short writes/EINTR), and calls `fsync`
before returning results to the caller. Matching itself performs no file I/O.

This is a synchronous audit baseline, not a high-throughput journal mode.
There is no write-ahead transaction rollback: a write/sync/serialization/engine
exception may leave in-memory state changed. The session then refuses every
subsequent mutation and cannot be sealed; callers must discard it. Destruction
closes the descriptor without silently marking an incomplete session complete.
`finish()` explicitly appends/syncs the seal and checks close errors.

`fsync` requests OS persistence; no directory fsync, macOS `F_FULLFSYNC`, hardware
power-loss guarantee, process-crash recovery or distributed durability is claimed.
An input whose result was not returned may still have reached the file. A record
larger than 16 MiB fails the session; there is no automatic record splitting.

## Run locally

```sh
cmake --build --preset debug
ctest --preset debug
./build/debug/apex --journal-demo
```

The demo creates a unique temporary directory, prints its journal path, records
the FIFO/replace example and kill-switch rejection, seals, and verifies replay
twice. The resulting final bid is 40 units at 10,100 ticks. The artifact remains
at the printed path for inspection. To choose a new destination and replay it:

```sh
mkdir -p tmp
./build/debug/apex --journal-demo tmp/my-session.apex
./build/debug/apex --replay tmp/my-session.apex
```

Choose a different filename if it exists. Errors print a message and exit with
status 1; no existing journal is overwritten.

Tests cover byte order/signed values, all commands, invalid enum preservation,
admin transitions, repeated seeded runs with byte-identical files, every truncated
prefix of a sample file, version/magic/CRC/length errors, sequence/tag errors,
semantic mismatches with recomputed CRC, unfinished sessions and trailing data.
A child-process file-size limit injects an actual write failure and verifies
that the session stops before returning success and refuses further mutations.
