#!/usr/bin/env python3
"""Compile the real Vulkan backend with shaderc and render dispatcher shaders.

SHADERC_LIBRARY selects libshaderc.so / libshaderc_shared.so (or an import library).
SDL_INCLUDE and VULKAN_INCLUDE select development headers. SPIRV_VAL is optional.
SCARLET_VULKAN_GPU_TEST=1 enables integer-pixel checks; Vulkan may use Lavapipe.
No game code or assets. Time measurements are reported, not used as flaky assertions.
"""
import itertools
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nvn/vulkan/shaders.h"
void rt_fatal_trace(void) { abort(); }
int main(int argc, char **argv) {
    assert(argc == 4); FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); assert(n >= 0);
    char *src = calloc((size_t)n + 1, 1); assert(fread(src, 1, (size_t)n, f) == (size_t)n); fclose(f);
    size_t words = 0; struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    uint32_t *code = vk_glsl_to_spirv(src, atoi(argv[2]), "regression", &words);
    if (!code) { free(src); return 2; }
    clock_gettime(CLOCK_MONOTONIC, &b);
    f = fopen(argv[3], "wb"); assert(f); assert(fwrite(code, 4, words, f) == words); fclose(f);
    printf("%zu %.6f\n", words, (b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9);
    free(src); free(code); return 0;
}
'''
VERTEX = '''#version 460
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
'''


def dispatcher(blocks, registers):
    """Same register/pc loop topology as translated Maxwell; branches depend on runtime data."""
    source = ['#version 460', '#extension GL_EXT_control_flow_attributes : require',
              'layout(location=0) out uvec4 color;', 'layout(push_constant) uniform U { uvec4 data; };',
              'void main() {']
    source += [f'uint r{j} = data.y + {j}u;' for j in range(registers)]
    source += ['uint pc = data.x; bool done = false;',
               '[[dont_unroll]] for (uint guard=0u; guard<100000u; guard++) { switch (pc) {']
    for i in range(blocks):
        r = i % registers
        source += [f'case {i}u: r{r} = r{r} * data.y + {i}u;',
                   f'pc = (data.z + {i + 1}u) % {blocks + 1}u; continue;']
    source += ['default: done=true; break; } if (done) break; }',
               'color = uvec4(' + '+'.join(f'r{j}' for j in range(registers)) +
               f', r0, r{registers - 1}, 0xfacefeedu);', '}']
    return '\n'.join(source)


def expected_pixel(blocks, registers, start):
    values = [3 + j for j in range(registers)]
    for i in range(start, blocks):
        r = i % registers
        values[r] = (values[r] * 3 + i) & 0xffffffff
    return [sum(values) & 0xffffffff, values[0], values[-1], 0xfacefeed]


class ShaderCompilationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.path = Path(cls.tmp.name)
        cls.cc = compiler_command()
        cls.library = os.environ.get('SHADERC_LIBRARY')
        if not cls.library or not Path(cls.library).is_file():
            raise unittest.SkipTest('requires SHADERC_LIBRARY pointing to the real shaderc library')
        cls.include = Path(os.environ.get('VULKAN_INCLUDE', '/usr/include'))
        cls.headers = ['-I', str(ROOT / 'runtime'), '-I', str(cls.include),
                       '-I', os.environ.get('SDL_INCLUDE', '/usr/include')]
        src = cls.path / 'compile.c'
        src.write_text(HARNESS)
        cls.exe = cls.path / 'compile'
        link = [cls.library]
        if os.name != 'nt':
            link += ['-Wl,-rpath,' + str(Path(cls.library).parent)]
        subprocess.run(cls.cc + ['-O2', '-UNDEBUG', '-ffunction-sections', '-fdata-sections', *cls.headers,
                       str(src), str(ROOT / 'runtime/nvn/vulkan/shaders.c'), *link,
                       '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def compile(self, source, stage, *, mode=None, name='shader', cwd=None, check=True):
        work = cwd or self.path
        shader = work / (name + '.glsl')
        output = work / (name + '.spv')
        shader.write_text(source)
        env = dict(os.environ)
        env.pop('LARECOMP_VK_OPT', None)
        env.pop('LARECOMP_VK_NO_SPIRV_CACHE', None)
        if mode is not None:
            env['LARECOMP_VK_OPT'] = mode
        result = subprocess.run([str(self.exe), str(shader), str(stage), str(output)], cwd=work,
                                env=env, capture_output=True, text=True, timeout=30)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr[:2000])
            validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
            if validator:
                r = subprocess.run([validator, '--target-env', 'vulkan1.3', str(output)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
        return output, result

    def test_large_dispatcher_avoids_ssa_expansion_and_stale_optimized_cache(self):
        work = self.path / 'stress'
        work.mkdir()
        source = dispatcher(1024, 128)
        optimized, opt_run = self.compile(source, 1, mode='1', name='optimized', cwd=work)
        safe, safe_run = self.compile(source, 1, name='safe', cwd=work)
        opt_words, safe_words = optimized.stat().st_size // 4, safe.stat().st_size // 4
        self.assertGreater(opt_words, safe_words * 3)
        self.assertEqual(len(list((work / 'cache/spirv').glob('*.spv'))), 2)
        cached, run = self.compile(source, 1, name='cached', cwd=work)
        self.assertEqual(cached.read_bytes(), safe.read_bytes())
        disabled, _ = self.compile(source, 1, mode='0', name='disabled', cwd=work)
        self.assertEqual(disabled.read_bytes(), safe.read_bytes())
        self.assertEqual(len(list((work / 'cache/spirv').glob('*.spv'))), 2)
        diagnostics = work / 'build/shader-diagnostics'
        inputs = list(diagnostics.glob('*.frag.glsl'))
        self.assertEqual(len(inputs), 2)
        for src in inputs:
            self.assertEqual(src.read_text(), source)
            key = src.name.split('.')[0]
            spv = src.with_suffix('.spv')
            self.assertEqual(spv.read_bytes(), (work / 'cache/spirv' / (key + '.spv')).read_bytes())
        self.assertIn('[shader-diagnostic] input ', run.stderr)
        print(f'\nDispatcher: -O {opt_words} words / {opt_run.stdout.split()[1]} s; '
              f'default {safe_words} words / {safe_run.stdout.split()[1]} s', flush=True)

    def test_source_survives_a_compiler_failure(self):
        work = self.path / 'failure'
        work.mkdir()
        source = '#version 460\n#error diagnostic_fixture\n/*' + 'x' * 66000 + '*/'
        _, result = self.compile(source, 1, cwd=work, check=False)
        self.assertEqual(result.returncode, 2)
        saved = list((work / 'build/shader-diagnostics').glob('*.glsl'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_text(), source)
        self.assertFalse(list((work / 'build/shader-diagnostics').glob('*.spv')))

    def test_vertex_still_has_no_contraction(self):
        for mode in (None, '1'):
            with self.subTest(mode=mode):
                path, _ = self.compile(VERTEX, 0, mode=mode, name='vertex' + str(mode))
                data = path.read_bytes()
                words = struct.unpack('<%dI' % (len(data) // 4), data)
                i, precise = 5, set()
                while i < len(words):
                    count, op = words[i] >> 16, words[i] & 0xffff
                    if op == 71 and count >= 3 and words[i + 2] == 42:
                        precise.add(words[i + 1])
                    i += count
                self.assertTrue(precise)

    def test_fragment_register_dispatcher_renders_correct_pixels(self):
        if not os.environ.get('SCARLET_VULKAN_GPU_TEST'):
            self.skipTest('requires SCARLET_VULKAN_GPU_TEST=1 and a Vulkan device')
        exe = self.path / 'render'
        library = os.environ.get('VULKAN_LIBRARY', '-lvulkan')
        subprocess.run(self.cc + ['-O2', '-UNDEBUG', '-I', str(self.include),
                       str(ROOT / 'tools/test_shader_compile_gpu.c'), library, '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        vertex, _ = self.compile(VERTEX, 0, name='triangle')
        source = dispatcher(128, 64)
        expected = expected_pixel(128, 64, 0) + expected_pixel(128, 64, 17)
        for mode, fast in itertools.product((None, '1'), (False, True)):
            with self.subTest(mode=mode, fast=fast):
                fragment, _ = self.compile(source, 1, mode=mode, name='fragment' + str(mode))
                run = subprocess.run([str(exe), str(vertex), str(fragment), str(int(fast))],
                                     capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual([int(v, 16) for v in run.stdout.split()], expected)
                print(f'GPU mode={mode} fast={fast}: {run.stderr.strip()}', flush=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
