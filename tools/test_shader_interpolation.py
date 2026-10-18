#!/usr/bin/env python3
"""Numerically execute production-generated IPA expressions; no game files/GPU.

Also checks emitted interpolation qualifiers and source/destination aliases.
SCARLET_TRANSLATE_SOURCE selects a pre-fix translator for a negative control.
"""
import itertools
import os
from pathlib import Path
import re
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
    if (argc != 6) return 2;
    uint64_t blob[19] = {0};
    ((uint8_t *)blob)[0x48] = (uint8_t)(atoi(argv[2]) * 0x55); // SPH + 0x18, generic 0
    blob[15] = 15;
    blob[17] = 0xe000000000000000ull | ((uint64_t)atoi(argv[1]) << 54)
        | ((uint64_t)atoi(argv[4]) << 51) | ((uint64_t)atoi(argv[5]) << 38)
        | (0x80ull << 28) | ((uint64_t)atoi(argv[3]) << 20) | (7ull << 16) | (255ull << 8) | 4;
    blob[18] = 0xe30000000007000full;
    ShaderKey key = {.glsl = 1, .vs_out_mask = 1};
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob, sizeof blob, STAGE_FRAGMENT, &key);
    if (!s->msl) { fprintf(stderr, "%s\n", s->error); return 1; }
    puts(s->msl); maxwell_free(s);
}
'''
NUMERIC = r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
static uint32_t as_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float as_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float saturate(float v) { return isnan(v) ? 0 : fminf(fmaxf(v, 0), 1); }
int main(void) {
/*CASES*/
}
'''


class InterpolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        src = cls.work / 'ipa.c'
        src.write_text(HARNESS)
        cls.exe = cls.work / 'ipa'
        source = os.environ.get('SCARLET_TRANSLATE_SOURCE', str(ROOT / 'runtime/nvn/maxwell/translate.c'))
        build = subprocess.run(compiler_command() + ['-O1', '-I' + str(ROOT / 'runtime/nvn/maxwell'),
            str(src), source, '-lm', '-o', str(cls.exe)], capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stderr)

    def translate(self, mode, imap, mult=2, sat=0, idx=0):
        run = subprocess.run([str(self.exe), *map(str, (mode, imap, mult, sat, idx))],
            capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return run.stdout

    def test_numeric_pass_multiply_constant_linear_saturation_and_alias(self):
        cases = []
        for mode, imap, mult, sat in itertools.product(range(4), range(1, 4), (2, 4, 255), (0, 1)):
            source = self.translate(mode, imap, mult, sat)
            statements = re.findall(r'^    r4 = .*?;', source, re.M)
            self.assertTrue(statements)
            expr = '\n'.join(statements).replace('gl_FragCoord.w', 'fw')
            expected = .6 * (.25 if imap == 2 else 1)
            if mode == 1:
                expected *= 0 if mult == 255 else 4
            if sat:
                expected = min(max(expected, 0), 1)
            cases.append('''{
                float v0[4] = {.6f, 0, 0, 1}, fw = .25f;
                uint32_t r2 = as_u(4), r4 = as_u(4);
                %s
                if (fabsf(as_f(r4) - %.9ff) > 0.00001f) {
                    fprintf(stderr, "IPA mode=%d imap=%d mult=%d sat=%d: got %%g\\n", as_f(r4)); return 1;
                }
            }''' % (expr, expected, mode, imap, mult, sat))
        src, exe = self.work / 'numeric.c', self.work / 'numeric'
        src.write_text(NUMERIC.replace('/*CASES*/', '\n'.join(cases)))
        build = subprocess.run(compiler_command() + [str(src), '-lm', '-o', str(exe)],
            capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_sph_controls_qualifier_independently_of_ipa_arithmetic(self):
        for mode, imap in itertools.product(range(4), range(1, 4)):
            src = self.translate(mode, imap)
            qualifier = {1: 'flat ', 2: '', 3: 'noperspective '}[imap]
            self.assertIn('layout(location = 0) %sin vec4 v0;' % qualifier, src)

    def test_idx_with_zero_register_is_a_direct_attribute(self):
        self.assertEqual(self.translate(1, 2, idx=0), self.translate(1, 2, idx=1))

    def test_glsl_and_spirv_validate(self):
        glslang = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not glslang or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        for mode, imap, sat in itertools.product(range(4), range(1, 4), (0, 1)):
            src, spv = self.work / 'check.frag', self.work / 'check.spv'
            src.write_text(self.translate(mode, imap, mult=4, sat=sat))
            compiled = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.3',
                str(src), '-o', str(spv)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run([validator, '--target-env', 'vulkan1.3', str(spv)],
                capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
