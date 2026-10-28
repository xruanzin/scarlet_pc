#!/usr/bin/env python3
"""Run the production loader against synthetic DLL exports, without game files.

The Win32 library API is a test double; ABI negotiation, rollback, file discovery,
binding and exact-target checks execute the real aot_loader.c implementation.
"""
from pathlib import Path
import importlib.util
import shlex
import sys
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent

HEADER = r'''
#pragma once
#include <stdint.h>
typedef void *HMODULE;
typedef void (*FARPROC)(void);
HMODULE LoadLibraryA(const char *);
FARPROC GetProcAddress(HMODULE, const char *);
int FreeLibrary(HMODULE);
unsigned long GetLastError(void);
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "paths.h"
// Load platform-independent runtime headers first; mock only the DLL API.
#define _WIN32 1
#include "scarlet_native/aot_loader.c"
#include "scarlet_native/aot_execute.c"

Module g_modules[3];
int g_nmodules;
static int opens, closes, fail_at = -1, current, invalid_index;
static unsigned abi = 6, features;
static const char *missing;
static uint64_t base;
static uint32_t generation;
static int gg_ready;
static void block(ScarletAotGuestContext *c) { c->pc = 0; }
static ScarletAotBlockFn slots[4] = {block, NULL, block, NULL};
static ScarletAotBlockFn lookup(uint64_t pc) { return pc == base + 8 ? block : NULL; }
static void set_base(uint64_t value) { base = value; }
static int step, svc_seen, hle_seen;
static void run(ScarletAotGuestContext *c) {
    if (step == 0) {
        assert(c->pc == g_modules[0].base && c->x[0] == 7);
        assert(c->x[31] == 0x12340000 && c->vreg[31][1] == 99);
        assert(c->n == 1 && c->c == 1 && c->z == 0);
        assert(c->tpidr_el0 == 0x1234 && c->tpidrro_el0 == 0x5678);
        c->x[0] = 42; c->vreg[0][0] = 123;
        c->fpcr = 0x400000; c->fpsr = 16;
        c->pending_svc = 0xB; c->pc = g_modules[1].base;
    } else if (step == 1) {
        assert(c->pc == g_modules[1].base && svc_seen == 1);
        assert(c->x[0] == 43 && c->tpidr_el0 == 0x8765);
        assert(c->vreg[1][1] == 456);
        c->pc = HLE_BASE; c->x[30] = g_modules[2].base;
    } else {
        assert(step == 2 && hle_seen == 1 && c->pc == g_modules[2].base);
        assert(c->x[0] == 44 && c->vreg[2][1] == 789);
        c->pc = 0;
    }
    ++step;
}
static unsigned get_abi(void) { return abi; }
static unsigned get_features(void) { return features; }
static unsigned guard(unsigned version) { return version; }
static uint64_t entry(void) { return 0; }
static int index_export(uint64_t *lo, uint64_t *hi, ScarletAotBlockFn **out) {
    *lo = base; *hi = base + (invalid_index ? 0x10000 : 16); *out = slots; return 1;
}
static uint32_t *gg(unsigned v, uint64_t *lo, uint64_t *hi, const uint64_t **ptr) {
    assert(v == 2); gg_ready = 1; *lo = 0; *hi = 16; *ptr = &base; return &generation;
}
static unsigned fm(uint32_t p, uint32_t s, uint64_t mask, uint32_t table, uint32_t limit) {
    assert(p == 12 && s == 5 && mask == ~(uint64_t)3);
    assert(table == 872 && limit == 880); return gg_ready;
}
static unsigned fpx(uint32_t fpcr, uint32_t fpsr, uint64_t inhibit) {
    assert(fpcr == 848 && fpsr == 856 && inhibit == SCARLET_AOT_FPX_INHIBIT); return 1;
}
bool rt_resource(const char *rel, const char *fallback, char *out, size_t n) {
    (void)fallback; snprintf(out, n, "%s", rel); return true;
}
HMODULE LoadLibraryA(const char *path) {
    (void)path; gg_ready = 0; current = opens++; return current == fail_at ? NULL : (void *)(uintptr_t)(current + 1);
}
int FreeLibrary(HMODULE h) { assert(h); ++closes; return 1; }
unsigned long GetLastError(void) { return 126; }
FARPROC GetProcAddress(HMODULE h, const char *name) {
    (void)h;
    if (missing && !strcmp(name, missing)) return NULL;
#define EXPORT(n, f) if (!strcmp(name, "recomp_image_" n)) return (FARPROC)f
    EXPORT("lookup", lookup); EXPORT("set_base", set_base); EXPORT("run_slice", run);
    EXPORT("abi", get_abi); EXPORT("features", get_features); EXPORT("guard_v2", guard);
    EXPORT("index", index_export); EXPORT("entry", entry); EXPORT("guard_gen_v1", gg);
    EXPORT("fastmem_v1", fm); EXPORT("fpx_v1", fpx);
    return NULL;
}
void rt_unwind_point(void) {}
uint64_t rt_ticks(void) { return 100; }
uint64_t plat_now_ns(void) { static uint64_t ticks; return ++ticks; }
void rt_hle_profile_report(void) {}
void kernel_profile_report(void) {}
void rt_fatal_trace(void) { abort(); }
const char *loader_symbol_at(uint64_t pc, uint64_t *off) { (void)pc; *off = 0; return NULL; }
GuestFn dispatch_lookup(uint64_t pc) { (void)pc; return NULL; }
void rt_dispatch(Cpu *c) { (void)c; abort(); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    assert(imm == 0xB && pc == g_modules[1].base && c->x[0] == 42);
    assert((uint64_t)c->v[0] == 123 && c->fpcr == 0x400000 && c->fpsr == 16);
    c->x[0]++; c->tpidr = 0x8765; c->v[1] = (__uint128_t)456 << 64; ++svc_seen;
}
static void host_hle(Cpu *c) { c->x[0]++; c->v[2] = (__uint128_t)789 << 64; ++hle_seen; }
static uint64_t override_pc;
static int preflights, full_host_lookups, override_seen;
static void host_override(Cpu *c) { assert(c->pc == override_pc); c->x[0] += 10; ++override_seen; }
bool rt_dispatch_host_has_pc(uint64_t pc) {
    ++preflights;
    return pc == HLE_BASE || (override_pc && pc == override_pc);
}
GuestFn rt_dispatch_host_lookup(Cpu *c, uint64_t pc) {
    ++full_host_lookups;
    (void)c;
    if (pc == HLE_BASE) return host_hle;
    if (override_pc && pc == override_pc) return host_override;
    return NULL;
}
static void empty(void) {
    assert(!scarlet_aot_ready());
    for (int i = 0; i < 4; ++i) {
        assert(!g_aot[i].library && !g_aot[i].lookup && !g_aot[i].bound);
        assert(g_aot[i].filename && g_aot[i].name);
    }
}
int main(void) {
    char why[512];
    setenv("SCARLET_AOT_DIR", "missing-bundle", 1);
    assert(!scarlet_aot_present("."));
    unsetenv("SCARLET_AOT_DIR");
    assert(scarlet_aot_present("."));
    for (int i = 0; i < 4; ++i) {
        opens = closes = 0; fail_at = i;
        assert(!scarlet_aot_load(".", why, sizeof why));
        assert(strstr(why, "Win32 error 126"));
        assert(opens == i + 1 && closes == i); empty();
    }
    fail_at = -1; opens = closes = 0;
    missing = "recomp_image_run_slice";
    assert(!scarlet_aot_load(".", why, sizeof why));
    assert(strstr(why, missing) && opens == closes); empty();
    missing = NULL; abi = 5;
    assert(!scarlet_aot_load(".", NULL, 0)); empty();
    abi = 6; features = 0x80;
    assert(!scarlet_aot_load(".", why, sizeof why));
    assert(strstr(why, "unsupported AOT feature")); empty();
    features = SCARLET_AOT_FEATURE_SUPPORTED;
    assert(scarlet_aot_load(".", why, sizeof why));
    int before = opens;
    assert(scarlet_aot_load(".", NULL, 0) && opens == before);
    assert(!scarlet_aot_ready());
    g_nmodules = 3;
    const char *names[] = {"main", "subsdk0", "sdk"};
    for (int i = 0; i < 3; ++i)
        g_modules[i] = (Module){.name=names[i], .base=0x7000000000ull + i*0x1000,
                               .size=0x1000, .text_size=16};
    assert(scarlet_aot_bind_loaded_modules(why, sizeof why));
    assert(scarlet_aot_ready());
    for (int i = 0; i < 3; ++i) {
        assert(scarlet_aot_find_pc(g_modules[i].base) == &g_aot[i+1]);
        assert(!scarlet_aot_find_pc(g_modules[i].base + 1));
    }
    assert(!scarlet_aot_find_pc(0));
    assert(generation == 1);
    scarlet_aot_note_store(g_modules[0].base, 4);
    assert(generation == 2);
    scarlet_aot_note_store(g_modules[0].base + 128, 4);
    assert(generation == 2);
    g_nmodules = 0;
    assert(!scarlet_aot_bind_loaded_modules(NULL, 0));
    assert(!scarlet_aot_ready());
    g_nmodules = 3; invalid_index = 1;
    assert(!scarlet_aot_bind_loaded_modules(why, sizeof why));
    assert(!scarlet_aot_ready());
    invalid_index = 0;
    assert(scarlet_aot_bind_loaded_modules(why, sizeof why));
    Cpu cpu = {0};
    cpu.x[0] = 7; cpu.sp = 0x12340000; cpu.v[31] = (__uint128_t)99 << 64;
    cpu.nzcv = 0xa0000000; cpu.tpidr = 0x1234; cpu.tpidrro = 0x5678;
    scarlet_aot_guest_call(&cpu, g_modules[0].base);
    assert(step == 3 && svc_seen == 1 && hle_seen == 1 && cpu.pc == 0);
    assert(cpu.x[0] == 44 && cpu.sp == 0x12340000 && (uint64_t)(cpu.v[2] >> 64) == 789);
    // Three AOT slices (including a cross-module jump) needed only one
    // full Cpu lookup: the real HLE call. No guest-only slice may marshal
    // the Cpu state simply to decide whether an HLE override exists.
    assert(preflights == 4 && full_host_lookups == 1);
    override_pc = g_modules[0].base;
    cpu.x[30] = 0;
    cpu.x[0] = 50;
    scarlet_aot_guest_call(&cpu, override_pc);
    assert(step == 3 && override_seen == 1 && cpu.x[0] == 60);
    assert(full_host_lookups == 2 && preflights == 5);
    puts("native AOT: ABI, cross-module chaining, SVC, HLE, override and lazy Cpu marshaling passed");
}
'''

class NativeLoaderTest(unittest.TestCase):
    def test_loader_contract(self):
        if os.environ.get('CC'):
            cc = shlex.split(os.environ['CC'])
        elif shutil.which('clang'):
            cc = ['clang']
        elif importlib.util.find_spec('ziglang'):
            cc = [sys.executable, '-m', 'ziglang', 'cc']
        else:
            self.skipTest('Clang or Zig required')
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'windows.h').write_text(HEADER)
            (work / 'test.c').write_text(HARNESS)
            (work / 'aot').mkdir()
            for name in ('rtld', 'main', 'multimedia', 'sdk'):
                (work / 'aot' / f'scarlet-{name}.dll').touch()
            subprocess.run(cc + ['-std=gnu11', '-O1', '-mcx16', '-g', '-fsanitize=undefined',
                            '-I'+str(work), '-I'+str(ROOT/'runtime'),
                            str(work/'test.c'), '-o', str(work/'test')], check=True)
            env = os.environ.copy()
            env.pop('SCARLET_AOT_DIR', None)
            subprocess.run([str(work/'test')], cwd=work, env=env, check=True)

if __name__ == '__main__':
    unittest.main()
