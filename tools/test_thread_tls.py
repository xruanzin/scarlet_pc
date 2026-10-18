#!/usr/bin/env python3
"""Exercise real kernel thread creation and the observed malloc TLS load.

Linux native harness, Clang/Zig; no game files. Generator dependencies are the
same as test_indirect_resume.py. Run: python tools/test_thread_tls.py
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from lifter import Lifter
from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent


def lifted_probe(name, words):
    lifter = Lifter(0x7000000000, 'c')
    lines = [f'static void {name}(Cpu *c) {{']
    for i, word in enumerate(words):
        ins = lifter.lift(word, 0x7000000000 + 4 * i)
        if ins.kind == 'ret':
            lines.append('return;')
        elif ins.kind == 'op':
            lines.append(ins.code)
        else:
            raise AssertionError((name, hex(word), ins.kind))
    return '\n'.join(lines + ['}'])


HARNESS = r'''
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "kernel.c"

Module g_modules[3];
int g_nmodules;
void plat_commit(uint64_t addr, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(p == (void *)(uintptr_t)addr);
}
uint64_t plat_now_ns(void) { return 0; }
void plat_sleep_until_ns(uint64_t ns) { (void)ns; }
const char *plat_symbol(uint64_t pc, uint64_t *off) { (void)pc; (void)off; return NULL; }
void rt_fatal_trace(void) { abort(); }

/*PROBES*/

static int all_zero(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++) if (p[i]) return 0;
    return 1;
}

static void test_malloc_probe(void) {
    KThread *t = kernel_create_main_thread(0x7006000000ull, 0x1000, 44);
    assert(t->cpu.tpidr && t->cpu.tpidrro == t->tlr);
    assert(t->cpu.tpidr != t->cpu.tpidrro);
    assert(t->cpu.thread == t && t->cpu.sp % 16 == 0);
    assert(t->cpu.tpidr >= USER_TLS_BASE);
    assert(t->cpu.tpidr + USER_TLS_STRIDE <= USER_TLS_BASE + USER_TLS_SIZE);
    assert(all_zero((void *)(uintptr_t)t->cpu.tpidr, USER_TLS_STRIDE));
    memset((void *)(uintptr_t)t->cpu.tpidrro, 0x5a, 0x100);  // live IPC request
    Cpu c = t->cpu;
    c.x[0] = 0x38;  // size at the actual malloc crash
    c.x[30] = 0x700658f500ull;
    malloc_probe(&c);
    assert(c.x[8] == 0x38 && c.x[0] == 0x20 && c.x[9] == 0);
    assert(c.x[10] == t->cpu.tpidr && c.x[30] == 0x700658f500ull);
    for (int i = 0; i < 0x100; i++) assert(((unsigned char *)(uintptr_t)c.tpidrro)[i] == 0x5a);

    // Cached allocator pointer must be read from the same per-thread slot.
    *(uint64_t *)(uintptr_t)(c.tpidr + 0x20) = 0x7240001230ull;
    c.x[0] = 0x38;
    malloc_probe(&c);
    assert(c.x[9] == 0x7240001230ull);

    // Reproduce the old failure with an uninitialized writable thread pointer.
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        const struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        signal(SIGSEGV, SIG_DFL);
        c.tpidr = 0;
        c.x[0] = 0x38;
        malloc_probe(&c);
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    puts("malloc TLS prefix: old null pointer faults; initialized thread passes");
}

static void test_isolation_and_guest_msr(void) {
    KThread *a = thread_new(0, 123, 0x4000, 44, 0);
    KThread *b = thread_new(0, 456, 0x5000, 44, 0);
    assert(a->cpu.x[0] == 123 && b->cpu.x[0] == 456);
    assert(a->tlr != b->tlr && a->cpu.tpidr != b->cpu.tpidr);
    *(uint64_t *)(uintptr_t)(a->cpu.tpidr + 0x20) = 111;
    assert(*(uint64_t *)(uintptr_t)(b->cpu.tpidr + 0x20) == 0);
    Cpu c = a->cpu;
    c.x[0] = b->cpu.tpidr;
    guest_tp_probe(&c);  // MSR TPIDR_EL0 followed by both MRS variants
    assert(c.tpidr == b->cpu.tpidr && c.x[1] == b->cpu.tpidr);
    assert(c.tpidrro == a->tlr && c.x[2] == a->tlr);
    assert(a->cpu.tpidr != b->cpu.tpidr);  // Cpu copies are independent
    puts("thread isolation, IPC separation and guest MSR replacement: OK");
}

enum { WORKERS = 8, PER_WORKER = 32 };
static KThread *created[WORKERS][PER_WORKER];
static void *make_threads(void *arg) {
    size_t worker = (size_t)arg;
    for (int i = 0; i < PER_WORKER; i++)
        created[worker][i] = thread_new(0, worker, 0x6000, 44, 0);
    return NULL;
}
static void test_concurrent_creation(void) {
    pthread_t workers[WORKERS];
    for (size_t i = 0; i < WORKERS; i++)
        assert(pthread_create(&workers[i], NULL, make_threads, (void *)i) == 0);
    for (int i = 0; i < WORKERS; i++) assert(pthread_join(workers[i], NULL) == 0);
    for (int a = 0; a < WORKERS * PER_WORKER; a++) {
        KThread *x = created[a / PER_WORKER][a % PER_WORKER];
        assert(all_zero((void *)(uintptr_t)x->cpu.tpidr, USER_TLS_STRIDE));
        for (int b = a + 1; b < WORKERS * PER_WORKER; b++) {
            KThread *y = created[b / PER_WORKER][b % PER_WORKER];
            assert(x->tlr != y->tlr && x->cpu.tpidr != y->cpu.tpidr && x->id != y->id);
        }
    }
    puts("256 concurrent thread creations: unique TLR, writable TLS and IDs");
}

static void test_reset_reuse_and_bounds(void) {
    // Dirty every allocated slot before reboot.
    for (KThread *t = g_threads; t; t = t->anext) {
        memset((void *)(uintptr_t)t->cpu.tpidr, 0xaa, USER_TLS_STRIDE);
        memset((void *)(uintptr_t)t->tlr, 0xbb, 0x200);
    }
    kernel_reset();
    KThread *first = thread_new(0, 0, 0x4000, 44, 0);
    assert(first->tlr == TLS_BASE && first->cpu.tpidr == USER_TLS_BASE);
    assert(all_zero((void *)(uintptr_t)first->tlr, 0x200));
    assert(all_zero((void *)(uintptr_t)first->cpu.tpidr, USER_TLS_STRIDE));
    KThread *last = first;
    for (size_t i = 1; i < TLS_SIZE / 0x200; i++) last = thread_new(0, 0, 0x4000, 44, 0);
    assert(last->tlr + 0x200 == TLS_BASE + TLS_SIZE);
    assert(last->cpu.tpidr + USER_TLS_STRIDE == USER_TLS_BASE + USER_TLS_SIZE);
    // One more thread must fail before clearing/writing past the backing pool.
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        const struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        (void)freopen("/dev/null", "w", stderr);
        thread_new(0, 0, 0x4000, 44, 0);
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    kernel_reset();
    puts("reboot reuse zeroes TLS; final slot fits; pool exhaustion stops safely");
}

int main(void) {
    assert(USER_TLS_BASE >= TLS_BASE + TLS_SIZE);
    assert(USER_TLS_BASE + USER_TLS_SIZE <= HLE_HEAP);
    kernel_init();
    test_malloc_probe();
    test_isolation_and_guest_msr();
    test_concurrent_creation();
    test_reset_reuse_and_bounds();
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'Native kernel harness runs on Linux.')
class ThreadTlsTests(unittest.TestCase):
    def test_real_kernel_tls_and_lifted_malloc(self):
        probes = lifted_probe('malloc_probe', [
            0xAA0003E8, 0xD2A00000, 0xF2800400, 0xD503201F,
            0xD503201F, 0xD53BD04A, 0xF8606949, 0xD65F03C0,
        ])
        probes += '\n' + lifted_probe('guest_tp_probe', [
            0xD51BD040, 0xD53BD041, 0xD53BD062, 0xD65F03C0,
        ])
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            src, exe = work / 'tls_test.c', work / 'tls_test'
            src.write_text(HARNESS.replace('/*PROBES*/', probes))
            cmd = compiler_command() + ['-O2', '-g', '-ffunction-sections', '-fdata-sections',
                '-Wno-format', '-I' + str(ROOT / 'runtime'), str(src),
                '-Wl,--gc-sections', '-pthread', '-o', str(exe)]
            build = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('initialized thread passes', run.stdout)
            self.assertIn('256 concurrent thread creations', run.stdout)
            self.assertIn('pool exhaustion stops safely', run.stdout)
            print(run.stdout, end='')


if __name__ == '__main__':
    unittest.main(verbosity=2)
