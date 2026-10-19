#!/usr/bin/env python3
"""Native depth/stencil snapshot invalidation regression; no game assets/GPU."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from test_stencil_sampling import function
from test_transform_feedback import compiler_command

ROOT=Path(__file__).resolve().parent.parent
HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "nvn/vulkan/vk.h"
static struct { struct { NvnTexture *depth; } rt; uint8_t ds[8]; uint32_t stencil_mask[2]; } st;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8], *snap_reason="test";
bool renderer_format_has_stencil(uint32_t f) { return f==0x35; }
/*FUNCTIONS*/
static void draw_write(void) {
    if (depth_stencil_writes()) snapshots_invalidate(st.rt.depth);
}
int main(void) {
    NvnTexture depth={.format=0x35},other={.format=0x35};
    st.rt.depth=&depth; snap_tex[0]=&depth; snap_tex[1]=&other;
    snap_ok[0]=snap_ok[1]=true;
    // A stencil-only material pass after a read-only depth prepass.
    st.ds[0]=1; st.ds[1]=0; st.ds[3]=1;
    st.ds[5]=0x11; st.ds[6]=0x13; st.ds[7]=0x31; // replace on pass, front and back
    st.stencil_mask[0]=st.stencil_mask[1]=255;
    draw_write(); assert(!snap_ok[0] && snap_ok[1]);
    // A stencil test with KEEP operations has no side effect.
    snap_ok[0]=true; st.ds[5]=st.ds[6]=st.ds[7]=0x11;
    draw_write(); assert(snap_ok[0]);
    // Masked-out writes cannot change material IDs.
    st.ds[6]=0x13; st.ds[7]=0x31;
    st.stencil_mask[0]=st.stencil_mask[1]=0;
    draw_write(); assert(snap_ok[0]);
    // Back-face writes must invalidate independently of the front mask.
    st.stencil_mask[1]=255;
    draw_write(); assert(!snap_ok[0]);
    // Depth writes still invalidate even with stencil disabled.
    snap_ok[0]=true; st.ds[3]=0; st.ds[1]=1;
    draw_write(); assert(!snap_ok[0]);
    // No depth test: depthWrite alone does not write depth.
    snap_ok[0]=true; st.ds[0]=0;
    draw_write(); assert(snap_ok[0]);
    st.rt.depth=0; assert(!depth_stencil_writes());
}
'''

class FeedbackFreshnessTests(unittest.TestCase):
    def test_stencil_only_writes_invalidate_material_snapshot(self):
        include=os.environ.get('VULKAN_INCLUDE','/usr/include')
        if not (Path(include)/'vulkan/vulkan.h').exists():self.skipTest('Vulkan headers required')
        source=(ROOT/'runtime/nvn/vulkan/exec.c').read_text()
        funcs='\n'.join(function(source,n) for n in ('stencil_op','depth_stencil_writes','snapshots_invalidate'))
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
            c.write_text(HARNESS.replace('/*FUNCTIONS*/',funcs))
            run=subprocess.run(compiler_command()+['-O1','-UNDEBUG','-I'+str(ROOT/'runtime'),'-I'+include,
                '-I'+os.environ.get('VMA_INCLUDE',include),str(c),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
