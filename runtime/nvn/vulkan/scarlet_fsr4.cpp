// Scarlet Native Runtime - FSR4 v07 compatibility path.
//
// This preserves the existing Scarlet FSR4 provider behaviour without pulling
// video_core/Scheduler/MemoryAllocator into the native runtime. The current
// project path intentionally uses neutral depth/motion compatibility resources;
// unlike native FSR3, it is not advertised as true-temporal until the provider's
// expected R32/RG16 inputs are derived from Scarlet's retained D24S8/RGBA16 pair.
#include "scarlet_fsr4.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <ffx_vk_fsr4_v07.h>
#include <ffx_vk_fsr4_v07_assets.h>

#include "../../rt.h"

namespace {

struct FrameUse {
    uint64_t frame;
    uint64_t seq;
};

struct Fsr4State {
    std::vector<uint8_t> scratch;
    FfxInterface backend{};
    ffxContext context{};
    bool backend_ok{};
    bool context_ok{};
    bool failed{};
    int preset{-1};

    TexBackend color{};
    TexBackend output{};
    TexBackend depth{};
    TexBackend motion{};

    uint32_t input_w{}, input_h{}, output_w{}, output_h{};
    VkFormat source_format{VK_FORMAT_UNDEFINED};
    uint64_t next_frame{1};
    bool reset{true};
    std::deque<FrameUse> in_flight;
};

Fsr4State g;

int quality_preset() {
    const char *q = std::getenv("SCARLET_UPSCALER_QUALITY");
    if (!q || !*q || !std::strcmp(q, "quality")) return 1;
    if (!std::strcmp(q, "balanced")) return 2;
    if (!std::strcmp(q, "performance")) return 3;
    if (!std::strcmp(q, "ultra_performance")) return 4;
    if (!std::strcmp(q, "native") || !std::strcmp(q, "native_aa")) return 0;
    return 1;
}

FfxFsr4ModelPreset model_preset(int p) {
    switch (p) {
    case 0: return FFX_FSR4_MODEL_PRESET_NATIVE_AA;
    case 1: return FFX_FSR4_MODEL_PRESET_QUALITY;
    case 2: return FFX_FSR4_MODEL_PRESET_BALANCED;
    case 3: return FFX_FSR4_MODEL_PRESET_PERFORMANCE;
    default: return FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE;
    }
}

float frame_time_ms() {
    int fps = 60;
    if (const char *e = std::getenv("SCARLET_TARGET_FPS"); e && *e)
        fps = std::clamp(std::atoi(e), 30, 240);
    return 1000.0f / (float)fps;
}

std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (!n) return {};
    path.resize(n);
    return std::filesystem::path(path).parent_path();
#else
    return {};
#endif
}

std::filesystem::path asset_dir() {
    if (const char *e = std::getenv("SCARLET_FSR4_DIR"); e && *e)
        return std::filesystem::u8path(e);
    return executable_dir() / "assets" / "fsr4-v07";
}

bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto end = f.tellg();
    if (end <= 0) return false;
    data.resize((size_t)end);
    f.seekg(0);
    return (bool)f.read((char *)data.data(), (std::streamsize)data.size());
}

void release_image(TexBackend &b) {
    if (!b.image) return;
    vk_views_forget(&b);
    vk_defer_image(b.image, b.mem);
    b = {};
}

bool make_image(TexBackend &b, uint32_t w, uint32_t h, VkFormat format,
                VkImageUsageFlags usage) {
    if (b.image && b.width == w && b.height == h && b.format == format) return true;
    release_image(b);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
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
    b.width = w; b.height = h; b.depth = b.layers = b.levels = b.samples = 1;
    b.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    b.gpu_owned = b.uploaded = true;
    b.sx = b.sy = 1.0f;
    vk_image_init_layout(vk_cmd(), b.image, b.aspect);
    return true;
}

