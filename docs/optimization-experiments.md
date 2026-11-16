# Profile-guided optimization experiments (Phase 8)

A re-audited, portable [CSV summary](../results/summary/optimization.csv),
[run records and hashes](../results/summary/optimization.json) and
[measured source ZIP](../results/summary/measured-source.zip) now travel with the
source. [Provenance and reproduction](../results/summary/PROVENANCE.md) distinguish
these historical measurements from the current application and retained local raw data.

The selected default is **tree price levels + direct risk traversal + worst-case
event reservation**. Risk validation now reads the writer-owned book without
allocating owning snapshots. Business rules, command sequences, events and
journal format are unchanged. This is the only optimization promoted by default.

The flat sorted price-level index and smaller event-buffer policy remain explicit
research options so the comparisons can be repeated; neither changes the default.

## Native Mac profile

The Command Line Tools installation does not supply a usable Instruments
`xctrace` tool. The built-in `/usr/bin/sample` did capture a five-second CPU sample
of `apex_profile` repeating the original saved balanced workload. Sampling first
failed inside the execution sandbox; an approved run outside it succeeded.

Raw profile: [sample.txt](../results/raw/profile.ORkzGm/sample.txt), with
[driver output](../results/raw/profile.ORkzGm/driver.txt). The call tree shows
`RiskManager::usage -> OrderBook::snapshot -> top_levels` and repeated vector
allocation/free under `RiskManager::validate`. This is sampling evidence of a hot
path, not a measured per-operation allocation count or an exact CPU percentage.
No profiler was attached during the comparison timings.

Reproduce native profiling (macOS, separate from benchmark runs):

```sh
cmake --preset benchmark
cmake --build --preset benchmark --parallel 4
bash scripts/profile_macos.sh SAVED_WORKLOAD baseline
```

Use `default` instead of `baseline` to sample the selected policy. The native
sampler needs permission to inspect the target process; profiling is not required
to run normal tests or the timed comparison harness.

## Experimental controls

- Apple M5, arm64, same compiler/flags and binary for every measured policy.
- Three saved 100,000-command inputs: the exact Phase 7 balanced capture, plus
  seed-42 many-levels and large-partial streams. Inputs are retained unchanged.
- Five interleaved paired blocks per policy/workload; each bundle has one complete
  untimed warm-up pass, one fresh-book throughput pass and a separate fresh-book
  latency pass. Policy order is shuffled with seed 42 and retained in the manifest.
- Five policies, three workloads, five blocks = **75 measured bundles**, each
  with 100,000 latency samples, or **7,500,000 raw samples** in total.
- Every bundle was checked with the Phase 7 analyzer. Workload IDs, canonical
  event hashes, final book hashes and outcome counts agree across policies.
- Each reported speed ratio is the median of five *paired* candidate/control
  throughput ratios. It is not necessarily the ratio of the two throughput
  medians. Quantiles below are medians of five per-run nearest-rank quantiles.
- These are synchronous closed-loop engine results, not network/queue/journal
  latencies. All Phase 7 timing boundaries, timer quantization and macOS scheduling
  limitations still apply. No significance test or universal speedup is claimed.

Full data: [experiments.json](../results/raw/phase8-experiments/experiments.json).
Each named bundle contains CSV/JSON, raw samples and the copied input.
The [measured source archive](../results/raw/phase8-experiments/source.zip) matches
source SHA-256 `9f681f4cfd6598d308be224a735dc50a40f3a06e3144d6948877d8390d4fa9df`, independently recomputed from its members. The
archive precedes the later default-policy promotion; all measured choices were
explicit CLI flags. It includes code and tests, not downloaded dependencies.
Raw bundles are gitignored and must be retained separately for sharing.

## Results

Throughput is in millions of submitted commands per second. p99/p99.9 are ns.
All these generated inputs had zero rejects.

| Input | Policy | Median M commands/s | Median paired ratio | p99 ns | p99.9 ns |
| --- | --- | ---: | ---: | ---: | ---: |
| balanced | baseline | 4.727 | 1.000x | 584 | 791 |
| balanced | direct-risk | 10.386 | 2.188x | 250 | 292 |
| balanced | flat-prices | 4.870 | 1.032x | 542 | 709 |
| balanced | small-events | 4.725 | 1.011x | 584 | 791 |
| balanced | direct-flat | 11.189 | 2.361x | 209 | 292 |
| many-levels | baseline | 4.758 | 1.000x | 583 | 750 |
| many-levels | direct-risk | 10.175 | 2.139x | 250 | 333 |
| many-levels | flat-prices | 4.997 | 1.044x | 542 | 708 |
| many-levels | small-events | 4.716 | 0.991x | 584 | 750 |
| many-levels | direct-flat | 11.123 | 2.315x | 209 | 292 |
| large-partial | baseline | 4.993 | 1.000x | 833 | 875 |
| large-partial | direct-risk | 9.655 | 1.946x | 708 | 792 |
| large-partial | flat-prices | 4.909 | 0.977x | 833 | 875 |
| large-partial | small-events | 4.850 | 0.964x | 917 | 1000 |
| large-partial | direct-flat | 9.976 | 1.999x | 667 | 750 |

