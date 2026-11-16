"""Release packaging integrity/admission checks; Python standard library only."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package_release', Path(__file__).resolve().parents[1] / 'scripts/package_release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleasePackaging(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.evidence = self.root / 'evidence'
        self.evidence.mkdir()
        source = io.BytesIO()
        with zipfile.ZipFile(source, 'w') as archive:
            archive.writestr('CMakeLists.txt', 'project(Apex VERSION 0.1.0 LANGUAGES CXX)')
        (self.evidence / 'source.zip').write_bytes(source.getvalue())
        self.manifest = {'schema_version': 1, 'passed': True, 'mode': 'full', 'platform': 'test',
                         'source_sha256': 'test-source', 'source_archive_sha256': module.sha256(source.getvalue()),
                         'steps': [{'name': name, 'returncode': 0, 'log': name + '.log'} for name in sorted(module.REQUIRED)]}
        for step in self.manifest['steps']:
            (self.evidence / step['log']).write_text('passed')
        (self.evidence / 'input.workload').write_text('input')
        (self.evidence / 'benchmark').mkdir()
        for name in ['results.json', 'runs.csv', 'samples.csv', 'input.workload']:
            (self.evidence / 'benchmark' / name).write_text(name)
        self.save()
        self.output = self.root / 'release.zip'

    def save(self):
        self.manifest['files_sha256'] = {p.relative_to(self.evidence).as_posix(): module.sha256(p.read_bytes())
                                         for p in self.evidence.rglob('*') if p.is_file() and p.name != 'verification.json'}
        (self.evidence / 'verification.json').write_text(json.dumps(self.manifest))

    def test_payload_checksums_and_no_overwrite(self):
        result = module.package(self.evidence, self.output)
        self.assertEqual(result['sha256'], module.sha256(self.output.read_bytes()))
        with zipfile.ZipFile(self.output) as archive:
            for line in archive.read('SHA256SUMS').decode().splitlines():
                checksum, name = line.split('  ', 1)
                self.assertEqual(checksum, module.sha256(archive.read(name)))
            self.assertIn('START_HERE.md', archive.namelist())
        with self.assertRaises(FileExistsError):
            module.package(self.evidence, self.output)

    def test_failed_quick_missing_or_duplicate_checks_are_rejected(self):
        original = json.loads(json.dumps(self.manifest))
        for mutate in [lambda m: m.update(passed=False), lambda m: m.update(mode='quick-without-sanitizers'),
                       lambda m: m['steps'].pop(), lambda m: m['steps'].append(m['steps'][0]),
                       lambda m: m['steps'][0].update(returncode=1)]:
            self.manifest = json.loads(json.dumps(original)); mutate(self.manifest); self.save()
            with self.assertRaises(ValueError):
                module.package(self.evidence, self.output)
            self.assertFalse(self.output.exists())

    def test_tampered_source_and_version_mismatch_are_rejected(self):
        with self.assertRaises(ValueError):
            module.package(self.evidence, self.output, '0.2.0')
        (self.evidence / 'source.zip').write_bytes(b'tampered')
        with self.assertRaises(ValueError):
            module.package(self.evidence, self.output)

    def test_changed_evidence_is_rejected(self):
        (self.evidence / 'benchmark/samples.csv').write_text('changed after verification')
        with self.assertRaises(ValueError):
            module.package(self.evidence, self.output)

    def test_log_escape_and_missing_samples_are_rejected(self):
        self.manifest['steps'][0]['log'] = '../outside.log'; self.save()
        with self.assertRaises(ValueError):
            module.package(self.evidence, self.output)
        self.manifest['steps'][0]['log'] = self.manifest['steps'][0]['name'] + '.log'; self.save()
        (self.evidence / 'benchmark/samples.csv').unlink()
        with self.assertRaises(FileNotFoundError):
            module.package(self.evidence, self.output)


if __name__ == '__main__':
    unittest.main()