VkImageView view(TexBackend &b) {
    VkComponentMapping sw{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    return vk_image_view(&b, VK_IMAGE_VIEW_TYPE_2D, b.format, 0, 1, 0, 1, sw,
                         VK_IMAGE_ASPECT_COLOR_BIT);
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
             VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void retire_completed() {
    uint64_t done = vk_completed_seq();
    while (!g.in_flight.empty() && g.in_flight.front().seq <= done) {
        if (g.backend_ok) ffxFsr4VkRetireFrame(&g.backend, g.in_flight.front().frame);
        g.in_flight.pop_front();
    }
    while (g.in_flight.size() >= 3) {
        vk_wait_seq(g.in_flight.front().seq);
        if (g.backend_ok) ffxFsr4VkRetireFrame(&g.backend, g.in_flight.front().frame);
        g.in_flight.pop_front();
    }
}

void destroy_provider() {
    if (!g.in_flight.empty()) {
        vk_wait_seq(g.in_flight.back().seq);
        if (g.backend_ok) ffxFsr4VkRetireFrame(&g.backend, g.in_flight.back().frame);
        g.in_flight.clear();
    }
    if (g.context_ok) {
        ffxFsr4V07DestroyContext(&g.context, nullptr);
        g.context = nullptr;
        g.context_ok = false;
    }
    if (g.backend_ok) {
        ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext *>(g.scratch.data()));
        g.backend = {};
        g.backend_ok = false;
    }
    g.scratch.clear();
}

void release_resources() {
    release_image(g.color);
    release_image(g.output);
    release_image(g.depth);
    release_image(g.motion);
    g.input_w = g.input_h = g.output_w = g.output_h = 0;
    g.source_format = VK_FORMAT_UNDEFINED;
    g.reset = true;
}

bool create_provider(uint32_t out_w, uint32_t out_h, int wanted) {
    destroy_provider();

    FfxFsr4V07AssetSet assets{};
    if (!ffxFsr4V07BuildAssetSet(model_preset(wanted), out_w, out_h, &assets)) {
        LOG("[Scarlet.FSR4] output %ux%u unsupported by v07 model\n", out_w, out_h);
        return false;
    }

    const auto dir = asset_dir();
    std::array<std::vector<uint8_t>, FFX_FSR4_VK_PASS_COUNT> code;
    std::vector<uint8_t> initializer, weights;
    auto load = [&](const char *name, std::vector<uint8_t> &bytes) {
        auto path = dir / name;
        if (read_file(path, bytes)) return true;
        LOG("[Scarlet.FSR4] missing asset %s\n", path.u8string().c_str());
        return false;
    };

    if (!load(assets.pre, code[0])) return false;
    for (uint32_t pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass)
        if (!load(assets.model[pass], code[1 + pass])) return false;
    if (!load(assets.post, code[13]) || !load(assets.rcas, code[14]) ||
        !load(assets.spdAutoExposure, code[15]) ||
        !load(assets.initializer, initializer) || !load(assets.prePassWeights, weights))
        return false;
    if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
        weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES)
        return false;

    static const auto entries = [] {
        std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names{};
        names.fill("main");
        for (uint32_t p = 1; p <= FFX_FSR4_MODEL_PASS_COUNT; ++p)
            names[p] = "fsr4_model_v07_i8_pass" + std::to_string(p);
        return names;
    }();

    FfxFsr4VkCreateInfo ci{};
    ci.device = g_vk.dev;
    ci.physicalDevice = g_vk.phys;
    for (uint32_t i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
        if (code[i].empty() || code[i].size() % sizeof(uint32_t)) return false;
        ci.shaders[i] = {reinterpret_cast<const uint32_t *>(code[i].data()),
                         code[i].size(), entries[i].c_str()};
    }
    ci.modelInitializer = initializer.data();
    ci.modelInitializerSize = initializer.size();
    ci.prePassWeights = weights.data();
    ci.prePassWeightsSize = weights.size();
    g.scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
    ci.scratchBuffer = g.scratch.data();
    ci.scratchBufferSize = g.scratch.size();

    VkResult vr = ffxFsr4VkCreateContext(&ci, &g.backend);
    if (vr != VK_SUCCESS) {
        LOG("[Scarlet.FSR4] Vulkan provider creation failed %d\n", (int)vr);
        g.scratch.clear();
        return false;
    }
    g.backend_ok = true;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = {(out_w + 7u) & ~7u, (out_h + 7u) & ~7u};
    desc.maxUpscaleSize = desc.maxRenderSize;

    ffxFsr4V07SetBackendInterface(&g.backend);
    ffxReturnCode_t rc = ffxFsr4V07CreateContext(&g.context, &desc.header, nullptr);
    ffxFsr4V07SetBackendInterface(nullptr);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[Scarlet.FSR4] context creation failed %d\n", (int)rc);
        destroy_provider();
        return false;
    }
    g.context_ok = true;
    g.preset = wanted;
    LOG("[Scarlet.FSR4] native v07 INT8 provider ready model=%s tier=%s output=%ux%u\n",
        ffxFsr4ModelPresetName(model_preset(wanted)),
        assets.tier, out_w, out_h);
    return true;
}

