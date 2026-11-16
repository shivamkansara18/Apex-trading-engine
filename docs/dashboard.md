# Local exchange dashboard (0.4.0)

The React + TypeScript dashboard shows best bid/ask, top-depth levels, cumulative
liquidity, recent trades, feed event rate, sequence gaps and connection health.
It uses real gateway publications; an empty exchange displays empty states.
The Node WebSocket bridge is a separate feed and trading adapter. No UI code or web
runtime runs in C++ matching. Everything runs natively on macOS; no venv is used.

## Start the full local stack

Requirements: the existing C++ build tools and Node.js 24 or newer with npm.
From the project folder:

```sh
cmake --preset debug
cmake --build --preset debug
npm ci --prefix ui
npm --prefix ui run build
npm --prefix ui start
```

Open **http://127.0.0.1:8080**. The launcher starts a fresh gateway on 9000,
then the HTTP/WebSocket bridge on 8080. If either port is busy, choose others:

```sh
npm --prefix ui start -- --gateway-port 9001 --port 8081
```

Ctrl-C stops both owned processes. Orders live only in this gateway session.
To journal it, use a new file (create the parent directory first):

```sh
mkdir -p tmp
npm --prefix ui start -- --journal tmp/dashboard-session.apex
```

On graceful stop the journal is sealed. Replay with
`./build/debug/apex --replay tmp/dashboard-session.apex`.
The launcher never overwrites a journal or restarts/replaces an existing gateway.

## Symbol selection

Start with `npm --prefix ui start -- --symbols APEX,ALPHA,BETA`, then choose a market
in the session bar. Its book, tape, sequences, agent sessions and trading identity
are isolated. A symbol switch closes the current trading connection and clears
feed state; resting orders stay in their original book. Use
`npm --prefix ui run demo -- --symbol ALPHA` for named adaptive activity.
See [multi-symbol lifecycle and journaling](multi-symbol.md) for ports, routing
versions, failure isolation and per-book replay.

## Light and dark themes

The header includes a Light mode / Dark mode switch. Light mode uses a warm cream
canvas and off-white panels with darker green/red accents. The selected theme is
saved in browser storage and applied before paint on reload. If storage is
unavailable, switching still works for the current page. Theme changes do not
reconnect the feed or change orders.

## Create live activity

For an animated local demo, leave the dashboard running and use a second terminal:

```sh
npm --prefix ui run demo
```

This starts adaptive participants 9-12 over persistent TCP sessions. They react
to live depth and fill-derived inventory; action choices and sizes use a printed
random seed. Use `-- --steps 100 --seed 42` for a shorter run, or
`-- --gateway-port 9001` for another port. Ctrl-C or normal completion attempts
to cancel the agents' quotes. Use participants 1-8 for your own manual orders.
See [responsive simulation and scaling](scaling.md) for parameters and model limits.
The original scripted example is now `npm --prefix ui run walkthrough`.

In a second terminal, from the same project folder and using the printed gateway
port (9000 by default):

```sh
./build/debug/apex_client --port 9000 --participant 1 --command "BUY 1 100 10000"
./build/debug/apex_client --port 9000 --participant 2 --command "BUY 2 50 10000"
./build/debug/apex_client --port 9000 --participant 3 --command "SELL 3 120 9900"
./build/debug/apex_client --port 9000 --participant 4 --command "SELL 4 80 10100"
```

Expected: two trades at 10,000 ticks for quantities 100 and 20, a remaining bid
of 30 at 10,000, an ask of 80 at 10,100 and a spread of 100 ticks. Use fresh order
IDs for more activity; existing live IDs reject. Alternatively use the Trading desk
in the dashboard to connect a participant and submit limit/market/cancel/replace
commands. Actual engine ACKs, private execution reports and rejection reasons are
shown there. On connect, open orders are restored from the engine; Refresh my orders repeats
the authoritative read. Missed execution history is not reconstructed. See
[trading behavior and reconnect limits](scaling.md).

