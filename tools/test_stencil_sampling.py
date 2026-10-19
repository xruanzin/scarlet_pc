#!/usr/bin/env python3
"""Synthetic depth/stencil view, snapshot and GLSL regressions; no game assets.

CC selects a native compiler. VULKAN_INCLUDE and VMA_INCLUDE select headers.
GLSLANG_VALIDATOR / SPIRV_VAL enable shader validation. GPU execution is not
part of this test. SCARLET_TEXTURE_SOURCE selects an old texture backend for
a negative control of the sampled-aspect regression.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_transform_feedback import compiler_command

ROOT = Path(__file__).resolve().parent.parent


def function(source, name):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('missing function ' + name)
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


VIEW_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn/vulkan/vk.h"
#include "rt.h"
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;
static const VkComponentMapping IDENTITY={0};
static TexBackend backend;
static VkImageAspectFlags seen;
static uint32_t seen_layer, seen_layers;
static VkImageView feedback_view[4][2];
static struct { NvnDescPool *tpool; uint64_t tex[6][32]; } st;
TexBackend *vk_tex(NvnTexture *t, bool render) { return &backend; }
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0,
    uint32_t nlv, uint32_t ly0, uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    seen=aspect; seen_layer=ly0; seen_layers=nly; return (VkImageView)(uintptr_t)(100+aspect);
}
VkImageView vk_dummy_view(int textype) { assert(!"unexpected dummy view"); return VK_NULL_HANDLE; }
/*FUNCTIONS*/
int main(void) {
    NvnTexture t={.format=0x35};
    NvnTextureView v={.setmask=16,.ds_mode=1};
    backend=(TexBackend){.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.width=1920,.height=1080,
        .depth=1,.levels=1,.layers=1,.samples=1,.base_type=VK_IMAGE_VIEW_TYPE_2D,
        .aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT};
    vk_view(&t,&v,false,false,TEXTYPE_2D); assert(seen==VK_IMAGE_ASPECT_DEPTH_BIT);
    vk_view(&t,&v,true,false,TEXTYPE_2D); assert(seen==VK_IMAGE_ASPECT_STENCIL_BIT);
    t.ds_mode=1; vk_view(&t,NULL,false,false,TEXTYPE_2D); assert(seen==VK_IMAGE_ASPECT_STENCIL_BIT);
    v.ds_mode=0; vk_view(&t,&v,true,false,TEXTYPE_2D); assert(seen==VK_IMAGE_ASPECT_DEPTH_BIT);
    vk_view(&t,&v,true,true,TEXTYPE_2D); assert(seen==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));
    t.format=0x34; assert(!nvn_texture_samples_stencil(&t,NULL,false));
    t.format=0x31; assert(nvn_texture_samples_stencil(&t,NULL,false));
    t.format=0x25; assert(!nvn_texture_samples_stencil(&t,&v,true));
    t.format=0x35; t.ds_mode=0; v.ds_mode=1;
    NvnTexEntry entries[2]={{.tex=&t},{.tex=&t,.view=v,.has_view=true}};
    NvnDescPool pool={.entries=entries,.count=2}; st.tpool=&pool;
    st.tex[1][3]=1; st.tex[1][8]=1ull<<20; st.tex[1][9]=99;
    assert(stencil_texture_bindings(1)==(1u<<3));
    assert(stencil_texture_bindings(0)==0);
    entries[1].view.ds_mode=0; assert(stencil_texture_bindings(1)==0);
    st.tpool=NULL; assert(stencil_texture_bindings(1)==0);
    feedback_set_views(0,&backend);
    assert(feedback_view[0][0] == (VkImageView)(uintptr_t)(100+VK_IMAGE_ASPECT_DEPTH_BIT));
    assert(feedback_view[0][1] == (VkImageView)(uintptr_t)(100+VK_IMAGE_ASPECT_STENCIL_BIT));
    VkImageCopy regions[2];
    assert(feedback_copy_regions(&backend,regions)==2);
    assert(regions[0].srcSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
    assert(regions[1].srcSubresource.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT);
    for (int i=0;i<2;i++) {
        assert(regions[i].dstSubresource.aspectMask==regions[i].srcSubresource.aspectMask);
        assert(regions[i].extent.width==1920 && regions[i].extent.height==1080);
    }
    backend.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    assert(feedback_copy_regions(&backend,regions)==1);
    assert(regions[0].srcSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);
    feedback_set_views(1,&backend); assert(feedback_view[1][1]==VK_NULL_HANDLE);
    backend.base_type=VK_IMAGE_VIEW_TYPE_3D; backend.depth=32; backend.layers=1; backend.levels=2;
    backend.format=VK_FORMAT_A2B10G10R10_UNORM_PACK32; backend.width=backend.height=32;
    t.format=0x3d; v=(NvnTextureView){.setmask=2,.num_layers=1};
    for (uint16_t slice=0;slice<32;slice++) {
        v.min_layer=slice; vk_view(&t,&v,true,true,TEXTYPE_2D);
        assert(seen_layer==slice && seen_layers==1);
    }
    v.setmask|=1; v.base_level=1; v.num_levels=1; v.min_layer=15;
    vk_view(&t,&v,true,true,TEXTYPE_2D); assert(seen_layer==15 && seen_layers==1);
    puts("depth/stencil selection, explicit depth override, attachment and snapshot aspects passed");
}
'''

SHADER_HARNESS = r'''
#include <stdio.h>
#include TRANSLATE_SOURCE
int main(int argc, char **argv) {
    ShaderKey key={.glsl=1,.stencil_tex_mask=1u<<2};
    Ctx *c=calloc(1,sizeof *c); c->key=&key; c->glsl=1;
    c->stage=STAGE_FRAGMENT; c->info=calloc(1,sizeof *c->info);
    // Translation slot 0 is NVN binding 2; slot 1 is NVN binding 1.
    int stencil=tex_slot(c,0x30/4,TEXTYPE_2D,false,false);
    int depth=tex_slot(c,0x28/4,TEXTYPE_2D,false,false);
    write_tex_result(c,stencil,"texelFetch(t0, ivec2(0), 0)",1,0,RZ,false,false);
    write_tex_result(c,depth,"texelFetch(t1, ivec2(0), 0)",1,1,RZ,false,false);
    // Keep results alive in an integer target: material code and raw depth bits.
    c->omap_target=3; key.rt_kind[0]=2;
    ShaderInfo *s=glsl_finish(c,false,false);
    if (!s->msl) return 2;
    puts(s->msl); maxwell_free(s); return 0;
}
'''


class StencilSamplingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        cls.cc = compiler_command()

    def build(self, text, name, extra=()):
        path, exe = self.work / (name + '.c'), self.work / name
        path.write_text(text)
        run = subprocess.run(self.cc + ['-O1', '-UNDEBUG', '-I' + str(ROOT / 'runtime'),
            '-I' + str(ROOT / 'runtime/nvn/maxwell'), *extra, str(path), '-lm', '-o', str(exe)],
            capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr[:3000])
        return exe

    def test_native_sampled_aspects_and_snapshot_planes(self):
        include = os.environ.get('VULKAN_INCLUDE', '/usr/include')
        if not (Path(include) / 'vulkan/vulkan.h').is_file():
            self.skipTest('requires Vulkan and VMA headers')
        textures = Path(os.environ.get('SCARLET_TEXTURE_SOURCE', str(ROOT / 'runtime/nvn/vulkan/textures.c'))).read_text()
        executor = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        funcs = '\n'.join(function(textures, name) for name in ('fmt_of','vk_format','view_type_for','vk_view'))
        funcs += '\n' + '\n'.join(function(executor, name) for name in
            ('stencil_texture_bindings','feedback_copy_regions','feedback_set_views'))
        exe = self.build(VIEW_HARNESS.replace('/*FUNCTIONS*/', funcs), 'views',
            ['-I' + include, '-I' + os.environ.get('VMA_INCLUDE', include)])
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def shader(self):
        translator = ROOT / 'runtime/nvn/maxwell/translate.c'
        exe = self.build(SHADER_HARNESS, 'shader', ['-DTRANSLATE_SOURCE="' + str(translator) + '"'])
        return subprocess.check_output([str(exe)], text=True)

    def test_stencil_register_is_integer_and_depth_stays_float(self):
        source = self.shader()
        self.assertIn('uniform usampler2D t0;', source)
        self.assertIn('uniform sampler2D t1;', source)
        self.assertIn('uvec4 t = texelFetch(t0,', source)
        self.assertIn('r0 = t[0];', source)
        self.assertIn('r1 = as_u(t[0]);', source)

    def test_typed_fetches_compile_and_validate(self):
        compiler = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not compiler or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        src, output = self.work / 'stencil.frag', self.work / 'stencil.spv'
        src.write_text(self.shader())
        for cmd in ([compiler, '-V', '--target-env', 'vulkan1.1', str(src), '-o', str(output)],
                    [validator, '--target-env', 'vulkan1.1', str(output)]):
            run = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
