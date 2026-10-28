#!/usr/bin/env python3
"""Run the production draw-state emitter against a recording Vulkan mock."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
single = {
    'DepthTestEnable': 'depth_test', 'DepthWriteEnable': 'depth_write',
    'DepthCompareOp': 'depth_compare', 'StencilTestEnable': 'stencil_test',
    'CullMode': 'cull', 'FrontFace': 'front', 'PrimitiveTopology': 'topology',
    'PrimitiveRestartEnable': 'restart', 'RasterizerDiscardEnable': 'discard',
    'DepthBiasEnable': 'bias_enable',
}
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t VkCommandBuffer, VkPipeline;
typedef uint32_t VkBool32, VkStencilOp, VkCompareOp, VkCullModeFlags;
typedef uint32_t VkFrontFace, VkPrimitiveTopology, VkStencilFaceFlags;
#define VK_NULL_HANDLE 0
#define VK_PIPELINE_BIND_POINT_GRAPHICS 0
#define VK_STENCIL_FACE_FRONT_BIT 1
#define VK_STENCIL_FACE_BACK_BIT 2
static void vkCmdBindPipeline(VkCommandBuffer, unsigned, VkPipeline);
static void vkCmdSetStencilOp(VkCommandBuffer, unsigned, unsigned, unsigned, unsigned, unsigned);
static void vkCmdSetStencilCompareMask(VkCommandBuffer, unsigned, unsigned);
static void vkCmdSetStencilWriteMask(VkCommandBuffer, unsigned, unsigned);
static void vkCmdSetStencilReference(VkCommandBuffer, unsigned, unsigned);
static void vkCmdSetBlendConstants(VkCommandBuffer, const float *);
static void vkCmdSetDepthBias(VkCommandBuffer, float, float, float);
'''
for name in single:
    fixture += f'static void vkCmdSet{name}(VkCommandBuffer, unsigned);\n'
fixture += r'''
#include "runtime/nvn/vulkan/draw_state_cache.h"
static ScarletDrawState observed;
static unsigned calls, binds;
static VkCommandBuffer last_cb;
static VkPipeline last_pipe;
static void touch(VkCommandBuffer cb) { assert(cb == last_cb); ++calls; }
static void vkCmdBindPipeline(VkCommandBuffer cb, unsigned point, VkPipeline pipe) {
    assert(point == VK_PIPELINE_BIND_POINT_GRAPHICS);
    last_cb = cb; last_pipe = pipe; ++binds;
    memset(&observed, 0, sizeof observed); // pipeline transition may invalidate dynamic state
}
static unsigned face_index(unsigned face) {
    assert(face == VK_STENCIL_FACE_FRONT_BIT || face == VK_STENCIL_FACE_BACK_BIT);
    return face == VK_STENCIL_FACE_BACK_BIT;
}
static void vkCmdSetStencilOp(VkCommandBuffer cb, unsigned face, unsigned fail,
                             unsigned pass, unsigned depth_fail, unsigned compare) {
    touch(cb); ScarletStencilState *s = &observed.stencil[face_index(face)];
    s->fail=fail; s->pass=pass; s->depth_fail=depth_fail; s->compare=compare;
}
static void vkCmdSetStencilCompareMask(VkCommandBuffer cb, unsigned f, unsigned v) {
    touch(cb); observed.stencil[face_index(f)].compare_mask=v;
}
static void vkCmdSetStencilWriteMask(VkCommandBuffer cb, unsigned f, unsigned v) {
    touch(cb); observed.stencil[face_index(f)].write_mask=v;
}
static void vkCmdSetStencilReference(VkCommandBuffer cb, unsigned f, unsigned v) {
    touch(cb); observed.stencil[face_index(f)].reference=v;
}
static void vkCmdSetBlendConstants(VkCommandBuffer cb, const float *v) {
    touch(cb); memcpy(observed.blend, v, sizeof observed.blend);
}
static void vkCmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
    touch(cb); observed.bias_constant=constant; observed.bias_clamp=clamp; observed.bias_slope=slope;
}
static void equivalent(const ScarletDrawState *expected) {
    ScarletDrawState actual=observed, want=*expected;
    if (!want.stencil_test) {
        memset(actual.stencil, 0, sizeof actual.stencil);
        memset(want.stencil, 0, sizeof want.stencil);
    }
    assert(!memcmp(&actual, &want, sizeof want));
}
'''
for name, field in single.items():
    fixture += f'static void vkCmdSet{name}(VkCommandBuffer cb, unsigned v) {{ touch(cb); observed.{field}=v; }}\n'
fixture += r'''
int main(void) {
    ScarletDrawStateCache cache={0};
    ScarletDrawState state={0}; state.stencil_test=1;
    scarlet_apply_draw_state(&cache, 1, 2, &state);
    equivalent(&state); assert(calls == 20 && binds == 1);
    for (unsigned i=0; i<1000; ++i) scarlet_apply_draw_state(&cache, 1, 2, &state);
    assert(calls == 20 && binds == 1);
'''
fields = list(single.values()) + [f'blend[{i}]' for i in range(4)]
fields += ['bias_constant', 'bias_clamp', 'bias_slope']
fields += [f'stencil[{i}].{name}' for i in range(2)
           for name in ('fail', 'pass', 'depth_fail', 'compare', 'compare_mask', 'write_mask', 'reference')]
for field in fields:
    fixture += f'''
    {{
        unsigned before=calls;
        state.{field} += 1;
        scarlet_apply_draw_state(&cache, 1, 2, &state);
        assert(calls > before); equivalent(&state);
        before=calls; scarlet_apply_draw_state(&cache, 1, 2, &state); assert(calls == before);
    }}
'''
fixture += r'''
    scarlet_apply_draw_state(&cache, 1, 3, &state); // different pipeline
    equivalent(&state); assert(binds == 2 && last_pipe == 3);
    scarlet_apply_draw_state(&cache, 2, 3, &state); // different command buffer
    equivalent(&state); assert(binds == 3 && last_cb == 2);
    memset(&observed, 0xcc, sizeof observed); // clear/post-process overwrote state
    scarlet_draw_state_invalidate(&cache);
    scarlet_apply_draw_state(&cache, 2, 3, &state); // same handle, fresh recording
    equivalent(&state); assert(binds == 4);
    state.stencil_test=0;
    scarlet_apply_draw_state(&cache, 2, 3, &state); equivalent(&state);
    state.stencil_test=1;
    scarlet_apply_draw_state(&cache, 2, 3, &state); equivalent(&state);
    // The diagnostic uncached path must have the same observable state.
    for (unsigned i=0; i<10; ++i) {
        scarlet_draw_state_invalidate(&cache);
        scarlet_apply_draw_state(&cache, 2, 3, &state); equivalent(&state);
    }
    assert(binds == 14);
    puts("draw state: repeated draws, every state field, pipeline/CB changes and invalidation passed");
}
'''
# The emitter alone cannot detect callers that bypass its state tracking.
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
lines = source.splitlines()
for i, line in enumerate(lines):
    if 'vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,' in line:
        assert 'scarlet_draw_state_invalidate' in lines[i - 1], line
assert 'scarlet_draw_state_invalidate(&draw_state_cache);\n        begin_cb(&s->main' in source
assert 'static void dyn_defaults(VkCommandBuffer cb) {\n    scarlet_draw_state_invalidate' in source
assert 'if (!draw_state_reuse) scarlet_draw_state_invalidate' in source

compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'state.c'
    exe = Path(directory) / ('state.exe' if os.name == 'nt' else 'state')
    c.write_text(fixture)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