For a gateway you already started, run only the bridge:

```sh
npm --prefix ui run bridge -- --gateway-port 9000 --port 8080
```

The bridge serves the production build. After UI source changes, rerun
`npm --prefix ui run build` and reload. After bridge changes, restart the bridge.

## Private order synchronization

Connect the same participant and symbol after a disconnect to restore its open
orders, including offline remaining-quantity changes. Submissions remain disabled
until synchronization completes. Use **Refresh my orders** after connecting.
Only that participant's live orders are returned. This does not replay missed
trades, infer terminal outcomes or retry uncertain submissions.
See [private recovery protocol and limits](private-orders.md).

## Feed contract and recovery

Browser observers of a symbol share one observer-0 upstream TCP session per bridge.
Joining/recovering viewers request an anchor snapshot broadcast to the group.
Each browser trading identity uses its own persistent TCP connection. Trade and subsequent book publications must increase the public feed
sequence consecutively. Command/event numbers are separate and may have gaps for
an observer. A detected public sequence gap pauses data application and requests
a new snapshot. The anchor replaces depth and restores sequencing. The gap counter
stays visible for that connection. Recent trades retain at most 50 items and are
not a historical tape: snapshots recover current depth, not missed trade history.

Reconnects retry with 1..5 second delays and start with a new anchor. Old depth is
marked disconnected during retry, then cleared when the socket opens. A snapshot
button explicitly refreshes depth. Snapshot requests are coalesced to one every
250 ms, including a delayed request rather than dropping the final request.

Bridge heartbeats keep a quiet market distinguishable from a dead connection.
No gateway signal for 12 seconds marks the UI stale; the bridge disconnects an
unresponsive upstream after 15 seconds (checked on its 5-second heartbeat loop).
Last market update age tracks only trades/books, while heartbeats refresh signal
health. The displayed rate counts observed trade + book publications over a
rolling five-second window. It is not engine throughput or a latency benchmark.
Trade timestamps are bridge receipt times. Prices/quantities and 64-bit sequences
cross JSON as decimal strings, preserving precision beyond JavaScript Numbers.

The bridge binds 127.0.0.1, fixes its upstream to loopback, verifies Host and
same-origin WebSocket upgrades. `/stream` accepts only observer snapshot requests;
`/trade?participant=N&symbol=S` binds a persistent trading identity and symbol.
`/api/markets` exposes routing version 2 and configured markets. It caps browser
WebSocket sessions at 32 (both observer and trader sockets count), inbound WebSocket messages at 4 KiB and queued outbound
WebSocket data at 1 MiB. Overflow terminates that browser session; gateway and
matching continue. These local simulator identities are not authentication.

## Checks

```sh
npm --prefix ui run build
npm --prefix ui test
```

The twenty-four Node tests cover TCP codecs, every header split, payload truncation,
large integers, gap suspension/anchor recovery, bounded retention and rate state.
Real-process integration launches a journaled gateway and bridge, submits orders,
checks depth/trades, deliberately skips intermediate UI updates to trigger gap
recovery through a real snapshot request, reconnects, rejects trading messages on the observer endpoint,
checks origin/Host restrictions, seals and replays the journal. Additional checks
cover manual browser fills, replace/cancel/risk rejection, shared upstream fanout
and limits, adaptive book/inventory response and quote cleanup.

Local Phase 11 verification: all 150 CTest cases passed in Debug, Release
benchmark, ASAN/UBSAN and native macOS ThreadSanitizer builds before UI work.
UI typecheck/production build and all twenty-four UI/bridge tests pass. A small Release
benchmark export also passed as a functionality smoke check, not new performance
evidence. Hosted CI is configured for the UI but has not run.

Visual browser QA could not be completed: the computer-use service reported that
macOS screen-capture permission was denied. HTTP serving, WebSocket data and state
recovery were verified automatically; rendered layout and browser interaction
remain to be checked in a normal browser.
