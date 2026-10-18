#!/usr/bin/env python3
"""Exercise guest allocation, metadata reuse and cross-thread frees without ROM data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
static _Atomic unsigned long metadata_allocs, metadata_frees;
static void *count_malloc(size_t n) {
    void *p = malloc(n);
    if (p) atomic_fetch_add(&metadata_allocs, 1);
    return p;
}
static void count_free(void *p) {
    if (p) atomic_fetch_add(&metadata_frees, 1);
    free(p);
}
#define malloc count_malloc
#define free count_free
#include HEAPS_SOURCE
#undef malloc
#undef free
void plat_commit(uint64_t addr, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(p == (void *)(uintptr_t)addr);
}
static void verify_empty(void) {
    assert(!g_std_recs && !g_std_live_bytes);
    stdalloc_reset();
    assert(atomic_load(&metadata_allocs) == atomic_load(&metadata_frees));
}
static void semantics(void) {
    for (unsigned a = 16; a <= 8192; a *= 2) {
        uint64_t p = std_guest_alloc(17, a);
        assert(p >= HEAP_BASE && p < HEAP_BASE + HEAP_MAX && !(p % a));
        assert(std_guest_size(p) == 17);
        memset((void *)(uintptr_t)p, 0x5a, 17);
        uint64_t q = std_guest_realloc(p, 31);
        assert(q && !(q % a) && std_guest_size(q) == 31);
        for (int i = 0; i < 17; i++) assert(((unsigned char *)(uintptr_t)q)[i] == 0x5a);
        p = std_guest_realloc(q, 8193);
        assert(p && !(p % a) && std_guest_size(p) == 8193);
        for (int i = 0; i < 17; i++) assert(((unsigned char *)(uintptr_t)p)[i] == 0x5a);
        std_guest_free(p);
    }
    assert(!std_guest_alloc(UINT64_MAX, 16));
    uint64_t zero = std_guest_alloc(0, 24);
    assert(zero && !(zero % 32) && std_guest_size(zero) == 1);
    std_guest_free(zero);
    verify_empty();
}
#define N 512
static uint64_t pending[N];
static void cycle(void) {
    for (int i = 0; i < N; i++) {
        unsigned size = 1 + (i % 64) * 31, align = 16u << (i % 5);
        pending[i] = std_guest_alloc(size, align);
        assert(pending[i] && !(pending[i] % align));
        assert(std_guest_size(pending[i]) == size);
        memset((void *)(uintptr_t)pending[i], i & 255, size);
    }
    for (int i = N - 1; i >= 0; i--) std_guest_free(pending[i]);
}
static void reuse(void) {
    cycle(); // warm all classes and metadata
    uint64_t frontier = g_std_next;
    unsigned long before = atomic_load(&metadata_allocs);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int j = 0; j < 300; j++) cycle();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("153600 mixed allocations/frees: %.3f ms; metadata allocations after warmup: %lu\n",
        (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6,
        atomic_load(&metadata_allocs) - before);
    fflush(stdout);
    assert(atomic_load(&metadata_allocs) == before);
    assert(g_std_next == frontier);
    verify_empty();
}
static pthread_barrier_t barrier;
static uint64_t shared[8][64];
static void *worker(void *arg) {
    unsigned id = (uintptr_t)arg;
    for (int round = 0; round < 100; round++) {
        for (int j = 0; j < 64; j++) {
            uint64_t p = std_guest_alloc(80, 64);
            assert(p && !(p % 64)); shared[id][j] = p;
            memset((void *)(uintptr_t)p, id + 1, 80);
        }
        pthread_barrier_wait(&barrier);
        if (id == 0)
            for (int a = 0; a < 512; a++) for (int b = a + 1; b < 512; b++)
                assert(shared[a / 64][a % 64] != shared[b / 64][b % 64]);
        pthread_barrier_wait(&barrier);
        for (int j = 0; j < 64; j++) {
            uint64_t p = shared[(id + 1) % 8][j];
            for (int k = 0; k < 80; k++)
                assert(((unsigned char *)(uintptr_t)p)[k] == (id + 1) % 8 + 1);
            std_guest_free(p);
        }
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}
static void concurrency(void) {
    pthread_t threads[8]; pthread_barrier_init(&barrier, NULL, 8);
    for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)i));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&barrier);
    verify_empty();
}
static void limits(void) {
    // Exercise per-class overflow and >64 MiB across multiple classes.
    const int n = 18000;
    uint64_t *p = calloc(n, sizeof *p);
    for (int i = 0; i < 3000; i++) { p[i] = std_guest_alloc(80, 64); assert(p[i]); }
    for (int i = 0; i < 3000; i++) std_guest_free(p[i]);
    assert(g_std_small_count[std_small_class(80, 64)] == STD_SMALL_CLASS_LIMIT);
    verify_empty();
    for (int i = 0; i < n; i++) {
        p[i] = std_guest_alloc(4000 + 16 * (i % 7), 16u << (i % 5));
        assert(p[i]);
    }
    for (int i = 0; i < n; i++) std_guest_free(p[i]);
    free(p);
    uint64_t bytes = 0;
    for (int b = 0; b < STD_SMALL_CLASSES; b++) {
        assert(g_std_small_count[b] <= STD_SMALL_CLASS_LIMIT);
        for (StdAllocRec *r = g_std_small[b]; r; r = r->next) bytes += r->capacity;
    }
    assert(bytes == g_std_small_bytes && bytes <= STD_SMALL_CACHE_LIMIT);
    assert(bytes > STD_SMALL_CACHE_LIMIT - 4096); // actually reached the budget
    verify_empty();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "semantics")) semantics();
    else if (!strcmp(argv[1], "reuse")) reuse();
    else if (!strcmp(argv[1], "threads")) concurrency();
    else if (!strcmp(argv[1], "limits")) limits();
    else abort();
}
'''


class StdAllocatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        work = Path(cls.tmp.name)
        source = os.environ.get('SCARLET_HEAPS_SOURCE', str(ROOT / 'runtime/hle/heaps.c'))
        harness = HARNESS
        if os.environ.get('SCARLET_HEAPS_SOURCE'):
            # Old sources have no cache internals; preserve behavioral reuse assertions.
            start = harness.index('static void limits(void)')
            end = harness.index('int main(', start)
            harness = harness[:start] + 'static void limits(void) {}\n' + harness[end:]
        (work / 'allocator.c').write_text(harness.replace('HEAPS_SOURCE', '"' + source + '"'))
        cls.exe = work / 'allocator'
        build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-ffunction-sections',
            '-fdata-sections', '-Wno-format', '-I' + str(ROOT / 'runtime'),
            '-I' + str(ROOT / 'runtime/hle'),
            str(work / 'allocator.c'), '-Wl,--gc-sections', '-pthread', '-o', str(cls.exe)],
            capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        run = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        if run.stdout:
            print(run.stdout.strip())

    def test_alignment_sizes_reallocation_and_reset(self): self.run_case('semantics')
    def test_reuse_without_new_metadata_or_frontier_growth(self): self.run_case('reuse')
    def test_cross_thread_frees_and_live_data(self): self.run_case('threads')
    def test_bounded_cache_and_reset(self): self.run_case('limits')


if __name__ == '__main__':
    unittest.main(verbosity=2)
