#!/usr/bin/env python3
"""Run production LUT slicing with stub draw calls; no GPU or game files."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_stencil_sampling import function
from test_transform_feedback import compiler_command

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/nvn.h"
/*RTSTATE*/
static struct { RtState rt; bool rt_dirty; } st;
static bool lut_slice_active;
static unsigned calls, ended, filled, flushed;
static int fail_slice=-1;
static uint64_t stat_draws, g_draws_total;
static void vk_end_pass(void) { ended++; }
void renderer_flush(void) { flushed++; }
bool renderer_fill_identity_lut_3d(NvnTexture *t) { filled++; return true; }
static void draw_(CmdRec *r, bool indexed);
#define LOG(...) fprintf(stderr,__VA_ARGS__)
/*FUNCTION*/
static void draw_(CmdRec *r, bool indexed) {
    // Must bypass recursion into the split path and preserve shader instance ID.
    assert(!draw_lut_slices(r,indexed));
    assert(lut_slice_active);
    assert(st.rt_dirty && st.rt.has_cview[0]);
    assert(st.rt.cview[0].num_layers==1 && st.rt.cview[0].min_layer==calls);
    assert(r->x[indexed?5:3]==calls && r->x[indexed?6:4]==1);
    assert(r->x[2]==3);
    if ((int)calls==fail_slice) return;
    calls++;
    g_draws_total++;
}
int main(void) {
    unsetenv("LARECOMP_LUT_IDENTITY");
    NvnTexture t={.target=2,.format=0x3d,.width=32,.height=32,.depth=32};
    for (int indexed=0;indexed<=1;indexed++) {
        st.rt=(RtState){.color={&t},.ncolor=8};
        st.rt.cview[0]=(NvnTextureView){.setmask=8,.swizzle={2,3,4,5}};
        st.rt.has_cview[0]=1;
        RtState before=st.rt;
        CmdRec r={.op=23,.size=sizeof(CmdRec),.x={4,0,3,0,0,0,0}};
        r.x[indexed?6:4]=32;
        CmdRec old=r;
        calls=ended=filled=0;
        assert(draw_lut_slices(&r,indexed));
        assert(calls==32 && ended==1 && filled==0);
        assert(!memcmp(&st.rt,&before,sizeof before));
        assert(!memcmp(&r,&old,sizeof r));
        assert(st.rt_dirty);
        assert(!lut_slice_active);
    }
    CmdRec r={.x={4,0,3,0,32}};
    calls=ended=filled=flushed=0; fail_slice=3;
    assert(draw_lut_slices(&r,false));
    assert(calls==3 && filled==1 && flushed==1 && ended==1);
    assert(!lut_slice_active);
    fail_slice=-1;
    setenv("LARECOMP_LUT_IDENTITY","1",1); calls=ended=filled=0;
    assert(draw_lut_slices(&r,false) && filled==1 && calls==0);
    unsetenv("LARECOMP_LUT_IDENTITY");
    t.width=64; assert(!draw_lut_slices(&r,false));
    t.width=32; r.x[4]=8; assert(draw_lut_slices(&r,false) && filled==2);
    st.rt.has_cview[0]=true; st.rt.cview[0].setmask|=2; st.rt.cview[0].num_layers=1;
    assert(!draw_lut_slices(&r,false));
    st.rt.cview[0].num_layers=16; assert(!draw_lut_slices(&r,false));
    puts("32 individual slices, instance IDs, state restoration and compatibility fallback passed");
}
'''


class LutSliceTests(unittest.TestCase):
    def test_original_guest_shader_draws_each_slice_and_restores_state(self):
        executor = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        end = executor.index('} RtState;') + len('} RtState;')
        start = executor.rfind('typedef struct {', 0, end)
        source = HARNESS.replace('/*RTSTATE*/', executor[start:end]).replace(
            '/*FUNCTION*/', function(executor, 'draw_lut_slices'))
        with tempfile.TemporaryDirectory() as tmp:
            path, exe = Path(tmp) / 'slices.c', Path(tmp) / 'slices'
            path.write_text(source)
            result = subprocess.run(compiler_command() + ['-O1', '-UNDEBUG',
                '-I' + str(ROOT / 'runtime'), str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
