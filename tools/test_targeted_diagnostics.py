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
    _Alignas(8) uint8_t control[0x880], header[0x80];
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
    nvn_diag_shader_release(&s);
    // Fill the ordinary quota with early UI programs.
    for (int i = 1; i < 128; i++) {
        s.stage = 0; s.code_addr = (uintptr_t)header + i;
        nvn_diag_shader(&s); nvn_diag_shader_release(&s);
    }
    assert(diag_nseen == 128 && !diag_snapshot_bytes);
    NvnProgram p = {0}; NvnCmdBuf cb = {.guest = 0x123};
    for (int i = 128; i < 257; i++) {
        memset(control, 0x5e, sizeof control);
        *(uint32_t *)(control + 0x00) = 0x98761234;
        *(uint32_t *)(control + 0x6f0) = 0x880;
        *(uint32_t *)(control + 0x6f8) = 0x50;
        *(uint32_t *)(control + 0x704) = 0x80;
        *(uint32_t *)(control + 0x708) = 0x30;
        *(uint32_t *)(control + 0x714) = 0;
        p.stages[0] = (NvnShaderStage){.stage = 0, .code_addr = (uintptr_t)header + i,
            .control = control};
        nvn_diag_shader(&p.stages[0]);
        // Once the ordinary quota is full, late programs consume no retained
        // snapshot memory. TF captures the currently bound program on use.
        assert(!p.stages[0].diag_snapshot);
        nvn_diag_bind_program(&cb, &p, 1);
        if (i == 128) {
            assert(diag_tf_nseen == 0); // bound before Begin
            nvn_diag_tf_state(&cb, "nvnCommandBufferBeginTransformFeedback");
            assert(diag_tf_nseen == 1);
        }
        nvn_diag_bind_program(&cb, &p, 1); // repeated TF bindings must deduplicate
        nvn_diag_bind_program(&cb, NULL, 1); // stage unbinding
    }
    assert(diag_tf_nseen == 128 && !diag_snapshot_bytes);
    // A TF output later consumed as a vertex source must be correlated.
    Cpu tf = {.x = {0x123, 2, 0x1000, 0x100}};
    nvn_diag_tf_call("nvnCommandBufferBindTransformFeedbackBuffer", &tf);
    uint64_t vb_range[2] = {0x1080, 0x40};
    nvn_diag_vertex_buffers(0x456, 5, 1, vb_range);
    assert(diag_tf_vertex_links == 1);
    nvn_diag_tf_state(&cb, "nvnCommandBufferPauseTransformFeedback");
    assert(!cb.diag_tf_active);
    nvn_diag_tf_state(&cb, "nvnCommandBufferResumeTransformFeedback");
    assert(cb.diag_tf_active);
    nvn_diag_tf_state(&cb, "nvnCommandBufferEndTransformFeedback");
    assert(!cb.diag_tf_active);
    // Binding before Begin must also be selected when Begin arrives.
    memset(control, 0x5e, sizeof control);
    *(uint32_t *)(control + 0x00) = 0x98761234;
    *(uint32_t *)(control + 0x6f0) = 0x880;
    *(uint32_t *)(control + 0x704) = 0x80;
    *(uint32_t *)(control + 0x708) = 0x30;
    p.stages[0] = (NvnShaderStage){.stage = 0, .code_addr = (uintptr_t)header + 128, .control = control};
    nvn_diag_shader(&p.stages[0]);
    nvn_diag_bind_program(&cb, &p, 1);
    nvn_diag_tf_state(&cb, "nvnCommandBufferBeginTransformFeedback");
    nvn_diag_shader_release(&p.stages[0]);
    assert(diag_nseen == 128 && diag_tf_nseen == 128 && !diag_snapshot_bytes);
}
'''

LIFETIME = r'''
#include <assert.h>
#include "nvn/objects.c"
#include "nvn/diagnostics.c"
int main(void) {
    _Alignas(8) uint8_t code[0x80] = {0}, control[20][0x704] = {{0}};
    *(uint32_t *)code = 0x12345678;
    *(uint32_t *)(code + 0x30) = 1u << 10; // vertex
    uint64_t data[3] = {(uintptr_t)code, (uintptr_t)control[0], 0};
    uint64_t guest_program = 0;
    Cpu c = {.x = {(uintptr_t)&guest_program}};
    hle_nvnProgramInitialize(&c);
    NvnProgram *p = (NvnProgram *)(uintptr_t)guest_program;
    for (int i = 0; i < 20; i++) {
        data[1] = (uintptr_t)control[i]; // distinct inputs exercise replacement without inventory dedup
        c.x[0] = (uintptr_t)&guest_program; c.x[1] = 1; c.x[2] = (uintptr_t)data;
        if (i % 2) hle_nvnProgramSetShadersExt(&c);
        else hle_nvnProgramSetShaders(&c);
        assert(c.x[0] == 1 && p->stages[0].diag_snapshot);
        assert(diag_snapshot_bytes == sizeof(DiagShaderSnapshot));
    }
    c.x[0] = (uintptr_t)&guest_program; hle_nvnProgramFinalize(&c);
    assert(!guest_program && !diag_snapshot_bytes);
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
            source = DUMPS.replace('header[0x80]', 'header[0x200]')
            exe = self.compile(work, 'dumps', source)
            out = work / 'output'; out.mkdir()
            env = dict(os.environ, LARECOMP_TF_DIAG_DIR=str(out))
            off = subprocess.run([str(exe), 'off'], env=env, capture_output=True, text=True)
            self.assertEqual(off.returncode, 0, off.stderr)
            self.assertFalse(list(out.iterdir()))
            run = subprocess.run([str(exe), 'on'], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            controls = list(out.glob('*.control.bin'))
            self.assertEqual(len(controls), 256)
            self.assertEqual(sum(p.stat().st_size == 0x704 for p in controls), 128)
            tf_controls = [p for p in controls if p.stat().st_size == 0x880]
            self.assertEqual(len(tf_controls), 128)
            self.assertTrue(all(int.from_bytes(p.read_bytes()[:4], 'little') == 0x98761234
                                for p in tf_controls))
            headers = list(out.glob('*.header.bin'))
            self.assertEqual(len(headers), 256)
            self.assertTrue(all(p.stat().st_size == 0x80 for p in headers))
            code = list(out.glob('*.code.bin'))
            self.assertEqual(len(code), 128)
            self.assertTrue(all(p.stat().st_size == 0x80 for p in code))
            self.assertEqual(len((out / 'tf_calls.txt').read_text().splitlines()), 128)
            links = (out / 'tf_vertex_links.txt').read_text().splitlines()
            self.assertEqual(len(links), 1)
            self.assertIn('vb=1080+40', links[0])
            self.assertIn('tf=1000+100', links[0])

    def test_program_replacement_and_finalize_release_snapshots(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = self.compile(work, 'lifetime', LIFETIME)
            out = work / 'output'; out.mkdir()
            run = subprocess.run([str(exe)],
                env=dict(os.environ, LARECOMP_TF_DIAG_DIR=str(out)),
                capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
