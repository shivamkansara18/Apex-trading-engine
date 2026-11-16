# Participant simulation and saved workloads (Phase 6)

This phase supplies reproducible input streams for correctness demos and future
benchmarks. Generation and execution run natively in C++20 on macOS. No Python,
venv, networking or real-time scheduler is needed.

## Run examples

```sh
cmake --build --preset debug
ctest --preset debug
./build/debug/apex --workload-demo
./build/debug/apex --run-workload examples/workloads/large-partial.workload
./build/debug/apex --run-workload examples/workloads/balanced.workload
```

The demo generates the same seed twice, saves and reloads the inputs, and checks
that two executions have identical event hashes and final books. It prints the
saved file's unique temporary path. The large-partial example creates 16 small
asks across four prices, then executes a large market buy, yielding 16 trades
and cancellation of the unfilled market remainder.

Generate your own file (destination must not already exist):

```sh
mkdir -p tmp
./build/debug/apex --generate-workload tmp/my.workload --seed 42 --commands 1000 --profile balanced
./build/debug/apex --run-workload tmp/my.workload
./build/debug/apex --run-workload tmp/my.workload --journal tmp/my-session.apex
./build/debug/apex --replay tmp/my-session.apex
```

Optional recording uses the existing Phase 5 facade, syncs input/output records,
and seals the journal. It currently supports one symbol. The run summary's event
hash includes symbol IDs; it differs from the journal replay event hash, which
has no symbol prefix. Event counts and final per-symbol book hashes agree.

## Configuration

Options are value pairs after `--generate-workload NEW_FILE`. Presets apply first
and explicit options override them regardless of argument order. Duplicate or
unknown options fail. All prices and quantities use integer ticks/units.

| Option | Default (balanced) | Meaning |
| --- | --- | --- |
| `--seed` | 42 | uint64 seed, including zero |
| `--commands` | 1000 | Exact number of scheduled commands; 0..100,000 |
| `--symbols` | 1 | 1..32 independent books, zero-based symbol IDs |
| `--participants` | 6 | 2..1000 registered participants per symbol |
| `--center` | 10000 | Fixed center price; greater than width + 1, at most 1 billion |
| `--width` | 20 | Price offset width, 0..1 million |
| `--min-quantity` / `--max-quantity` | 1 / 100 | Inclusive range, positive and at most 1 million |
| `--cancel-percent` / `--replace-percent` | 20 / 10 | Action probabilities when live orders exist; sum <=100 |
| `--market-percent` | 30 | Probability of market type among random-flow new orders |
| `--rate` | 10000 | Logical commands per second, 1..1 billion |
| `--burst-size` | 1 | Commands sharing one scheduled instant, 1..100,000 |
| `--distribution` | uniform | `uniform` or `triangular` integer price offsets |
| `--profile` | balanced | Profile from the table below |

The symbol/participant product may not exceed 10,000. Bounds prevent arithmetic
overflow and constrain generation/parsing memory; large valid workloads may
still be slow in the current correctness baseline.

```sh
./build/debug/apex --generate-workload tmp/multi.workload --symbols 3 --participants 9 --distribution triangular --burst-size 20 --rate 5000
./build/debug/apex --run-workload tmp/multi.workload
```

## Participants and profiles

Participant IDs repeat roles in groups of three: 1/4/7 are liquidity providers,
2/5/8 are takers, and 3/6/9 are random flow. The configured participant count
determines the role mix. New requests choose a participant uniformly.

Providers submit limit quotes around a fixed center and adjust new quotes to
remain passive against the current book. Takers submit markets. Random agents
choose limit/market according to market-percent. Cancellation and replacement
actions select an existing order in sorted ID order with random indexing, using
its real owner; they may operate on any role's order. Replacement prices may
cross. If no live order exists, a cancel/replace draw falls back to a new order.
Probabilities describe generation choices, not guaranteed final command ratios.

| Profile | Behavior |
| --- | --- |
| balanced | Providers, takers and random flow with default mixed commands |
| hot-price | All limit/replace prices equal center; deliberately overrides passive quoting |
| many-levels | Default width 1000 to exercise many distinct price levels |
| high-cancel | Default cancel probability 80%, replace 0%; falls back to new orders if empty |
| market-burst | Random-flow market probability 85%, no cancel/replace, default burst size 25 |
| large-partial | Per-symbol script: 16 asks of min-quantity at center+1..4 from participant 1, then a market buy of max-quantity from participant 2 |

The large-partial script overrides normal role, action and price-distribution
choices. With defaults it sweeps all 16 makers; custom size settings can change
how much liquidity is consumed. Hot-price ignores width for request prices.
Other profiles customize defaults rather than guaranteeing a particular final
depth, fill ratio or market-order count.

## Determinism and logical scheduling

Generation uses `std::mt19937_64` and an explicit unbiased rejection sampler;
it avoids implementation-dependent standard-library distributions. Triangular
offsets average two integer uniform draws. Live-order selection uses ordered
maps, never unordered iteration. A shadow engine per symbol processes each
generated command, keeping cancel/replace targets live. Generation itself is
outside any future benchmark timing.

For global command index i, batch size B and rate R, scheduled nanoseconds are
`floor(floor(i / B) * B * 1,000,000,000 / R)`. New-order client timestamps copy
this value. It never affects matching priority. The generator emits exactly the
requested command count. Zero-command workloads are valid.

Multiple symbols route sequentially into distinct single-symbol engines, each
with local IDs, sequences and participant limits. This is deterministic workload
partitioning, not a multi-symbol exchange, parallel matcher or shared risk book.

The runner processes commands as fast as possible in file order and checks book
invariants after each command. It reports commands, rejects, events, trades,
executed quantity (counted once per trade), an aggregate event hash and final
book hash per symbol. It does **not** sleep to scheduled timestamps, create an
open-loop load, measure throughput, or estimate latency. Phase 7 adds measurement.
The FNV event hash covers each symbol ID followed by its canonical event vector
in global input order. Book hashes use the Phase 5 representation.

## Saved format, version 1

Files are inspectable whitespace-delimited text with deterministic classic-locale
formatting. A configuration header is followed by exactly its command count:

```text
APEX_WORKLOAD 1
CONFIG seed commands symbols participants center width min_qty max_qty cancel_pct replace_pct market_pct rate burst distribution profile
scheduled_ns symbol NEW id participant BUY|SELL LIMIT|MARKET price quantity client_timestamp_ns
scheduled_ns symbol CANCEL id participant
scheduled_ns symbol REPLACE old_id new_id participant price new_quantity
END
```

The header and entry count, integer ranges, command tags, side/type, participants,
symbols, nondecreasing schedule and terminal END are validated. Extra tokens are
rejected. Limits are checked before reserving entry memory; files above 64 MiB
are refused. Business-invalid fields such as zero quantity remain representable
for scripted rejection workloads. Generated inputs have no expected rejection.

Saving uses exclusive creation and never overwrites files. A write failure can
leave an incomplete file; saving has no fsync/durability promise. The workload
format is an input fixture, not an audit journal: it has no checksum or expected
outputs. Use Phase 5 recording to verify exact outputs against a sealed journal.
Saved inputs remain the authoritative workload when generator/engine versions
change. Same-seed reproducibility assumes the same generator version/config.

Tests include a known seed prefix, repeated generation/byte-identical saves,
lossless load, all profiles, passive providers, exact schedules and size bounds,
multi-symbol isolation, repeated events/final books, optional journal replay,
malformed files and resource bounds. No performance result is claimed here.
