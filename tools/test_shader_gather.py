#!/usr/bin/env python3
"""Exercise the production Maxwell translator without game binaries or a GPU.

CC selects the C compiler; defaults to Clang/Zig as in the other native tests.
GLSLANG_VALIDATOR and SPIRV_VAL can point to shader validation tools. Compiler
validation is skipped explicitly when they are absent, never counted as passed.
SCARLET_TRANSLATE_SOURCE selects an older translator for a negative regression run.
"""
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
int main(int argc, char **argv) {
    if (argc != 3) return 3;
    uint64_t blob[19] = {0};
    blob[15] = 15; /* SPH color output mask at 0x78 */
    blob[17] = strtoull(argv[1], NULL, 16);
    blob[18] = 0xe30000000007000full; /* EXIT */
    ShaderKey key = {.glsl = (uint8_t)atoi(argv[2])};
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob, sizeof blob, STAGE_FRAGMENT, &key);
    if (!s->msl) { fprintf(stderr, "%s\n", s->error); maxwell_free(s); return 1; }
    if (s->ntex != 1 || s->tex[0].type != TEXTYPE_2D || s->tex[0].is_array || s->tex[0].is_image ||
        s->tex[0].shadow != ((blob[17] >> 50) & 1) || s->tex[0].handle_offset != ((blob[17] >> 36) & 8191) * 4) {
        fprintf(stderr, "incorrect texture binding metadata\n"); maxwell_free(s); return 2;
    }
    puts(s->msl);
    maxwell_free(s);
    return 0;
}
'''


def instruction(*, half=False, shadow=False, offset=False, component=0, d0=4, d1=8):
    return (0xdf00000000000000 | (int(half) << 55) | (component << 52) |
            (int(offset) << 51) | (int(shadow) << 50) | (0x123 << 36) |
            (d1 << 28) | (12 << 20) | (7 << 16) | (2 << 8) | d0)


class GatherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        harness = cls.directory / 'gather.c'
        harness.write_text(HARNESS)
        cls.exe = cls.directory / ('gather.exe' if os.name == 'nt' else 'gather')
        source = os.environ.get('SCARLET_TRANSLATE_SOURCE', str(ROOT / 'runtime/nvn/maxwell/translate.c'))
        subprocess.run(compiler_command() + ['-O1', '-I', str(ROOT / 'runtime/nvn/maxwell'),
                       str(harness), source, '-lm', '-o', str(cls.exe)], check=True, capture_output=True, text=True)

    def translate(self, word, glsl=True):
        return subprocess.run([str(self.exe), f'{word:016x}', str(int(glsl))], capture_output=True, text=True)

    def source(self, word):
        result = self.translate(word)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_exact_scarlet_log_instructions(self):
        for word, regs, coords in (
            (0xdf0200c181070f1a, [26, 27, 24, 25], (15, 16)),
            (0xdf0000c0e3b73a0c, [12, 13, 14, 15], (58, 59)),
        ):
            with self.subTest(word=f'{word:016x}'):
                src = self.source(word)
                self.assertIn('textureGather(t0, vec2(as_f(r%d), as_f(r%d)), 0)' % coords, src)
                for component, reg in enumerate(regs):
                    self.assertIn(f'r{reg} = as_u(t[{component}]);', src)
                self.assertNotIn('packHalf2x16(vec2(t[', src)

    def test_coordinates_depth_offsets_precision_and_component(self):
        for half, shadow, offset, component in itertools.product((False, True), (False, True), (False, True), range(4)):
            with self.subTest(half=half, shadow=shadow, offset=offset, component=component):
                src = self.source(instruction(half=half, shadow=shadow, offset=offset, component=component))
                y = 3 if offset or shadow else 12
                args = f'vec2(as_f(r2), as_f(r{y}))'
                if shadow:
                    args += f', as_f(r{13 if offset else 12})'
                if offset:
                    args += ', ivec2(int(r12 << 26) >> 26, int(r12 << 18) >> 26)'
                if not shadow:
                    args += f', {component}'
                self.assertIn(f'textureGather{"Offset" if offset else ""}(t0, {args})', src)
                self.assertIn('uniform sampler2DShadow t0;' if shadow else 'uniform sampler2D t0;', src)
                if half:
                    self.assertIn('r4 = packHalf2x16(vec2(t[0], t[1]));', src)
                    self.assertIn('r8 = packHalf2x16(vec2(t[2], t[3]));', src)
                else:
                    for reg, component in zip((4, 5, 8, 9), range(4)):
                        self.assertIn(f'r{reg} = as_u(t[{component}]);', src)

    def test_aliases_and_zero_register(self):
        src = self.source(instruction(d0=2, d1=12))
        # All source operands are evaluated into t before any aliased destination is written.
        self.assertLess(src.index('float4 t =') if 'float4 t =' in src else src.index('vec4 t ='), src.index('r2 = as_u(t[0])'))
        for half in (False, True):
            src = self.source(instruction(half=half, d0=255, d1=254 if not half else 255))
            self.assertNotIn('r255', src)
            self.assertNotIn('r256', src)

    def test_invalid_and_unimplemented_operations_still_fail(self):
        for word in (instruction(d0=5), instruction(shadow=True) ^ (1 << 8),
                     instruction(shadow=True, offset=True) ^ (1 << 20),
                     0xeb20010600f70004, 0xeb50008602070a00):
            with self.subTest(word=f'{word:016x}'):
                self.assertNotEqual(self.translate(word).returncode, 0)
        result = self.translate(instruction(), glsl=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('TLD4S', result.stderr)

    def test_glsl_compiles_and_spirv_validates(self):
        glslang = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not glslang or not validator:
            self.skipTest('GLSL validation requires glslangValidator and spirv-val')
        words = [0xdf0200c181070f1a, 0xdf0000c0e3b73a0c]
        words += [instruction(half=h, shadow=s, offset=o, component=c)
                  for h, s, o, c in itertools.product((False, True), (False, True), (False, True), range(4))]
        for word in words:
            with self.subTest(word=f'{word:016x}'):
                shader = self.directory / 'gather.frag'
                spv = self.directory / 'gather.spv'
                shader.write_text(self.source(word))
                result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.3', '-o', str(spv), str(shader)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([validator, '--target-env', 'vulkan1.3', str(spv)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
