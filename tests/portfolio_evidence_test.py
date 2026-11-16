import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('portfolio_evidence',ROOT/'tools/check_portfolio_evidence.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class PortfolioEvidence(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.folder=Path(self.temp.name)/'summary';shutil.copytree(ROOT/'results/summary',self.folder)
        self.path=self.folder/'optimization.json';self.data=json.loads(self.path.read_text())
    def save(self):self.path.write_text(json.dumps(self.data))
    def test_real_summary_validates(self):self.assertEqual(module.validate(self.folder)['raw_samples_audited'],7500000)
    def test_pairing_and_correctness_tampering_fail(self):
        self.data['records'][0]['run']['event_hash']='changed';self.save()
        with self.assertRaises(ValueError):module.validate(self.folder)
    def test_csv_or_source_changes_fail(self):
        csv=self.folder/'optimization.csv';original=csv.read_text();csv.write_text(original.replace(',1.0,',',9.0,',1))
        with self.assertRaises(ValueError):module.validate(self.folder)
        csv.write_text(original);(self.folder/'measured-source.zip').write_bytes(b'changed')
        with self.assertRaises(ValueError):module.validate(self.folder)

if __name__=='__main__':unittest.main()
