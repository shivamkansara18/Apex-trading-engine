#!/usr/bin/env python3
"""Validate portable experiment arithmetic, CSV agreement and measured source identity.
Raw sample re-auditing requires the separately retained bundles and exporter.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import zipfile

POLICIES = {'baseline':('tree','snapshot','worst-case'),'direct-risk':('tree','direct','worst-case'),
            'flat-prices':('flat','snapshot','worst-case'),'small-events':('tree','snapshot','small'),
            'direct-flat':('flat','direct','worst-case')}
PROFILES = ('balanced','many-levels','large-partial')


def validate(directory):
    directory=Path(directory)
    data=json.loads((directory/'optimization.json').read_text())
    if data['schema_version']!=1 or data['build']['sanitized'] or data['bundles']!=75:
        raise ValueError('Invalid experiment identity')
    records=data['records']
    expected={(profile,policy,block) for profile in PROFILES for policy in POLICIES for block in range(1,6)}
    keys=[(r['profile'],r['policy'],r['block']) for r in records]
    if len(keys)!=75 or set(keys)!=expected:
        raise ValueError('Incomplete or duplicate policy blocks')
    for record in records:
        run=record['run'];engine=record['engine'];index,risk,reservation=POLICIES[record['policy']]
        if engine!={'price_index':index,'risk_traversal':risk,'event_reservation':reservation}:
            raise ValueError('Mislabeled policy')
        if run['commands']!=100000 or run['samples']!=100000 or run['elapsed_ns']<=0:
            raise ValueError('Invalid timing/sample count')
        if not math.isclose(run['throughput_ops_s'],run['commands']*1e9/run['elapsed_ns'],rel_tol=1e-12):
            raise ValueError('Throughput arithmetic mismatch')
        for name in ('input.workload','results.json','runs.csv','samples.csv'):
            digest=record['files_sha256'][name]
            if len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest):
                raise ValueError('Invalid raw artifact digest')
    if sum(r['run']['samples'] for r in records)!=data['raw_samples_audited'] or data['raw_samples_audited']!=7500000:
        raise ValueError('Audited sample total mismatch')
    groups={(g['profile'],g['policy']):g for g in data['groups']}
    if len(data['groups'])!=15 or len(groups)!=15:
        raise ValueError('Incorrect group count')
    with (directory/'optimization.csv').open(newline='') as stream:
        rows=list(csv.DictReader(stream))
    csv_groups={(r['profile'],r['policy']):r for r in rows}
    if len(rows)!=15 or set(csv_groups)!=set(groups):
        raise ValueError('CSV groups mismatch')
    for profile in PROFILES:
        selected=[r for r in records if r['profile']==profile]
        signatures={(r['workload_hash'],r['run']['event_hash'],tuple(r['run']['book_hashes']),
                     r['run']['events'],r['run']['trades'],r['run']['rejects']) for r in selected}
        if len(signatures)!=1:
            raise ValueError('Correctness signatures differ across policies')
        controls={r['block']:r for r in selected if r['policy']=='baseline'}
        for policy in POLICIES:
            runs=sorted((r for r in selected if r['policy']==policy),key=lambda r:r['block'])
            ratios=[r['run']['throughput_ops_s']/controls[r['block']]['run']['throughput_ops_s'] for r in runs]
            group=groups[profile,policy]
            if group['paired_ratios']!=ratios:
                raise ValueError('Pairing mismatch')
            calculated={'blocks':5,'median_paired_ratio':statistics.median(ratios),
                        'median_throughput_ops_s':statistics.median(r['run']['throughput_ops_s'] for r in runs),
                        **{field:statistics.median(r['run'][field] for r in runs) for field in ('p50_ns','p95_ns','p99_ns','p999_ns')}}
            for field,value in calculated.items():
                if not math.isclose(group[field],value,rel_tol=1e-12) or not math.isclose(float(csv_groups[profile,policy][field]),value,rel_tol=1e-12):
                    raise ValueError(f'Summary/CSV {field} mismatch')
    source=directory/'measured-source.zip'
    if hashlib.sha256(source.read_bytes()).hexdigest()!=data['measured_source_zip_sha256']:
        raise ValueError('Measured source ZIP changed')
    with zipfile.ZipFile(source) as archive:
        names=[name for name in archive.namelist() if
               (name.startswith('include/') and name.endswith('.hpp')) or
               (name.startswith('src/') and name.endswith(('.cpp','.hpp'))) or
               (name.startswith('benchmarks/') and name.endswith(('.cpp','.cmake','.in'))) or
               name in ('CMakeLists.txt','CMakePresets.json')]
        fingerprint=''.join(name+':'+hashlib.sha256(archive.read(name)).hexdigest()+'\n' for name in sorted(names))
    if hashlib.sha256(fingerprint.encode()).hexdigest()!=data['build']['source_sha256']:
        raise ValueError('Measured source fingerprint mismatch')
    return data


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',nargs='?',type=Path,default=Path(__file__).resolve().parents[1]/'results/summary')
    args=parser.parse_args()
    try:
        result=validate(args.directory)
        print('Portfolio evidence verified: 75 recorded bundles, paired arithmetic, CSV and measured source.')
        print('Raw sample rows are retained separately; use export_portfolio_evidence.py to re-audit them.')
    except (OSError,ValueError,KeyError,zipfile.BadZipFile) as error:parser.exit(1,f'Portfolio evidence invalid: {error}\n')
