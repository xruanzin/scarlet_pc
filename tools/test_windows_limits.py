#!/usr/bin/env python3
"""Exercise the Windows resource calls with mocks and compile their real SDK declarations when Zig is available.

The mocked tests verify policy and API call ordering. They do not claim an on-device Windows memory test.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = (ROOT / 'runtime/platform/windows.c').read_text()


def region(start, end):
    a = WINDOWS.index(start)
    return WINDOWS[a:WINDOWS.index(end, a)]


MEMORY = region('static uint64_t process_commit_limit(', 'void plat_init(')
BOOST = region('void plat_set_cpu_boost(', 'void plat_guest_thread_config(')
EMERGENCY = region('void plat_emergency_exit(', '\n}',) + '\n}\n'

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef uint64_t SIZE_T;
typedef struct { DWORD dwLength; uint64_t ullTotalPhys, ullAvailPhys; } MEMORYSTATUSEX;
typedef struct { struct { DWORD LimitFlags; } BasicLimitInformation; SIZE_T ProcessMemoryLimit; } JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY 0x100
#define JobObjectExtendedLimitInformation 9
#define NORMAL_PRIORITY_CLASS 32
static int mode, closed, assigned, configured, priorities;
static bool GlobalMemoryStatusEx(MEMORYSTATUSEX *s) { s->ullTotalPhys = 16ull << 30; s->ullAvailPhys = 12ull << 30; return true; }
static HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)9; }
static bool SetPriorityClass(HANDLE p, DWORD cls) { assert(p == GetCurrentProcess() && cls == NORMAL_PRIORITY_CLASS); priorities++; return true; }
static HANDLE CreateJobObjectW(void *attrs, void *name) { return (HANDLE)(uintptr_t)1; }
static bool SetInformationJobObject(HANDLE h, int cls, void *p, DWORD size) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION *limits = p;
    assert(h && cls == 9 && size == sizeof *limits);
    assert(limits->BasicLimitInformation.LimitFlags == JOB_OBJECT_LIMIT_PROCESS_MEMORY);
    assert(limits->ProcessMemoryLimit == 8ull << 30); configured++;
    return mode != 1;
}
static bool AssignProcessToJobObject(HANDLE h, HANDLE p) { assert(h && p == GetCurrentProcess()); assigned++; return mode != 2; }
static DWORD GetLastError(void) { return 5; }
static void CloseHandle(HANDLE h) { assert(h); closed++; }
/*PRODUCTION*/
int main(int argc, char **argv) {
    assert(argc == 2); mode = atoi(argv[1]);
    assert(process_commit_limit(16ull << 30, 12ull << 30) == 8ull << 30);
    assert(process_commit_limit(64ull << 30, 32ull << 30) == 8ull << 30);
    assert(process_commit_limit(8ull << 30, 6ull << 30) < 5ull << 30);
    assert(process_commit_limit(16ull << 30, 4ull << 30) == 3ull << 30);
    plat_set_cpu_boost(false); plat_set_cpu_boost(true); plat_set_cpu_boost(false); assert(priorities == 3);
    limit_process_memory(); assert(configured == 1);
    if (!mode) { assert(assigned == 1 && !closed); limit_process_memory(); assert(configured == 1); }
    else assert(closed == 1 && assigned == (mode == 2));
}
'''


class WindowsLimitsTests(unittest.TestCase):
    def test_real_policy_and_api_calls(self):
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d) / 'policy.c', Path(d) / 'policy'
            src.write_text(MOCKS.replace('/*PRODUCTION*/', MEMORY + BOOST))
            r = subprocess.run(compiler_command() + ['-O1', '-UNDEBUG', str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            for mode in range(3):
                r = subprocess.run([str(exe), str(mode)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn('HIGH_PRIORITY_CLASS', BOOST)
            init = region('void plat_init(', 'bool plat_exe_path(')
            self.assertIn('NORMAL_PRIORITY_CLASS', init)
            self.assertIn('limit_process_memory();', init)

    def test_cross_compile_windows_api(self):
        if shutil.which('zig'):
            cc = ['zig', 'cc']
        elif importlib.util.find_spec('ziglang'):
            cc = [sys.executable, '-m', 'ziglang', 'cc']
        else:
            self.skipTest('Zig required for the Windows SDK cross compilation')
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'windows_api.c'
            src.write_text('#include <windows.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdbool.h>\n#include <unistd.h>\n' +
                MEMORY + BOOST + EMERGENCY)
            r = subprocess.run(cc + ['-target', 'x86_64-windows-gnu', '-O1', '-Wall', '-Wno-unused-function', '-c', str(src),
                '-o', str(Path(d) / 'windows_api.o')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_amd_translated_pipeline_policy(self):
        source = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        a = source.index('static VkPipelineCreateFlags translated_pipeline_flags(')
        b = source.index('static void build_pipeline(', a)
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d) / 'pipeline.c', Path(d) / 'pipeline'
            src.write_text('#include <stdint.h>\n#include <assert.h>\ntypedef uint32_t VkPipelineCreateFlags;\n'
                '#define VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT 1u\n' + source[a:b] +
                'int main(void) { assert(translated_pipeline_flags(0x1002)==1); '
                'assert(!translated_pipeline_flags(0x10de)); assert(!translated_pipeline_flags(0x8086)); }\n')
            r = subprocess.run(compiler_command() + ['-O1', '-UNDEBUG', str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(source.count('.flags = translated_pipeline_flags(g_vk.props.vendorID)'), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
