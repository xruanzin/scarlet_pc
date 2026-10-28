#!/usr/bin/env python3
"""Exhaustively compare production grouped bindings to per-stream bindings."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('static void bind_vertex_streams(')
function = source[start:source.index('\n}\n', start) + 3]
fixture = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t VkBuffer;
typedef uint64_t VkDeviceSize;
typedef uint64_t VkCommandBuffer;
static unsigned expected, seen, calls;
static void vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                  const VkBuffer *buffers, const VkDeviceSize *offsets) {
    assert(cb == 42 && count && first + count <= 17);
    ++calls;
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = first + i, bit = 1u << slot;
        assert((expected & bit) && !(seen & bit));
        assert(buffers[i] == 100 + slot && offsets[i] == slot * 256);
        seen |= bit;
    }
}
'''
checks = r'''
int main(void) {
    for (unsigned mask = 0; mask < (1u << 17); ++mask) {
        VkBuffer buffers[17]; VkDeviceSize offsets[17];
        unsigned runs = 0;
        for (unsigned s = 0; s < 17; ++s) {
            buffers[s] = (mask & (1u << s)) ? 100 + s : UINT64_MAX;
            offsets[s] = (mask & (1u << s)) ? s * 256 : UINT64_MAX;
            if ((mask & (1u << s)) && (!s || !(mask & (1u << (s - 1))))) ++runs;
        }
        expected = mask; seen = calls = 0;
        bind_vertex_streams(42, mask, buffers, offsets);
        assert(seen == mask && calls == runs);
    }
    assert(calls == 1); // all 17 streams use one call instead of seventeen
    puts("vertex bindings: all 131072 masks preserve buffers, offsets and holes");
}
'''
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'bindings.c'
    exe = Path(directory) / ('bindings.exe' if os.name == 'nt' else 'bindings')
    c.write_text(fixture + function + checks)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
