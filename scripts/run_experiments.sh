#!/usr/bin/env bash
set -euo pipefail
apex_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$apex_root"
cmake --preset benchmark
cmake --build --preset benchmark --parallel 4
ctest --preset benchmark
mkdir -p results/raw
apex_output="${1:-results/raw/experiments-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
python3 tools/run_experiments.py --output "$apex_output"
