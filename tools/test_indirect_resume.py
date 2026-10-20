#!/usr/bin/env python3
"""Main/SDK interior-branch regression tests, using only synthetic ARM64 code.

Requires the generator dependencies (lz4, unicorn, capstone). The native
dispatch harness runs on Linux with Clang or Zig; discovery tests are portable.
Run: python tools/test_indirect_resume.py
CC can name a compiler command, e.g. CC='zig cc'. No game files are needed.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import unicorn as uc
from unicorn import arm64_const as A

from gen import Module

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x7006000000


def write_fixture(path):
    """An unwind function split by a NOTYPE entry before its switch case.

    100: prologue; increment x19; br x10; ret
    110: increment x20 (must be skipped on resume)
    114: case: mov w0,7; epilogue; ret
    120: a separate unwind function returning 99
    """
    image = bytearray(0x3000)
    struct.pack_into('<I', image, 4, 0x1000)  # MOD0 pointer
    words = (0xD10043FF, 0x91000673, 0xD61F0140, 0xD65F03C0,
             0x91000694, 0x528000E0, 0x910043FF, 0xD65F03C0,
             0x52800C60, 0xD65F03C0)
    struct.pack_into('<10I', image, 0x100, *words)
    # Compact byte-indexed switch with the same CMP/MOV/ADRP/ADD/ADR/LDRB/
    # ADD/BR shape as main+0xa7daf0 in the supplied boot. ADR splits the
    # emitted function at 0x234; the selected case at 0x244 is not an entry.
    compact = {
        0x200: 0xD10043FF, 0x204: 0x91000673, 0x208: 0x71004C3F,
        0x20C: 0x540002A8, 0x210: 0x2A0103E8, 0x214: 0xB0000009,
        0x218: 0x91080129, 0x21C: 0x100000CA, 0x220: 0x3868692B,
        0x224: 0x8B0B094A, 0x228: 0xD61F0140,
        0x234: 0x528000E0, 0x238: 0x910043FF, 0x23C: 0xD65F03C0,
        0x244: 0x52800540, 0x248: 0x17FFFFFC,
        0x260: 0x52800C60, 0x264: 0x910043FF, 0x268: 0xD65F03C0,
    }
    for at in range(0x200, 0x26C, 4):
        struct.pack_into('<I', image, at, compact.get(at, 0xD503201F))
    image[0x1200:0x1214] = bytes([0, 4] * 10)
    image[0x1000:0x1004] = b'MOD0'
    fields = (0x1040, 0x2100, 0x2100, 0x11C0, 0x11E4, 0x2080)
    struct.pack_into('<6i', image, 0x1004, *(a - 0x1000 for a in fields))
    for i, (tag, val) in enumerate(((12, 0x100), (4, 0x1100),
                                  (6, 0x1120), (5, 0x1180), (0, 0))):
        struct.pack_into('<QQ', image, 0x1040 + 16 * i, tag, val)
    struct.pack_into('<II', image, 0x1100, 1, 4)  # DT_HASH, 4 symbols
    names = b'\0entry\0fragment\0next\0'
    image[0x1180:0x1180 + len(names)] = names
    for i, (name, info, value, size) in enumerate(((0, 0, 0, 0),
            (1, 2, 0x100, 0x20), (7, 0, 0x110, 0), (16, 2, 0x120, 8))):
        struct.pack_into('<IBBHQQ', image, 0x1120 + 24 * i,
                         name, info, 0, int(i != 0), value, size)
    # eh_frame_hdr: data-relative signed table, three real unwind starts.
    struct.pack_into('<BBBBiIiiiiii', image, 0x11C0, 1, 0x1B, 3, 0x3B,
                     0, 3, 0x100 - 0x11C0, 0, 0x120 - 0x11C0, 0,
                     0x200 - 0x11C0, 0)
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    header[0x40:0x60] = bytes.fromhex('11' * 32)
    raw = header
    for i, (off, size) in enumerate(((0, 0x26C), (0x1000, 0x400), (0x2000, 0x100))):
        struct.pack_into('<III', header, 0x10 + i * 16, len(raw), off, size)
        struct.pack_into('<I', header, 0x60 + i * 4, size)
        raw += image[off:off + size]
    # raw and header are the same bytearray; segment headers were filled in place.
    path.write_bytes(raw)
    return image


def compiler_command():
    if os.environ.get('CC'):
        return shlex.split(os.environ['CC'])
    if shutil.which('clang'):
        return ['clang']
    if shutil.which('zig'):
        return ['zig', 'cc']
    if importlib.util.find_spec('ziglang'):
        return [sys.executable, '-m', 'ziglang', 'cc']
    raise unittest.SkipTest('Native tests require Clang or Zig (or CC).')


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'sdk'
        write_fixture(self.path)
        self.m = Module('sdk', str(self.path), BASE, 'c')
        self.entries = {BASE + a: (BASE + end, *self._scan(a, end))
                        for a, end in ((0x100, 0x110), (0x110, 0x120), (0x120, 0x128))}

    def _scan(self, start, end):
        labels, ext, tables = self.m.scan(BASE + start, BASE + end)
        return labels, tables

    def test_loaded_br_can_reach_local_instruction(self):
        # BR x10 has no nearby arithmetic or bounds guard. The original
        # generator sent it straight to global dispatch, even for local PCs.
        self.assertIn(BASE + 0x108, self.entries[BASE + 0x100][2])
        body = '\n'.join(self.m.emit_function(BASE + 0x100, *self.entries[BASE + 0x100], {}))
        self.assertIn('goto L_dispatch;', body)

    def test_resume_covers_other_fragment_but_not_next_unwind_function(self):
        resumes = self.m.indirect_resume_entries(self.entries, {})
        self.assertEqual(resumes, {BASE + 0x100, BASE + 0x110})

    def test_hle_function_keeps_its_override(self):
        resumes = self.m.indirect_resume_entries(self.entries, {BASE + 0x110: 'test_hle'})
        self.assertEqual(resumes, {BASE + 0x100})

    def test_missing_unwind_uses_known_boundaries(self):
        self.m.nso.eh_start = 0
        resumes = self.m.indirect_resume_entries(self.entries, {})
        self.assertEqual(resumes, {BASE + 0x100})


HARNESS = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "gamelib.h"
#include "hle.h"

extern const GameLibTables *lasgame_tables(void);
const GameLibTables *gamelib_tables(void) { return lasgame_tables(); }
@OTHER_TABLES@
__attribute__((used, section("hle_impls"))) static const HleImplEntry dummy = {0};
volatile int g_fnprof, g_fnhook;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
int rt_fnhook(Cpu *c, uint64_t a) { (void)c; (void)a; return 0; }
void rt_unwind_point(void) {}
uint64_t plat_now_ns(void) { return 0; }
Cpu *kernel_current_cpu(void) { return NULL; }
const char *loader_symbol_at(uint64_t a, uint64_t *off) { (void)a; *off = 0; return NULL; }
void plat_print_backtrace(void) {}
uint64_t plat_register_thread(void) { return 1; }
void plat_dump_other_threads(void) {}
void plat_fatal_notice(void) {}
void @FRAGMENT@(Cpu *c);
static void override(Cpu *c) { c->x[0] = 1234; }

int main(int argc, char **argv) {
    assert(argc == 7);
    const struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    assert(lasgame_tables()->abi == GAMELIB_ABI);
    dispatch_init();
    const uint64_t b = @BASE@ull;
    assert(dispatch_lookup(b + 0x100));
#ifndef TEST_NO_RESUME
    assert(dispatch_lookup(b + 0x104));
    assert(dispatch_lookup(b + 0x11c));
#endif
    assert(dispatch_lookup(b + 0x120));
    assert(!dispatch_lookup(b + 0xff));
    assert(!dispatch_lookup(b + 0x115));
    assert(!dispatch_lookup(b + 0x124));  // valid text, outside a declared resume range
    assert(!dispatch_lookup(b + 0x128));
    Cpu c = {0};
    c.pc = b + 0x114;  // stale PC must not change a normal direct function call
    c.x[10] = b + strtoull(argv[2], NULL, 0);
    c.x[19] = strtoull(argv[3], NULL, 0);
    c.x[20] = strtoull(argv[4], NULL, 0);
    c.sp = strtoull(argv[5], NULL, 0);
    c.x[30] = strtoull(argv[6], NULL, 0);
    switch (atoi(argv[1])) {
        case 0: c.pc = b + 0x100; rt_dispatch(&c); break;
        case 1: @FRAGMENT@(&c); break;
        case 2: rt_dispatch(&c); break;
        case 3: hle_override(b + 0x114, override, "test"); rt_dispatch(&c); break;
        case 4: {
            void *ro = mmap((void *)(uintptr_t)(b + 0x1000), 0x1000,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            assert(ro == (void *)(uintptr_t)(b + 0x1000));
            for (int i = 0; i < 20; i++) ((unsigned char *)ro)[0x200 + i] = (i & 1) ? 4 : 0;
            c.x[1] = strtoull(argv[2], NULL, 0);
            c.pc = b + 0x200;
            rt_dispatch(&c);
            break;
        }
        default: abort();
    }
    printf("%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
           c.x[0], c.x[19], c.x[20], c.sp, c.x[30]);
    return 0;
}
'''


def native_harness(module, base):
    others = []
    for name in ('main', 'sdk', 'subsdk0'):
        if name == module:
            continue
        others.append(f'const uint64_t {name}_fn_count = 0, {name}_fn_addrs[] = {{0}};')
        others.append(f'const GuestFn {name}_fn_ptrs[] = {{0}};')
        others.append(f'const GameLibResumeRange {name}_resume_ranges[] = {{{{0, 0, 0}}}};')
        others.append(f'const uint64_t {name}_resume_count = 0;')
    return (HARNESS.replace('@OTHER_TABLES@', '\n'.join(others))
            .replace('@FRAGMENT@', f'f_{base + 0x110:x}')
            .replace('@BASE@', hex(base)))


@unittest.skipUnless(sys.platform.startswith('linux'), 'Native dispatch harness runs on Linux.')
class NativeTests(unittest.TestCase):
    module = 'sdk'
    base = BASE
    @classmethod
    def setUpClass(cls):
        cls.cc = compiler_command()
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        nso = work / cls.module
        cls.image = write_fixture(nso)
        gen = work / 'generated'
        subprocess.run([sys.executable, str(ROOT / 'tools/gen.py'), cls.module, str(nso),
                        hex(cls.base), str(gen), '--simd', 'c', '--indirect-resume'],
                       check=True, capture_output=True, text=True)
        harness = work / 'harness.c'
        harness.write_text(native_harness(cls.module, cls.base))
        cls.exe = work / 'resume_test'
        cls.compile_args = ['-O2', '-UNDEBUG', '-g', '-ffunction-sections', '-fdata-sections', '-Wno-format',
                    '-I' + str(ROOT / 'runtime'), str(harness),
                    str(ROOT / 'runtime/dispatch.c'), str(ROOT / 'tools/gamelib_entry.c')]
        cmd = cls.cc + cls.compile_args
        cmd += [str(p) for p in sorted(gen.glob('*.c'))]
        cmd += ['-Wl,--gc-sections', '-pthread', '-o', str(cls.exe)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f'Native compile failed:\n{result.stderr}')

    def test_without_resume_reproduces_missing_interior_target(self):
        work = Path(self.temp.name)
        gen = work / 'without_resume'
        subprocess.run([sys.executable, str(ROOT / 'tools/gen.py'), self.module, str(work / self.module),
                        hex(self.base), str(gen), '--simd', 'c'],
                       check=True, capture_output=True, text=True)
        exe = work / 'without_resume_test'
        cmd = self.cc + self.compile_args + ['-DTEST_NO_RESUME']
        cmd += [str(p) for p in sorted(gen.glob('*.c'))]
        cmd += ['-Wl,--gc-sections', '-pthread', '-o', str(exe)]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        result = subprocess.run([str(exe), '0', '0x114', '41', '71', '2097152', str(self.base + 0x2000)],
                                capture_output=True, text=True, cwd=work)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'no function at {self.base + 0x114:x}', result.stderr)
        if self.module == 'main':
            result = subprocess.run([str(exe), '4', '1', '41', '71', '2097152', str(self.base + 0x2000)],
                                    capture_output=True, text=True, cwd=work)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f'no function at {self.base + 0x244:x}', result.stderr)

    def run_native(self, mode, target, x19, x20, sp, lr):
        result = subprocess.run([str(self.exe), str(mode), hex(target), str(x19),
                                 str(x20), str(sp), str(lr)],
                                check=True, capture_output=True, text=True)
        return tuple(map(int, result.stdout.split()))

    def run_arm64(self, entry, target, x19, x20, sp, lr):
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.mem_map(self.base, len(self.image))
        mu.mem_write(self.base, bytes(self.image))
        for reg, value in ((A.UC_ARM64_REG_X10, self.base + target),
                           (A.UC_ARM64_REG_X19, x19), (A.UC_ARM64_REG_X20, x20),
                           (A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_X30, lr)):
            mu.reg_write(reg, value)
        mu.emu_start(self.base + entry, lr, count=100)
        self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), lr)
        return tuple(mu.reg_read(r) for r in (A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X19,
                                              A.UC_ARM64_REG_X20, A.UC_ARM64_REG_SP,
                                              A.UC_ARM64_REG_X30))

    def test_native_matches_arm64_for_cross_fragment_switch_cases(self):
        # In particular 0x114 is not a normal function-table entry. A prologue
        # replay would increment x20, and a missing resume would abort.
        for i in range(32):
            for target in (0x110, 0x114, 0x118, 0x120):
                with self.subTest(i=i, target=hex(target)):
                    args = (target, i * 17, 0x1000 + i, 0x200000 + 16 * i, self.base + 0x2000)
                    self.assertEqual(self.run_native(0, *args), self.run_arm64(0x100, *args))

    def test_normal_direct_entry_ignores_stale_dispatch_pc(self):
        args = (0x114, 41, 71, 0x200000, self.base + 0x2000)
        self.assertEqual(self.run_native(1, *args), self.run_arm64(0x110, *args))

    def test_resume_preserves_lr_sp_and_skips_guest_prologue(self):
        args = (0x114, 41, 71, 0x200000, self.base + 0x2000)
        self.assertEqual(self.run_native(2, *args), self.run_arm64(0x114, *args))

    def test_hle_override_wins_over_resume_range(self):
        got = self.run_native(3, 0x114, 41, 71, 0x200000, self.base + 0x2000)
        self.assertEqual(got, (1234, 41, 71, 0x200000, self.base + 0x2000))


class MainNativeTests(NativeTests):
    module = 'main'
    base = 0x7000000000

    def test_compact_byte_switch_matches_arm64(self):
        for index in range(24):  # both table cases and the out-of-bounds path
            with self.subTest(index=index):
                args = (index, 41, 71, 0x200000, self.base + 0x2000)
                mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
                mu.mem_map(self.base, len(self.image))
                mu.mem_write(self.base, bytes(self.image))
                for reg, val in ((A.UC_ARM64_REG_X1, index), (A.UC_ARM64_REG_X19, 41),
                                 (A.UC_ARM64_REG_X20, 71), (A.UC_ARM64_REG_SP, args[3]),
                                 (A.UC_ARM64_REG_X30, args[4])):
                    mu.reg_write(reg, val)
                mu.emu_start(self.base + 0x200, args[4], count=100)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), args[4])
                expected = tuple(mu.reg_read(r) for r in (A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X19,
                                   A.UC_ARM64_REG_X20, A.UC_ARM64_REG_SP, A.UC_ARM64_REG_X30))
                self.assertEqual(self.run_native(4, *args), expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
