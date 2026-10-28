#!/usr/bin/env python3
"""Compare production lean copies byte-for-byte with the former full blocks."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'runtime/nvn/vulkan/exec.c').read_text()
code = s[s.index('static bool scarlet_lean_constants'):s.index('// Constant buffer k of a stage')]
fixture = r"""
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef struct { uint64_t addr, size; } Range;
typedef struct { uint64_t buf, off; void *ptr; } VkSpan;
static struct { Range sbuf[6][16]; } st;
static uint64_t seq = 1, copies;
static unsigned char arena[8<<20];
static uint64_t used;
static uint64_t vk_pending_seq(void) { return seq; }
static VkSpan vk_transient(uint64_t size, uint64_t align) {
    used = (used + align - 1) & ~(align - 1);
    assert(used + size <= sizeof arena); VkSpan s = {1, used, arena + used};
    used += size; copies++; return s;
}
static VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s=vk_transient(size,align); memcpy(s.ptr,data,size); return s;
}
"""
checks = r"""
int main(void) {
    assert(scarlet_lean_constants());
    for(int sg=0;sg<2;sg++) for(int nt=0;nt<=64;nt++) {
        float rs[65][4], full[65][4]={0}; memset(rs,0xa5,sizeof rs);
        for(int i=0;i<=nt;i++) for(int j=0;j<4;j++) rs[i][j]=full[i][j]=(float)(i*4+j);
        VkSpan s=scarlet_scale_copy(sg,(const float (*)[4])rs,nt);
        assert(!memcmp(s.ptr,full,sizeof full)); uint64_t n=copies;
        assert(scarlet_scale_copy(sg,(const float (*)[4])rs,nt).off==s.off && copies==n);
        rs[nt][1]+=1; assert(scarlet_scale_copy(sg,(const float (*)[4])rs,nt).off!=s.off);
        seq++; assert(scarlet_scale_copy(sg,(const float (*)[4])rs,nt).off!=s.off);
    }
    for(int stage=0;stage<2;stage++) for(int i=0;i<16;i++) {
        st.sbuf[stage][i]=(Range){0x123456780000ull+i*47, 0x100000000ull+i*13};
    }
    for(int stage=0;stage<2;stage++) for(unsigned size=1;size<=2048;size++) {
        uint32_t full[512]={0}; unsigned base=stage==1?0x510:0x110;
        for(int i=0;i<16;i++) { Range r=st.sbuf[stage][i]; unsigned k=(base+i*16)/4;
            full[k]=r.addr; full[k+1]=r.addr>>32; full[k+2]=r.size; }
        VkSpan s=scarlet_driver_copy(stage,size); assert(!memcmp(s.ptr,full,size));
        uint64_t n=copies;
        assert(scarlet_driver_copy(stage,size).off==s.off && copies==n);
    }
    VkSpan old=scarlet_driver_copy(0,2048); st.sbuf[0][15].addr++;
    assert(scarlet_driver_copy(0,2048).off!=old.off);
    old=scarlet_driver_copy(0,2048); seq++;
    assert(scarlet_driver_copy(0,2048).off!=old.off);
    puts("lean constants: all sizes 1..2048, scale counts 0..64, changes and epochs match full bytes");
}
"""
cc=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'test.c'; e=Path(d)/'test'; c.write_text(fixture+code+checks)
    subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(e)],check=True)
    subprocess.run([str(e)],check=True)
