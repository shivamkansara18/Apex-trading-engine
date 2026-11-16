# Live symbol partitions (0.3.0)

Live trading can now route to 1-8 independent symbol books. Each symbol gets its
own C++ gateway process, single matching writer, participant risk registry, command
and event counters, market-data sequence and optional sealed journal. Different
books can execute in parallel across processes. No competing writers access one
book. The shared Node bridge routes sessions and fans out each symbol's own feed.

ALPHA and BETA are placeholder instrument symbols within one simulated exchange,
not separate venues such as NYSE and LSE. Separate venues would additionally need
venue-specific sessions, rules, fees and routing; those are not modeled here.

## Start and trade

Rebuild the current sources, stop any old dashboard, then start named markets:

```sh
cmake --preset debug
cmake --build --preset debug
npm --prefix ui run build
npm --prefix ui start -- --symbols APEX,ALPHA,BETA
```

Open http://127.0.0.1:8080 and choose a market in the session bar. Trading desk
connects a participant to that selected book. Use participants 1-8 for manual
orders; agents use 9-12. The same participant and order IDs may be used in different
books. Prices/quantities from different symbols never match with one another.

Generate adaptive activity in a selected book:

```sh
npm --prefix ui run demo -- --symbol ALPHA
# In another terminal, another independent book:
npm --prefix ui run demo -- --symbol BETA --seed 42
```

For a custom HTTP port add `-- --port 8081` to startup (alongside other options),
and pass `--bridge-port 8081` to the demo after `--`. `--symbol` resolves the
native gateway via `/api/markets`; combining it with `--gateway-port` is rejected
to avoid ambiguous destinations. A direct `--gateway-port N` remains available
for a native single-book session without discovery. All symbols are simulator
labels, not live exchange subscriptions or external price sources.

The default remains one `APEX` book on TCP 9000 and dashboard 8080:
`npm --prefix ui start` and `npm --prefix ui run demo` still work. The first symbol
uses `--gateway-port` (9000 by default); additional partitions use OS-assigned
loopback ports, printed on startup and returned by discovery. To allocate every
TCP port dynamically, pass `--gateway-port 0`. Existing `apex_client --port N`
continues to operate against the selected book's printed native port.

Symbol names are case-sensitive, unique ASCII names of 1-16 characters, starting
with A-Z and followed by A-Z, digits, dot, underscore or hyphen. The launcher
rejects duplicates and more than eight partitions before spawning gateways.

## Isolation and switching

Each WebSocket connection binds a symbol at upgrade. It cannot switch books by
sending another symbol in a payload. Observer and trader messages carry
`schemaVersion: 2` and the canonical `symbol`. Unknown symbols reject the upgrade;
there is no fallback to another book. No-symbol clients bind the configured first
symbol for backward compatibility.

The browser clears depth, recent trades, sequences and feed-rate state on a symbol
switch. It closes the prior observer/trader sessions and requests the new book's
anchor. Late callbacks from a previous feed are ignored. The selector is disabled
while a trading command is pending, avoiding a switch during unknown completion.
Resting orders remain in their original book after disconnection. Version 0.4.0 restores current private open orders on reconnect before enabling
submissions. Missed execution history and terminal outcomes are not restored; see
[private snapshot semantics](private-orders.md).

Participant registration, open-order/notional/price limits and kill switches are
per book. There is no aggregate cross-symbol exposure, cash account, margin ledger,
atomic cross-book transaction or globally ordered event stream. Two feeds may
both have sequence 1 or trade ID 1; consumers must key data by symbol as well.

## Journaling and replay

For multiple symbols use a new journal directory:

```sh
mkdir -p tmp
npm --prefix ui start -- --symbols APEX,ALPHA,BETA --journal-dir tmp/multi-session
```

The directory must not exist. It contains `APEX.apex`, `ALPHA.apex`, `BETA.apex`
and an atomically updated `session.json` with symbol/port/PID/journal mappings and
process status. Each C++ process syncs and seals its own journal. Stop the launcher
with Ctrl-C, then replay independently:

```sh
./build/debug/apex --replay tmp/multi-session/ALPHA.apex
./build/debug/apex --replay tmp/multi-session/BETA.apex
```

Single-symbol `--journal NEW_FILE` remains compatible. Multiple symbols require
`--journal-dir`; both options together reject startup. No existing journal or
session directory is overwritten. This records separate instrument histories,
not one total cross-book ordering or durable live recovery.

## Failure and resource boundaries

Startup waits for every gateway, then opens the bridge. If startup fails (including
an occupied HTTP port), it stops all gateways it started. It never terminates a
server that already owned a port. Journals for successfully started processes
seal during this cleanup; a partition that failed before journal creation may
have no journal. The session manifest records which processes started and exited.

After a multi-symbol launch succeeds, one partition's exit marks that symbol
unavailable. Its connections close, new upgrades return unavailable, and healthy
books remain usable. There is no automatic process restart that would silently
replace a book with empty state. The browser shows the failed market and users
can select a healthy one. Discovery's availability describes the launcher's known
process status; heartbeat/stale-feed checks still determine actual feed health.
Externally managed bridge targets have no launcher process supervision.

Graceful whole-stack shutdown closes the bridge and signals all owned gateways,
waiting up to five seconds before forced termination. A force-killed process may
leave an unsealed journal, which strict replay rejects. Original single-symbol
unexpected-exit behavior remains: the launcher exits with failure.

The browser WebSocket cap remains 32 across all books and both connection types.
Each active symbol has at most one shared observer upstream per bridge. Trading
sessions remain separate per participant and symbol. Output, queue, order counts
and heartbeat bounds remain in effect per gateway; total resource use grows with
partition count. A single Node bridge can still become a fanout bottleneck.
This version demonstrates functional process isolation, not a measured linear
speedup or a production-capacity guarantee.

## Routing version and verification

The browser routing/metadata schema is version 2. The native TCP codec and journal
record formats stay at version 1 because connections are routed to dedicated
symbol endpoints, rather than multiplexing untagged commands on one TCP stream.
A future multiplexed C++ gateway must explicitly version a symbol-bearing frame
format; it must not reinterpret existing v1 bytes. Sessions and journal filenames
provide the symbol association in this process-partitioned design.

The 22 Node tests include symbol/port validation, browser feed-route guards,
non-crossing books with otherwise crossing prices, independent identities/order
IDs/sequences, per-book replay, named adaptive activity, partition outage isolation
and failed-start cleanup. The existing native cases and five packaging tests
remain part of full verification. Use `python3 scripts/verify_release.py --clean-source`
for native sanitizer, UI, source reproduction and benchmark audit evidence.
Browser visual capture remains unavailable on this Mac; the new market selector
still needs ordinary-browser visual review.
