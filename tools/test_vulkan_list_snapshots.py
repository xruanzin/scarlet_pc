#!/usr/bin/env python3
"""Check list snapshot lifetime using production cache/executor entry functions.

No Vulkan driver or game assets. SCARLET_EXEC_SOURCE selects a pre-fix source
for the negative control; pending GPU retirement sequence is deliberately fixed.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MAX_QUEUE_LISTS 16
typedef struct { uint64_t buf, off; void *ptr; } VkSpan;
static unsigned copies, lists, submits, locked;
static uint64_t value;
static uint64_t storage[2048];
static uint64_t vk_pending_seq(void) { return 7; }
static VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    assert(size == sizeof(uint64_t) && align == 256 && copies < 2048);
    memcpy(&storage[copies], data, size);
    VkSpan s = {1, copies * 256, &storage[copies]}; copies++;
    return s;
}
/*CACHE*/
static int prof;
static uint64_t prof_exec, cum_exec;
#define PROF_T0() uint64_t prof_t0_ = 0
#define PROF_ADD(v) ((void)(v))
static uint64_t SDL_GetTicksNS(void) { return 0; }
static void vk_big_lock(void) { assert(!locked); locked++; }
static void vk_big_unlock(void) { assert(locked); locked--; }
static bool vk_exec_diag_enter(uint64_t handle) { return false; }
static void vk_exec_diag_leave(bool enabled) {}
static bool ip_enabled(void) { return false; }
static void ip_note_list(uint64_t handle) { abort(); }
static void renderer_flush(void) { submits++; }
static void renderer_submit_list(void);
#define VK_NULL_HANDLE 0
static unsigned qbatch_lists;
static bool qbatch_finish_current(bool force_main) { qbatch_lists++; return true; }
static void qbatch_submit(int wait, int signal) {
    assert(!wait && !signal && qbatch_lists > 0 && qbatch_lists <= 16);
    qbatch_lists = 0; submits++;
}
static void exec_list(uint64_t handle, int depth) {
    assert(locked && value == handle);
    unsigned before = copies;
    VkSpan a = guest_copy((uintptr_t)&value, sizeof value);
    VkSpan b = guest_copy((uintptr_t)&value, sizeof value);
    assert(*(uint64_t *)a.ptr == value); // stale snapshot fails here on the old source
    assert(a.ptr == b.ptr && copies == before + 1); // reuse within this list
    lists++;
}
/*EXECUTE*/
/*POLICY*/
/*SUBMIT_LIST*/
int main(int argc, char **argv) {
    assert(argc == 2);
    unsetenv("LARECOMP_SYNC_SUBMIT");
    if (!strcmp(argv[1], "cache")) {
        unsetenv("LARECOMP_QUEUE_BATCH");
        for (value = 1; value <= 1000; value++) backend_execute(value);
        assert(copies == 1000 && lists == 1000 && submits == 62 && qbatch_lists == 8 && !locked);
        puts("1000 list executions: current bytes across lists, one snapshot within each list");
    } else {
        if (!strcmp(argv[1], "single")) setenv("LARECOMP_QUEUE_BATCH", "1", 1);
        else unsetenv("LARECOMP_QUEUE_BATCH");
        for (int i = 0; i < 32; i++) renderer_submit_list();
        assert(submits == (!strcmp(argv[1], "single") ? 32 : 2) && !qbatch_lists);
        puts("configured list grouping submits at its requested boundary");
    }
}
'''


class ListSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        directory = Path(cls.tmp.name)
        source = Path(os.environ.get('SCARLET_EXEC_SOURCE', ROOT / 'runtime/nvn/vulkan/exec.c')).read_text()
        start = source.index('#define GCOPY_TAB')
        end = source.index('// ================================================================== worker threads', start)
        harness = HARNESS.replace('/*CACHE*/', source[start:end])
        harness = harness.replace('/*EXECUTE*/', function(source, 'void backend_execute(uint64_t handle)'))
        harness = harness.replace('/*POLICY*/', function(source, 'static int queue_batch_limit(void)'))
        harness = harness.replace('/*SUBMIT_LIST*/', function(source, 'static void renderer_submit_list(void) {'))
        (directory / 'lists.c').write_text(harness)
        cls.exe = directory / 'lists'
        run = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', str(directory / 'lists.c'),
            '-o', str(cls.exe)], capture_output=True, text=True)
        if run.returncode:
            raise RuntimeError(run.stderr)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def run_case(self, name):
        run = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_new_list_reads_updated_guest_memory(self): self.run_case('cache')
    def test_sixteen_lists_default(self): self.run_case('default')
    def test_single_list_override(self): self.run_case('single')


if __name__ == '__main__': unittest.main(verbosity=2)
