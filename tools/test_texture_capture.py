#!/usr/bin/env python3
"""F9 captures actual bound CPU sources; reject invalid/GPU-owned byte ranges."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include "nvn/texture_capture.c"
#include "nvn/formats.c"
#include "nvn/tegra.c"
uint64_t plat_now_ns(void) { static uint64_t now = 100; return ++now; }
int main(void) {
    unsigned char bytes[4096]; memset(bytes, 0x6e, sizeof bytes);
    NvnPool pool = {.storage = (uintptr_t)bytes, .size = sizeof bytes};
    NvnTexture t = {.debug_id = 1, .addr = (uintptr_t)bytes, .width = 32, .height = 32,
        .depth = 1, .levels = 1, .format = 0x25, .pool = &pool,
        .layout = {.width = 32, .height = 32, .depth = 1, .layers = 1, .levels = 1}};
    t.layout.fmt = nvn_format_info(t.format);
    nvn_diag_texture_source(&t, false, 7);
    nvn_diag_texture_source(&t, false, 7);
    assert(capture_count == 1 && capture_bytes == 4096);
    t.debug_id = 2; t.addr = 1;
    nvn_diag_texture_source(&t, false, 7); // must not dereference an invalid address
    t.debug_id = 3; t.addr = (uintptr_t)bytes;
    nvn_diag_texture_source(&t, true, 7); // stale CPU backing is not current GPU pixels
    t.debug_id = 4; pool.size = 16;
    nvn_diag_texture_source(&t, false, 7); // insufficient backing: metadata only
    assert(capture_bytes == 4096 && capture_count == 4);
    pool.size = sizeof bytes;
    for (int i = 5; i < 140; i++) { t.debug_id = i; nvn_diag_texture_source(&t, false, 7); }
    assert(capture_count == CAP_TEXTURES);
    // A separate F9 frame must get a fresh directory/quota, not earlier files.
    t.debug_id = 999;
    nvn_diag_texture_source(&t, false, 8);
    assert(capture_count == 1 && capture_bytes == 4096);
    // Enforce budget without allocating/copying a large fixture.
    capture_bytes = CAP_BYTES;
    t.debug_id = 1000; nvn_diag_texture_source(&t, false, 8);
    assert(capture_bytes == CAP_BYTES);
}
'''


class TextureCaptureTests(unittest.TestCase):
    def test_pool_bounds_dedup_gpu_ownership_limits_and_frame_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); src = work / 'capture.c'; exe = work / 'capture'
            src.write_text(HARNESS)
            build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-Wno-format',
                '-I' + str(ROOT / 'runtime'), str(src), '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], cwd=work, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            frames = sorted((work / 'build/forensics-textures').iterdir())
            self.assertEqual(len(frames), 2)
            first = next(p for p in frames if p.name.startswith('frame_7_'))
            raw = list((first / 'texture_1').glob('*.bin'))
            self.assertEqual(len(raw), 1)
            self.assertEqual(raw[0].read_bytes(), b'\x6e' * 4096)
            for i, status in [(2, 'no-valid-pool-range'), (3, 'gpu-owned-not-current-in-cpu-backing'), (4, 'incomplete-pool-range')]:
                folder = first / ('texture_' + str(i))
                self.assertFalse(list(folder.glob('*.bin')))
                self.assertIn('status=' + status, (folder / 'texture.txt').read_text())
            second = next(p for p in frames if p.name.startswith('frame_8_'))
            self.assertEqual(len(list(second.glob('texture_*'))), 2)
            self.assertIn('status=capture-budget', (second / 'texture_1000/texture.txt').read_text())


if __name__ == '__main__': unittest.main(verbosity=2)
