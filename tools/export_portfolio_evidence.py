#!/usr/bin/env python3
"""Re-audit paired raw benchmarks and export a compact, shareable historical evidence summary."""
import argparse
import contextlib
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import statistics
import zipfile
from analyze_results import analyze


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    experiment_path = source / 'experiments.json'
    experiment = json.loads(experiment_path.read_text())
    if experiment['schema_version'] != 1 or experiment['build']['sanitized']:
        raise ValueError('Expected unsanitized version-1 paired experiments')
    records = experiment['records']
    if len(records) != 75 or len({(r['profile'],r['block'],r['policy']) for r in records}) != 75:
        raise ValueError('Expected 3 inputs x 5 policies x 5 distinct blocks')
    # Recompute the exact build fingerprint from the measured archive's own members.
    with zipfile.ZipFile(source / experiment['source_archive']) as archive:
        names = [name for name in archive.namelist() if
                 (name.startswith('include/') and name.endswith('.hpp')) or
                 (name.startswith('src/') and name.endswith(('.cpp','.hpp'))) or
                 (name.startswith('benchmarks/') and name.endswith(('.cpp','.cmake','.in'))) or
                 name in ('CMakeLists.txt','CMakePresets.json')]
        fingerprint = ''.join(name + ':' + hashlib.sha256(archive.read(name)).hexdigest() + '\n' for name in sorted(names))
    if hashlib.sha256(fingerprint.encode()).hexdigest() != experiment['build']['source_sha256']:
        raise ValueError('Measured source fingerprint does not match archive')
    verified = []; sample_count = 0
    for record in records:
        relative = Path(record['bundle'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid bundle path')
        folder = source / relative
        with contextlib.redirect_stdout(io.StringIO()):
            report = analyze(folder)
        if report['build'] != experiment['build'] or report['engine'] != record['engine']:
            raise ValueError('Build/policy mismatch')
        for field, value in experiment['machine'].items():
            if report['machine'][field] != value:
                raise ValueError('Machine changed between bundles')
        if len(report['runs']) != 1 or report['runs'][0] != record['run'] or report['workload']['hash_fnv1a64'] != record['workload_hash']:
            raise ValueError('Recorded experiment differs from raw bundle')
        if record['run']['commands'] != 100000 or record['run']['samples'] != 100000:
            raise ValueError('Expected 100,000-command measured inputs')
        sample_count += record['run']['samples']
        verified.append({**record,'files_sha256':{name:digest(folder/name) for name in ('input.workload','results.json','runs.csv','samples.csv')}})
    groups = []
    for profile in ('balanced','many-levels','large-partial'):
        inputs = [r for r in verified if r['profile'] == profile]
        signatures = {(r['workload_hash'],r['run']['event_hash'],tuple(r['run']['book_hashes']),r['run']['trades'],r['run']['events'],r['run']['rejects']) for r in inputs}
        if len(signatures) != 1:
            raise ValueError('Policies have different correctness outcomes')
        controls = {r['block']:r for r in inputs if r['policy'] == 'baseline'}
        for policy in experiment['policies']:
            selected = sorted((r for r in inputs if r['policy'] == policy),key=lambda r:r['block'])
            if len(selected) != 5 or set(controls) != {r['block'] for r in selected}:
                raise ValueError('Incomplete paired comparison')
            ratios = [r['run']['throughput_ops_s']/controls[r['block']]['run']['throughput_ops_s'] for r in selected]
            speeds = [r['run']['throughput_ops_s'] for r in selected]
            result = {'profile':profile,'policy':policy,'blocks':5,'median_throughput_ops_s':statistics.median(speeds),
                      'paired_ratios':ratios,'median_paired_ratio':statistics.median(ratios),
                      **{field:statistics.median(r['run'][field] for r in selected) for field in ('p50_ns','p95_ns','p99_ns','p999_ns')}}
            original = next(g for g in experiment['groups'] if g['profile'] == profile and g['policy'] == policy)
            if not math.isclose(result['median_paired_ratio'],original['median_paired_throughput_ratio'],rel_tol=1e-12):
                raise ValueError('Published paired ratio is inconsistent')
            groups.append(result)
    summary = {'schema_version':1,'scope':'Historical synchronous engine calls; excludes network, queueing, journal and UI',
               'build':experiment['build'],'machine':experiment['machine'],'bundles':75,'raw_samples_audited':sample_count,
               'method':experiment['method'],'experiment_sha256':digest(experiment_path),
               'measured_source_zip_sha256':digest(source/experiment['source_archive']),
               'groups':groups,'records':verified,
               'limitations':['Uncontrolled macOS scheduling/affinity','Five paired blocks, not independent market samples',
                              'No confidence interval or statistical-significance claim','Seeded synthetic inputs, not real market data',
                              'Historical measured source is distinct from the current application version']}
    if destination.exists():
        raise FileExistsError('Choose a new output directory')
    destination.mkdir(parents=True)
    (destination/'optimization.json').write_text(json.dumps(summary,indent=2)+'\n')
    with (destination/'optimization.csv').open('w',newline='') as stream:
        writer = csv.DictWriter(stream,fieldnames=['profile','policy','blocks','median_throughput_ops_s','median_paired_ratio','p50_ns','p95_ns','p99_ns','p999_ns'])
        writer.writeheader(); writer.writerows({key:g[key] for key in writer.fieldnames} for g in groups)
    print(f'Audited {len(records)} bundles and {sample_count:,} raw samples. Summary: {destination}')
    return summary


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    try: export(args.source,args.output)
    except (ValueError,KeyError,OSError,zipfile.BadZipFile) as error:parser.exit(1,f'Evidence export failed: {error}\n')
