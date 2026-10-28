#!/usr/bin/env python3
"""Native launcher readiness and no-rebuild contract, with synthetic NPDM files."""
from pathlib import Path
import os
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import sys
import importlib.util

ROOT = Path(__file__).resolve().parent.parent
SOURCE = r'''
#include <assert.h>
#include <unistd.h>
#include "gamesetup.c"
static bool aot = true, las;
static int builds, tools_checked;
bool scarlet_aot_present(const char *root) { (void)root; return aot; }
bool gamelib_present(const char *root) { (void)root; return las; }
bool gamebuild_available(void) { ++tools_checked; return true; }
bool gamebuild_run(const char *root, const GameBuildOpts *opts, char *why, size_t n) {
    (void)root; (void)opts; (void)why; (void)n; ++builds; return true;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    unsetenv("SCARLET_RUNTIME");
    assert(setup_ready_at(argv[1])); // AOT, without lasgame
    assert(build_code() && !builds && !tools_checked);
    aot = false; las = true;
    assert(!setup_ready_at(argv[1])); // LAS cannot disguise absent AOT
    assert(!build_code() && setup_state() == SETUP_FAIL_AOT);
    assert(!builds && !tools_checked);
    setenv("SCARLET_RUNTIME", "las", 1);
    assert(setup_ready_at(argv[1]));
    assert(build_code() && builds == 1 && tools_checked == 1);
    setenv("SCARLET_RUNTIME", "native", 1); aot = true;
    char path[1200];
    snprintf(path, sizeof path, "%s/romfs.bin", argv[1]);
    assert(!unlink(path));
    assert(!setup_ready_at(argv[1]));
    snprintf(path, sizeof path, "%s/romfs", argv[1]);
    assert(!mkdir(path, 0700));
    assert(setup_ready_at(argv[1]));
    snprintf(path, sizeof path, "%s/exefs", argv[1]);
    setenv("LARECOMP_EXEFS_DIR", path, 1);
    snprintf(path, sizeof path, "%s/romfs", argv[1]);
    setenv("LARECOMP_ROMFS_DIR", path, 1);
    assert(setup_ready_at("missing-root")); // package mount paths
    puts("native setup: AOT-only, LAS comparison, no regeneration, spaced root, mounted paths passed");
}
'''

class SetupTest(unittest.TestCase):
    def test_native_readiness(self):
        if os.environ.get('CC'):
            cc = shlex.split(os.environ['CC'])
        elif shutil.which('clang'):
            cc = ['clang']
        elif importlib.util.find_spec('ziglang'):
            cc = [sys.executable, '-m', 'ziglang', 'cc']
        else:
            self.skipTest('Clang or Zig required')
        with tempfile.TemporaryDirectory(prefix='scarlet setup ') as tmp:
            work = Path(tmp)
            (work/'SDL3').mkdir()
            (work/'SDL3/SDL.h').write_text('#pragma once\nstatic inline int SDL_GetNumLogicalCPUCores(void){return 6;}\nstatic inline int SDL_GetSystemRAM(void){return 16384;}\n')
            (work/'exefs').mkdir()
            meta = bytearray(0x100)
            meta[:4] = b'META'
            struct.pack_into('<I', meta, 0x70, 0x80)
            meta[0x80:0x84] = b'ACI0'
            struct.pack_into('<Q', meta, 0x90, 0x0100A3D008C5C000)
            (work/'exefs/main.npdm').write_bytes(meta)
            (work/'romfs.bin').touch()
            (work/'test.c').write_text(SOURCE)
            subprocess.run(cc + ['-std=gnu11', '-O1', '-ffunction-sections', '-fdata-sections',
                                '-Wl,--gc-sections', '-I'+str(work), '-I'+str(ROOT/'runtime'),
                                str(work/'test.c'), '-o', str(work/'test')], check=True)
            env = os.environ.copy()
            for key in ('LARECOMP_EXEFS_DIR', 'LARECOMP_ROMFS_DIR', 'LARECOMP_ROMFS_IMAGE'):
                env.pop(key, None)
            subprocess.run([str(work/'test'), tmp], env=env, check=True)

if __name__ == '__main__':
    unittest.main()
