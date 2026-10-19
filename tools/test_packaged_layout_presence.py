#!/usr/bin/env python3
"""Distinguish explicit zero layout from defaults/null without builder overflow."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include OBJECTS_SOURCE
#include "nvn/formats.c"
#include "nvn/tegra.c"
static void configure(Cpu *c, uint32_t fmt, uint32_t w, uint32_t h) {
    hle_nvnTextureBuilderSetDefaults(c);
    c->x[1] = fmt; hle_nvnTextureBuilderSetFormat(c);
    c->x[1] = w; c->x[2] = h; hle_nvnTextureBuilderSetSize2D(c);
}
static void check_pixels(uint64_t builder) {
    TexLayout t = {0}; builder_layout(builder, &t);
    LevelLayout l = tegra_level(&t, 0);
    assert(l.block_height_log2 == 0);
    assert(l.wb * t.fmt->bpb == 128 && l.hb == 32);
    unsigned char raw[4096], linear[4096];
    // Independent physical fixture: 2 GOB columns x 4 GOB rows, BH=1 GOB.
    // Each complete 512-byte GOB has a distinct value. No swizzler is used to
    // generate the fixture, and x/y expected values come from its grid.
    for (int i = 0; i < 8; i++) memset(raw + i * 512, 0x10 + i, 512);
    tegra_deswizzle(raw, linear, l.wb, l.hb, 1, t.fmt->bpb, l.block_height_log2, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 128; x++)
        assert(linear[y * 128 + x] == 0x10 + (y / 8) * 2 + x / 64);
}
int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mem = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED && !mprotect(mem + page, page, PROT_NONE));
    unsigned char *builder = mem + page - 0x60;
    memset(builder - 16, 0xa5, 16);
    Cpu c = {.x = {(uintptr_t)builder}};
    uint64_t explicit_layout = 0;
    for (int compressed = 0; compressed < 2; compressed++) {
        configure(&c, compressed ? 0x4d : 0x25, 32, compressed ? 128 : 32);
        TexLayout t = {0}; builder_layout(c.x[0], &t);
        assert(t.block_height_log2 == 2); // default is inferred, not explicit zero
        unsigned char data_marker[8] = {0};
        c.x[1] = (uintptr_t)data_marker; hle_nvnTextureBuilderSetPackagedTextureData(&c);
        builder_layout(c.x[0], &t); assert(t.block_height_log2 == 2);
        c.x[1] = (uintptr_t)&explicit_layout;
        hle_nvnTextureBuilderSetPackagedTextureLayout(&c);
        check_pixels(c.x[0]);
        unsigned char copied[0x60]; memcpy(copied, builder, sizeof copied);
        check_pixels((uintptr_t)copied); // guest-side builder copies preserve presence
        c.x[1] = 0x1234; hle_nvnTextureBuilderSetGLTextureName(&c);
        check_pixels(c.x[0]); // adjacent field must not overwrite the presence word
        c.x[1] = 0; hle_nvnTextureBuilderSetPackagedTextureLayout(&c);
        builder_layout(c.x[0], &t); assert(t.block_height_log2 == 2);
        c.x[1] = (uintptr_t)&explicit_layout; hle_nvnTextureBuilderSetPackagedTextureLayout(&c);
        configure(&c, compressed ? 0x4d : 0x25, 32, compressed ? 128 : 32);
        builder_layout(c.x[0], &t); assert(t.block_height_log2 == 2); // reset clears it
    }
    for (uint64_t value = 1; value <= 5; value++) {
        c.x[1] = (uintptr_t)&value; hle_nvnTextureBuilderSetPackagedTextureLayout(&c);
        TexLayout t = {0}; builder_layout(c.x[0], &t); assert(t.block_height_log2 == value);
    }
    for (int i = 0; i < 16; i++) assert(builder[i - 16] == 0xa5);
    assert(!munmap(mem, page * 2));
}
'''


class PackagedLayoutTests(unittest.TestCase):
    def test_explicit_zero_grid_copy_reset_null_and_96_byte_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); src = work / 'layout.c'; exe = work / 'layout'
            source = os.environ.get('SCARLET_OBJECTS_SOURCE', str(ROOT / 'runtime/nvn/objects.c'))
            src.write_text(HARNESS.replace('OBJECTS_SOURCE', '"' + source + '"'))
            build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-ffunction-sections',
                '-fdata-sections', '-Wno-format', '-I' + str(ROOT / 'runtime'),
                '-I' + str(ROOT / 'runtime/nvn'), str(src), '-Wl,--gc-sections', '-pthread', '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
