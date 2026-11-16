#!/usr/bin/env bash
set -euo pipefail
apex_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$apex_root"
apex_workload="${1:-results/raw/phase7-macos-baseline/input.workload}"
mkdir -p results/raw
apex_output="$(mktemp -d "$apex_root/results/raw/profile.XXXXXX")"
./build/benchmark/apex_profile "$apex_workload" 10 "${2:-baseline}" > "$apex_output/driver.txt" 2>&1 &
apex_pid=$!
trap 'kill "$apex_pid" 2>/dev/null || true' EXIT
/usr/bin/sample "$apex_pid" 5 1 -file "$apex_output/sample.txt"
wait "$apex_pid"
trap - EXIT
echo "Profile artifacts: $apex_output"
