// Scarlet Native Runtime - FireBurn/FSR-Vulkan temporal upscaler.
//
// This is deliberately renderer-owned: no suyu Scheduler, MemoryAllocator,
// TextureCache, or video_core object participates. FSR records compute commands
// into the same ordered Vulkan command buffer used by the native render thread.
// C++ <atomic> must be parsed before vk.h/nvn.h can expose C11 stdatomic
// compatibility macros through the renderer headers (notably on MinGW/Clang).
#include <atomic>

#include "scarlet_fsr3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <ffx_vk_portable.h>

#include "../../rt.h"
#include "scarlet_temporal.h"

namespace {

constexpr VkFormat kInputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct FsrState {
    FfxVkPortableUpscaleContext *context{};
    FfxVkPortableUpscaleCreateInfo create{};
    TexBackend normalize{};
    TexBackend output{};
    uint32_t input_w{}, input_h{}, output_w{}, output_h{};
    VkFormat source_format{VK_FORMAT_UNDEFINED};
    uint64_t last_temporal_generation{};
    uint64_t frame_id{};
    uint64_t last_use_seq{};
    bool reset{true};
    bool failed{};
    std::chrono::steady_clock::time_point last_dispatch{};
};

FsrState g;

bool env_true(const char *name, bool fallback) {
    const char *e = std::getenv(name);
    if (!e || !*e) return fallback;
    return std::strcmp(e, "0") != 0 && std::strcmp(e, "false") != 0;
}

float env_float(const char *name, float fallback) {
    const char *e = std::getenv(name);
    if (!e || !*e) return fallback;
    char *end = nullptr;
    float v = std::strtof(e, &end);
    return end != e && std::isfinite(v) && v != 0.0f ? v : fallback;
}

bool supported_color(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

FfxVkPortableImage portable_image(VkImage image, VkFormat format, uint32_t w, uint32_t h,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                  FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = format;
    out.extent = {w, h};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = usage;
    out.aspect = aspect;
    out.state = state;
    return out;
}

void log_issues(const char *stage, uint64_t issues) {
    if (!issues) return;
    for (uint32_t bit = 0; bit < 64; ++bit) {
        uint64_t flag = UINT64_C(1) << bit;
        if (!(issues & flag)) continue;
        const char *name = ffxVkPortableValidationIssueName(flag);
        LOG("[Scarlet.FSR3] %s validation: %s (0x%llx)\n",
            stage, name ? name : "unknown", (unsigned long long)flag);
    }
}

void image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                   VkImageLayout old_layout = VK_IMAGE_LAYOUT_GENERAL,
                   VkImageLayout new_layout = VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

void release_image(TexBackend &b) {
    if (!b.image) return;
    vk_views_forget(&b);
    vk_defer_image(b.image, b.mem);
    b = {};
}

bool create_image(TexBackend &b, uint32_t w, uint32_t h, VkFormat format,
                  VkImageUsageFlags usage) {
    if (b.image && b.width == w && b.height == h && b.format == format) return true;
    release_image(b);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b.image, &b.mem, nullptr) != VK_SUCCESS) {
        b = {};
        return false;
    }
    b.format = format;
    b.base_type = VK_IMAGE_VIEW_TYPE_2D;
    b.width = w;
    b.height = h;
    b.depth = b.layers = b.levels = b.samples = 1;
    b.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    b.gpu_owned = true;
    b.uploaded = true;
    b.sx = b.sy = 1.0f;
    vk_image_init_layout(vk_cmd(), b.image, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

float frame_time_ms() {
    auto now = std::chrono::steady_clock::now();
    if (g.last_dispatch.time_since_epoch().count() == 0) {
        g.last_dispatch = now;
        int fps = 60;
        if (const char *e = std::getenv("SCARLET_TARGET_FPS"); e && *e)
            fps = std::clamp(std::atoi(e), 30, 240);
        return 1000.0f / (float)fps;
    }
    float ms = std::chrono::duration<float, std::milli>(now - g.last_dispatch).count();
    g.last_dispatch = now;
    return std::clamp(ms, 1.0f, 100.0f);
}

void destroy_context() {
    if (g.context) {
        if (g.last_use_seq > vk_completed_seq())
            vk_wait_seq(g.last_use_seq);
        ffxVkPortableUpscaleContextDestroy(g.context);
        g.context = nullptr;
    }
    release_image(g.normalize);
    release_image(g.output);
    g.create = {};
    g.input_w = g.input_h = g.output_w = g.output_h = 0;
    g.source_format = VK_FORMAT_UNDEFINED;
    g.last_temporal_generation = 0;
    g.frame_id = 0;
    g.last_dispatch = {};
    g.reset = true;
}

bool ensure_context(TexBackend *source, uint32_t out_w, uint32_t out_h) {
    if (!source || !source->image || !source->width || !source->height || !out_w || !out_h)
        return false;

    // Temporal upscaling needs an actual lower-resolution render. Never spend
    // compute time on a 1:1 pass and never mask a failed render-scale setup.
    if (source->width >= out_w || source->height >= out_h)
        return false;

    if (g.context && g.input_w == source->width && g.input_h == source->height &&
        g.output_w == out_w && g.output_h == out_h && g.source_format == source->format)
        return true;

    destroy_context();
    g.input_w = source->width;
    g.input_h = source->height;
    g.output_w = out_w;
    g.output_h = out_h;
    g.source_format = source->format;

    if (!create_image(g.output, out_w, out_h, kOutputFormat,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return false;

    if (!supported_color(source->format) &&
        !create_image(g.normalize, source->width, source->height, kInputFormat,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return false;

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(g_vk.phys, &features);

    FfxVkPortableDeviceInfo device{};
    device.structSize = sizeof(device);
    device.instance = g_vk.inst;
    device.physicalDevice = g_vk.phys;
    device.device = g_vk.dev;
    device.getDeviceProcAddr = vkGetDeviceProcAddr;
    device.queue = g_vk.queue;
    device.queueFamilyIndex = g_vk.qfam;
    // The native device does not request optional fp16/subgroup-size-control
    // features today; tell FidelityFX the truth instead of advertising physical
    // support that was not enabled at vkCreateDevice.
    device.shaderFloat16Enabled = VK_FALSE;
    device.subgroupSizeControlEnabled = VK_FALSE;
    device.computeFullSubgroupsEnabled = VK_FALSE;
    device.synchronization2Enabled = VK_TRUE;
    device.shaderStorageImageWriteWithoutFormatEnabled =
        features.shaderStorageImageWriteWithoutFormat ? VK_TRUE : VK_FALSE;

    g.create.structSize = sizeof(g.create);
    g.create.flags = FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    g.create.maxRenderSize = {g.input_w, g.input_h};
    g.create.maxOutputSize = {out_w, out_h};

    uint64_t issues = ffxVkPortableValidateUpscaleCreateInfo(&g.create);
    if (issues) {
        log_issues("create", issues);
        g.failed = true;
        return false;
    }

    FfxVkPortableResult rc = ffxVkPortableUpscaleContextCreate(&device, &g.create, &g.context);
    if (rc != FFX_VK_PORTABLE_OK || !g.context) {
        LOG("[Scarlet.FSR3] native context creation failed rc=%d\n", (int)rc);
        g.failed = true;
        return false;
    }

    g.reset = true;
    LOG("[Scarlet.FSR3] native portable temporal upscaler ready %ux%u -> %ux%u\n",
        g.input_w, g.input_h, g.output_w, g.output_h);
    return true;
}

bool normalize_color(VkCommandBuffer cb, TexBackend *source, VkImage *image, VkFormat *format) {
    *image = source->image;
    *format = source->format;
    if (supported_color(source->format)) {
        image_barrier(cb, source->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return true;
    }

    if (!g.normalize.image) return false;

    image_barrier(cb, source->image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(cb, g.normalize.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t)source->width, (int32_t)source->height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t)source->width, (int32_t)source->height, 1};
    vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   g.normalize.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    image_barrier(cb, source->image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(cb, g.normalize.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    *image = g.normalize.image;
    *format = kInputFormat;
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[Scarlet.FSR3] normalizing unsupported native scanout format %d to RGBA8\n",
            (int)source->format);
    }
    return true;
}

} // namespace

extern "C" bool scarlet_fsr3_requested(void) {
    const char *e = std::getenv("SCARLET_UPSCALER");
    return e && (!std::strcmp(e, "fsr3") || !std::strcmp(e, "fsr31"));
}

extern "C" TexBackend *scarlet_fsr3_upscale(TexBackend *source) {
    if (!scarlet_fsr3_requested() || g.failed || !source) return source;
    if (!env_true("SCARLET_FSR_TRUE_TEMPORAL_INPUTS", true)) {
        static bool logged;
        if (!logged) {
            logged = true;
            LOG("[Scarlet.FSR3] native path requires true temporal inputs; disabled by configuration\n");
        }
        return source;
    }

    ScarletTemporalImages temporal{};
    if (!scarlet_temporal_get(&temporal)) {
        g.reset = true;
        static unsigned logs;
        if (logs++ < 4)
            LOG("[Scarlet.FSR3] waiting for native Scarlet depth/motion pair\n");
        return source;
    }

    uint32_t out_w = g_vk.sc_extent.width;
    uint32_t out_h = g_vk.sc_extent.height;
    const char *ew = std::getenv("SCARLET_WINDOW_WIDTH");
    const char *eh = std::getenv("SCARLET_WINDOW_HEIGHT");
    if (ew && *ew && std::atoi(ew) > 0) out_w = (uint32_t)std::atoi(ew);
    if (eh && *eh && std::atoi(eh) > 0) out_h = (uint32_t)std::atoi(eh);
    // The output resource is consumed by this window's present pass; never make
    // it larger than the actual swapchain layer.
    out_w = std::min(out_w, g_vk.sc_extent.width);
    out_h = std::min(out_h, g_vk.sc_extent.height);

    if (!ensure_context(source, out_w, out_h)) return source;

    VkCommandBuffer cb = vk_cmd();
    VkImage color_image{};
    VkFormat color_format{};
    if (!normalize_color(cb, source, &color_image, &color_format)) return source;

    image_barrier(cb, temporal.depth_image, temporal.depth_aspect,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cb, temporal.motion_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cb, g.output.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    FfxVkPortableUpscaleDispatchInfo d{};
    d.structSize = sizeof(d);
    d.commandBuffer = cb;
    d.color = portable_image(color_image, color_format, source->width, source->height,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    d.depth = portable_image(temporal.depth_image, temporal.depth_format,
                             temporal.depth_width, temporal.depth_height,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             temporal.depth_aspect, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    d.motionVectors = portable_image(temporal.motion_image, temporal.motion_format,
                                     temporal.motion_width, temporal.motion_height,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    d.output = portable_image(g.output.image, kOutputFormat, out_w, out_h,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    d.exposure.structSize = sizeof(d.exposure);
    d.reactiveMask.structSize = sizeof(d.reactiveMask);
    d.transparencyAndCompositionMask.structSize = sizeof(d.transparencyAndCompositionMask);
    d.jitterOffset = {0.0f, 0.0f};

    const bool rgba16_motion = temporal.motion_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    d.motionVectorScale = {
        env_float("SCARLET_FSR_MOTION_SCALE_X", rgba16_motion ? 1.0f : (float)source->width),
        env_float("SCARLET_FSR_MOTION_SCALE_Y", rgba16_motion ? 1.0f : (float)source->height)
    };
    d.renderSize = {source->width, source->height};
    d.outputSize = {out_w, out_h};
    d.frameTimeMilliseconds = frame_time_ms();
    d.preExposure = 1.0f;
    d.cameraNear = 0.1f;
    d.cameraFar = 1000.0f;
    d.cameraVerticalFovRadians = 1.0471975512f;
    d.viewSpaceToMeters = 1.0f;
    d.sharpness = 0.0f;
    d.enableSharpening = VK_FALSE;
    if (g.last_temporal_generation != temporal.generation) g.reset = true;
    d.reset = g.reset ? VK_TRUE : VK_FALSE;
    d.frameId = g.frame_id++;

    uint64_t issues = ffxVkPortableValidateUpscaleDispatchInfo(&g.create, &d);
    if (issues) {
        log_issues("dispatch", issues);
        g.failed = true;
        return source;
    }

    FfxVkPortableResult rc = ffxVkPortableUpscaleContextRecordDispatch(g.context, &d);
    if (rc != FFX_VK_PORTABLE_OK) {
        LOG("[Scarlet.FSR3] native dispatch failed rc=%d; disabling for this run\n", (int)rc);
        g.failed = true;
        return source;
    }

    image_barrier(cb, g.output.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    g.last_temporal_generation = temporal.generation;
    g.last_use_seq = vk_pending_seq();
    g.reset = false;

    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[Scarlet.FSR3] first native true-temporal dispatch recorded; motion=%s scale=%.3fx%.3f\n",
            rgba16_motion ? "RGBA16F/pixel" : "generic/uv",
            d.motionVectorScale.x, d.motionVectorScale.y);
    }
    return &g.output;
}

extern "C" void scarlet_fsr3_reset_history(void) {
    g.reset = true;
    g.last_temporal_generation = 0;
}

extern "C" void scarlet_fsr3_shutdown(void) {
    destroy_context();
    g.failed = false;
}
