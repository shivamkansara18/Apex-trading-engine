#!/usr/bin/env bash
set -euo pipefail

apex_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$apex_root"
cmake --preset benchmark
cmake --build --preset benchmark --parallel 4
ctest --preset benchmark

if [[ $# -gt 2 ]]; then
  echo 'Usage: bash scripts/run_baseline.sh [SAVED_WORKLOAD] [NEW_OUTPUT_DIRECTORY]' >&2
  exit 1
fi
mkdir -p results/raw
apex_output="${2:-results/raw/baseline-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
if [[ $# -ge 1 ]]; then
  apex_workload="$1"
else
  mkdir -p tmp
  apex_temp="$(mktemp -d "$apex_root/tmp/benchmark.XXXXXX")"
  apex_workload="$apex_temp/input.workload"
  ./build/benchmark/apex --generate-workload "$apex_workload" --seed 42 --commands 100000 --profile balanced
fi
./build/benchmark/apex_benchmark --workload "$apex_workload" --output "$apex_output" --repetitions 5 --warmup 1 \
  --price-index tree --risk-traversal snapshot --event-reservation worst-case
