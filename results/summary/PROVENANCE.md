# Optimization evidence

These compact files are intended to travel with the source tree. They report the
historical Phase 8 experiment, not the latest application's network performance.

- `optimization.csv`: all 15 workload/policy summary rows.
- `optimization.json`: host/build identity, five per-block ratios, all 75 run
  records, hashes of every input/report/raw-sample file, limitations and source hashes.
- `measured-source.zip`: exact source snapshot used for those comparisons, before
  later default-policy promotion and networking/UI additions.

`tools/export_portfolio_evidence.py` re-read every bundle with the independent
CSV/JSON/sample analyzer, checked elapsed-time/throughput/quantile arithmetic,
paired results by block, compared workload/event/final-state signatures across
policies and recomputed the measured CMake source fingerprint from the archive.
It audited 75 bundles and 7,500,000 latency samples before exporting these files.
The source zip has no downloaded dependencies, build output or implementation PDF.

The full local raw capture is `results/raw/phase8-experiments` (~455 MiB), which
is intentionally excluded from source distribution. Its absence in a fresh checkout
is expected: hashes and run records are included here, not the 7.5 million raw rows.
Raw rows can be retained/shared separately. To re-audit the retained capture:

```sh
python3 tools/export_portfolio_evidence.py results/raw/phase8-experiments --output tmp/NEW_AUDIT_DIRECTORY
```

To generate another experiment, use `bash scripts/run_experiments.sh`. It creates
new inputs/results and measured source, which will differ from the historical
source and timings. Reproduce the controlled procedure, not identical timings.

Scope: synchronous C++ engine calls, excluding sockets, queueing, journals and UI.
Host: Apple M5 arm64, AppleClang 21, unsanitized Release. Controls: three saved
100,000-command synthetic inputs, five interleaved paired blocks, five policies,
untimed warmup and separate bulk-throughput/per-command-latency passes. Each run
was checked against untimed correctness outputs. Ratios are medians of paired
candidate/control throughput ratios, not ratios of throughput medians.

macOS scheduling/affinity is uncontrolled; clocks are quantized. These five blocks
are not independent market samples. No confidence interval, statistical significance,
network latency, live-market realism or cross-machine speed guarantee is claimed.
