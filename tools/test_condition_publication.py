#!/usr/bin/env python3
"""Force a guest signal-fast-path race against the production Horizon wait.

Only the atomic-store scheduling is instrumented: a second pthread inspects
the condition key immediately after the mutex is released. No game files.
SCARLET_KERNEL_SOURCE can select the pre-fix source for a negative control.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "kernel.h"
static uint32_t mutex_word, condition_key;
static atomic_bool released, inspected;
static void observe_store(uint32_t *p, uint32_t value) {
    __atomic_store_n(p, value, __ATOMIC_SEQ_CST);
    if (p == &mutex_word && value == 0) {
        atomic_store(&released, true);
        // Schedule the guest fast path precisely at unlock, before the next
        // production statement. It reads the key without taking g_klock.
        while (!atomic_load(&inspected)) sched_yield();
    }
}
#define __atomic_store_n(p, v, order) observe_store((uint32_t *)(p), (uint32_t)(v))
#include KERNEL_SOURCE
#undef __atomic_store_n
void rt_fatal_trace(void) { abort(); }
static void *signal_fast_path(void *unused) {
    while (!atomic_load(&released)) sched_yield();
    uint32_t expected = 0;
    assert(__atomic_compare_exchange_n(&mutex_word, &expected, 0x102, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    uint32_t has_waiter = __atomic_load_n(&condition_key, __ATOMIC_SEQ_CST);
    atomic_store(&inspected, true);
    if (has_waiter) svc_signal_process_wide_key((uintptr_t)&condition_key, 1);
    svc_arbitrate_unlock((uintptr_t)&mutex_word);
    return NULL;
}
int main(void) {
    KThread waiter = {0};
    waiter.handle = 0x101; waiter.prio = 44;
    pthread_cond_init(&waiter.cond, NULL); t_cur = &waiter;
    for (int i = 0; i < 100; i++) {
        mutex_word = waiter.handle; condition_key = 0;
        atomic_store(&released, false); atomic_store(&inspected, false);
        pthread_t signaler;
        assert(!pthread_create(&signaler, NULL, signal_fast_path, NULL));
        uint32_t result = svc_wait_process_wide_key((uintptr_t)&mutex_word,
            (uintptr_t)&condition_key, waiter.handle, 200000000);
        pthread_join(signaler, NULL);
        if (result != R_OK) {
            fprintf(stderr, "lost condition signal: result=%x iteration=%d\n", result, i);
            return 1;
        }
        assert(mutex_word == waiter.handle && condition_key == 0 && !g_waitlist);
    }
    pthread_cond_destroy(&waiter.cond);
    puts("100 forced unlock/signal interleavings passed");
}
'''


class ConditionPublicationTests(unittest.TestCase):
    def test_signal_cannot_skip_new_waiter(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'race.c', Path(tmp) / 'race'
            kernel = os.environ.get('SCARLET_KERNEL_SOURCE', str(ROOT / 'runtime/kernel.c'))
            src.write_text(HARNESS.replace('KERNEL_SOURCE', '"' + kernel + '"'))
            build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                '-Wno-format', '-I' + str(ROOT / 'runtime'), str(src), '-Wl,--gc-sections',
                '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('100 forced', run.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