bool ensure(TexBackend *source, uint32_t out_w, uint32_t out_h) {
    if (!source || !source->image || !source->width || !source->height || !out_w || !out_h)
        return false;

    int wanted = quality_preset();
    if (!g.context_ok || g.preset != wanted || g.output_w != out_w || g.output_h != out_h) {
        if (!create_provider(out_w, out_h, wanted)) return false;
        release_resources();
    }

    if (g.color.image && g.input_w == source->width && g.input_h == source->height &&
        g.output_w == out_w && g.output_h == out_h && g.source_format == source->format)
        return true;

    release_resources();
    g.input_w = source->width; g.input_h = source->height;
    g.output_w = out_w; g.output_h = out_h; g.source_format = source->format;

    VkFormatProperties sp{}, cp{};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, source->format, &sp);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R16G16B16A16_SFLOAT, &cp);
    if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(cp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !(cp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !(cp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return false;

    const VkImageUsageFlags io = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_image(g.color, g.input_w, g.input_h, VK_FORMAT_R16G16B16A16_SFLOAT, io) ||
        !make_image(g.output, out_w, out_h, VK_FORMAT_R16G16B16A16_SFLOAT, io) ||
        !make_image(g.depth, g.input_w, g.input_h, VK_FORMAT_R32_SFLOAT, io) ||
        !make_image(g.motion, g.input_w, g.input_h, VK_FORMAT_R16G16_SFLOAT, io))
        return false;

    VkCommandBuffer cb = vk_cmd();
    VkClearColorValue zero{};
    VkClearColorValue far_depth{};
    far_depth.float32[0] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g.depth.image, VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1, &range);
    vkCmdClearColorImage(cb, g.motion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    g.reset = true;
    LOG("[Scarlet.FSR4] native compatibility inputs %ux%u -> %ux%u\n",
        g.input_w, g.input_h, out_w, out_h);
    return true;
}

FfxApiResource resource(VkImageView v, uint32_t fmt, uint32_t w, uint32_t h, uint32_t state) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void *>(v);
    r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.state = state;
    return r;
}

bool register_image(VkImage image, VkImageView v, VkPipelineStageFlags stage,
                    VkAccessFlags access, VkPipelineStageFlags restore_stage,
                    VkAccessFlags restore_access) {
    FfxFsr4VkExternalImageState s{};
    s.structSize = sizeof(s);
    s.image = image;
    s.view = v;
    s.layout = VK_IMAGE_LAYOUT_GENERAL;
    s.stageMask = stage;
    s.accessMask = access;
    s.restoreLayout = VK_IMAGE_LAYOUT_GENERAL;
    s.restoreStageMask = restore_stage;
    s.restoreAccessMask = restore_access;
    return ffxFsr4VkSetExternalImageState(&g.backend, &s) == VK_SUCCESS;
}

} // namespace

extern "C" bool scarlet_fsr4_requested(void) {
    const char *e = std::getenv("SCARLET_UPSCALER");
    return e && !std::strcmp(e, "fsr4");
}

extern "C" TexBackend *scarlet_fsr4_upscale(TexBackend *source) {
    if (!scarlet_fsr4_requested() || g.failed || !source) return source;

    uint32_t out_w = g_vk.sc_extent.width, out_h = g_vk.sc_extent.height;
    if (!ensure(source, out_w, out_h)) {
        static unsigned logs;
        if (logs++ < 4) LOG("[Scarlet.FSR4] native provider not ready; using source frame\n");
        return source;
    }

    retire_completed();
    VkCommandBuffer cb = vk_cmd();

    barrier(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, g.color.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t)source->width, (int32_t)source->height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t)g.input_w, (int32_t)g.input_h, 1};
    vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   g.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    barrier(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(cb, g.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    uint64_t frame = g.next_frame++;
    if (ffxFsr4VkBeginFrame(&g.backend, frame) != VK_SUCCESS) {
        g.failed = true;
        return source;
    }

    VkImageView cv = view(g.color), dv = view(g.depth), mv = view(g.motion), ov = view(g.output);
    constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
    if (!register_image(g.color.image, cv, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) ||
        !register_image(g.depth.image, dv, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) ||
        !register_image(g.motion.image, mv, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) ||
        !register_image(g.output.image, ov, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, read)) {
        g.failed = true;
        return source;
    }

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = reinterpret_cast<void *>(cb);
    d.color = resource(cv, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                       g.input_w, g.input_h, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(dv, FFX_SURFACE_FORMAT_R32_FLOAT,
                       g.input_w, g.input_h, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(mv, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                               g.input_w, g.input_h, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(ov, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                        g.output_w, g.output_h, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {0.0f, 0.0f};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {g.input_w, g.input_h};
    d.upscaleSize = {g.output_w, g.output_h};
    d.enableSharpening = false;
    d.enableAutoExposure = true;
    d.sharpness = 0.0f;
    d.frameTimeDelta = frame_time_ms();
    d.preExposure = 1.0f;
    d.reset = g.reset;
    d.cameraNear = 0.1f;
    d.cameraFar = 1000.0f;
    d.cameraFovAngleVertical = 1.0471975512f;
    d.viewSpaceToMetersFactor = 1.0f;

    ffxFsr4V07SetBackendInterface(&g.backend);
    ffxReturnCode_t rc = ffxFsr4V07Dispatch(&g.context, &d.header);
    ffxFsr4V07SetBackendInterface(nullptr);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[Scarlet.FSR4] native dispatch failed %d\n", (int)rc);
        g.failed = true;
        return source;
    }

    g.in_flight.push_back({frame, vk_pending_seq()});
    g.reset = false;
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[Scarlet.FSR4] first native compatibility dispatch recorded\n");
    }
    return &g.output;
}

extern "C" void scarlet_fsr4_reset_history(void) {
    g.reset = true;
}

extern "C" void scarlet_fsr4_shutdown(void) {
    destroy_provider();
    release_resources();
    g.failed = false;
    g.preset = -1;
    g.next_frame = 1;
}
