#!/usr/bin/env python3
"""Exercise bounded shader-control dumps and HLE profiling without game data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
PROFILE = r'''
#include <assert.h>
#include "dispatch.c"
static _Atomic uint64_t tick;
uint64_t plat_now_ns(void) { return atomic_fetch_add(&tick, 1000); }
static void change_registers(Cpu *c) { c->x[0]++; c->pc = 0x1234; }
static void outer(Cpu *c) {
    c->pc = HLE_BASE + 16;
    hle_profile_call(c);
    c->x[1] = 42;
}
static void *worker(void *arg) {
    Cpu c = {0};
    for (int i = 0; i < 2000; i++) {
        c.pc = HLE_BASE + 16; hle_profile_call(&c);
    }
    assert(c.x[0] == 2000 && c.pc == 0x1234);
    return NULL;
}
int main(void) {
    hle_slots[0] = (HleSlot){"outer", outer};
    hle_slots[1] = (HleSlot){"inner", change_registers}; hle_count = 2;
    Cpu c = {.pc = HLE_BASE}; hle_profile_call(&c);
    assert(c.x[0] == 1 && c.x[1] == 42 && c.pc == 0x1234);
    assert(atomic_load(&hle_prof_calls[0]) == 1 && atomic_load(&hle_prof_ns[0]) == 3000);
    assert(atomic_load(&hle_prof_calls[1]) == 1 && atomic_load(&hle_prof_ns[1]) == 1000);
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&hle_prof_calls[1]) == 16001);
    rt_hle_profile_report();
    assert(!atomic_load(&hle_prof_calls[0]) && !atomic_load(&hle_prof_calls[1]));
    assert(!atomic_load(&hle_prof_ns[0]) && !atomic_load(&hle_prof_ns[1]));
    rt_hle_profile_report(); // empty interval must not repeat previous results
}
'''
DUMPS = r'''
#include <assert.h>
#include "nvn/diagnostics.c"
int main(int argc, char **argv) {
    assert(argc == 2);
    _Alignas(8) uint8_t control[0x704], header[0x80];
    memset(control, 0xa5, sizeof control); memset(header, 0x7c, sizeof header);
    NvnShaderStage s = {.stage = 0, .code_addr = (uintptr_t)header, .control = control,
        .const_offset = 0x400, .const_size = 0x20};
    Cpu c = {.x = {0x123,0x456,0x789,0xabc}};
    if (!strcmp(argv[1], "off")) {
        unsetenv("LARECOMP_TF_DIAG_DIR");
        // Disabled diagnostics must not dereference caller-owned pointers.
        s.control = (void *)1; s.code_addr = 1;
        nvn_diag_shader(&s); nvn_diag_tf_call("disabled", &c);
        return 0;
    }
    for (int i = 0; i < 1000; i++) { nvn_diag_shader(&s); nvn_diag_tf_call("test", &c); }
    s.stage = 1; nvn_diag_shader(&s); s.stage = 5; nvn_diag_shader(&s);
    assert(diag_nseen == 1);
    // Distinct control objects test the hard output cap without dereferencing
    // synthetic invalid pointers after the collector is full.
    for (int i = 1; i < 128; i++) { s.stage = 0; s.code_addr = (uintptr_t)header + i; nvn_diag_shader(&s); }
    s.control = (void *)1; s.code_addr = 1; nvn_diag_shader(&s);
    assert(diag_nseen == 128);
}
'''


class DiagnosticsTests(unittest.TestCase):
    def compile(self, directory, name, source):
        src, exe = directory / (name + '.c'), directory / name
        src.write_text(source)
        run = subprocess.run(compiler_command() + ['-O1', '-UNDEBUG', '-ffunction-sections',
            '-fdata-sections', '-Wno-format', '-I' + str(ROOT / 'runtime'), str(src),
            '-Wl,--gc-sections', '-pthread', '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return exe

    def test_hle_register_results_nested_calls_concurrency_and_interval_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.compile(Path(tmp), 'profile', PROFILE)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stderr.count('top inclusive'), 1)
            self.assertIn('16001 calls inner', run.stderr)
            self.assertIn('1 calls outer', run.stderr)

    def test_dump_prefix_bounds_deduplication_and_disabled_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            # All actual header accesses remain in a real backing allocation.
            source = DUMPS.replace('header[0x80]', 'header[0x100]')
            exe = self.compile(work, 'dumps', source)
            out = work / 'output'; out.mkdir()
            env = dict(os.environ, LARECOMP_TF_DIAG_DIR=str(out))
            off = subprocess.run([str(exe), 'off'], env=env, capture_output=True, text=True)
            self.assertEqual(off.returncode, 0, off.stderr)
            self.assertFalse(list(out.iterdir()))
            run = subprocess.run([str(exe), 'on'], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            controls = list(out.glob('*.control.bin'))
            self.assertEqual(len(controls), 128)
            for p in controls:
                self.assertEqual(p.read_bytes(), b'\xa5' * 0x704)
            headers = list(out.glob('*.header.bin'))
            self.assertEqual(len(headers), 128)
            self.assertTrue(all(p.stat().st_size == 0x80 for p in headers))
            self.assertEqual(len((out / 'tf_calls.txt').read_text().splitlines()), 128)


if __name__ == '__main__':
    unittest.main(verbosity=2)