### E1: remove risk snapshot copies

`direct-risk` uses the existing tree/list book and read-only callbacks on the
single writer. Usage still scans the authoritative book; market preflight still
uses price/FIFO order and stops at self-trade or fulfilled quantity. It introduces
no cached reservation ledger and changes no risk policy. Public snapshots remain
owning copies; callback references must not escape.

This improved paired throughput by 1.946..2.188x across the three inputs and
reduced both p99 and p99.9. Every individual paired throughput ratio exceeded
1.88x. The result agrees with the profile's snapshot-allocation hot path.
**Retained as the default.** No object pool is justified by this experiment;
removing the unnecessary allocations is simpler than pooling copied snapshots.

### E2: tree levels versus flat sorted price levels

`flat-prices` changes only the price index. It stores sorted contiguous price/level
pairs with the same FIFO `std::list` nodes and hash ID index. Binary search is
O(log P); new-level insertion/removal shifts O(P) level objects. Moving the fixed
standard-allocator lists preserves order-node iterators. Tests deliberately force
reallocation and middle erasure of 600 levels/orders and verify every index.

The isolated flat index has small balanced/many-level gains and a slight median
large-partial regression. One large-partial paired ratio was 0.355x; it remains
in the raw data and is not attributed to the container alone. The combined
`direct-flat` configuration is faster on all three captures, but these are shallow
books: an untimed scan found peaks of 23, 28 and 4 active levels (34, 28 and 16
orders), respectively. Many distinct input prices do not imply a deep settled
book. See [depth diagnostics](../results/raw/phase8-experiments/workload-depth.json)
and its retained C++ source.

**Opt-in only.** The original tree remains the general-purpose default, preserving
O(log P) level insertion/removal for unbounded price ranges/deeper books. The
measured captures do not establish flat-vector scaling at high active depth.

### E3: smaller initial event buffers

`small-events` reserves at most eight events initially, growing when necessary,
instead of reserving the full worst-case bound. Sequence capacity still uses the
same conservative bound. Allocation failure remains fatal to the session.

The balanced result is near neutral, many-level throughput is slightly worse,
and large-partial throughput/tails regress. Large-partial p99.9 rises from 875 ns
to 1,000 ns. Growing the output vector during multi-fill commands trades smaller
initial capacity for extra allocations/copies. **Rejected as a default change**;
the pre-reserved worst-case policy remains in production and in the control.

Process peak RSS is preserved in each record, but it includes input parsing,
verification, sample vectors and other harness memory. It is not standalone
book memory or an allocation count. No per-order memory reduction is claimed.

## Correctness and replay gate

All eight combinations of the three policies are tested against the explicit
snapshot/tree/worst-case reference after every command, including risk failures,
admin changes, self-trade handling and replacement. The six generated workload
profiles, overflowing quantities, ownership rules and large fill buffers are
covered by the existing and differential suites.

Policy journals are byte-identical. A sealed journal captured by the engine
**before** the optimization is checked in as a regression fixture: its 100
commands still reproduce all 272 events and the original final book hash under
the selected default. Changing a physical policy requires no journal-format bump
because observable output is identical, not because replay validation is weakened.

## Reproduction

```sh
bash scripts/run_experiments.sh
```

This builds/tests Release and writes a new comparison bundle under `results/raw`.
It reuses the Phase 7 balanced input when present; otherwise it generates seed 42.
Python uses the standard library only; no venv is required. Run it without a
profiler or concurrent build/heavy workload. The full run retains substantial raw
data (about 455 MiB for this capture).

For one explicit comparison configuration:

```sh
./build/benchmark/apex_benchmark --workload SAVED_WORKLOAD --output results/raw/NEW_RUN --repetitions 5 --warmup 1 --price-index tree --risk-traversal direct --event-reservation worst-case
```

`baseline_engine_options()` and CLI snapshot/tree/worst-case reproduce the
reference physical policies. `scripts/run_baseline.sh` now passes those flags
explicitly; the engine and benchmark CLI otherwise default to direct traversal.
Expect timing variation, not byte-identical times. Compare current policies with
fresh paired controls rather than dividing by the historical Phase 7 number.
