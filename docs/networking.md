# Run the local exchange (Phase 9)

Build natively on your Mac, with no venv:

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
```

There are now separate `apex_gateway` and `apex_client` executables. `apex` still
runs the earlier core demos, workload tools and journal replay.

## One-command end-to-end demonstration

```sh
python3 scripts/network_demo.py
```

This standard-library script chooses a free loopback port, starts the real
gateway with a journal, starts an observer and an interactive maker, submits
seven orders/cancel/replace requests through client processes, and verifies three
trades and journal replay. The maker receives fills while waiting for console
input. The script gracefully stops its own processes, prints the results, and
retains logs plus the sealed journal in a unique directory under `tmp`.

CTest runs this demo when Python 3 is available. The engine/server/client
themselves require no Python dependency.

## Use separate terminals

Terminal 1 - start the gateway:

```sh
./build/debug/apex_gateway --port 9000
```

Terminal 2 - watch live depth and trades:

```sh
./build/debug/apex_client --port 9000 --participant 0 --watch
```

Terminal 3 - submit the price-time example:

```sh
./build/debug/apex_client --port 9000 --participant 1 --command "BUY 1 100 10000"
./build/debug/apex_client --port 9000 --participant 2 --command "BUY 2 50 10000"
./build/debug/apex_client --port 9000 --participant 3 --command "SELL 3 120 9900"
```

Expected: order 3 trades 100 with order 1 and 20 with order 2, both at 10,000
ticks. Order 2 remains bid for 30. Orders survive a one-shot client's disconnect.
Use fresh IDs or a fresh server for repeated examples; duplicate live IDs reject.

For an interactive order session:

```sh
./build/debug/apex_client --port 9000 --participant 2
```

Then enter:

```text
BOOK
REPLACE 2 4 40 10100
CANCEL 4
PING
QUIT
```

Supported commands:

| Command | Meaning |
| --- | --- |
| `BUY id qty price` / `SELL id qty price` | Limit order, integer ticks |
| `MARKET_BUY id qty` / `MARKET_SELL id qty` | Market order, cancel unfilled remainder |
| `CANCEL id` | Cancel own live order |
| `REPLACE old new qty price` | New desired remaining quantity; resets priority |
| `ORDERS` | Recover the bound participant's current open orders |
| `BOOK` | Current top-depth snapshot |
| `PING` | Heartbeat round trip (no latency measurement) |
| `HELP` / `QUIT` | Local help or close the session |

Interactive clients show unsolicited maker reports while idle at the prompt.
Only one writable session per participant is allowed; observers may share ID 0.
An observer cannot submit orders. Gateway defaults register participants 1..16;
use `--participants N` at startup to choose 1..32.

## Journal a network session

```sh
mkdir -p tmp
./build/debug/apex_gateway --port 9000 --journal tmp/network-session.apex
```

Submit commands from the other terminals. Stop the server with Ctrl-C to seal
the log, then verify:

```sh
./build/debug/apex --replay tmp/network-session.apex
```

The journal destination must be new. Existing files are never overwritten and
server restart does not recover an old book. Journaling mode may pause the event
loop for synchronous disk writes; it is an audit baseline, not a network benchmark.

## Verification and limits

The network suite checks all frame split points, coalesced frames, maximum
length, unknown version/type, malformed payloads, every event type, sequence
errors, observer/identity restrictions, duplicate sessions, business rejections,
write-half-close, maker reports, feed gaps/anchors and connection limits.

Eight real clients concurrently place 80 orders and receive unique engine
sequences 1..80. A taker then generates 80 trades and 241 response events, crossing
the server's write budget and exercising split output. The resulting journal
replays to the empty book with all 401 events verified. All sockets use ephemeral
ports in integration tests, so they do not conflict with a server on 9000.

The service binds only 127.0.0.1. IDs bind simulator sessions; there is no password
authentication, TLS, remote bind, UDP or reconnect retry cache. A separate local
WebSocket/browser observer is now available; see [dashboard](dashboard.md). Snapshots are full top-depth updates, not book deltas. Bounded
buffers, overload metrics, deadlines and stress evidence are documented in
[backpressure](backpressure.md). See [protocol](protocol.md) for precise acceptance, disconnect
and persistence behavior.

If a test runner blocks socket bind with `Operation not permitted`, enable local
loopback access for that runner. These tests must not silently skip socket
verification. A normal local Terminal build needs no Linux environment.
