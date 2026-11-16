# Baseline benchmark methodology (Phase 7)

The custom C++ harness measures the full synchronous matching API rather than an
isolated data-structure operation. This keeps risk validation, matching and event
allocation in the baseline. Google Benchmark microbenchmarks can be added for
profile-driven experiments later; this phase's throughput and latency harnesses
have no additional runtime dependency.

## One-command local run

```sh
bash scripts/run_baseline.sh
```

This configures the unsanitized Release `benchmark` preset, builds, tests,
generates a 100,000-command balanced workload with seed 42, then performs one
warm-up pass and five measured repetitions. Results go to a new unique directory
under `results/raw`. CMake/Ninja and Clang run natively on macOS; no venv is used.
Since Phase 8 the baseline script explicitly selects tree/snapshot/worst-case,
while the engine and benchmark CLI default to tree/direct/worst-case. JSON records
the physical policy. See [optimization experiments](optimization-experiments.md).
The first configure needs GitHub access if GoogleTest is not already available.

For an existing input file and new output directory:

```sh
bash scripts/run_baseline.sh examples/workloads/balanced.workload results/raw/my-smoke
```

The 100-command example is a functional smoke check, not a credible tail study.
To control repetitions directly:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --parallel 4
./build/benchmark/apex_benchmark --workload SAVED_FILE --output results/raw/NEW_RUN --repetitions 5 --warmup 1
python3 tools/analyze_results.py results/raw/NEW_RUN
```

The analyzer uses Python's standard library only, recomputes percentiles from all
saved samples, checks CSV/JSON agreement and throughput arithmetic, and verifies
the summary medians. Existing output directories are refused.

## Exact timing boundaries

Inputs are fully loaded and validated before timing. An untimed correctness run
computes expected events and final books. Warm-up consists of complete discarded
workload passes using fresh engines. Each measured pass also starts with fresh
empty engines and participants registered with default Phase 4 limits. Warm-up
warms code/cache/allocator paths; it does not prefill the measured book or skip a
prefix of the saved input. This avoids reusing live IDs across repeated inputs.

Each repetition contains two independent passes:

| Pass | Inside interval | Outside interval |
| --- | --- | --- |
| Throughput | Global dispatch loop, synchronous `process()`, returned event vector destruction | Engine construction/registration, generation/loading, final-state verification, statistics and output |
| Latency | One `process()` call, including validation, risk scans, matching and output-event construction | Dispatch selection, sample storage, event accounting/hash/destruction, final verification and file output |

Throughput is submitted commands * 1e9 / elapsed nanoseconds. A separate accepted
throughput excludes rejections. Rejects are still real work in the submitted
throughput metric. Trade/reject/event counts are taken from the verified latency
pass over the identical input. Each measured final book is compared with the
correctness run; the latency pass also compares canonical event hashes/counts.

Latency uses `std::chrono::steady_clock`. It is **synchronous engine-call
duration**, not client/server round trip, queue residence time or journal latency.
Input schedules are ignored: the client submits the next command after the
previous returns. This is closed-loop, unpaced traffic. It does not reveal
queueing under overload and must not be described as an open-loop/network result.

Per-command instrumentation and untimed hashing between latency samples perturb
cache/allocator behavior; the latency and throughput passes have distinct scopes
and must not be algebraically equated. No disk/console output occurs inside
either timed region. Clock-read/allocator overhead is not subtracted.

## Percentiles and repetitions

For sorted samples of size N, percentile p is element `ceil(p*N) - 1` (nearest
rank), with no interpolation. Report min, p50, p95, p99, p99.9, max and N for each
repetition. p99.9 is exported as null/blank below 100,000 samples **per run**;
samples from different repetitions are not pooled to meet that threshold.
Even above it, tail stability depends on the workload and environment.

Summary metrics are medians of the per-run metrics; even repetition counts use
the arithmetic mean of the two middle values. Throughput min/max across runs
provides a simple variability range. Raw samples allow independent recalculation
and alternative analysis. One or two repetitions are useful only as smoke checks.

Calibration records 1,000 back-to-back clock-read pairs: their median, minimum
positive duration, and zero count. Zero-duration pairs show timer quantization,
not a zero-cost clock. Submicrosecond figures on this Mac are visibly quantized;
the observed minimum positive pair is a diagnostic, not a guaranteed resolution
or a correction factor. No clock overhead is subtracted from command samples.

## Artifacts and identity

Each new output directory contains:

- `input.workload`: the exact parsed inputs and generator configuration.
- `results.json`: machine/build/methodology metadata, summaries and every run.
- `runs.csv`: one row per repetition with workload/build identifiers and metrics.
- `samples.csv`: every latency sample in command order, indexed by repetition.

JSON records CPU model, architecture, logical CPUs, physical RAM, OS/kernel,
governor/affinity where available, compiler/path/flags, build type, sanitizers,
UTC timestamp, git commit/dirty status and a source SHA-256 fingerprint. Metadata
is collected outside timing. Process peak RSS covers the entire benchmark
process (including loading, validation, warm-up, hashes and sample vectors), not
per-order engine memory. It is not an allocation count.

CMake fingerprints engine/header/app/benchmark source files and build templates
at configure time and reruns configuration when those files change. Git fields
are configure-time metadata; rerun configure after committing. `uncommitted`
means no commit exists, not a fake revision. Workload identity uses FNV-1a over
all config fields and canonical ordered inputs; it is not a cryptographic hash.
Input files are retained even if the generator changes later.

The benchmark executable refuses Debug and sanitizer builds. Metrics code also
builds under Debug/ASan for unit tests; temporary test exports are not performance
evidence. The `benchmark` preset uses Release flags recorded in each output.

## Interpreting the current baseline

Run on one documented host, close heavy applications, avoid concurrent builds,
and repeat before drawing optimization conclusions. macOS scheduling, DVFS,
thermal state and core selection are uncontrolled here. Linux affinity/governor
metadata is supported but no Linux result has been measured in this session.

The initial run has about 20 ms of bulk work per repetition, so scheduler noise
and background load remain relevant despite five repetitions. Results describe
this specific mixed workload (including fast cancels and empty markets), not
every order type or adversarial deep book. Do not label p50 a trade-only latency
or claim exchange-equivalent performance. Compare future changes using the same
saved inputs, machine, flags and methodology.

The historical Phase 7 result uses map/list storage, risk snapshot scans and
conservative event reservation. Phase 8 profiles and compares explicit physical
policies separately; it promotes direct risk reads and preserves the reference.
