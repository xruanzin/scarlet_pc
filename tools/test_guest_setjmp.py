#!/usr/bin/env python3
"""Synthetic protected-call tests of generated C and the production dispatcher."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from gen import Module
from test_indirect_resume import compiler_command, native_harness, write_fixture

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x7000000000


def fixture(path):
    image = write_fixture(path)
    image[:0x400] = bytes(0x400)
    struct.pack_into('<I', image, 4, 0x1000)
    # The parent must execute its +100 continuation exactly once after a jump.
    parent = [0xD10083FF, 0xA9007BFD, 0xF9000BF3, 0x9400003D,
              0x91019000, 0xF9400BF3, 0xA9407BFD, 0x910083FF, 0xD65F03C0]
    # A Lua-style protected call, no BR/switch in this unwind interval.
    protected = {0x200: 0xD10083FF, 0x204: 0xA9007BFD, 0x208: 0xF9000BF3,
                 0x20C: 0x910003FD, 0x210: 0xAA1403E0, 0x214: 0x9400003B,
                 0x218: 0x35000100, 0x21C: 0xAA1503E0, 0x220: 0xD63F02C0,
                 0x224: 0xD28000E0, 0x228: 0x14000004,
                 0x238: 0xF9400BF3, 0x23C: 0xA9407BFD, 0x240: 0x910083FF, 0x244: 0xD65F03C0}
    struct.pack_into('<9I', image, 0x100, *parent)
    for off in range(0x200, 0x248, 4):
        struct.pack_into('<I', image, off, protected.get(off, 0xD503201F))
    struct.pack_into('<I', image, 0x300, 0xD65F03C0)
    # Reuse the fixture symbol 'next' for a tiny synthetic setjmp callee, HLE'd
    # by this test. Guest context restoration is simulated by the callback.
    struct.pack_into('<QQ', image, 0x1120 + 3 * 24 + 8, 0x300, 4)
    struct.pack_into('<iiiiii', image, 0x11CC,
                     0x100 - 0x11C0, 0, 0x200 - 0x11C0, 0, 0x300 - 0x11C0, 0)
    header = bytearray(0x100); header[:4] = b'NSO0'; header[0x40:0x60] = b'\x11' * 32
    raw = header
    for i, (off, size) in enumerate(((0, 0x304), (0x1000, 0x400), (0x2000, 0x100))):
        struct.pack_into('<III', header, 0x10 + i * 16, len(raw), off, size)
        struct.pack_into('<I', header, 0x60 + i * 4, size)
        raw += image[off:off + size]
    path.write_bytes(raw)


HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include "gamelib.h"
#include "hle.h"
extern const GameLibTables *lasgame_tables(void);
const GameLibTables *gamelib_tables(void) { return lasgame_tables(); }
const uint64_t sdk_fn_count = 0, sdk_fn_addrs[] = {0}, sdk_resume_count = 0;
const GuestFn sdk_fn_ptrs[] = {0};
const GameLibResumeRange sdk_resume_ranges[] = {{0,0,0}};
const uint64_t subsdk0_fn_count = 0, subsdk0_fn_addrs[] = {0};
const GuestFn subsdk0_fn_ptrs[] = {0};
__attribute__((used, section("hle_impls"))) static const HleImplEntry dummy = {0};
volatile int g_fnprof, g_fnhook;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
int rt_fnhook(Cpu *c, uint64_t a) { (void)c; (void)a; return 0; }
void rt_unwind_point(void) {}
Cpu *kernel_current_cpu(void) { return NULL; }
const char *loader_symbol_at(uint64_t a, uint64_t *off) { (void)a; *off=0; return NULL; }
void plat_print_backtrace(void) {}
uint64_t plat_register_thread(void) { return 1; }
void plat_dump_other_threads(void) {}
void plat_fatal_notice(void) {}
static __thread Cpu saved[2];
static __thread int level, mode, value, calls, inner_returned;
static uint64_t callback_address;

void hle_next(Cpu *c) { saved[level] = *c; c->x[0] = 0; }
static void jump_to(Cpu *c, int target) {
    *c = saved[target]; c->x[0] = value ? value : 1;
    c->pc = c->x[30];
    rt_dispatch(c);
    // A metadata-only fix incorrectly returns here into abandoned native code.
    fputs("abandoned callback resumed after longjmp\n", stderr); abort();
}
static void callback(Cpu *c) {
    calls++;
    if (!mode) return;
    if ((mode == 2 || mode == 3) && level == 0) {
        level = 1; c->x[30] = 0x7000000390ull; c->pc = 0x7000000200ull;
        rt_dispatch(c);
        inner_returned++; level = 0; return;
    }
    jump_to(c, mode == 2 ? 0 : level);
}
static void run_case(int which, int result) {
    _Alignas(16) unsigned char stack[0x1000];
    Cpu c = {0}; c.sp = (uintptr_t)(stack + sizeof stack);
    c.x[19] = 19; c.x[20] = 20; c.x[21] = 21; c.x[22] = callback_address;
    c.x[29] = 0x123456780ull; c.x[30] = 0;
    uint64_t sp = c.sp;
    level = calls = inner_returned = 0; mode = which; value = result;
    guest_call(&c, 0x7000000100ull);
    assert(c.sp == sp && c.x[29] == 0x123456780ull && c.x[19] == 19 && c.x[30] == 0);
    uint64_t expected = which == 0 || which == 3 ? 107 : 100 + (result ? result : 1);
    assert(c.x[0] == expected);
    assert(calls == (which >= 2 ? 2 : 1));
    assert(inner_returned == (which == 3 ? 1 : 0));
}
static void *worker(void *arg) {
    for (int i=0; i<50; i++) run_case(i%4, (int)(uintptr_t)arg + i);
    return NULL;
}
int main(int argc, char **argv) {
    const struct rlimit no_core={0,0}; setrlimit(RLIMIT_CORE,&no_core);
    dispatch_init(); callback_address = hle_register("callback", callback);
    if (argc > 1) { run_case(1, 1); return 0; }
    for(int i=0;i<4;i++) { run_case(i,0); run_case(i,9); }
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)i));
    for(int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    puts("normal/zero/nonzero, inner/outer jumps, ancestor continuation, thread isolation and scope reuse: OK");
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'Native tests run on Linux.')
class GuestSetjmpTests(unittest.TestCase):
    def test_generated_protected_calls_unwind_native_frames(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp); nso = work / 'main'; fixture(nso)
            hle = work / 'hle.txt'; hle.write_text('next\n')
            harness = work / 'harness.c'; harness.write_text(HARNESS)
            base = [sys.executable, str(ROOT / 'tools/gen.py'), 'main', str(nso), hex(BASE)]
            for variant in ('fixed', 'split', 'missing', 'metadata_only'):
                gen = work / variant
                command = base + [str(gen), '--simd', 'c', '--hle', str(hle)]
                if variant in ('fixed', 'split'):
                    command += ['--setjmp-target', hex(BASE + 0x300)]
                if variant in ('split', 'metadata_only'):
                    command += ['--extra-entry', hex(BASE + 0x218)]
                run = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                exe = work / (variant + '_test')
                command = compiler_command() + ['-O2', '-fno-strict-aliasing', '-ffunction-sections',
                    '-fdata-sections', '-Wno-format', '-I' + str(ROOT / 'runtime'), str(harness),
                    str(ROOT / 'runtime/dispatch.c'), str(ROOT / 'tools/gamelib_entry.c')]
                command += [str(p) for p in sorted(gen.glob('*.c'))]
                command += ['-Wl,--gc-sections', '-pthread', '-o', str(exe)]
                compiled = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                run = subprocess.run([str(exe)] + (['negative'] if variant in ('missing','metadata_only') else []),
                                     capture_output=True, text=True, cwd=work)
                if variant in ('fixed', 'split'):
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertIn('ancestor continuation', run.stdout)
                    self.assertIn('main_setjmp_site_count = 1;', (gen / 'main_table.c').read_text())
                else:
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn('no function at 7000000218' if variant == 'missing' else
                                  'abandoned callback resumed', run.stderr)

    def test_automatic_target_is_build_scoped(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'main'; fixture(path)
            self.assertEqual(Module('main', str(path), BASE, 'c').setjmp_targets, set())
            raw = bytearray(path.read_bytes())
            raw[0x40:0x60] = bytes.fromhex('ca44e7dd62e61a2a4e8889b3319ae173375770fd').ljust(32, b'\0')
            path.write_bytes(raw)
            self.assertEqual(Module('main', str(path), BASE, 'c').setjmp_targets, {BASE + 0x3017B70})
            self.assertEqual(Module('sdk', str(path), BASE, 'c').setjmp_targets, set())


if __name__ == '__main__':
    unittest.main(verbosity=2)
