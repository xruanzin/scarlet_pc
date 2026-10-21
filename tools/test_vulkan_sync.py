#!/usr/bin/env python3
"""Native synchronization regressions; no game files or Vulkan driver needed.

Uses production barrier/lock functions. A worker must acquire the renderer
mutex during pacing, while the presentation gate remains exclusive. Vulkan
headers are required for the resource dependency tests (VULKAN_INCLUDE).
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_stencil_sampling import function
from test_transform_feedback import compiler_command

ROOT = Path(__file__).resolve().parent.parent

BARRIERS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/vulkan/sync_scope.h"
typedef struct {
    VkImage image;
    VkImageAspectFlags aspect;
    unsigned levels, layers;
} TexBackend;
static uint64_t stat_full_barriers, stat_image_dependencies, stat_scoped_barrier_calls;
static unsigned emitted, count, full;
static VkImageMemoryBarrier2 seen[4];
static VkMemoryBarrier2 memory;
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    emitted++; count = di->imageMemoryBarrierCount; full = di->memoryBarrierCount;
    assert(count <= 4);
    if (count) memcpy(seen, di->pImageMemoryBarriers, count * sizeof *seen);
    if (full) memory = *di->pMemoryBarriers;
}
/*FUNCTIONS*/
static void check_common(const VkImageMemoryBarrier2 *b, const TexBackend *image) {
    assert(b->image == image->image);
    assert(b->srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    assert(b->srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT);
    assert(b->oldLayout == VK_IMAGE_LAYOUT_GENERAL && b->newLayout == VK_IMAGE_LAYOUT_GENERAL);
    assert(b->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(b->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(b->subresourceRange.aspectMask == image->aspect);
    assert(!b->subresourceRange.baseMipLevel && b->subresourceRange.levelCount == image->levels);
    assert(!b->subresourceRange.baseArrayLayer && b->subresourceRange.layerCount == image->layers);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    TexBackend src = {(VkImage)(uintptr_t)1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 4, 6};
    TexBackend dst = {(VkImage)(uintptr_t)2, src.aspect, 4, 6};
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)3;
    vk_scoped_image_barriers(cb, 0, NULL);
    assert(!emitted);
    transfer_images(cb, &src, &dst);
    if (!strcmp(argv[1], "full")) {
        assert(full == 1 && count == 0 && stat_full_barriers == 1 && !stat_image_dependencies);
        assert(memory.dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        assert(memory.dstAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT);
        return 0;
    }
    assert(count == 2 && !full && stat_scoped_barrier_calls == 1 && stat_image_dependencies == 2);
    check_common(&seen[0], &src); check_common(&seen[1], &dst);
    assert(seen[0].dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    assert(seen[0].dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
    assert(seen[1].dstAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // Different mip/layer copies of one allocation need both read and write access.
    transfer_images(cb, &src, &src);
    assert(count == 1);
    check_common(&seen[0], &src);
    assert(seen[0].dstAccessMask == (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT));
    transfer_images(cb, NULL, &dst); // host upload: destination must still order prior readers/writers
    assert(count == 1); check_common(&seen[0], &dst);
    assert(seen[0].dstAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    transfer_images(cb, &src, NULL); // known transfer source: include stencil and all mips/layers
    assert(count == 1); check_common(&seen[0], &src);
    assert(seen[0].dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_sample_image(cb, &dst);
    assert(count == 1); check_common(&seen[0], &dst);
    assert(seen[0].dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    assert(seen[0].dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    assert(!stat_full_barriers && stat_scoped_barrier_calls == 5 && stat_image_dependencies == 6);
}
'''

PACING = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_present_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool owns_big;
static bool pass_active;
static unsigned sleeps, end_passes, repre;
static uint64_t SDL_GetTicksNS(void) { return 100; }
static uint64_t vk_sync_profile_begin(void) { return 0; }
#define vk_sync_profile_end(metric, start) ((void)(start))
static void vk_big_lock(void) { assert(!owns_big); pthread_mutex_lock(&big); owns_big = true; }
static void vk_big_unlock(void) { assert(owns_big); owns_big = false; pthread_mutex_unlock(&big); }
static void vk_end_pass(void) { assert(owns_big); pass_active = false; end_passes++; }
static void *record_worker(void *arg) {
    vk_big_lock(); // must finish while the presenter is sleeping
    assert(pthread_mutex_trylock(&g_present_lock) == EBUSY);
    pass_active = true;
    vk_big_unlock();
    return NULL;
}
static void SDL_DelayPrecise(uint64_t ns) {
    assert(ns == 100 && !owns_big);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, record_worker, NULL));
    assert(!pthread_join(worker, NULL));
    sleeps++;
}
static void backend_present(void *tex) { assert(!tex && !owns_big); repre++; }
/*FUNCTIONS*/
int main(void) {
    present_lock();
    present_wait_until(50); // expired deadline does not sleep or disturb state
    assert(!sleeps && !end_passes && owns_big);
    present_wait_until(200);
    assert(sleeps == 1 && end_passes == 1 && owns_big && !pass_active);
    assert(pthread_mutex_trylock(&g_present_lock) == EBUSY);
    present_unlock();
    assert(!pthread_mutex_trylock(&g_present_lock)); pthread_mutex_unlock(&g_present_lock);
    backend_represent_now(); assert(repre == 1);
}
'''

RESUME = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "nvn/vulkan/sync_scope.h"
static struct { bool ok; unsigned image; } cur_frame = {true, 1};
static struct { VkImage sc_images[2]; VkImageView sc_views[2]; VkExtent2D sc_extent; } g_vk;
static bool ready;
static VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void vk_scoped_image_barriers(VkCommandBuffer cb, uint32_t n, const VkImageMemoryBarrier2 *b) {
    assert(n == 1 && b->image == g_vk.sc_images[1]);
    assert(b->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && b->newLayout == b->oldLayout);
    assert(b->dstStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    assert(b->dstAccessMask == (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
    ready = true;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info) {
    assert(ready && info->pColorAttachments->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *s) {}
/*FUNCTIONS*/
int main(void) {
    g_vk.sc_images[1] = (VkImage)(uintptr_t)9;
    g_vk.sc_extent = (VkExtent2D){1280, 720};
    vk_frame_resume(); assert(ready);
    ready = false; cur_frame.ok = false;
    vk_frame_resume(); assert(!ready);
}
'''


class VulkanSyncTests(unittest.TestCase):
    def run_native(self, template, source, names, mode=None, headers=False):
        includes = ['-I' + str(ROOT / 'runtime')]
        if headers:
            include = Path(os.environ.get('VULKAN_INCLUDE', '/usr/include'))
            if not (include / 'vulkan/vulkan.h').exists():
                self.skipTest('Vulkan headers required')
            includes += ['-I' + str(include)]
        code = template.replace('/*FUNCTIONS*/', '\n'.join(function(source, n) for n in names))
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'sync.c'; exe = Path(tmp) / 'sync'
            c.write_text(code)
            build = subprocess.run(compiler_command() + ['-O2', '-UNDEBUG', '-pthread', *includes, str(c), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, LARECOMP_FULL_BARRIERS='1' if mode == 'full' else '0')
            run = subprocess.run([str(exe)] + ([mode] if mode else []), env=env,
                                 capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)

    def barriers(self, mode):
        self.run_native(BARRIERS, (ROOT/'runtime/nvn/vulkan/exec.c').read_text(),
            ('vk_full_barrier', 'vk_scoped_image_barriers', 'image_access', 'transfer_images', 'vk_sample_image'), mode, True)

    def test_image_access_preserves_mips_layers_and_stencil(self): self.barriers('scoped')
    def test_global_barrier_comparison_mode(self): self.barriers('full')

    def test_worker_progress_during_pacing_and_exclusive_presentation(self):
        self.run_native(PACING, (ROOT/'runtime/nvn/vulkan/device.c').read_text(),
            ('present_lock', 'present_unlock', 'present_wait_until', 'backend_represent_now'))

    def test_window_load_after_post_processing_has_own_dependency(self):
        self.run_native(RESUME, (ROOT/'runtime/nvn/vulkan/device.c').read_text(), ('vk_frame_resume',), headers=True)


if __name__ == '__main__': unittest.main(verbosity=2)
