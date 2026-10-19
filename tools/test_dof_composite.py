#!/usr/bin/env python3
"""Validate the opt-in sharp-scene variant of Scarlet's DOF composite."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_stencil_sampling import function
from test_transform_feedback import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <stdio.h>
#include TRANSLATE_SOURCE
int main(int argc, char **argv) {
    if (argc!=3) return 2;
    ShaderKey key={.glsl=1,.sharp_scene=atoi(argv[1]),.vs_out_mask=1};
    Ctx *c=calloc(1,sizeof *c); c->glsl=1; c->key=&key; c->stage=STAGE_FRAGMENT;
    c->info=calloc(1,sizeof *c->info); c->ipa_mode=-1; c->omap_target=15;
    for (int i=0;i<5;i++) tex_slot(c,8+i*2,TEXTYPE_2D,false,false);
    attr_load(c,0,0x80,NULL);
    if (atoi(argv[2])) c->info->tex[2].type=TEXTYPE_CUBE;
    ShaderInfo *s=glsl_finish(c,false,false);
    if (!s->msl) return 3;
    puts(s->msl); maxwell_free(s); return 0;
}
'''


class DofCompositeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        src, cls.exe = cls.work / 'generate.c', cls.work / 'generate'
        src.write_text(HARNESS)
        translator = ROOT / 'runtime/nvn/maxwell/translate.c'
        run = subprocess.run(compiler_command() + ['-O1','-UNDEBUG',
            '-I'+str(ROOT/'runtime/nvn/maxwell'), '-DTRANSLATE_SOURCE="'+str(translator)+'"',
            str(src),'-lm','-o',str(cls.exe)], capture_output=True,text=True)
        if run.returncode:
            raise AssertionError(run.stderr)

    def source(self, enabled, incompatible=0):
        return subprocess.check_output([str(self.exe),str(enabled),str(incompatible)],text=True)

    def test_opt_in_replaces_only_the_dof_composite_with_sharp_hdr(self):
        self.assertIn('c0 = texture(t2, v0.xy);\n  return;',self.source(1))
        self.assertNotIn('c0 = texture(t2, v0.xy);',self.source(0))
        self.assertNotIn('c0 = texture(t2, v0.xy);',self.source(1,1))

    def test_only_the_captured_composite_fingerprint_is_selected(self):
        executor=(ROOT/'runtime/nvn/vulkan/exec.c').read_text()
        code='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
'''+function(executor,'sharp_dof_variant')+'''
int main(void) {
    uint64_t h=0xfbff97881854688eull;
    unsetenv("LARECOMP_NO_DOF"); assert(!sharp_dof_variant(h));
    setenv("LARECOMP_NO_DOF","0",1); assert(!sharp_dof_variant(h));
    setenv("LARECOMP_NO_DOF","1",1); assert(sharp_dof_variant(h));
    assert(!sharp_dof_variant(h+1) && !sharp_dof_variant(0));
}
'''
        src,exe=self.work/'select.c',self.work/'select';src.write_text(code)
        run=subprocess.run(compiler_command()+['-UNDEBUG',str(src),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)
        run=subprocess.run([str(exe)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)

    def test_sharp_variant_compiles_and_validates(self):
        compiler=os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator=os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not compiler or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        src,spv=self.work/'sharp.frag',self.work/'sharp.spv';src.write_text(self.source(1))
        for cmd in ([compiler,'-V',str(src),'-o',str(spv)],[validator,str(spv)]):
            run=subprocess.run(cmd,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)


if __name__=='__main__':
    unittest.main()
