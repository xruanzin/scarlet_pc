#!/usr/bin/env python3
"""Production LDG/STG translation and optional headless Vulkan execution; no game files.

CC selects a native C compiler. GLSLANG_VALIDATOR and SPIRV_VAL select validation tools.
SCARLET_VULKAN_GPU_TEST=1 enables numerical Vulkan tests (needs headers + libvulkan).
VULKAN_INCLUDE and VULKAN_LIBRARY can select a local SDK; VK_ICD_FILENAMES selects a driver.
"""
import itertools
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from test_indirect_resume import compiler_command

ROOT=Path(__file__).resolve().parent.parent
GENERATOR=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "shader.h"
static uint64_t code[64];static int n;
static void emit(uint64_t w){if(n%4==0)code[n++]=0;code[n++]=w;}
static void mov(int r,uint32_t v){emit(0x0100000000070000ull|((uint64_t)v<<20)|(uint32_t)r);}
int main(int argc,char **argv){
 assert(argc==4);int store=atoi(argv[1]),size=atoi(argv[2]),stage=atoi(argv[3]);
 mov(2,0x40001000);mov(3,0x72);
 int off=size<2?1:size<4?2:0;
 if(store){mov(4,0x89abcdef);mov(5,0x01234567);mov(6,0xfedcba98);mov(7,0x76543210);}
 else emit(0xeed0000000070000ull|((uint64_t)size<<48)|(1ull<<45)|((uint64_t)off<<20)|(2<<8)|4);
 int outsize=store?size:size<4?4:size;
 emit(0xeed8000000070000ull|((uint64_t)outsize<<48)|(1ull<<45)|((uint64_t)(32+(store?off:0))<<20)|(2<<8)|4);
 emit(0xe30000000007000full);
 uint64_t blob[80]={0};int header=stage==STAGE_COMPUTE?0:16;
 for(int i=0;i<n;i++)blob[header+i]=code[i];if(stage==STAGE_FRAGMENT)blob[15]=15;
 ShaderKey key={.glsl=1,.storage_buffers=16};maxwell_gmem_base=0x7240000000ull;maxwell_gmem_size=0xd0000000;
 ShaderInfo *s=maxwell_translate((const uint8_t *)blob,(header+n)*8,stage,&key);
 if(!s->msl){fprintf(stderr,"%s\n",s->error);return 1;}
 assert(s->uses_gmem&&s->writes_gmem&&s->storage_buffers==16);
 puts(s->msl);maxwell_free(s);
}
'''
PLANNER=r'''
#include <assert.h>
#include "storage_ranges.h"
#include "indirect.h"
static CmdRec seen[8];static int ndraws,flushes;static uint32_t dispatched[3];
static void renderer_flush(void){flushes++;}
static void draw(CmdRec *r,bool indexed){assert(ndraws<8);assert(indexed==(r->op==CMD_DRAW_ELEMENTS));seen[ndraws++]=*r;}
static void dispatch_compute(uint32_t x,uint32_t y,uint32_t z){dispatched[0]=x;dispatched[1]=y;dispatched[2]=z;}
static void execute(CmdRec *r){switch(r->op){
/*EXECUTOR*/
default:break;}}
int main(void){
 StorageRange a[]={{0x1010,16},{0x10a0,128},{0x1100,64},{0x2004,4},{0,0},{UINT64_MAX-8,100}};StorageRange o[8];
 size_t n=storage_merge_ranges(a,6,256,o,8);assert(n==2&&o[0].addr==0x1000&&o[0].size==0x140&&o[1].addr==0x2000&&o[1].size==8);
 CmdRec c={0},d;c.x[0]=4;c.x[1]=1;c.x[2]=0x4000;
 uint32_t v[]={12,3,5,(uint32_t)-7,9};assert(nvn_indirect_draw(&c,v,true,&d));
 assert(d.x[2]==12&&d.x[3]==0x400a&&(int64_t)d.x[4]==-7&&d.x[5]==9&&d.x[6]==3);
 assert(nvn_indirect_draw(&c,v,false,&d));assert(d.x[1]==5&&d.x[2]==12&&d.x[3]==(uint32_t)-7&&d.x[4]==3);
 v[1]=0;assert(!nvn_indirect_draw(&c,v,true,&d));v[1]=3;v[0]=0;assert(!nvn_indirect_draw(&c,v,false,&d));
 uint32_t args[16]={3,2,5,9,0,0,0,0,7,4,11,13};uint32_t count=3;
 c=(CmdRec){.op=CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT};c.x[0]=4;c.x[1]=(uintptr_t)args;c.x[2]=(uintptr_t)&count;c.x[3]=2;c.x[4]=32;
 execute(&c);assert(ndraws==2&&flushes==1&&seen[0].x[1]==5&&seen[1].x[1]==11&&seen[1].x[4]==4);
 ndraws=0;c.x[3]=1;execute(&c);assert(ndraws==1);args[1]=0;ndraws=0;execute(&c);assert(!ndraws);
 uint32_t elements[]={9,5,7,(uint32_t)-2,10};c=(CmdRec){.op=CMD_DRAW_ELEMENTS_INDIRECT};c.x[0]=4;c.x[1]=2;c.x[2]=0x8000;c.x[3]=(uintptr_t)elements;
 execute(&c);assert(ndraws==1&&seen[0].x[3]==0x801c&&(int64_t)seen[0].x[4]==-2&&seen[0].x[5]==10&&seen[0].x[6]==5);
 uint32_t groups[]={17,4,2};c=(CmdRec){.op=CMD_DISPATCH_INDIRECT};c.x[0]=(uintptr_t)groups;execute(&c);
 assert(dispatched[0]==17&&dispatched[1]==4&&dispatched[2]==2);
 return 0;
}
'''


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        cls.path=Path(cls.temp.name)
        cls.cc=shlex.split(os.environ['CC']) if os.environ.get('CC') else compiler_command()
        src=cls.path/'gen.c';src.write_text(GENERATOR);cls.gen=cls.path/'gen'
        result=subprocess.run(cls.cc+['-O1','-UNDEBUG','-I',str(ROOT/'runtime/nvn/maxwell'),str(src),
            str(ROOT/'runtime/nvn/maxwell/translate.c'),'-lm','-o',str(cls.gen)],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
        cls.glslang=os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        cls.validator=os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')

    def compile_shader(self,store,size,stage):
        r=subprocess.run([str(self.gen),str(int(store)),str(size),str(stage)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn('gmem_.w[',r.stdout)
        self.assertIn('binding = 79',r.stdout)
        if not self.glslang or not self.validator:self.skipTest('requires glslangValidator and spirv-val')
        path=self.path/('storage.'+{0:'vert',1:'frag',5:'comp'}[stage]);path.write_text(r.stdout)
        spv=self.path/'storage.spv'
        r=subprocess.run([self.glslang,'-V','--target-env','vulkan1.3','-o',str(spv),str(path)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        r=subprocess.run([self.validator,'--target-env','vulkan1.3',str(spv)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return spv

    def test_all_access_widths_and_stages(self):
        for store,size,stage in itertools.product((False,True),range(7),(0,1,5)):
            with self.subTest(store=store,size=size,stage=stage):self.compile_shader(store,size,stage)

    def test_alias_planning_and_indirect_arguments(self):
        executor=(ROOT/'runtime/nvn/vulkan/exec.c').read_text()
        start=executor.index('        case CMD_DISPATCH_INDIRECT: {')
        end=executor.index('        case CMD_DISPATCH: dispatch_compute(',start)
        src=self.path/'planner.c';src.write_text(PLANNER.replace('/*EXECUTOR*/',executor[start:end]));exe=self.path/'planner'
        r=subprocess.run(self.cc+['-O2','-UNDEBUG','-I',str(ROOT/'runtime/nvn/vulkan'),'-I',str(ROOT/'runtime/nvn'),
                         str(src),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        r=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)

    def test_vulkan_numerical_execution(self):
        if os.environ.get('SCARLET_VULKAN_GPU_TEST')!='1':self.skipTest('set SCARLET_VULKAN_GPU_TEST=1 for a headless Vulkan device')
        exe=self.path/'gpu';command=self.cc+['-O2','-UNDEBUG']
        if os.environ.get('VULKAN_INCLUDE'):command+=['-I',os.environ['VULKAN_INCLUDE']]
        command+=[str(ROOT/'tools/test_storage_gpu.c'),os.environ.get('VULKAN_LIBRARY','-lvulkan'),'-o',str(exe)]
        r=subprocess.run(command,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
        inputs=[0x80fedcba,0x12345678,0xc0defeed,0x76543210]
        loads=[[0xdc],[0xffffffdc],[0x80fe],[0xffff80fe],inputs[:1],inputs[:2],inputs]
        stores=[[0x1122ef44],[0x1122ef44],[0xcdef3344],[0xcdef3344],[0x89abcdef],
                [0x89abcdef,0x01234567],[0x89abcdef,0x01234567,0xfedcba98,0x76543210]]
        optimizer=os.environ.get('SPIRV_OPT') or shutil.which('spirv-opt')
        for store,size,optimized in itertools.product((False,True),range(7),(False,True) if optimizer else (False,)):
            with self.subTest(store=store,size=size,optimized=optimized):
                spv=self.compile_shader(store,size,5)
                if optimized:
                    r=subprocess.run([optimizer,'-O',str(spv),'-o',str(spv)],capture_output=True,text=True)
                    self.assertEqual(r.returncode,0,r.stderr)
                r=subprocess.run([str(exe),str(spv)],capture_output=True,text=True,timeout=30)
                self.assertEqual(r.returncode,0,r.stderr)
                expected=(stores if store else loads)[size]
                expected=expected+[0x11223344]*(4-len(expected))
                self.assertEqual([int(v,16) for v in r.stdout.split()],expected)

    def test_optimizer_modes_and_cache_identity(self):
        include=Path(os.environ.get('VULKAN_INCLUDE','/usr/include'))
        if not (include/'shaderc/shaderc.h').exists():self.skipTest('requires shaderc development headers')
        production=(ROOT/'runtime/nvn/vulkan/shaders.c').read_text()
        start=production.index('static shaderc_optimization_level optimization_level(')
        end=production.index('static uint32_t *cache_load(',start)
        text='''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <shaderc/shaderc.h>
#define STAGE_VERTEX 0
'''+production[start:end]+'''
int main(void){
 unsetenv("LARECOMP_VK_OPT");
 assert(optimization_level(0)==shaderc_optimization_level_zero);
 assert(optimization_level(1)==shaderc_optimization_level_zero);
 assert(optimization_level(5)==shaderc_optimization_level_zero);
 uint64_t fragment=src_hash("test",1),vertex=src_hash("test",0);
 setenv("LARECOMP_VK_OPT","0",1);assert(fragment==src_hash("test",1));assert(vertex==src_hash("test",0));
 setenv("LARECOMP_VK_OPT","1",1);assert(fragment!=src_hash("test",1));assert(vertex!=src_hash("test",0));
 assert(optimization_level(1)==shaderc_optimization_level_performance);
 setenv("LARECOMP_VK_OPT","",1);assert(fragment==src_hash("test",1));
 setenv("LARECOMP_VK_OPT","invalid",1);assert(fragment==src_hash("test",1));
 return 0;
}
'''
        src=self.path/'cache.c';src.write_text(text);exe=self.path/'cache'
        r=subprocess.run(self.cc+['-O1','-UNDEBUG','-I',str(include),str(src),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        r=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)


if __name__=='__main__':unittest.main(verbosity=2)
