#!/usr/bin/env python3
"""Exercise production TF export generation and optional Vulkan buffer readback.

Synthetic shaders reproduce Scarlet's float-vector -> packed-scalar stages;
no game assets are needed. CC selects the C compiler. GLSLANG_VALIDATOR and
SPIRV_VAL enable compilation/validation. SCARLET_VULKAN_GPU_TEST=1 additionally
executes transform feedback (VULKAN_INCLUDE / VULKAN_LIBRARY select the SDK).
SCARLET_TRANSLATE_SOURCE can select the old translator as a negative control.
"""
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <stdio.h>
#include TRANSLATE_SOURCE
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int mode = atoi(argv[1]); // 0: xyz/xyz/xyz, 1: xyz/x/x, 2: xyz/xyz/x
    ShaderKey key = {.glsl=1, .xfb=1, .xfb_layout=1, .xfb_comp={4,4,4,0}};
    Ctx *c = calloc(1, sizeof *c);
    c->stage=STAGE_VERTEX; c->glsl=true; c->key=&key;
    c->info=calloc(1, sizeof *c->info);
    for (int stream=0; stream<3; stream++) {
        int nc = stream == 0 || mode == 0 || (mode == 2 && stream == 1) ? 3 : 1;
        for (int j=0; j<nc; j++) {
            int r=stream*4+j;
            if (stream && nc == 1)
                sp(&c->body, "    r%d = 0x7fc00001u ^ (uint(gl_VertexIndex) + %du);\n", r, stream);
            else
                sp(&c->body, "    r%d = as_type<uint>(float(gl_VertexIndex * 16 + %d) + 0.25f);\n", r, stream*4+j);
            attr_store(c, r, 0x80 + stream*16+j*4);
        }
    }
    ShaderInfo *s=glsl_finish(c, false, false);
    if (!s->msl) return 3;
    puts(s->msl); maxwell_free(s);
    return 0;
}
'''


def compiler_command():
    if os.environ.get('CC'):
        return shlex.split(os.environ['CC'])
    for name in ('clang', 'cc'):
        if shutil.which(name):
            return [name]
    raise unittest.SkipTest('requires a C compiler (CC)')


class TransformFeedbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        src = cls.work / 'generate.c'
        src.write_text(HARNESS)
        cls.exe = cls.work / 'generate'
        translator = os.environ.get('SCARLET_TRANSLATE_SOURCE', str(ROOT / 'runtime/nvn/maxwell/translate.c'))
        subprocess.run(compiler_command() + ['-O1', '-UNDEBUG', '-I' + str(ROOT / 'runtime/nvn/maxwell'),
            '-DTRANSLATE_SOURCE="' + translator + '"', str(src), '-lm', '-o', str(cls.exe)],
            check=True, capture_output=True, text=True)
        cls.sources = [subprocess.check_output([str(cls.exe), str(mode)], text=True) for mode in range(3)]

    def test_vector_and_packed_passes_use_different_widths(self):
        for mode, source in enumerate(self.sources):
            for i in range(3):
                vector = i == 0 or mode == 0 or (mode == 2 and i == 1)
                stride, ty, sw = (12, 'vec3', 'xyz') if vector else (4, 'uint', 'x')
                with self.subTest(mode=mode, stream=i):
                    self.assertIn(f'xfb_buffer = {i}, xfb_offset = 0, xfb_stride = {stride}) out {ty} v{i};', source)
                    rhs = f'xfbtmp{i}.{sw}' if vector else f'floatBitsToUint(xfbtmp{i}.{sw})'
                    self.assertIn(f'v{i} = {rhs};', source)

    def test_executor_allows_all_components_in_the_three_streams(self):
        source = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        for i in range(3):
            self.assertIn(f'vkey.xfb_comp[{i}] = 4;', source)

    def compile_shaders(self):
        compiler = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not compiler or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        paths = []
        for mode, source in enumerate(self.sources):
            src, output = self.work / f'feedback{mode}.vert', self.work / f'feedback{mode}.spv'
            src.write_text(source)
            subprocess.run([compiler, '-V', '--target-env', 'vulkan1.1', str(src), '-o', str(output)],
                           check=True, capture_output=True, text=True)
            subprocess.run([validator, '--target-env', 'vulkan1.1', str(output)],
                           check=True, capture_output=True, text=True)
            data = output.read_bytes()
            words = struct.unpack('<%dI' % (len(data) // 4), data)
            decorations, index = {}, 5
            while index < len(words):
                count, op = words[index] >> 16, words[index] & 0xffff
                if op == 71 and count == 4:  # OpDecorate
                    decorations.setdefault(words[index+1], {})[words[index+2]] = words[index+3]
                index += count
            # XfbBuffer=36, XfbStride=37: assert the compiled buffer ABI too.
            strides = {d[36]: d[37] for d in decorations.values() if 36 in d and 37 in d}
            self.assertEqual(strides, {0: 12, 1: 12 if mode != 1 else 4, 2: 12 if mode == 0 else 4})
            paths.append(output)
        return paths

    def test_generated_modules_compile_and_validate(self):
        self.compile_shaders()

    def test_vulkan_writes_all_vector_components_and_exact_packed_bits(self):
        if not os.environ.get('SCARLET_VULKAN_GPU_TEST'):
            self.skipTest('requires SCARLET_VULKAN_GPU_TEST=1 and VK_EXT_transform_feedback')
        paths = self.compile_shaders()
        exe = self.work / 'feedback_gpu'
        subprocess.run(compiler_command() + ['-O1', '-UNDEBUG',
            '-I' + os.environ.get('VULKAN_INCLUDE', '/usr/include'),
            str(ROOT / 'tools/test_transform_feedback_gpu.c'),
            os.environ.get('VULKAN_LIBRARY', '-lvulkan'), '-o', str(exe)],
            check=True, capture_output=True, text=True)
        for mode, path in enumerate(paths):
            result = subprocess.run([str(exe), str(path)], capture_output=True, text=True, timeout=30)
            if result.returncode == 77:
                self.skipTest(result.stderr)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = [int(word, 16) for word in result.stdout.split()]
            expected = []
            for stream in range(3):
                vector = stream == 0 or mode == 0 or (mode == 2 and stream == 1)
                words = []
                for vertex in range(4):
                    if vector:
                        words += [struct.unpack('<I', struct.pack('<f', vertex*16 + stream*4+j + .25))[0] for j in range(3)]
                    else:
                        words.append(0x7fc00001 ^ (vertex + stream))
                expected += words + [0xcdcdcdcd] * (16 - len(words))
            self.assertEqual(actual, expected, f'mode {mode}: {result.stderr}')


if __name__ == '__main__':
    unittest.main()
