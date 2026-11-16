#!/usr/bin/env python3
"""Recompute reported benchmark statistics from saved raw samples (stdlib only)."""
import csv
import json
import math
import statistics
import sys
from pathlib import Path


def analyze(directory):
    report = json.loads((directory / 'results.json').read_text())
    if report['schema_version'] != 1:
        raise ValueError('Unsupported results schema')
    samples = {}
    with (directory / 'samples.csv').open(newline='') as stream:
        for row in csv.DictReader(stream):
            repetition = int(row['repetition'])
            bucket = samples.setdefault(repetition, [])
            if int(row['command_index']) != len(bucket):
                raise ValueError('Duplicate, missing or reordered sample index')
            value = int(row['latency_ns'])
            if value < 0:
                raise ValueError('Negative latency sample')
            bucket.append(value)
    with (directory / 'runs.csv').open(newline='') as stream:
        csv_runs = list(csv.DictReader(stream))
    if len(csv_runs) != len(report['runs']) or len(samples) != len(report['runs']):
        raise ValueError('Run/sample count mismatch')
    for run, csv_run in zip(report['runs'], csv_runs):
        ordered = sorted(samples[run['repetition']])
        count = len(ordered)
        if count != run['samples'] or count != run['commands']:
            raise ValueError('Sample/command count mismatch')
        for field in ['repetition', 'commands', 'rejects', 'trades', 'events', 'elapsed_ns', 'samples', 'min_ns', 'max_ns']:
            if int(csv_run[field]) != run[field]:
                raise ValueError(f'{field} differs between CSV and JSON')
        if int(csv_run['accepted']) != run['commands'] - run['rejects']:
            raise ValueError('Accepted command count differs between CSV and JSON')
        for field, numerator, denominator in [('p50_ns', 50, 100), ('p95_ns', 95, 100),
                                              ('p99_ns', 99, 100), ('p999_ns', 999, 1000)]:
            expected = ordered[(count * numerator + denominator - 1) // denominator - 1]
            if field == 'p999_ns' and count < report['measurement']['p999_min_samples']:
                expected = None
            if run[field] != expected:
                raise ValueError(f'{field} differs from raw samples')
            csv_value = None if csv_run[field] == '' else int(csv_run[field])
            if csv_value != expected:
                raise ValueError(f'{field} differs between CSV and JSON')
        if run['min_ns'] != ordered[0] or run['max_ns'] != ordered[-1]:
            raise ValueError('Min/max differ from raw samples')
        expected = run['commands'] * 1e9 / run['elapsed_ns']
        accepted = (run['commands'] - run['rejects']) * 1e9 / run['elapsed_ns']
        if not math.isclose(run['throughput_ops_s'], expected, rel_tol=1e-12):
            raise ValueError('Throughput differs from elapsed time')
        if not math.isclose(run['accepted_ops_s'], accepted, rel_tol=1e-12):
            raise ValueError('Accepted throughput differs from elapsed time')
        for field in ['throughput_ops_s', 'accepted_ops_s']:
            if not math.isclose(float(csv_run[field]), run[field], rel_tol=1e-12):
                raise ValueError(f'{field} differs between CSV and JSON')
    median_throughput = statistics.median(run['throughput_ops_s'] for run in report['runs'])
    if not math.isclose(median_throughput, report['summary']['median_throughput_ops_s'], rel_tol=1e-12):
        raise ValueError('Summary median differs from repetitions')
    for field in ['p50_ns', 'p95_ns', 'p99_ns', 'p999_ns']:
        values = [run[field] for run in report['runs']]
        expected = None if values[0] is None else statistics.median(values)
        if report['summary'][f'median_run_{field}'] != expected:
            raise ValueError(f'Summary {field} median differs from repetitions')
    print(f"Verified {len(report['runs'])} runs and {sum(map(len, samples.values()))} raw samples.")
    print(f"Median throughput: {median_throughput:,.0f} commands/s")
    print(f"CPU: {report['machine']['cpu']} ({report['machine']['architecture']})")
    print('Scope:', report['measurement']['scope'])
    return report


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: python3 tools/analyze_results.py RESULTS_DIRECTORY')
    try:
        analyze(Path(sys.argv[1]))
    except (ValueError, KeyError, OSError, ZeroDivisionError) as error:
        raise SystemExit(f'Invalid benchmark results: {error}') from error
