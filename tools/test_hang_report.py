#!/usr/bin/env python3
"""Exercise real watchdog C code without game files or suspending real OS threads.

Windows calls are mocked for failure-path tests; the complete Windows platform
file is also compiled against Zig's real Windows SDK. No Windows runtime claim.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent


def region(file, start, end):
    s = (ROOT / file).read_text()
    a = s.index(start)
    return s[a:s.index(end, a)]


HISTORY = region('runtime/dispatch.c', '// ---- recent-HLE ring.', '\nstatic __thread uint64_t unimpl_pc;')
WINDOWS = region('runtime/platform/windows.c', '// ---- hang report:', '\nstatic LONG WINAPI unhandled_filter')
WORKER = region('runtime/nvn/vulkan/device.c', 'static atomic_bool hang_report_pending;', 'static void title_fps_update')

HISTORY_HARNESS = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
typedef struct { uint64_t x[31]; } Cpu;
static uint64_t plat_register_thread(void) { return (uint64_t)(uintptr_t)pthread_self(); }
static const char *loader_symbol_at(uint64_t pc, uint64_t *off) { return NULL; }
static void plat_dump_other_threads(void) {}
static void kernel_dump_guest_threads(void) {}
/*PRODUCTION*/
static atomic_bool done;
static void *write_calls(void *arg) {
    Cpu c = {0};
    for (unsigned i = 0; i < 100000; i++) {
        c.x[0] = i; c.x[1] = i ^ 0x1234; c.x[30] = i + 99;
        hhist_push("test-hle", &c);
    }
    atomic_store(&done, true);
    return NULL;
}
static void check_snapshot(const HhSnapshot *s) {
    assert(s->tid);
    uint64_t n = s->n < HHIST ? s->n : HHIST;
    for (uint64_t i = s->n - n; i < s->n; i++) {
        const HhEntry *e = &s->e[i & (HHIST - 1)];
        assert(e->name && !strcmp(e->name, "test-hle"));
        assert(e->x1 == (e->x0 ^ 0x1234) && e->x30 == e->x0 + 99);
    }
}
int main(void) {
    pthread_t t; assert(!pthread_create(&t, NULL, write_calls, NULL));
    unsigned snapshots = 0;
    while (!atomic_load(&done)) {
        HhSnapshot s;
        if (hhist_snapshot(&hh_all[0], &s) && s.n) { check_snapshot(&s); snapshots++; }
        sched_yield();
    }
    assert(!pthread_join(t, NULL));
    // Shared storage remains valid after its writer's TLS has been destroyed.
    HhSnapshot s; assert(hhist_snapshot(&hh_all[0], &s)); check_snapshot(&s);
    assert(s.n && s.n <= 100000);
    HhSnapshot saved = s;
    rt_hang_report();
    // A busy diagnostic snapshot must not wait or overwrite data.
    hh_current = &hh_all[0]; atomic_store(&hh_current->busy, true);
    Cpu c = {0}; hhist_push("must-be-dropped", &c);
    assert(!hhist_snapshot(hh_current, &s));
    atomic_store(&hh_current->busy, false);
    assert(hhist_snapshot(hh_current, &s)); assert(s.n == saved.n);
    hhist_dump();
    // Exhaustion uses a private fallback, never a TLS pointer in the shared registry.
    hh_current = NULL; atomic_store(&hh_nall, MAX_HH_THREADS);
    hhist_push("overflow", &c); assert(hh_current == &hh_fallback);
    rt_hang_report();
}
'''

WINDOWS_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#define WINAPI
#define VOID void
#define FALSE 0
#define TRUE 1
#define STILL_ACTIVE 259
#define THREAD_QUERY_INFORMATION 64
#define WCT_MAX_NODE_COUNT 16
#define ERROR_SUCCESS 0
#define ERROR_ACCESS_DENIED 5
#define ERROR_MORE_DATA 234
#define ERROR_TOO_MANY_THREADS 565
#define WctThreadType 8
typedef void *HANDLE, *HMODULE, *HWCT, *PVOID, *PWAITCHAINCALLBACK;
typedef uint32_t DWORD, *LPDWORD;
typedef uintptr_t DWORD_PTR;
typedef int32_t LONG;
typedef int BOOL, *LPBOOL;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { int ObjectType, ObjectStatus; struct { DWORD ProcessId, ThreadId; } ThreadObject; } WAITCHAIN_NODE_INFO, *PWAITCHAIN_NODE_INFO;
static int mode, duplicates, opens, gets, closes, frees;
static DWORD current_tid = 42;
static DWORD GetCurrentThreadId(void) { return current_tid; }
static HANDLE GetCurrentProcess(void) { return (void*)1; }
static HANDLE GetCurrentThread(void) { return (void*)2; }
static LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
    __atomic_compare_exchange_n(p, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return expected;
}
static PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID value, PVOID expected) {
    __atomic_compare_exchange_n(p, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return expected;
}
static BOOL DuplicateHandle(HANDLE a, HANDLE b, HANDLE c, HANDLE *out, DWORD rights, BOOL inherit, DWORD flags) {
    assert(a == (void*)1 && b == (void*)2 && c == a && rights == THREAD_QUERY_INFORMATION && !inherit && !flags);
    duplicates++; *out = (void*)3; return mode != 5;
}
static DWORD GetLastError(void) { return mode == 3 ? ERROR_MORE_DATA : ERROR_ACCESS_DENIED; }
static BOOL GetExitCodeThread(HANDLE h, DWORD *code) { assert(h == (void*)3); *code = mode == 4 ? 0 : STILL_ACTIVE; return TRUE; }
static BOOL GetThreadTimes(HANDLE h, FILETIME *a, FILETIME *b, FILETIME *k, FILETIME *u) {
    assert(h == (void*)3); *k = (FILETIME){30000,0}; *u = (FILETIME){90000,0}; return TRUE;
}
static HWCT open_mock(DWORD flags, PWAITCHAINCALLBACK cb) { assert(!flags && !cb); opens++; return mode == 1 ? NULL : (void*)4; }
static BOOL get_mock(HWCT h, DWORD_PTR ctx, DWORD flags, DWORD tid, DWORD *n, PWAITCHAIN_NODE_INFO nodes, BOOL *cycle) {
    assert(h == (void*)4 && !ctx && !flags && tid == 42 && *n == 16); gets++;
    if (mode == 2) return FALSE;
    for (unsigned i = 0; i < 16; i++) { nodes[i].ObjectType = WctThreadType; nodes[i].ObjectStatus = 2; nodes[i].ThreadObject.ThreadId = 42 + i; }
    *n = mode == 3 ? 64 : 2; *cycle = TRUE; return mode != 3;
}
static void close_mock(HWCT h) { assert(h == (void*)4); closes++; }
static HMODULE LoadLibraryW(const void *name) { return (void*)5; }
static void *GetProcAddress(HMODULE dll, const char *name) {
    assert(dll == (void*)5);
    if (!strcmp(name, "OpenThreadWaitChainSession")) return (void*)open_mock;
    if (!strcmp(name, "GetThreadWaitChain")) return (void*)get_mock;
    if (!strcmp(name, "CloseThreadWaitChainSession")) return (void*)close_mock;
    abort();
}
static BOOL FreeLibrary(HMODULE dll) { assert(dll == (void*)5); frees++; return TRUE; }
/*PRODUCTION*/
int main(int argc, char **argv) {
    assert(argc == 2); mode = atoi(argv[1]);
    assert(plat_register_thread() == 42); assert(plat_register_thread() == 42); assert(duplicates == 1);
    current_tid = 99;
    plat_dump_other_threads();
    assert(opens == 1 && frees == 1 && closes == (mode != 1));
    assert(gets == (mode != 1 && mode != 4 && mode != 5));
    if (mode == 0) { current_tid = 42; plat_dump_other_threads(); assert(gets == 1); }
}
'''

WORKER_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int pthread_t;
static int creates, detaches, reports, fail;
static void *(*entry)(void *);
static int pthread_create(pthread_t *t, void *attrs, void *(*fn)(void *), void *arg) { creates++; entry = fn; return fail; }
static int pthread_detach(pthread_t t) { detaches++; return 0; }
static void rt_hang_report(void) { reports++; }
#define LOG(...) fprintf(stderr, __VA_ARGS__)
/*PRODUCTION*/
int main(void) {
    request_hang_report(); request_hang_report(); assert(creates == 1 && detaches == 1 && !reports);
    entry(NULL); assert(reports == 1 && !atomic_load(&hang_report_pending));
    fail = 11; request_hang_report(); assert(creates == 2 && !atomic_load(&hang_report_pending));
    fail = 0; request_hang_report(); assert(creates == 3 && detaches == 2);
    entry(NULL); assert(reports == 2);
}
'''


class HangReportTests(unittest.TestCase):
    def run_c(self, harness, production, args=(), flags=()):
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d) / 'probe.c', Path(d) / 'probe'
            src.write_text(harness.replace('/*PRODUCTION*/', production))
            r = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-pthread', *flags, str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stderr

    def test_history_concurrent_snapshot_and_thread_exit(self):
        output = self.run_c(HISTORY_HARNESS, HISTORY)
        self.assertIn('test-hle', output)
        self.assertIn('os tid ', output)
        self.assertIn('history capacity reached', output)

    def test_windows_passive_state_and_failure_paths(self):
        for mode in range(6):
            with self.subTest(mode=mode):
                out = self.run_c(WINDOWS_HARNESS, WINDOWS, [str(mode)])
                if mode in (0, 3): self.assertIn('cycle=YES', out)
                if mode in (0, 1, 2, 3): self.assertIn('CPU user=9 ms kernel=3 ms', out)
                if mode == 1: self.assertIn('CPU times only', out)
                if mode == 2: self.assertIn('wait chain unavailable (5)', out)
                if mode == 3:
                    self.assertIn('nodes=16', out)
                    self.assertNotIn('node 16 ', out)
        # The automatic reporter must never acquire suspend rights or inspect live stacks.
        for forbidden in ('SuspendThread(', 'RtlVirtualUnwind(', 'GetThreadContext(', 'THREAD_SUSPEND_RESUME'):
            self.assertNotIn(forbidden, WINDOWS)

    def test_diagnostics_leave_ui_free_and_limit_pending_reports(self):
        self.run_c(WORKER_HARNESS, WORKER)
        ui = region('runtime/nvn/vulkan/device.c', 'static void title_fps_update', '\nint ui_run(')
        self.assertIn('request_hang_report();', ui)
        self.assertNotIn('rt_hang_report();', ui)

    def test_complete_windows_platform_compiles(self):
        if shutil.which('zig'):
            cc = ['zig', 'cc']
        elif importlib.util.find_spec('ziglang'):
            cc = [sys.executable, '-m', 'ziglang', 'cc']
        else:
            self.skipTest('Zig Windows SDK required')
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(cc + ['-target', 'x86_64-windows-gnu', '-O2', '-D_FILE_OFFSET_BITS=64', '-DWIN32_LEAN_AND_MEAN',
                '-DNOMINMAX', '-I' + str(ROOT / 'runtime'), '-c', str(ROOT / 'runtime/platform/windows.c'), '-o', str(Path(d) / 'windows.o')],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
