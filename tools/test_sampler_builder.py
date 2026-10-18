#!/usr/bin/env python3
"""Native tests of the production NVN HLE against Scarlet's 64-byte builder.

No game files. Runs on Linux with Clang/Zig. SCARLET_NVN_TEST_SOURCE can select
a retained objects.c revision to verify the regression fails before the fix.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_indirect_resume import compiler_command
from test_thread_tls import lifted_probe

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include OBJECTS_SOURCE

/*PROBES*/

static void arg0(Cpu *c, void *p) { c->x[0] = (uintptr_t)p; }
static void floats(Cpu *c, float a, float b) {
    c->v[0] = c->v[1] = 0;
    memcpy(&c->v[0], &a, 4); memcpy(&c->v[1], &b, 4);
}
static void configure(Cpu *c, void *builder) {
    arg0(c, builder); c->x[1] = 0x7000abcd00ull; hle_nvnSamplerBuilderSetDevice(c);
    arg0(c, builder); hle_nvnSamplerBuilderSetDefaults(c);
    arg0(c, builder); c->x[1] = 3; c->x[2] = 1; hle_nvnSamplerBuilderSetMinMagFilter(c);
    arg0(c, builder); c->x[1] = 1; c->x[2] = 7; c->x[3] = 2; hle_nvnSamplerBuilderSetWrapMode(c);
    arg0(c, builder); floats(c, -2.5f, 9.25f); hle_nvnSamplerBuilderSetLodClamp(c);
    arg0(c, builder); floats(c, 0.75f, 0); hle_nvnSamplerBuilderSetLodBias(c);
    arg0(c, builder); c->x[1] = 1; c->x[2] = 6; hle_nvnSamplerBuilderSetCompare(c);
    float white[4] = {1, 1, 1, 1};
    arg0(c, builder); c->x[1] = (uintptr_t)white; hle_nvnSamplerBuilderSetBorderColor(c);
    arg0(c, builder); floats(c, 8.0f, 0); hle_nvnSamplerBuilderSetMaxAnisotropy(c);
    arg0(c, builder); c->x[1] = 2; hle_nvnSamplerBuilderSetReductionFilter(c);
    arg0(c, builder); floats(c, 0, 0); hle_nvnSamplerBuilderSetLodSnap(c);
}

static void check_desc(const NvnSamplerDesc *d) {
    assert(d->min_filter == 3 && d->mag_filter == 1);
    assert(d->wrap[0] == 1 && d->wrap[1] == 7 && d->wrap[2] == 2);
    assert(d->lod_min == -2.5f && d->lod_max == 9.25f && d->lod_bias == 0.75f);
    assert(d->compare_mode == 1 && d->compare_func == 6 && d->reduction == 2);
    assert(d->max_aniso == 8.0f && d->backend == NULL);
    for (int i = 0; i < 4; i++) assert(d->border[i] == 1.0f);
}

static void test_real_frame(void) {
    _Alignas(16) unsigned char stack[0x300];
    memset(stack, 0xa5, sizeof stack);
    Cpu c = {0};
    c.sp = (uintptr_t)(stack + 0x200);
    c.x[29] = (uintptr_t)(stack + 0x280); c.x[30] = 0x70007e5ee4ull;
    c.x[19] = 19; c.x[20] = 20; c.x[21] = 21;
    uint64_t sp = c.sp, fp = c.x[29], lr = c.x[30];
    sampler_frame_enter(&c);  // actual sub/stp/add/str prologue from the report
    unsigned char *builder = (void *)(uintptr_t)c.sp;
    unsigned char saved[0x30];
    memcpy(saved, builder + 0x40, sizeof saved);
    configure(&c, builder);
    if (memcmp(saved, builder + 0x40, sizeof saved)) {
        fprintf(stderr, "sampler builder overwrote saved X29/LR: fp=%llx lr=%llx\n",
                (unsigned long long)*(uint64_t *)(builder + 0x40),
                (unsigned long long)*(uint64_t *)(builder + 0x48));
        abort();
    }
    uint64_t sampler[6] = {0};
    arg0(&c, sampler); c.x[1] = (uintptr_t)builder; hle_nvnSamplerInitialize(&c);
    assert(c.x[0] == 1); check_desc(HOST(NvnSamplerDesc, (uintptr_t)sampler));
    assert(!memcmp(saved, builder + 0x40, sizeof saved));
    sampler_frame_leave(&c);
    assert(c.sp == sp && c.x[29] == fp && c.x[30] == lr);
    assert(c.x[19] == 19 && c.x[20] == 20 && c.x[21] == 21);
    // The exact guest store which faulted at main+0x7e5ef0 now has a valid FP.
    post_sampler_store(&c);
    assert(*(uint64_t *)(uintptr_t)(fp - 0x20) == 0);
    assert(*(uint64_t *)(uintptr_t)(fp - 0x18) == 0);
    arg0(&c, sampler); hle_nvnSamplerFinalize(&c);
    puts("64-byte builder preserves the real saved frame and the following guest store succeeds");
}

static void test_guard_page_and_copy(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mem = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED && mprotect(mem + page, page, PROT_NONE) == 0);
    unsigned char *b = mem + page - 0x40;
    memset(b - 32, 0x5a, 32);
    Cpu c = {0}; configure(&c, b);
    for (int i = 1; i <= 32; i++) assert(b[-i] == 0x5a);
    arg0(&c, b); hle_nvnSamplerBuilderGetDevice(&c); assert(c.x[0] == 0x7000abcd00ull);
    uint32_t values[3]; float f[2];
    arg0(&c, b); c.x[1] = (uintptr_t)&values[0]; c.x[2] = (uintptr_t)&values[1];
    hle_nvnSamplerBuilderGetMinMagFilter(&c); assert(values[0] == 3 && values[1] == 1);
    arg0(&c, b); c.x[3] = (uintptr_t)&values[2]; hle_nvnSamplerBuilderGetWrapMode(&c);
    assert(values[0] == 1 && values[1] == 7 && values[2] == 2);
    arg0(&c, b); c.x[1] = (uintptr_t)&f[0]; c.x[2] = (uintptr_t)&f[1];
    hle_nvnSamplerBuilderGetLodClamp(&c); assert(f[0] == -2.5f && f[1] == 9.25f);
    arg0(&c, b); hle_nvnSamplerBuilderGetLodBias(&c); memcpy(&f[0], &c.v[0], 4); assert(f[0] == 0.75f);
    arg0(&c, b); hle_nvnSamplerBuilderGetMaxAnisotropy(&c); memcpy(&f[0], &c.v[0], 4); assert(f[0] == 8);
    arg0(&c, b); c.x[1] = (uintptr_t)&values[0]; c.x[2] = (uintptr_t)&values[1];
    hle_nvnSamplerBuilderGetCompare(&c); assert(values[0] == 1 && values[1] == 6);
    arg0(&c, b); hle_nvnSamplerBuilderGetReductionFilter(&c); assert(c.x[0] == 2);
    arg0(&c, b); hle_nvnSamplerBuilderGetLodSnap(&c); memcpy(&f[0], &c.v[0], 4); assert(f[0] == 0);

    // Copying the opaque 64 bytes must preserve all state without host pointers.
    _Alignas(8) unsigned char copy[0x40]; memcpy(copy, b, sizeof copy);
    uint64_t sampler[6] = {0};
    arg0(&c, sampler); c.x[1] = (uintptr_t)b; hle_nvnSamplerInitialize(&c);
    check_desc(HOST(NvnSamplerDesc, (uintptr_t)sampler));
    arg0(&c, sampler); hle_nvnSamplerFinalize(&c);
    arg0(&c, sampler); c.x[1] = (uintptr_t)copy; hle_nvnSamplerInitialize(&c);
    check_desc(HOST(NvnSamplerDesc, (uintptr_t)sampler));
    arg0(&c, sampler); hle_nvnSamplerFinalize(&c);

    // Signed/unsigned border variants are raw bit copies, not float conversions.
    uint32_t border[4] = {0x80000000, 0xffffffff, 0x7fffffff, 0x7fc01234}, actual[4];
    HostFn setters[] = {hle_nvnSamplerBuilderSetBorderColori, hle_nvnSamplerBuilderSetBorderColorui};
    HostFn getters[] = {hle_nvnSamplerBuilderGetBorderColori, hle_nvnSamplerBuilderGetBorderColorui};
    for (int i = 0; i < 2; i++) {
        arg0(&c, b); c.x[1] = (uintptr_t)border; setters[i](&c);
        arg0(&c, b); c.x[1] = (uintptr_t)actual; getters[i](&c); assert(!memcmp(border, actual, 16));
        arg0(&c, sampler); c.x[1] = (uintptr_t)b; hle_nvnSamplerInitialize(&c);
        assert(!memcmp(HOST(NvnSamplerDesc, (uintptr_t)sampler)->border, border, 16));
        arg0(&c, sampler); hle_nvnSamplerFinalize(&c);
    }
    arg0(&c, b); hle_nvnSamplerBuilderSetDefaults(&c);
    arg0(&c, b); hle_nvnSamplerBuilderGetDevice(&c); assert(c.x[0] == 0x7000abcd00ull);
    assert(munmap(mem, 2 * page) == 0);
    puts("guard-page bounds, getters, device preservation, builder copy and border bit patterns: OK");
}

static void test_pool_conversion(void) {
    _Alignas(8) unsigned char b[0x40]; Cpu c = {0}; configure(&c, b);
    NvnPool storage = {.storage = 0x100000, .size = 0x1000};
    uint64_t memory[32] = {(uintptr_t)&storage}, pool[4] = {0}, sampler[6] = {0};
    arg0(&c, pool); c.x[1] = (uintptr_t)memory; c.x[2] = 0; c.x[3] = 4;
    hle_nvnSamplerPoolInitialize(&c); assert(c.x[0] == 1);
    NvnDescPool *p = HOST(NvnDescPool, (uintptr_t)pool);
    arg0(&c, pool); c.x[1] = 2; c.x[2] = (uintptr_t)b; hle_nvnSamplerPoolRegisterSamplerBuilder(&c);
    check_desc(&((NvnSamplerDesc *)p->entries)[2]);
    arg0(&c, sampler); c.x[1] = (uintptr_t)b; hle_nvnSamplerInitialize(&c);
    arg0(&c, pool); c.x[1] = 3; c.x[2] = (uintptr_t)sampler; hle_nvnSamplerPoolRegisterSampler(&c);
    check_desc(&((NvnSamplerDesc *)p->entries)[3]);
    arg0(&c, sampler); hle_nvnSamplerFinalize(&c);
    free(p->entries); free(p); memset(g_sampler_pools, 0, sizeof g_sampler_pools);
    puts("direct sampler and builder pool registration produce the same host descriptor: OK");
}

int main(void) {
    const struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
    test_real_frame(); test_guard_page_and_copy(); test_pool_conversion(); return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'Native harness runs on Linux.')
class SamplerBuilderTests(unittest.TestCase):
    def test_guest_bounds_frame_restore_and_host_conversion(self):
        probes = lifted_probe('sampler_frame_enter', [
            0xD101C3FF, 0xA9047BFD, 0x910103FD, 0xF9002BF5, 0xA9064FF4, 0xD65F03C0])
        probes += '\n' + lifted_probe('sampler_frame_leave', [
            0xF9402BF5, 0xA9464FF4, 0xA9447BFD, 0x9101C3FF, 0xD65F03C0])
        probes += '\n' + lifted_probe('post_sampler_store', [0xA93E7FBF, 0xD65F03C0])
        source = os.environ.get('SCARLET_NVN_TEST_SOURCE', str(ROOT / 'runtime/nvn/objects.c'))
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            harness = work / 'sampler.c'
            harness.write_text(HARNESS.replace('/*PROBES*/', probes).replace('OBJECTS_SOURCE', json.dumps(source)))
            exe = work / 'sampler_test'
            command = compiler_command() + ['-O2', '-fno-strict-aliasing', '-ffunction-sections',
                       '-fdata-sections', '-Wno-format', '-I' + str(ROOT / 'runtime'),
                       '-I' + str(ROOT / 'runtime/nvn'), str(harness), '-Wl,--gc-sections', '-lm', '-pthread', '-o', str(exe)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('following guest store succeeds', run.stdout)
            self.assertIn('guard-page bounds', run.stdout)
            self.assertIn('builder pool registration', run.stdout)
            print(run.stdout, end='')


if __name__ == '__main__':
    unittest.main(verbosity=2)
