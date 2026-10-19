#!/usr/bin/env python3
"""Validate the opt-in isotropic hair lighting approximation."""
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
    ShaderKey key={.glsl=1,.hair_isotropic=atoi(argv[1])};
    Ctx *c=calloc(1,sizeof *c); c->glsl=1; c->key=&key; c->stage=STAGE_FRAGMENT;
    c->info=calloc(1,sizeof *c->info); c->ipa_mode=-1; c->omap_target=15;
    for (int i=0;i<64;i++) c->reg_used[i]=1;
    sp(&c->body,"    r47 = 24u; r6 = 1u; r7 = 2u; r32 = 3u;\n");
    emit_insn(c, 0x3800000000000000ull | (0x303ull<<20) | (7ull<<16) |
        ((uint64_t)(atoi(argv[2])?46:47)<<8) | 62ull, 8);
    sp(&c->body,"    r0 = r62; r1 = r6; r2 = r7; r3 = r32;\n");
    ShaderInfo *s=glsl_finish(c,false,false);
    puts(s->msl); maxwell_free(s); return 0;
}
'''



class HairLightingTests(unittest.TestCase):
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

    def test_only_enabled_material_id_extraction_changes(self):
        marker='if (r62 == 3u) { r62 = 0u; r6 = 0u; r7 = 0u; r32 = 0u; }'
        self.assertIn(marker,self.source(1))
        self.assertNotIn(marker,self.source(0))
        self.assertNotIn(marker,self.source(1,1))

    def test_only_deferred_lighting_fingerprint_is_selected(self):
        executor=(ROOT/'runtime/nvn/vulkan/exec.c').read_text()
        code='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
'''+function(executor,'hair_lighting_variant')+'''
int main(void) {
    uint64_t h=0xaa68a5939321c84aull;
    unsetenv("LARECOMP_HAIR_ISOTROPIC"); assert(!hair_lighting_variant(h));
    setenv("LARECOMP_HAIR_ISOTROPIC","0",1); assert(!hair_lighting_variant(h));
    setenv("LARECOMP_HAIR_ISOTROPIC","1",1); assert(hair_lighting_variant(h));
    assert(!hair_lighting_variant(h+1) && !hair_lighting_variant(0));
}
'''
        src,exe=self.work/'select.c',self.work/'select';src.write_text(code)
        run=subprocess.run(compiler_command()+['-UNDEBUG',str(src),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)
        run=subprocess.run([str(exe)],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)

    def test_compatibility_variant_compiles_and_validates(self):
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
