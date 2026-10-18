#!/usr/bin/env python3
"""Bounded Maxwell CB1 jump-table regressions, real translation and optional Vulkan pixels.

Uses only synthetic bytecode/constants. GLSLANG_VALIDATOR / SPIRV_VAL select shader tools.
SCARLET_VULKAN_GPU_TEST=1 enables native fragment-pipeline checks (VULKAN_INCLUDE/LIBRARY).
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
GENERATOR = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
static uint64_t words[8192]; static size_t n;
static size_t emit(uint64_t w) { if (!(n%4)) words[n++]=0; size_t at=n; words[n++]=w; return at; }
static void mov(int r, uint32_t v) { emit(0x0100000000070000ull|((uint64_t)v<<20)|r); }
int main(int argc, char **argv) {
    assert(argc==5); int mode=atoi(argv[1]), stage=atoi(argv[2]), glsl=atoi(argv[3]), filler=atoi(argv[4]);
    assert(filler>=0 && filler<6000);
    if(mode==17) for(int j=0;j<3;j++) emit(0xefd800000007ff00ull|((uint64_t)(0x80+j*16)<<20)|(uint64_t)(90+j));
    emit(0xef9400000007ff00ull); // LDC.U32 R0,c0[RZ+0]: runtime selector
    for(int i=0;i<filler;i++) mov(8+i%80,(uint32_t)i);
    size_t entry=0;
    if(mode==6) entry=emit(0xe2400000000f000full); // never-taken BRA; still an external entry in the proof
    size_t clamp=emit(0x3820000000070002ull|(7ull<<39)|(2ull<<20)); // UMIN R2,R0,2,PT
    size_t shl=emit(0x3848000000070202ull|(2ull<<20));
    size_t ldc=emit(0xef94001000c70202ull); // LDC.U32 R2,c1[R2+12]
    size_t branch=emit(0xe25000000007020full);
    words[branch]|=((uint64_t)(-(int64_t)((branch+1)*8))&0xffffffull)<<20;
    uint32_t constants[8]={0}; size_t exits[3];
    for(int j=0;j<3;j++) {
        if(j==1) { while(n%4) emit(0x50b0000000070f00ull); constants[3+j]=(uint32_t)n*8; }
        else constants[3+j]=(uint32_t)(n%4?n:n+1)*8;
        for(int k=0;k<4;k++) mov(k,(uint32_t)(10*(j+1)+k));
        exits[j]=emit(0xe24000000007000full);
    }
    size_t end;
    if(mode==15 || mode==16) {
        end=n%4?n:n+1;
        emit(0x3820000000070002ull|(7ull<<39)|(2ull<<20));
        emit(0x3848000000070202ull|(2ull<<20));
        emit(0xef94001000070202ull); // second table at c1+0
        size_t b2=emit(0xe25000000007020full);
        words[b2]|=((uint64_t)(-(int64_t)((b2+1)*8))&0xffffffull)<<20;
        size_t finish[3];
        for(int j=0;j<3;j++) {
            constants[j]=(uint32_t)(n%4?n:n+1)*8;
            for(int k=0;k<4;k++)mov(k,(uint32_t)(100*(j+1)+k));
            finish[j]=emit(0xe24000000007000full);
        }
        size_t exit=emit(0xe30000000007000full);
        for(int j=0;j<3;j++)words[finish[j]]|=((uint64_t)((int64_t)exit-(int64_t)finish[j]-1)*8&0xffffffull)<<20;
    } else end=emit(0xe30000000007000full);
    for(int j=0;j<3;j++) words[exits[j]]|=((uint64_t)((int64_t)end-(int64_t)exits[j]-1)*8&0xffffffull)<<20;
    if(mode==2) { /* truncated immutable constant section */ }
    if(mode==3) words[clamp]^=1ull<<42; // selects MAX, not MIN
    if(mode==4) words[ldc]&=~(7ull<<16); // predicated definition
    if(mode==5) words[ldc]^=(1ull<<36)|(2ull<<36); // mutable c2 is not the compiler section
    if(mode==6) words[entry]|=((uint64_t)((int64_t)shl-(int64_t)entry-1)*8&0xffffffull)<<20;
    if(mode==7) words[shl]^=1ull<<8; // source isn't the bounded register
    if(mode==8) constants[4]=0x80000000u; // target outside code
    if(mode==9) constants[4]=(uint32_t)shl*8; // table enters the middle of the proof
    if(mode==10) constants[4]|=1u; // unaligned target
    if(mode==11) words[clamp]|=1ull<<48; // signed minimum does not bound unsigned negative indices
    if(mode==12) words[clamp]=(words[clamp]&~(0x7ffffull<<20))|(256ull<<20); // oversized table
    if(mode==13) words[clamp]^=1ull<<16; // predicated clamp
    if(mode==14) words[ldc]|=1ull<<44; // indirect LDC mode
    uint64_t blob[8208]={0}; size_t header=stage==STAGE_COMPUTE?0:16;
    memcpy(blob+header,words,n*8); if(stage==STAGE_FRAGMENT)blob[15]=15;
    ShaderKey key={.glsl=(uint8_t)glsl}; key.rt_kind[0]=2;
    if(mode==17) {key.attr_kind[1]=1;key.attr_kind[2]=2;}
    ShaderInfo *s=(mode==1 || mode==16)?maxwell_translate((const uint8_t *)blob,(n+header)*8,stage,&key):
        maxwell_translate_with_constants((const uint8_t *)blob,(n+header)*8,stage,&key,
            (const uint8_t *)constants,mode==2?16:sizeof constants);
    if(!s->msl){fprintf(stderr,"%s\n",s->error);return 2;}
    fprintf(stderr,"%u %u %u %u\n",s->num_insts,s->dispatch_labels,s->indirect_tables,s->indirect_fallbacks);
    puts(s->msl);maxwell_free(s);
}
'''


class IndirectShaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.path = Path(cls.tmp.name)
        cls.cc = compiler_command()
        src = cls.path / 'generate.c'
        src.write_text(GENERATOR)
        cls.exe = cls.path / 'generate'
        subprocess.run(cls.cc + ['-O2', '-UNDEBUG', '-I', str(ROOT / 'runtime/nvn/maxwell'),
                       str(src), str(ROOT / 'runtime/nvn/maxwell/translate.c'), '-lm', '-o', str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def source(self, mode=0, stage=1, glsl=True, filler=0):
        run = subprocess.run([str(self.exe), str(mode), str(stage), str(int(glsl)), str(filler)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return run.stdout, tuple(int(n) for n in run.stderr.split())

    def compile(self, source, stage, name):
        glslang = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not glslang or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        path = self.path / (name + '.' + {0:'vert', 1:'frag', 5:'comp'}[stage])
        path.write_text(source)
        spv = path.with_suffix('.spv')
        for cmd in ([glslang, '-V', '--target-env', 'vulkan1.3', '-o', str(spv), str(path)],
                    [validator, '--target-env', 'vulkan1.3', str(spv)]):
            run = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return spv

    def test_only_proven_table_destinations_become_cases(self):
        for stage in (0,1,5):
            with self.subTest(stage=stage):
                compact, info = self.source(stage=stage, filler=1800)
                reference, old = self.source(mode=1, stage=stage, filler=1800)
                self.assertEqual(info[2:], (1,0))
                self.assertEqual(old[2:], (0,1))
                self.assertEqual(info[0], old[0])
                self.assertGreater(old[1], 1800)
                self.assertEqual(info[1], 5) # entry, three table destinations and shared EXIT
                # Same body/instructions: the only differences are unnecessary switch labels.
                remove_labels = lambda s: re.sub(r'^  case \d+u:\n', '', s, flags=re.M)
                self.assertEqual(remove_labels(compact), remove_labels(reference))
                self.compile(compact, stage, 'compact' + str(stage))

    def test_unproven_or_unsafe_patterns_keep_complete_dispatch(self):
        for mode in range(2,15):
            with self.subTest(mode=mode):
                source, info = self.source(mode=mode, filler=20)
                self.assertGreater(info[3], 0)
                self.assertEqual(info[0], info[1])
                self.compile(source, 1, 'fallback' + str(mode))

    def test_two_tables_share_compiler_constants_without_losing_destinations(self):
        for stage in (0,1,5):
            with self.subTest(stage=stage):
                compact, info = self.source(mode=15, stage=stage, filler=1800)
                reference, old = self.source(mode=16, stage=stage, filler=1800)
                self.assertEqual(info[2:], (2,0))
                self.assertEqual(old[2:], (0,2))
                self.assertEqual(info[0], old[0])
                self.assertEqual(info[1], 9)
                remove_labels = lambda s: re.sub(r'^  case \d+u:\n', '', s, flags=re.M)
                self.assertEqual(remove_labels(compact), remove_labels(reference))
                self.compile(compact, stage, 'two_tables' + str(stage))

    def test_metal_source_stays_identical(self):
        for stage in (0,1,5):
            with self.subTest(stage=stage):
                compact, _ = self.source(stage=stage, glsl=False)
                reference, _ = self.source(mode=1, stage=stage, glsl=False)
                self.assertEqual(compact, reference)

    def test_float_and_integer_vertex_attributes_keep_their_types(self):
        source, _ = self.source(mode=17, stage=0)
        for text in ('in vec4 a0;', 'in ivec4 a1;', 'in uvec4 a2;',
                     'r90 = as_u(a0[0]);', 'r91 = uint(a1[0]);', 'r92 = uint(a2[0]);'):
            self.assertIn(text, source)
        self.compile(source, 0, 'attribute_types')

    def test_vulkan_pixels_match_full_dispatch_for_all_table_entries(self):
        if not os.environ.get('SCARLET_VULKAN_GPU_TEST'):
            self.skipTest('requires SCARLET_VULKAN_GPU_TEST=1 and a Vulkan device')
        from test_shader_compilation import VERTEX
        vertex = self.compile(VERTEX, 0, 'triangle')
        exe = self.path / 'render'
        subprocess.run(self.cc + ['-O2', '-UNDEBUG', '-I', os.environ.get('VULKAN_INCLUDE','/usr/include'),
                       str(ROOT / 'tools/test_shader_compile_gpu.c'),
                       os.environ.get('VULKAN_LIBRARY','-lvulkan'), '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        for mode, fast, start in itertools.product((0,1), (False,True), (0,2,0xffffffff)):
            with self.subTest(mode=mode, fast=fast, start=start):
                source, _ = self.source(mode=mode)
                # Adapt only the input binding to the test harness; CB1 is immutable compiler data.
                # Obtain target PCs by reading MOV R0 immediates in the emitted source (10,20,30).
                targets = []
                for value in (10,20,30):
                    prefix = source[:source.index(f'    r0 = 0x{value:08x}u;')]
                    targets.append(int(re.findall(r'  case (\d+)u:', prefix)[-1]))
                targets[1] -= 8 # the second entry intentionally names the preceding scheduling word
                source = re.sub(r'^layout\(std140, set = 1, binding = 0\).*?; };\n',
                                'layout(push_constant) uniform Input { uvec4 data; };\n#define cb0 uvec4[](data)\n',
                                source, flags=re.M)
                const = 'const uvec4 cb1[2] = uvec4[](uvec4(0u,0u,0u,%du),uvec4(%du,%du,0u,0u));\n' % tuple(targets)
                source = re.sub(r'^layout\(std140, set = 1, binding = 1\).*?; };\n', const, source, flags=re.M)
                fragment = self.compile(source, 1, f'pixels_{mode}_{fast}_{start}')
                run = subprocess.run([str(exe), str(vertex), str(fragment), str(int(fast)), str(start)],
                                     capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                expected = []
                for index in (start, (start+1)&0xffffffff):
                    value = (min(index,2)+1)*10
                    expected += [value+k for k in range(4)]
                self.assertEqual([int(n,16) for n in run.stdout.split()], expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
