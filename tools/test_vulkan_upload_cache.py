#!/usr/bin/env python3
"""Test production upload-cache functions without game assets or a GPU."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
cache = source[source.index('enum { DEDUPE_RSCALE_V'):source.index('// ================================================================== worker threads')]
fixture = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef struct { uint64_t buf, off; void *ptr; } VkSpan;
static uint64_t pending = 1;
static unsigned copies;
static unsigned char captured[2048];
static uint64_t vk_pending_seq(void) { return pending; }
static VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    assert(size && align == 256 && copies < sizeof captured);
    captured[copies] = *(const unsigned char *)data;
    VkSpan span = {1, copies++, (void *)data};
    return span;
}
'''
checks = r'''
static unsigned char guest[1 << 20];
int main(void) {
    uint64_t addresses[4]; unsigned count = 0, target = 0;
    for (unsigned off = 0; off + 16 <= sizeof guest && count < 4; off += 16) {
        uint64_t a = (uint64_t)(uintptr_t)(guest + off);
        uint64_t h = (a * 0x9E3779B97F4A7C15ull) ^ (16ull * 0xC2B2AE3D27D4EB4Full);
        unsigned slot = (unsigned)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
        if (!off) target = slot;
        if (slot == target) addresses[count++] = a;
    }
    assert(count == 4);
    VkSpan saved[4];
    for (unsigned i = 0; i < 4; ++i) saved[i] = guest_copy(addresses[i], 16);
    for (unsigned i = 0; i < 1000; ++i)
        assert(guest_copy(addresses[i % 4], 16).off == saved[i % 4].off);
    assert(copies == 4);
    *(unsigned char *)(uintptr_t)addresses[0] = 77;
    assert(guest_copy(addresses[0], 16).off == saved[0].off); // old snapshot survives CPU writes
    guest_copy_flush();
    VkSpan changed = guest_copy(addresses[0], 16);
    assert(changed.off != saved[0].off && captured[changed.off] == 77);
    assert(guest_copy(addresses[0], 32).off != changed.off);
    ++pending;
    assert(guest_copy(addresses[0], 16).off != changed.off);
    // The former packed seq + (generation << 40) key aliased these epochs.
    gcopy_gen = 1; pending = 1;
    VkSpan epoch = guest_copy(addresses[0], 16);
    gcopy_gen = 0; pending = 1 + (1ull << 40);
    assert(guest_copy(addresses[0], 16).off != epoch.off);

    unsigned char map[256] = {0};
    VkSpan v = copy_dedupe(DEDUPE_STORAGE_V, map, sizeof map, 256);
    assert(copy_dedupe(DEDUPE_STORAGE_V, map, sizeof map, 256).off == v.off);
    assert(copy_dedupe(DEDUPE_STORAGE_F, map, sizeof map, 256).off != v.off);
    map[255] = 1;
    VkSpan updated = copy_dedupe(DEDUPE_STORAGE_V, map, sizeof map, 256);
    assert(updated.off != v.off);
    ++pending;
    assert(copy_dedupe(DEDUPE_STORAGE_V, map, sizeof map, 256).off != updated.off);
    puts("upload cache: collisions, snapshots, ranges, epochs and storage maps passed");
}
'''
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'cache.c'
    exe = Path(directory) / ('cache.exe' if os.name == 'nt' else 'cache')
    c.write_text(fixture + cache + checks)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
