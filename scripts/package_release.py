#!/usr/bin/env python3
"""Package a fully verified snapshot and its evidence, without changing Git or publishing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

PRESETS = ('debug', 'asan', 'tsan', 'benchmark')
REQUIRED = {f'{preset}-{stage}' for preset in PRESETS for stage in ('configure', 'build', 'tests')}
REQUIRED.update(('cmake-version', 'compiler-version', 'node-version', 'npm-version', 'ui-build', 'ui-tests',
                 'generate-workload', 'benchmark', 'benchmark-audit', 'source-configure', 'source-build',
                 'source-tests', 'source-ui-install', 'source-ui-build', 'source-ui-tests', 'package-tests', 'source-package-tests', 'portfolio-tests', 'source-portfolio-tests'))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def package(evidence, destination, version=None):
    evidence, destination = Path(evidence).resolve(), Path(destination).resolve()
    if version is not None and not re.fullmatch(r'\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?', version):
        raise ValueError('Use a semantic version such as 0.1.0')
    if destination.exists() or destination.with_suffix(destination.suffix + '.sha256').exists():
        raise FileExistsError('Release destination or checksum already exists')

    hashes = None

    def read(relative):
        path = (evidence / relative).resolve()
        if not path.is_relative_to(evidence):
            raise ValueError('Evidence path escapes its directory')
        data = path.read_bytes()
        if hashes is not None and sha256(data) != hashes.get(relative):
            raise ValueError(f'Evidence file changed since verification: {relative}')
        return data

    manifest_bytes = read('verification.json')
    manifest = json.loads(manifest_bytes)
    if manifest.get('schema_version') != 1 or manifest.get('passed') is not True or manifest.get('mode') != 'full':
        raise ValueError('Packaging requires a passing full verification run')
    steps = manifest.get('steps', [])
    names = [step['name'] for step in steps]
    if len(set(names)) != len(names) or not REQUIRED.issubset(names) or any(step.get('returncode') != 0 for step in steps):
        raise ValueError('Verification is missing required checks or contains failed/duplicate steps')
    hashes = manifest.get('files_sha256')
    if not isinstance(hashes, dict):
        raise ValueError('Verification must include evidence file hashes; rerun the current verification tool')
    source = read('source.zip')
    if sha256(source) != manifest.get('source_archive_sha256'):
        raise ValueError('Source archive SHA-256 does not match verification')
    # Confirm version from the tested archive, not the possibly newer working tree.
    import io
    with zipfile.ZipFile(io.BytesIO(source)) as archive:
        if archive.testzip() is not None:
            raise ValueError('Source archive failed CRC verification')
        cmake = archive.read('CMakeLists.txt').decode()
        match = re.search(r'project\(Apex\s+VERSION\s+(\d+\.\d+\.\d+)', cmake)
        if version is None and match: version = match.group(1)
        if not match or match.group(1) != version.split('-')[0]:
            raise ValueError('Requested release version differs from the verified source')

    payload = {'source.zip': source, 'evidence/verification.json': manifest_bytes}
    for step in steps:
        name = step['name']
        if not re.fullmatch(r'[a-z0-9-]+', name) or step.get('log') != name + '.log':
            raise ValueError('Invalid verification log name')
        payload['evidence/' + step['log']] = read(step['log'])
    payload['evidence/input.workload'] = read('input.workload')
    benchmark = evidence / 'benchmark'
    for required in ('results.json', 'runs.csv', 'samples.csv', 'input.workload'):
        payload['evidence/benchmark/' + required] = read('benchmark/' + required)
    # Preserve any additional benchmark metadata alongside the audited raw samples.
    for path in sorted(benchmark.rglob('*')):
        if path.is_file():
            relative = path.relative_to(evidence).as_posix()
            payload['evidence/' + relative] = read(relative)
    payload['START_HERE.md'] = f'''# Apex {version} — local release candidate

This bundle contains the tested source snapshot, full local verification logs,
raw benchmark results and checksums. It does not contain compiled executables or
runtime dependencies. The snapshot was verified on {manifest['platform']}.

## Build and demonstrate

Unzip source.zip into a new folder, open a terminal there, then run:

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
npm ci --prefix ui
npm --prefix ui run build
npm --prefix ui start
```

Use C++20, CMake 3.20+, Ninja, Git, Python 3 and Node.js 24+. No venv is needed.
Open http://127.0.0.1:8080. In a second terminal run `npm --prefix ui run demo`
to submit real simulator orders. The dashboard includes dark and cream themes.
See docs/release.md and docs/dashboard.md inside source.zip for the walkthrough.

## Evidence and scope

The full matrix covers Debug, ASAN/UBSAN, native TSAN, Release, UI/bridge tests,
journal replay, source-archive reproduction and a five-run benchmark audit.
Clean-source dependency mode: {manifest.get('clean_source_dependency', 'not recorded')}.
Core benchmark numbers exclude network/journal/browser time. The archive packages
the verified snapshot; later working-tree edits are not implicitly included.

Hosted clean-clone CI, release tagging and visual review of the cream theme remain
outstanding. This is a local candidate, not a published or tagged release.
No external market feed is connected. See docs/status.md for implementation limits.

SHA256SUMS covers every payload file except itself. Verify with `shasum -a 256 -c
SHA256SUMS` from this extracted bundle. The accompanying .zip.sha256 checks this
outer ZIP. Hashes detect accidental changes; they are not cryptographic signatures.
'''.encode()
    payload['SHA256SUMS'] = ''.join(f'{sha256(data)}  {name}\n' for name, data in sorted(payload.items())).encode()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(destination) as archive:
        if archive.testzip() is not None:
            raise ValueError('Packaged release failed CRC verification')
    checksum = sha256(destination.read_bytes())
    destination.with_suffix(destination.suffix + '.sha256').write_text(f'{checksum}  {destination.name}\n')
    return {'path': str(destination), 'sha256': checksum, 'files': len(payload), 'source_sha256': manifest['source_sha256']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path, help='Passed full verify_release.py --clean-source output')
    parser.add_argument('--output', required=True, type=Path, help='New ZIP destination')
    parser.add_argument('--version', help='Defaults to the version in the verified source')
    args = parser.parse_args()
    try:
        print(json.dumps(package(args.evidence, args.output, args.version), indent=2))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Packaging failed: {error}\n')


if __name__ == '__main__':
    main()
