#!/usr/bin/env python3
"""Synthetic instruction tests for the read-only TLS report (no game files)."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from nso import Nso
from scarlet_tls_report import direct_callers, make_report, tls_writes
from test_indirect_resume import write_fixture


class TlsReportTests(unittest.TestCase):
    def test_exact_write_decode_callers_and_read_only_report(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('main', 'sdk', 'subsdk0'):
                path = root / name
                write_fixture(path)
                raw = bytearray(path.read_bytes())
                # fixture NSO's .text begins at file offset 0x100
                # 100: BL 110; 104: MRS TPIDR_EL0 (must not be listed as a write)
                # 110: MSR TPIDR_EL0,X9; 114: MSR TPIDR_EL0,XZR
                for off, word in ((0x100, 0x94000004), (0x104, 0xD53BD040),
                                  (0x110, 0xD51BD049), (0x114, 0xD51BD05F)):
                    struct.pack_into('<I', raw, 0x100 + off, word)
                path.write_bytes(raw)
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir()}
            n = Nso(str(root / 'sdk'))
            self.assertEqual(tls_writes(n), [(0x110, 9), (0x114, 31)])
            self.assertEqual(direct_callers(n, {0x110})[0x110], [0x100])
            report, missing = make_report(root)
            self.assertEqual(missing, [])
            self.assertIn('[summary] MSR sites=6;', report)
            self.assertIn('source=xzr (explicit zero)', report)
            self.assertIn('source=x9', report)
            self.assertIn('caller 0x7006000100', report)
            self.assertIn('fixed-address focuses were skipped', report)
            self.assertNotIn('[logged SDK focus]', report)
            after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir()}
            self.assertEqual(before, after)

    def test_missing_modules_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write_fixture(root / 'main')
            report, missing = make_report(root)
            self.assertEqual(missing, ['sdk', 'subsdk0'])
            self.assertIn('[missing module] sdk', report)

    def test_cli_writes_report_and_rejects_output_over_game_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            for name in ('main', 'sdk', 'subsdk0'):
                write_fixture(exefs / name)
            script = Path(__file__).with_name('scarlet_tls_report.py')
            output = root / 'report.txt'
            command = [sys.executable, str(script), str(exefs), '--output']
            run = subprocess.run(command + [str(output)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('[summary]', output.read_text())
            original = (exefs / 'sdk').read_bytes()
            rejected = subprocess.run(command + [str(exefs / 'sdk')], capture_output=True, text=True)
            self.assertEqual(rejected.returncode, 2)
            self.assertEqual((exefs / 'sdk').read_bytes(), original)


if __name__ == '__main__':
    unittest.main(verbosity=2)
