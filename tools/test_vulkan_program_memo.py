#!/usr/bin/env python3
"""Exercise the actual program memo, cache misses and fallback table hits."""
from pathlib import Path
import os,shutil,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
s=(r/'runtime/nvn/vulkan/exec.c').read_text()
start=s.index('// PokeZ\'s exact last-program memo:')
end=s.index('// ================================================================== vertex formats', start)
code=s[start:end]
fixture=r"""
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "nvn/maxwell/shader.h"
#define LOG(...) ((void)0)
typedef struct { uint64_t code_addr; uint32_t const_offset,const_size; } NvnShaderStage;
typedef struct { NvnShaderStage stages[6]; uint32_t stage_mask; } NvnProgram;
typedef struct { uint64_t code[2],cmax[2]; const uint8_t *constants[2]; uint32_t constants_size[2]; ShaderKey vkey,fkey; } Prog;
#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];
static unsigned jobs;
static void translate_program(void *p) { (void)p; }
static void run_job(void (*fn)(void *),void *p) { (void)fn;(void)p; ++jobs; }
"""
checks=r"""
int main(void) {
    NvnProgram np={0};np.stage_mask=3;np.stages[0].code_addr=0x1000;np.stages[1].code_addr=0x2000;
    ShaderKey vk={0},fk={0};
    Prog *p=get_program(&np,&vk,&fk);assert(p && jobs==1);
    for(int i=0;i<1000;i++) assert(get_program(&np,&vk,&fk)==p && jobs==1);
    last_program.prog=NULL; assert(get_program(&np,&vk,&fk)==p && jobs==1);
    np.stages[0].const_offset=128;np.stages[0].const_size=64;
    Prog *q=get_program(&np,&vk,&fk);assert(q!=p && jobs==2);
    np.stages[0].const_offset=0;np.stages[0].const_size=0;
    assert(get_program(&np,&vk,&fk)==p && jobs==2);
    vk.attr_kind[31]=2;assert(get_program(&np,&vk,&fk)!=p && jobs==3);
    vk.attr_kind[31]=0;fk.rt_kind[7]=1;assert(get_program(&np,&vk,&fk)!=p && jobs==4);
    np.stage_mask=0;assert(!get_program(&np,&vk,&fk));
    np.stage_mask=3;memset(prog_tab,0,sizeof prog_tab);last_program.prog=NULL;
    assert(get_program(&np,&vk,&fk)!=p && jobs==5);
    puts("program memo: repeats, compiler constants, vertex kind, output kind and reset passed");
}
"""
cc=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'test.c';e=Path(d)/'test';c.write_text(fixture+code+checks)
    subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(r/'runtime'),str(c),'-o',str(e)],check=True)
    subprocess.run([str(e)],check=True)
