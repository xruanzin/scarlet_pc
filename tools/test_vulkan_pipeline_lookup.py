#!/usr/bin/env python3
"""Check the production last-pipeline shortcut against its normal table lookup."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
key_start = source.index('typedef struct {\n    Prog *prog;\n    VkFormat color[8]')
key_end = source.index('// SSBU-Recomp', key_start)
types = source[key_start:key_end]
def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]
hash_code = function('static uint64_t hash_words(uint64_t h, const void *p, size_t n) {')
hash_code = hash_code.replace('hash_words(', 'real_hash_words(', 1)
lookup = function('static Pipe *lookup_draw_pipeline(')
fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
typedef struct {int unused;} Prog;
typedef uint32_t VkFormat;
typedef uint64_t VkPipeline;
static unsigned hashes;
'''
wrapper = r'''
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    ++hashes; return real_hash_words(h, p, n);
}
'''
checks = r'''
int main(void) {
    PipeKey k={0}; k.attr_mask=3; k.used_streams=1;
    uint64_t hash; uint32_t slot;
    assert(!lookup_draw_pipeline(&k, &hash, &slot));
    assert(hashes == 1 && slot < PIPE_TAB);
    Pipe p={0}; p.k=k; p.hash=hash;
    pipe_tab[slot].key=hash; pipe_tab[slot].pipe=&p;
    last_draw_pipeline=&p;
    for (unsigned i=0; i<1000; ++i) {
        assert(lookup_draw_pipeline(&k, &hash, &slot) == &p);
        assert(hash == p.hash && slot == UINT32_MAX);
    }
    assert(hashes == 1);
    // A change anywhere in the baked key must not use the last pipeline.
    for (size_t i=0; i<sizeof k; ++i) {
        PipeKey changed=k;
        ((unsigned char *)&changed)[i] ^= 1;
        unsigned before=hashes;
        (void)lookup_draw_pipeline(&changed, &hash, &slot);
        assert(hashes == before + 1 && slot < PIPE_TAB);
    }
    last_draw_pipeline=NULL;
    assert(lookup_draw_pipeline(&k, &hash, &slot) == &p); // ordinary table hit, e.g. prewarm
    memset(pipe_tab, 0, sizeof pipe_tab); last_draw_pipeline=NULL;
    assert(!lookup_draw_pipeline(&k, &hash, &slot)); // in-place reboot cannot retain old handles
    puts("pipeline lookup: repeated keys bypass hashing; key changes and reset use the table");
}
'''
reset = source[source.index('void vk_exec_reset(void) {'):source.index('void vk_exec_reset(void) {') + 500]
assert 'last_draw_pipeline = NULL;' in reset
assert 'scarlet_draw_state_invalidate(&draw_state_cache);' in reset
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'lookup.c'
    exe = Path(directory) / ('lookup.exe' if os.name == 'nt' else 'lookup')
    c.write_text(fixture + types + hash_code + wrapper + lookup + checks)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
