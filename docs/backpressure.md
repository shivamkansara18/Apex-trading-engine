# Phase 10: bounded admission and liveness

The gateway uses a mutex-protected `BoundedQueue<Pending>` with nonblocking
try-push/try-pop. The mutex publishes moved values; consumers receive owned values,
never references into queue storage. Eight-producer stress checks 40,000 values
for completeness, uniqueness and ordering within each producer. Lock-free queues
remain deferred until a measured contention bottleneck justifies comparison.

Matching, sessions and socket writes still belong to one event-loop thread.
The queue supports multiple producers, but the current gateway has one I/O
producer. This phase does not claim a throughput gain from adding a mutex.

Admission is bounded by `inbox_capacity` (default 256). A full inbox sends
Overloaded and closes that session. Its already queued, unexecuted frames are
discarded; an admitted frame is not a promise of execution. Clients must use
command ACKs and the journal to determine outcomes. Other sessions remain usable.
Each read and write gets a 16 KiB budget; processing gets 64 frames per loop and
accept gets 32 connections. These bound work per iteration, but do not promise
strict fairness under adversarial traffic. Capacity is shared across peers.

Pending output is capped per connection (default 1 MiB). Overflow aborts that
consumer immediately rather than blocking matching. A business command already
being matched finishes even if a response consumer overflows. There is no rollback;
a reconnect must request a snapshot. A single large command can exceed the cap
before any socket flush, even with a responsive reader. Configure capacity for
expected fill sizes. The 400-maker integration test exercises this case explicitly.

GatewayConfig supplies positive handshake (5 seconds), idle (60 seconds), and
partial-frame (5 seconds) deadlines. Handshake time starts at accept; idle time
refreshes on received bytes, including heartbeats. The partial-frame deadline
starts with the unfinished frame and does not refresh for a trickle of bytes.
Timeouts abort connections and preserve resting orders. Polling checks deadlines
at roughly 20 ms granularity; processing and synchronous journal I/O can delay
checks. Deadlines are not hard real-time guarantees.

`TcpGateway::stats()` is safe during execution. It exposes frames processed and
discarded, output overflows, deadline disconnects, current inbox depth, high-water
mark and rejected pushes. Atomic counters and a locked queue snapshot are
observational rather than one transactional sample. Processed counts protocol
frames passed to handling, including HELLO and rejected requests. Discarded counts
queued frames skipped after their session closes. CLI prints final counters on
shutdown. Shutdown stops admission, drains eligible queued frames, flushes output
for up to 250 ms and seals an optional journal.

Native macOS validation commands (no venv):

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

ASAN and TSAN use separate build directories. Sanitizer results cover the tested
workloads, not proof of freedom from every race or failure mode. Existing network
stress also checks concurrent clients, unique command sequences and sealed replay.

Validation on this Mac: all 150 CTest cases passed in Debug, ASAN/UBSAN and
native ThreadSanitizer builds. Hosted CI was not run.
