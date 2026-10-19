#!/usr/bin/env python3
"""Production shader regression for the logical layer of split LUT draws.

The guest VS exports instance+1 at attribute 0x64; the FS reads that integer
and subtracts one to reconstruct the LUT's blue coordinate. Physical Vulkan
attachment layer zero must not replace that logical value. No game assets/GPU.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_transform_feedback import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <stdio.h>
#include TRANSLATE_SOURCE
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int vertex=atoi(argv[1]);
    ShaderKey key={.glsl=1,.layer_varying=atoi(argv[2])};
    Ctx *c=calloc(1,sizeof *c); c->glsl=1; c->key=&key;
    c->info=calloc(1,sizeof *c->info); c->ipa_mode=-1;
    c->stage=vertex?STAGE_VERTEX:STAGE_FRAGMENT;
    if (vertex) {
        attr_load(c,0,0x2f8,NULL);
        sp(&c->body,"    r0 = r0 + 1u;\n");
        attr_store(c,0,0x64);
        if (atoi(argv[3])) c->out_attr_mask|=1u<<31;
    } else {
        attr_load(c,0,0x64,NULL);
        sp(&c->body,"    r0 = r0 - 1u;\n");
        c->omap_target=1; key.rt_kind[0]=2;
        if (atoi(argv[3])) c->in_attr_mask|=1u<<31;
    }
    ShaderInfo *s=glsl_finish(c,false,false);
    if (!s->msl) { fprintf(stderr,"%s\n",s->error); maxwell_free(s); return 3; }
    puts(s->msl); maxwell_free(s); return 0;
}
'''


class LayerVaryingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        src, cls.exe = cls.work / 'generate.c', cls.work / 'generate'
        src.write_text(HARNESS)
        translator = ROOT / 'runtime/nvn/maxwell/translate.c'
        result = subprocess.run(compiler_command() + ['-O1', '-UNDEBUG',
            '-I' + str(ROOT / 'runtime/nvn/maxwell'), '-DTRANSLATE_SOURCE="' + str(translator) + '"',
            str(src), '-lm', '-o', str(cls.exe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def generate(self, vertex, enabled=1, conflict=0):
        return subprocess.run([str(self.exe), str(int(vertex)), str(enabled), str(conflict)],
                              capture_output=True, text=True)

    def test_guest_layer_is_flat_uint_across_both_stages(self):
        vs, fs = self.generate(True), self.generate(False)
        self.assertEqual(vs.returncode, 0, vs.stderr)
        self.assertEqual(fs.returncode, 0, fs.stderr)
        self.assertIn('layout(location = 31) flat out uint guest_layer;', vs.stdout)
        self.assertIn('layout(location = 31) flat in uint guest_layer;', fs.stdout)
        self.assertIn('r0 = uint(gl_InstanceIndex);', vs.stdout)
        self.assertIn('guest_layer = r0;', vs.stdout)
        self.assertIn('r0 = guest_layer;', fs.stdout)
        for source in (vs.stdout, fs.stdout):
            self.assertNotIn('/* attr 0x64 */', source)
            self.assertNotIn('gl_Layer', source)

    def test_ordinary_variants_do_not_add_a_layer_interface(self):
        for vertex in (False, True):
            result = self.generate(vertex, enabled=0)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('guest_layer', result.stdout)

    def test_varying_collision_is_rejected_for_compatibility_fallback(self):
        for vertex in (False, True):
            result = self.generate(vertex, conflict=1)
            self.assertEqual(result.returncode, 3)
            self.assertIn('conflicts', result.stderr)

    def test_shader_pair_links_compiles_and_validates(self):
        compiler = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not compiler or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        sources = []
        for stage, vertex in (('vert', True), ('frag', False)):
            path, output = self.work / ('layer.' + stage), self.work / (stage + '.spv')
            path.write_text(self.generate(vertex).stdout)
            sources.append(path)
            for cmd in ([compiler, '-V', '--target-env', 'vulkan1.1', str(path), '-o', str(output)],
                        [validator, '--target-env', 'vulkan1.1', str(output)]):
                run = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        run = subprocess.run([compiler, '-V', '-l', *map(str, sources), '-o', str(self.work / 'linked.spv')],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
