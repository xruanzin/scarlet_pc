#!/usr/bin/env python3
"""Production regressions for first-use commands, Npad batches and storage-image ownership.

No game files. SDL_INCLUDE / VULKAN_INCLUDE can select unpacked development headers.
SCARLET_VULKAN_GPU_TEST=1 enables numerical surface writes on a Vulkan device.
"""
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_indirect_resume import compiler_command

ROOT = Path(__file__).resolve().parent.parent

WAIT = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static _Atomic int jobs_pending;
static uint64_t stat_shader_wait_ns;
static uint64_t SDL_GetTicksNS(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void SDL_Delay(uint32_t ms) { usleep(ms * 1000); }
/*WAIT*/
static void *build(void *arg) {
    usleep(40000);
    atomic_store((_Atomic int *)arg, 1);
    return NULL;
}
int main(void) {
    const char *operations[] = {"program", "pipeline", "compute"};
    for (int i = 0; i < 3; i++) {
        _Atomic int state = 0; pthread_t thread;
        assert(!pthread_create(&thread, NULL, build, &state));
        wait_built(&state, operations[i], 0x1000, 0x2000);
        // A one-time glyph/indirect-buffer write must execute on its first submission.
        uint32_t result = 0;
        if (atomic_load(&state) == 1) result = 0xcafef00d;
        assert(result == 0xcafef00d);
        pthread_join(thread, NULL);
        state = 2; wait_built(&state, operations[i], 0, 0);  // failure is terminal too
    }
    assert(stat_shader_wait_ns >= 90000000ull);
}
'''

INPUT = r'''
#include <assert.h>
#include <string.h>
#include "runtime/hle/input.c"
static bool captured;
bool menu_input_captured(void) { return captured; }
uint64_t renderer_present_count(void) { return 20; }
uint64_t g_watch_lo, g_watch_hi;
Settings g_settings;
int main(void) {
    setenv("LARECOMP_NPAD", "handheld", 1);
    void (*batches[])(Cpu *) = {hid_states_fullkey, hid_states_handheld,
        hid_states_dual, hid_states_left, hid_states_right};
    uint32_t id = 0x20;
    _Alignas(8) unsigned char out[0x50];
    int64_t previous = 0;
    key_buttons = 1; key_lx = 1234; pad_buttons = mouse_buttons = script_buttons = 0;
    for (unsigned i = 0; i < sizeof batches / sizeof *batches; i++) {
        for (int count = 1; count <= 16; count += 15) {
            memset(out, 0xa5, sizeof out);
            Cpu c = {0}; c.x[0] = (uintptr_t)out; c.x[1] = count; c.x[2] = (uintptr_t)&id;
            batches[i](&c);
            assert(c.x[0] == 1 && c.x[1] == (uint64_t)count && c.x[2] == (uintptr_t)&id);
            assert(*(int64_t *)out == previous + 1); previous++;
            assert(*(uint64_t *)(out + 8) == 1 && *(int32_t *)(out + 16) == 1234);
            assert(*(uint32_t *)(out + 32) == 1 && *(uint32_t *)(out + 36) == 0);
            for (int k = 0x28; k < sizeof out; k++) assert(out[k] == 0xa5);
        }
        for (int count = -1; count <= 0; count++) {
            Cpu c = {0}; c.x[1] = (uint32_t)count; batches[i](&c);
            assert(!c.x[0]);  // null output/id pointers must not be touched
        }
    }
    captured = true;
    Cpu c = {0}; c.x[0] = (uintptr_t)out; c.x[1] = 1; c.x[2] = (uintptr_t)&id;
    hid_states_handheld(&c);
    assert(!*(uint64_t *)(out + 8) && !*(int32_t *)(out + 16));
    captured = false; id = 7;
    c.x[0] = (uintptr_t)out; hid_states_handheld(&c);
    assert(c.x[0] == 1 && !*(uint32_t *)(out + 32) && !*(uint64_t *)(out + 8));
    // The singular API and the batch API must see the same buttons and monotonic sampling.
    id = 0x20; c.x[0] = (uintptr_t)out; c.x[1] = (uintptr_t)&id;
    hid_state_handheld(&c);
    assert(*(uint64_t *)(out + 8) == 1 && *(int32_t *)(out + 16) == 1234);
}
'''

VIEWS = r'''
#include <assert.h>
#include "runtime/nvn/vulkan/vk.h"
static TexBackend backend;
static unsigned uploads;
static bool fail_view;
static const VkComponentMapping IDENTITY = {0, 0, 0, 0};
static TexBackend *tex_backend(NvnTexture *t) { return &backend; }
TexBackend *vk_tex(NvnTexture *t, bool render) {
    assert(!render); uploads++; return &backend;
}
VkFormat vk_format(uint32_t f) { return backend.format; }
typedef struct { VkFormat vf; } VkFmt;
static VkFmt fmt_of(uint32_t f) { return (VkFmt){backend.format}; }
static bool format_supports(VkFormat f, VkFormatFeatureFlags want) { return true; }
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv,
    uint32_t nlv, uint32_t ly, uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    return fail_view ? VK_NULL_HANDLE : (VkImageView)(uintptr_t)1;
}
/*VIEWS*/
static struct { NvnDescPool *tpool; } st;
static NvnTexture *marked[2]; static unsigned nmarked;
void renderer_mark_gpu_owned(NvnTexture *t) { assert(nmarked < 2); marked[nmarked++] = t; }
/*OWNERSHIP*/
int main(void) {
    NvnTexture t = {0};
    backend = (TexBackend){.image=(VkImage)(uintptr_t)1,.samples=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,
        .base_type=VK_IMAGE_VIEW_TYPE_2D,.levels=1,.layers=1,.format=VK_FORMAT_R8G8B8A8_UNORM};
    assert(vk_storage_write_view(&t, NULL, false));
    assert(uploads == 1 && !backend.gpu_owned);
    assert(!vk_storage_atomic_u32_view(&t, NULL, false));
    assert(uploads == 1 && !backend.gpu_owned);  // incompatible probe has no ownership/upload side effects
    backend.format = VK_FORMAT_R32_UINT;
    assert(!vk_storage_write_view(&t, NULL, false));
    assert(uploads == 1 && !backend.gpu_owned);
    assert(vk_storage_atomic_u32_view(&t, NULL, false));
    assert(uploads == 2 && !backend.gpu_owned);
    fail_view = true;
    assert(!vk_storage_atomic_u32_view(&t, NULL, false) && uploads == 2);
    fail_view = false; backend.gpu_owned = true;
    assert(vk_storage_atomic_u32_view(&t, NULL, false) && backend.gpu_owned);
    backend.samples = 4;
    assert(!vk_storage_write_view(&t, NULL, false));
    NvnDescPool pool = {0}, other = {0}; NvnTexture float_tex = {0}, uint_tex = {0};
    st.tpool = &pool; surface_cache[0].pool = &pool; surface_cache[0].map_buf = (VkBuffer)(uintptr_t)1;
    surface_cache[0].write_tex[0] = &float_tex; surface_cache[0].atomic_tex[0] = &uint_tex;
    ShaderInfo si = {0}; surface_finish_writes(&si); assert(!nmarked);
    si.uses_surface_write = true; surface_finish_writes(&si);
    assert(nmarked == 2 && marked[0] == &float_tex && marked[1] == &uint_tex);
    nmarked = 0; si.uses_surface_write = false; si.uses_surface_atomic = true;
    surface_finish_writes(&si); assert(nmarked == 1 && marked[0] == &uint_tex);
    nmarked = 0; st.tpool = &other; surface_finish_writes(&si); assert(!nmarked);
}
'''

SURFACE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "shader.h"
static uint64_t words[96]; static int n;
static void emit(uint64_t w) { if (!(n % 4)) words[n++] = 0; words[n++] = w; }
static void mov(int r, uint32_t v) { emit(0x0100000000070000ull | ((uint64_t)v << 20) | r); }
int main(int argc, char **argv) {
    assert(argc == 3); int atomic = atoi(argv[1]), stage = atoi(argv[2]);
    mov(0, atomic ? 7 : 1); mov(1, atomic ? 3 : 0); mov(2, 3);
    mov(4, 0x3f000000); mov(5, 0x3e800000); mov(6, 0x3f800000); mov(7, 0x3f800000);
    mov(10, 1); mov(11, 0);
    emit(atomic ? 0xeb50008602070a00ull : 0xeb20010600f70004ull);
    emit(0xe30000000007000full);
    uint64_t blob[112] = {0}; int header = stage == STAGE_COMPUTE ? 0 : 16;
    for (int i = 0; i < n; i++) blob[header+i] = words[i];
    ShaderKey key = {.glsl=1,.bindless_storage=16};
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob, (header+n)*8, stage, &key);
    if (!s->msl) { fprintf(stderr, "%s\n", s->error); return 1; }
    assert(!s->surface_fallbacks && (atomic ? s->uses_surface_atomic : s->uses_surface_write));
    puts(s->msl); maxwell_free(s);
}
'''


class SceneProgressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.path = Path(cls.tmp.name)
        cls.cc = compiler_command()

    def harness(self, name, source, *, headers=False, extra=()):
        src = self.path / (name + '.c')
        exe = self.path / name
        src.write_text(source)
        command = self.cc + ['-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections', '-I', str(ROOT)]
        if headers:
            command += ['-I', os.environ.get('SDL_INCLUDE', '/usr/include'),
                        '-I', os.environ.get('VULKAN_INCLUDE', '/usr/include')]
        command += [str(src), '-Wl,--gc-sections', '-lm', '-pthread', *extra, '-o', str(exe)]
        r = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return exe

    def run_harness(self, *args):
        r = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_first_use_operations_are_preserved(self):
        source = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        start = source.index('static void wait_built(')
        end = source.index('// ================================================================== state', start)
        self.run_harness(self.harness('wait', WAIT.replace('/*WAIT*/', source[start:end])))
        for invocation in ('wait_built(&pr->state, "program"', 'wait_built(&pp->state, "pipeline"',
                           'wait_built(&cp->p.state, "compute"'):
            self.assertIn(invocation, source)
        self.assertNotIn('LARECOMP_SHADER_WAIT_MS', source)

    def test_real_npad_batch_adapters(self):
        self.run_harness(self.harness('input', INPUT, headers=True))

    def test_storage_view_probes_preserve_cpu_contents(self):
        source = (ROOT / 'runtime/nvn/vulkan/textures.c').read_text()
        start = source.index('static bool storage_float_format(')
        end = source.index('VkImageView vk_dummy_storage_view(', start)
        executor = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()
        cache_start = executor.index('typedef struct {\n    NvnDescPool *pool;')
        cache_end = executor.index('static void surface_cache_reset(', cache_start)
        write_start = executor.index('static void surface_finish_writes(')
        write_end = executor.index('// Writes the descriptor set', write_start)
        ownership = executor[cache_start:cache_end] + executor[write_start:write_end]
        harness = VIEWS.replace('/*VIEWS*/', source[start:end]).replace('/*OWNERSHIP*/', ownership)
        self.run_harness(self.harness('views', harness, headers=True))
        self.assertLess(executor.index('    vkCmdDispatch(cb, gx, gy, gz);'),
                        executor.index('    surface_finish_writes(cp->si);'))
        self.assertLess(executor.index('else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);'),
                        executor.index('    surface_finish_writes(pr->vi);'))

    def test_finalized_textures_leave_no_descriptor_references(self):
        source = (ROOT / 'runtime/nvn/objects.c').read_text()
        start = source.index('static void forget_texture_descriptors(')
        end = source.index('NVN(nvnTextureFinalize)', start)
        harness = r'''
#include <assert.h>
#include <string.h>
#include "runtime/nvn/nvn.h"
static NvnDescPool *g_texture_pools[64];
/*FORGET*/
int main(void) {
    NvnTexture deleted = {0}, live = {0};
    NvnTexEntry entries[3] = {{.tex=&deleted,.is_image=true},{.tex=&live},{.tex=&deleted}};
    NvnTexEntry untouched[1] = {{.tex=&live}};
    NvnDescPool pool = {.entries=entries,.count=3,.generation=12};
    NvnDescPool other = {.entries=untouched,.count=1,.generation=40};
    g_texture_pools[0] = &pool; g_texture_pools[63] = &other;
    forget_texture_descriptors(&deleted);
    assert(!entries[0].tex && !entries[2].tex && entries[1].tex == &live);
    assert(!entries[0].is_image && pool.generation == 13 && other.generation == 40);
    forget_texture_descriptors(&deleted); assert(pool.generation == 13);
}
'''
        self.run_harness(self.harness('forget', harness.replace('/*FORGET*/', source[start:end])))
        self.assertIn('forget_texture_descriptors(t); backend_texture_destroyed(t); free(t);', source)

    def surface_spirv(self, atomic, stage):
        if not hasattr(self, 'surface_generator'):
            translator = os.environ.get('SCARLET_SCENE_TRANSLATOR', str(ROOT / 'runtime/nvn/maxwell/translate.c'))
            self.surface_generator = self.harness('surface', SURFACE,
                extra=('-I', str(ROOT / 'runtime/nvn/maxwell'), translator))
        src = self.run_harness(self.surface_generator, int(atomic), stage)
        glslang = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not glslang or not validator:
            self.skipTest('requires glslangValidator and spirv-val')
        shader = self.path / ('surface.' + {0: 'vert', 1: 'frag', 5: 'comp'}[stage])
        shader.write_text(src)
        spv = self.path / 'surface.spv'
        self.run_harness(glslang, '-V', '--target-env', 'vulkan1.3', '-o', spv, shader)
        self.run_harness(validator, '--target-env', 'vulkan1.3', spv)
        return spv

    def test_surface_shaders_compile_in_all_stages(self):
        for atomic, stage in itertools.product((False, True), (0, 1, 5)):
            with self.subTest(atomic=atomic, stage=stage):
                self.surface_spirv(atomic, stage)

    def test_vulkan_surface_values(self):
        if os.environ.get('SCARLET_VULKAN_GPU_TEST') != '1':
            self.skipTest('set SCARLET_VULKAN_GPU_TEST=1 for Vulkan numerical execution')
        exe = self.harness('surface_gpu', (ROOT / 'tools/test_surface_gpu.c').read_text(), headers=True,
            extra=(os.environ.get('VULKAN_LIBRARY', '-lvulkan'),))
        optimizer = os.environ.get('SPIRV_OPT') or shutil.which('spirv-opt')
        for mode, optimized in itertools.product((0, 1, 2), (False, True) if optimizer else (False,)):
            with self.subTest(mode=mode, optimized=optimized):
                spv = self.surface_spirv(mode == 2, 5)
                if optimized:
                    self.run_harness(optimizer, '-O', spv, '-o', spv)
                values = [int(v, 16) for v in self.run_harness(exe, spv, mode).split()]
                expected = [0, 0xffff4080, 0, 0] if mode == 0 else [11, 18 if mode == 2 else 0x3f000000, 11, 11]
                self.assertEqual(values, expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
