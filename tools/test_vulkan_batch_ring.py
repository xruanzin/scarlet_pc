#!/usr/bin/env python3
"""Exercise the production submission-slot reset before/after 16-list batching."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command
from test_vulkan_list_snapshots import function

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t VkCommandBuffer;
typedef uint64_t VkDescriptorPool;
#define VK_SUCCESS 0
#define VK_QUERY_RESULT_64_BIT 0
/*CONSTANTS*/
/*SUB_TYPE*/
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq, prof_gpu, prof_qp;
static struct { int dev; struct { struct { double timestampPeriod; } limits; } props; } g_vk;
static void vk_wait_seq(uint64_t seq) {
    assert(seq <= submit_seq); // must not wait on a batch that has not been submitted
}
static int vkGetQueryPoolResults(int dev, uint64_t pool, unsigned first, int n,
                                uint64_t size, uint64_t *out, int stride, int flags) {
    out[0] = 0; out[1] = 1; return 0;
}
static void vkResetDescriptorPool(int dev, uint64_t pool, int flags) {}
/*SUB_FUNCTION*/
int main(void) {
    unsigned pending = 0, cbs = 0, physical = 0;
    // Record upload+main for every list. Retain ended flags exactly as the real
    // finalizer does; sub() must reset them only after the prior batch retires.
    for (int i = 0; i < 4096; i++) {
        Sub *s = sub();
        assert(!s->main_begun && !s->upload_begun && !s->cur_pool);
        assert(cbs + 2 <= QBATCH_MAX_CBS);
        s->main_begun = s->upload_begun = true;
        s->npools = 1; s->cur_pool = 1;
        assert(s->seq == submit_seq + 1);
        cbs += 2; pending++;
        cur_sub = (cur_sub + 1) % SUBS;
        if (pending == 16) {
            assert(cbs == 32);
            submit_seq++; physical++; pending = cbs = 0;
        }
    }
    assert(physical == 256 && !pending && !cbs);
    puts("4096 logical lists / 256 physical batches; slot wrap only after submission");
}
'''


class BatchRingTests(unittest.TestCase):
    def test_full_batches_never_reend_or_overwrite_pending_slots(self):
        source = Path(os.environ.get('SCARLET_EXEC_SOURCE', ROOT / 'runtime/nvn/vulkan/exec.c')).read_text()
        constants = '\n'.join(re.findall(r'^#define (?:MAX_QUEUE_LISTS|SUBS|QBATCH_MAX_CBS) .+$', source, re.M))
        start = source.index('typedef struct {', source.index('// ================================================================== submissions'))
        end = source.index('} Sub;', start) + len('} Sub;')
        code = HARNESS.replace('/*CONSTANTS*/', constants).replace('/*SUB_TYPE*/', source[start:end])
        code = code.replace('/*SUB_FUNCTION*/', function(source, 'static Sub *sub(void)'))
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); src = work / 'ring.c'; exe = work / 'ring'
            src.write_text(code)
            build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', str(src), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
