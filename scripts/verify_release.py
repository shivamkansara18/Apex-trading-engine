#!/usr/bin/env python3
"""Verify local release inputs, retain logs/metadata and optionally test an extracted source archive."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='New artifact directory; defaults to tmp/release-TIMESTAMP-PID')
    parser.add_argument('--quick', action='store_true', help='Debug/Release only; explicitly excludes sanitizers')
    parser.add_argument('--clean-source', action='store_true', help='Also build/test extracted source and npm ci its UI')
    args = parser.parse_args()
    os.chdir(ROOT)
    output = (args.output or ROOT / 'tmp' / f'release-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{os.getpid()}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    summary = {'schema_version': 1, 'started_utc': datetime.now(timezone.utc).isoformat(),
               'platform': platform.platform(), 'machine': platform.machine(), 'python': sys.version,
               'mode': 'quick-without-sanitizers' if args.quick else 'full', 'steps': [], 'passed': False}

    def save():
        (output / 'verification.json').write_text(json.dumps(summary, indent=2) + '\n')

    def run(name, command, cwd=ROOT, env=None, timeout=600):
        print(f'[{name}] {" ".join(map(str, command))}', flush=True)
        started = time.monotonic()
        step = {'name': name, 'command': list(map(str, command)), 'seconds': None, 'returncode': None, 'log': name + '.log'}
        summary['steps'].append(step)
        with (output / step['log']).open('w') as log:
            try:
                result = subprocess.run(step['command'], cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                step['returncode'] = result.returncode
            finally:
                step['seconds'] = round(time.monotonic() - started, 3)
                save()
        if result.returncode:
            raise RuntimeError(f'{name} failed; inspect {output / step["log"]}')
        return (output / step['log']).read_text()

    try:
        run('cmake-version', ['cmake', '--version'])
        run('compiler-version', ['c++', '--version'])
        run('node-version', ['node', '--version'])
        run('npm-version', ['npm', '--version'])
        run('package-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'package_release_test.py', '-v'])
        run('portfolio-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'portfolio_evidence_test.py', '-v'])
        # Archive explicit source roots, never build output, dependencies or session artifacts.
        candidates = []
        for name in ['.github', 'benchmarks', 'docs', 'examples', 'include', 'scripts', 'src', 'tests', 'tools', 'ui', 'results/summary',
                     '.gitignore', 'CMakeLists.txt', 'CMakePresets.json', 'README.md', 'CHANGELOG.md']:
            entry = ROOT / name
            for file in entry.rglob('*') if entry.is_dir() else [entry]:
                relative = file.relative_to(ROOT)
                if any(part in {'node_modules', 'dist', '__pycache__', '.DS_Store'} for part in relative.parts):
                    continue
                if file.is_file() and not file.is_relative_to(output):
                    candidates.append(file)
        digest = hashlib.sha256()
        with zipfile.ZipFile(output / 'source.zip', 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(candidates):
                name = file.relative_to(ROOT).as_posix()
                content = file.read_bytes()
                digest.update(name.encode() + b'\0' + content + b'\0')
                archive.writestr(name, content)
        summary['source_sha256'] = digest.hexdigest()
        summary['source_archive_sha256'] = hashlib.sha256((output / 'source.zip').read_bytes()).hexdigest()
        summary['source_files'] = len(candidates)
        for preset in (['debug', 'benchmark'] if args.quick else ['debug', 'asan', 'tsan', 'benchmark']):
            run(f'{preset}-configure', ['cmake', '--preset', preset])
            run(f'{preset}-build', ['cmake', '--build', '--preset', preset, '--parallel', '4'])
            run(f'{preset}-tests', ['ctest', '--preset', preset, '--output-on-failure'])
        run('ui-build', ['npm', '--prefix', 'ui', 'run', 'build'])
        run('ui-tests', ['npm', '--prefix', 'ui', 'test'])
        executable = ROOT / 'build/benchmark/apex'
        workload = output / 'input.workload'
        run('generate-workload', [executable, '--generate-workload', workload, '--seed', '42', '--commands', '100000', '--profile', 'balanced'])
        run('benchmark', [ROOT / 'build/benchmark/apex_benchmark', '--workload', workload,
                          '--output', output / 'benchmark', '--repetitions', '5', '--warmup', '1'])
        run('benchmark-audit', [sys.executable, ROOT / 'tools/analyze_results.py', output / 'benchmark'])
        if args.clean_source:
            source = output / 'source'
            with zipfile.ZipFile(output / 'source.zip') as archive:
                archive.extractall(source)
            build = output / 'clean-build'
            command = ['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug']
            cached_gtest = ROOT / 'build/debug/_deps/googletest-src'
            if cached_gtest.is_dir():
                command.append(f'-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={cached_gtest}')
                summary['clean_source_dependency'] = 'Pinned GoogleTest source reused from local cache'
            else:
                summary['clean_source_dependency'] = 'CMake dependency discovery/fetch'
            run('source-configure', command)
            run('source-build', ['cmake', '--build', build, '--parallel', '4'])
            run('source-tests', ['ctest', '--test-dir', build, '--output-on-failure'])
            run('source-package-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'package_release_test.py', '-v'], cwd=source)
            run('source-portfolio-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'portfolio_evidence_test.py', '-v'], cwd=source)
            run('source-ui-install', ['npm', 'ci', '--prefix', source / 'ui', '--no-audit', '--no-fund'])
            run('source-ui-build', ['npm', '--prefix', source / 'ui', 'run', 'build'])
            run('source-ui-tests', ['npm', '--prefix', source / 'ui', 'test'], env={**os.environ, 'APEX_BUILD_DIR': str(build)})
        evidence_files = [output / 'source.zip', output / 'input.workload']
        evidence_files.extend(output / step['log'] for step in summary['steps'])
        evidence_files.extend(path for path in (output / 'benchmark').rglob('*') if path.is_file())
        summary['files_sha256'] = {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in sorted(evidence_files)}
        summary['passed'] = True
    except Exception as error:
        summary['error'] = str(error)
        print(error, file=sys.stderr)
    finally:
        summary['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(f'Verification {"PASSED" if summary["passed"] else "FAILED"}: {output}', flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
