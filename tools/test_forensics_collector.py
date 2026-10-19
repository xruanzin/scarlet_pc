#!/usr/bin/env python3
"""Exercise actual PowerShell collector paths and exact-frame texture selection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent


class ForensicsCollectorTests(unittest.TestCase):
    def test_runtime_root_and_exact_texture_capture(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh: self.skipTest('PowerShell 7 required')
        for nested in (False, True):
            with self.subTest(nested=nested), tempfile.TemporaryDirectory() as tmp:
                work = Path(tmp); repo = work / 'repo with spaces'; repo.mkdir()
                runtime = repo / 'build' if nested else repo
                build = runtime / 'build'; build.mkdir(parents=True)
                (repo / 'scarlet_boot.log').write_text('[paths] data: ' + str(runtime) + '\n')
                capture = build / 'forensics-textures/frame_7_101/texture_1'
                capture.mkdir(parents=True)
                (capture / 'test.bin').write_bytes(b'current-texture')
                stale = build / 'forensics-textures/frame_7_100'; stale.mkdir(parents=True)
                (stale / 'old.bin').write_bytes(b'stale')
                started = int(time.time()) - 30
                (build / 'scarlet_forensics_7.txt').write_text(
                    f'# capture_started_unix_s={started}\n'
                    '# texture_sources=build/forensics-textures/frame_7_101\nframe7\n')
                for name, timestamp in [('pass_mrt_7_abc_rt1.png', started + 2),
                                         ('pass_stale.png', started - 60)]:
                    image = build / name; image.write_bytes(b'fixture')
                    os.utime(image, (timestamp, timestamp))
                if nested:
                    old = repo / 'build/scarlet_forensics_6.txt'; old.write_text('old-root')
                    os.utime(old, (1000, 1000))
                env = dict(os.environ, TEMP=str(work / 'temporary'))
                run = subprocess.run([str(Path(pwsh).resolve()), '-NoLogo', '-NoProfile', '-File',
                    str(ROOT / 'tools/scarlet_collect_forensics.ps1'), '-Root', str(repo)],
                    env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                archives = list(repo.glob('scarlet-forensics-*.zip'))
                self.assertEqual(len(archives), 1)
                with zipfile.ZipFile(archives[0]) as archive:
                    names = archive.namelist()
                    self.assertIn('scarlet_forensics_7.txt', names)
                    self.assertIn('passes/pass_mrt_7_abc_rt1.png', names)
                    self.assertNotIn('passes/pass_stale.png', names)
                    self.assertIn('textures/texture_1/test.bin', names)
                    self.assertEqual(archive.read('textures/texture_1/test.bin'), b'current-texture')
                    self.assertFalse(any('old.bin' in n or 'forensics_6' in n for n in names))


if __name__ == '__main__': unittest.main(verbosity=2)
