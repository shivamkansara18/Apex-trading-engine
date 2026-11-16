#!/usr/bin/env python3
"""Interleaved, paired physical-policy comparisons. No third-party modules."""
import argparse
import contextlib
import io
import json
import random
import shutil
import statistics
import subprocess
import zipfile
from pathlib import Path

from analyze_results import analyze

POLICIES = {
    'baseline': ('tree', 'snapshot', 'worst-case'),
    'direct-risk': ('tree', 'direct', 'worst-case'),
    'flat-prices': ('flat', 'snapshot', 'worst-case'),
    'small-events': ('tree', 'snapshot', 'small'),
    'direct-flat': ('flat', 'direct', 'worst-case'),
}


def command(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/benchmark/apex_benchmark'))
    parser.add_argument('--apex', type=Path, default=Path('build/benchmark/apex'))
    parser.add_argument('--balanced', type=Path, help='Existing balanced input; by default reuse the Phase 7 capture or generate seed 42')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--blocks', type=int, default=5)
    parser.add_argument('--commands', type=int, default=100000,
                        help='Command count for the generated many-levels and large-partial inputs')
    args = parser.parse_args()
    if not 5 <= args.blocks <= 10 or not 17 <= args.commands <= 100000:
        parser.error('Use 5..10 paired blocks and 17..100000 generated commands')
    args.binary = args.binary.resolve()
    args.apex = args.apex.resolve()
    args.output.mkdir()  # Never overwrite a previous experiment.
    # Keep the measured source, even if a successful policy becomes default later.
    root = Path(__file__).resolve().parents[1]
    with zipfile.ZipFile(args.output / 'source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ['include', 'src', 'benchmarks', 'tests', 'examples', 'scripts', 'tools', 'docs', '.github']:
            for file in sorted((root / name).rglob('*')):
                if file.is_file() and '__pycache__' not in file.parts:
                    archive.write(file, file.relative_to(root))
        for name in ['CMakeLists.txt', 'CMakePresets.json', '.gitignore']:
            archive.write(root / name, name)
    inputs = args.output / 'inputs'
    inputs.mkdir()
    balanced = args.balanced or Path('results/raw/phase7-macos-baseline/input.workload')
    if balanced.exists() or args.balanced is not None:
        shutil.copyfile(balanced, inputs / 'balanced.workload')
    else:
        command([args.apex, '--generate-workload', inputs / 'balanced.workload',
                 '--seed', '42', '--commands', args.commands, '--profile', 'balanced'])
    for profile in ['many-levels', 'large-partial']:
        command([args.apex, '--generate-workload', inputs / f'{profile}.workload',
                 '--seed', '42', '--commands', args.commands, '--profile', profile])
    randomizer = random.Random(42)
    records = []
    identity = None
    for profile in ['balanced', 'many-levels', 'large-partial']:
        for block in range(args.blocks):
            order = list(POLICIES)
            randomizer.shuffle(order)
            print(f'{profile}: paired block {block + 1}/{args.blocks}, order={order}', flush=True)
            for policy in order:
                index, risk, events = POLICIES[policy]
                destination = args.output / f'{profile}-{block + 1}-{policy}'
                stdout = command([args.binary, '--workload', inputs / f'{profile}.workload', '--output', destination,
                                  '--repetitions', '1', '--warmup', '1', '--price-index', index,
                                  '--risk-traversal', risk, '--event-reservation', events])
                (destination / 'cli.txt').write_text(stdout)
                with contextlib.redirect_stdout(io.StringIO()):
                    report = analyze(destination)
                if report['engine'] != {'price_index': index, 'risk_traversal': risk, 'event_reservation': events}:
                    raise ValueError('Benchmark executable did not apply the requested policy')
                actual_identity = (report['build'], {key: report['machine'][key] for key in
                                                    ['os', 'kernel', 'architecture', 'cpu', 'ram_bytes', 'logical_cpus']})
                if identity is None:
                    identity = actual_identity
                if identity != actual_identity:
                    raise ValueError('Build or machine changed during experiment')
                records.append({'profile': profile, 'block': block + 1, 'policy': policy,
                                'bundle': destination.name, 'workload_hash': report['workload']['hash_fnv1a64'],
                                'engine': report['engine'], 'run': report['runs'][0],
                                'process_peak_rss_bytes': report['machine']['process_peak_rss_bytes']})
                (args.output / 'progress.json').write_text(json.dumps(records, indent=2) + '\n')
    groups = []
    for profile in ['balanced', 'many-levels', 'large-partial']:
        baseline = [item for item in records if item['profile'] == profile and item['policy'] == 'baseline']
        for policy in POLICIES:
            selected = [item for item in records if item['profile'] == profile and item['policy'] == policy]
            signatures = [(item['workload_hash'], item['run']['event_hash'], item['run']['book_hashes'],
                           item['run']['events'], item['run']['rejects'], item['run']['trades']) for item in selected + baseline]
            if any(signature != signatures[0] for signature in signatures):
                raise ValueError('Workload/events/state changed between physical policies')
            ratios = [item['run']['throughput_ops_s'] / baseline[index]['run']['throughput_ops_s']
                      for index, item in enumerate(selected)]
            speeds = [item['run']['throughput_ops_s'] for item in selected]
            quantiles = {field: None if selected[0]['run'][field] is None else
                         statistics.median(item['run'][field] for item in selected)
                         for field in ['p50_ns', 'p95_ns', 'p99_ns', 'p999_ns']}
            groups.append({'profile': profile, 'policy': policy, 'paired_blocks': args.blocks,
                           'median_throughput_ops_s': statistics.median(speeds),
                           'min_throughput_ops_s': min(speeds), 'max_throughput_ops_s': max(speeds),
                           'median_paired_throughput_ratio': statistics.median(ratios),
                           'paired_ratios': ratios, **quantiles,
                           'median_process_peak_rss_bytes': statistics.median(item['process_peak_rss_bytes'] for item in selected)})
    summary = {'schema_version': 1, 'build': identity[0], 'machine': identity[1], 'order_seed': 42,
               'source_archive': 'source.zip',
               'method': 'One full warm-up plus one fresh-book measurement per bundle; 5+ interleaved paired blocks per policy/workload; no profiler attached',
               'policies': POLICIES, 'groups': groups, 'records': records}
    (args.output / 'experiments.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'Verified {len(records)} bundles; comparison saved to {args.output / "experiments.json"}')
    for group in groups:
        print(f"{group['profile']:14} {group['policy']:13} {group['median_paired_throughput_ratio']:.3f}x "
              f"p99={group['p99_ns']} p999={group['p999_ns']}")


if __name__ == '__main__':
    main()
