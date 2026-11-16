# Initial macOS baseline

Phase 7 establishes an initial measurement, before optimization. These results
are specific to the saved balanced workload and synchronous engine-call scope.
They are not network, queue, journal, trade-only or exchange-grade latency claims.

Local artifacts: [results.json](../results/raw/phase7-macos-baseline/results.json),
[runs.csv](../results/raw/phase7-macos-baseline/runs.csv),
[raw latency samples](../results/raw/phase7-macos-baseline/samples.csv),
[saved input](../results/raw/phase7-macos-baseline/input.workload).
Raw directories are intentionally gitignored; retain this bundle when sharing
results or regenerate it with the documented commands.

## Host, build and workload

- Apple M5, arm64, 10 logical CPUs, 17,179,869,184 bytes RAM.
- Darwin 27.2.0; uncontrolled core scheduling/affinity and CPU governor.
- AppleClang 21.0.0.21000101, `/usr/bin/c++`, Release, `-O3 -DNDEBUG`, C++20;
  warnings enabled; no sanitizers. Exact flags are in JSON.
- Git state: uncommitted/dirty. Source SHA-256:
  `14460ea1e0dde677bf7e2bc99dbe4ecd34cdd3388c0edcde895a48c95fe24bec`.
- Balanced seed 42, 100,000 commands, one symbol, six participants, price center
  10,000 ticks, width 20, sizes 1..100. Cancel/replace action probabilities
  20%/10%; random-flow market probability 30%. Logical schedule is ignored.
- One complete warm-up pass, five repetitions. Each repetition uses separate
  fresh engines for throughput and per-command latency. 500,000 raw latency
  samples total, 100,000 per repetition. No pooling for per-run percentiles.
- Each run has zero rejects and 37,649 trades; output hashes and final books
  agree with the untimed correctness run.

## Observed results

| Metric | Median of five runs |
| --- | ---: |
| Submitted/accepted throughput | 4,798,100 commands/s |
| Synchronous engine-call p50 | 167 ns |
| p95 | 458 ns |
| p99 | 583 ns |
| p99.9 | 750 ns |

Throughput ranged from 4,751,064 to 4,872,760 commands/s. Bulk timing intervals
were approximately 20.5..21.0 ms. Per-run maximum observed command durations
ranged from 6,166 to 17,542 ns. Process peak RSS was 14,139,392 bytes, including
all harness input/sample storage and untimed verification work.

Calibration recorded 761 zero-duration clock-read pairs out of 1,000, a median
of zero, and a minimum positive pair of 41 ns. These numbers show the clock's
quantization at this scale; they do not imply a zero-cost timer. No overhead was
subtracted. The reported nanosecond quantiles are quantized observations.

The default mixed stream includes fast cancels, empty market orders, self-trade
cancellations and actual fills. Its p50 is not the latency of a trade-producing
order specifically. The short bulk intervals, uncontrolled macOS scheduling,
allocator/cache effects and untimed hashing between latency samples limit how
far these results generalize. Repeated runs on another host are not directly
comparable.

## Reproduction and verification

```sh
bash scripts/run_baseline.sh
python3 tools/analyze_results.py RESULTS_DIRECTORY_PRINTED_BY_SCRIPT
```

The script exports a fresh bundle with the exact inputs, metadata, all repetitions
and raw samples. The analysis tool independently reproduced the quantiles,
throughput arithmetic and summary medians for the linked local bundle. Numeric
results can vary with host load; reproduce the experiment, not an identical time.
See [methodology](benchmark-methodology.md) for exact timing boundaries.

This is the historical pre-optimization capture. Phase 8 now profiles and compares
fresh controls on these saved inputs; see [optimization experiments](optimization-experiments.md).
Its ratios use paired controls from the same experiment binary rather than this
older baseline number.
