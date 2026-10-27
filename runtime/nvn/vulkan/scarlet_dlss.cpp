// Scarlet Native Runtime - optional NVIDIA NGX / DLSS bridge.
//
// The NGX SDK stays out of the game executable. ScarletPC loads the existing
// MIT bridge DLL only when DLSS is selected, negotiates the bridge-requested
// Vulkan extensions before instance/device creation, and then feeds it the
// native renderer's real Scarlet depth/motion images.
//
// Keep C++ standard headers ahead of vk.h/nvn.h: the C runtime headers expose
// C11 stdatomic compatibility names which can otherwise collide with libstdc++.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "scarlet_dlss.h"
#include "../../rt.h"
#include "../../nvidia/scarlet_dlss_bridge.h"
#include "scarlet_temporal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

struct DlssState {
#ifdef _WIN32
    HMODULE module{};
#endif
    const ScarletDlssApi *api{};
    std::vector<VkExtensionProperties> instance_extensions;
    std::vector<VkExtensionProperties> device_extensions;

    TexBackend output{};
    VkImageView depth_view{};
    VkImageView motion_view{};
    VkImage depth_image{};
    VkImage motion_image{};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkFormat motion_format{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags depth_aspect{};
    uint32_t depth_w{}, depth_h{}, motion_w{}, motion_h{};

    uint32_t input_w{}, input_h{}, output_w{}, output_h{};
    VkFormat input_format{VK_FORMAT_UNDEFINED};
    VkFormat output_format{VK_FORMAT_UNDEFINED};
    int quality{1};

    uint64_t last_temporal_generation{};
    uint64_t last_use_seq{};
    bool load_tried{};
    bool initialized{};
    bool feature_ready{};
    bool reset{true};
    bool failed{};
    std::chrono::steady_clock::time_point last_dispatch{};
};

DlssState g;

bool selected() {
    const char *e = std::getenv("SCARLET_UPSCALER");
    if (!e) return false;
#ifdef _WIN32
    return _stricmp(e, "dlss") == 0;
#else
    return std::strcmp(e, "dlss") == 0;
#endif
}

#ifdef _WIN32
std::filesystem::path exe_dir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return std::filesystem::current_path();
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
}

void bridge_log(int warning, const char *message) {
    LOG("[Scarlet.DLSS] %s%s\n", warning ? "warning: " : "",
        message ? message : "");
}
#endif

bool load_bridge() {
    if (!selected()) return false;
    if (g.api) return true;
    if (g.load_tried) return false;
    g.load_tried = true;
#ifdef _WIN32
    const auto root = exe_dir();
    const auto dll = root / L"ScarletDlssBridge.dll";
    g.module = LoadLibraryW(dll.c_str());
    if (!g.module) {
        LOG("[Scarlet.DLSS] ScarletDlssBridge.dll not found beside ScarletPC.exe\n");
        return false;
    }
    auto get_api = reinterpret_cast<ScarletDlssGetApiFn>(
        GetProcAddress(g.module, "ScarletDlssGetApi"));
    if (!get_api) {
        LOG("[Scarlet.DLSS] bridge has no ScarletDlssGetApi export\n");
        FreeLibrary(g.module);
        g.module = nullptr;
        return false;
    }
    g.api = get_api();
    if (!g.api || g.api->abi != SCARLET_DLSS_BRIDGE_ABI) {
        LOG("[Scarlet.DLSS] bridge ABI mismatch\n");
        g.api = nullptr;
        FreeLibrary(g.module);
        g.module = nullptr;
        return false;
    }

    const auto cache = root / L"user" / L"cache" / L"scarlet" / L"ngx";
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    if (!g.api->Configure(root.c_str(), cache.c_str(), &bridge_log)) {
        LOG("[Scarlet.DLSS] bridge configuration failed\n");
        g.api = nullptr;
        FreeLibrary(g.module);
        g.module = nullptr;
        return false;
    }
    LOG("[Scarlet.DLSS] NGX bridge loaded for native Vulkan\n");
    return true;
#else
    return false;
#endif
}

uint32_t copy_extensions(std::vector<VkExtensionProperties> &dst,
                         uint32_t count, const VkExtensionProperties *src,
                         const char **names, uint32_t capacity) {
    dst.clear();
    if (!src || !count || !names || !capacity) return 0;
    dst.assign(src, src + count);
    uint32_t n = std::min<uint32_t>(count, capacity);
    for (uint32_t i = 0; i < n; ++i) names[i] = dst[i].extensionName;
    if (count > capacity)
        LOG("[Scarlet.DLSS] extension list truncated: %u > %u\n", count, capacity);
    return n;
}

int quality() {
    const char *q = std::getenv("SCARLET_UPSCALER_QUALITY");
    if (!q || !*q || !std::strcmp(q, "quality")) return 1;
    if (!std::strcmp(q, "balanced")) return 2;
    if (!std::strcmp(q, "performance")) return 3;
    if (!std::strcmp(q, "ultra_performance")) return 4;
    if (!std::strcmp(q, "dlaa")) return 0;
    return 1;
}

float frame_time_ms() {
    auto now = std::chrono::steady_clock::now();
    if (g.last_dispatch.time_since_epoch().count() == 0) {
        g.last_dispatch = now;
        int fps = 60;
        if (const char *e = std::getenv("SCARLET_TARGET_FPS"); e && *e)
            fps = std::clamp(std::atoi(e), 30, 240);
        return 1000.0f / static_cast<float>(fps);
    }
    float ms = std::chrono::duration<float, std::milli>(now - g.last_dispatch).count();
    g.last_dispatch = now;
    return std::clamp(ms, 1.0f, 100.0f);
}

VkImageAspectFlags depth_only(VkImageAspectFlags aspect) {
    return (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : aspect;
}

VkImageSubresourceRange range(VkImageAspectFlags aspect) {
    VkImageSubresourceRange r{};
    r.aspectMask = aspect;
    r.baseMipLevel = 0;
    r.levelCount = 1;
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    return r;
}

VkComponentMapping identity_swizzle() {
    VkComponentMapping sw{};
    sw.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    sw.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    sw.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    sw.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    return sw;
}

void image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range(aspect);
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

VkImageView raw_view(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.components = identity_swizzle();
    ci.subresourceRange = range(aspect);
    VkImageView view{};
    if (vkCreateImageView(g_vk.dev, &ci, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

void release_temporal_views() {
    if (g.depth_view) vk_defer_view(g.depth_view);
    if (g.motion_view) vk_defer_view(g.motion_view);
    g.depth_view = g.motion_view = VK_NULL_HANDLE;
    g.depth_image = g.motion_image = VK_NULL_HANDLE;
    g.depth_format = g.motion_format = VK_FORMAT_UNDEFINED;
    g.depth_aspect = 0;
    g.depth_w = g.depth_h = g.motion_w = g.motion_h = 0;
}

void release_output() {
    if (!g.output.image) return;
    vk_views_forget(&g.output);
    vk_defer_image(g.output.image, g.output.mem);
    g.output = {};
}

void release_feature() {
    if (g.last_use_seq > vk_completed_seq()) vk_wait_seq(g.last_use_seq);
    if (g.api && g.feature_ready && g.api->ReleaseFeature) g.api->ReleaseFeature();
    g.feature_ready = false;
    release_temporal_views();
    release_output();
    g.input_w = g.input_h = g.output_w = g.output_h = 0;
    g.input_format = g.output_format = VK_FORMAT_UNDEFINED;
    g.last_temporal_generation = 0;
    g.last_dispatch = {};
    g.reset = true;
}

VkFormat writable_output_format(VkFormat source) {
    if (source == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    if (source == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    return source;
}

bool create_output(uint32_t w, uint32_t h, VkFormat format) {
    release_output();
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &props);
    }
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &g.output.image, &g.output.mem, nullptr) != VK_SUCCESS) {
        g.output = {};
        return false;
    }
    g.output.format = format;
    g.output.base_type = VK_IMAGE_VIEW_TYPE_2D;
    g.output.width = w;
    g.output.height = h;
    g.output.depth = g.output.layers = g.output.levels = g.output.samples = 1;
    g.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    g.output.gpu_owned = true;
    g.output.uploaded = true;
    g.output.sx = g.output.sy = 1.0f;
    vk_image_init_layout(vk_cmd(), g.output.image, VK_IMAGE_ASPECT_COLOR_BIT);
    g.output_format = format;
    return true;
}

bool ensure_temporal_views(const ScarletTemporalImages &t) {
    VkImageAspectFlags da = depth_only(t.depth_aspect);
    if (g.depth_image == t.depth_image && g.motion_image == t.motion_image &&
        g.depth_format == t.depth_format && g.motion_format == t.motion_format &&
        g.depth_aspect == da && g.depth_view && g.motion_view)
        return true;

    release_temporal_views();
    g.depth_view = raw_view(t.depth_image, t.depth_format, da);
    g.motion_view = raw_view(t.motion_image, t.motion_format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!g.depth_view || !g.motion_view) {
        release_temporal_views();
        return false;
    }
    g.depth_image = t.depth_image;
    g.motion_image = t.motion_image;
    g.depth_format = t.depth_format;
    g.motion_format = t.motion_format;
    g.depth_aspect = da;
    g.depth_w = t.depth_width;
    g.depth_h = t.depth_height;
    g.motion_w = t.motion_width;
    g.motion_h = t.motion_height;
    return true;
}

bool ensure_feature(TexBackend *source, uint32_t out_w, uint32_t out_h) {
    const int q = quality();
    if (g.feature_ready && g.input_w == source->width && g.input_h == source->height &&
        g.output_w == out_w && g.output_h == out_h && g.input_format == source->format &&
        g.quality == q)
        return true;

    release_feature();
    if (!create_output(out_w, out_h, writable_output_format(source->format))) return false;

    g.input_w = source->width;
    g.input_h = source->height;
    g.output_w = out_w;
    g.output_h = out_h;
    g.input_format = source->format;
    g.quality = q;

    ScarletDlssFeature feature{};
    feature.input_width = g.input_w;
    feature.input_height = g.input_h;
    feature.output_width = g.output_w;
    feature.output_height = g.output_h;
    feature.quality = q;
    feature.depth_inverted = std::getenv("SCARLET_DEPTH_INVERTED") ? 1 : 0;
    feature.preset = 0;
    feature.hdr = 0;

    if (!g.api->CreateFeature(vk_cmd(), &feature)) {
        LOG("[Scarlet.DLSS] feature creation failed; native presentation kept\n");
        release_output();
        return false;
    }
    g.feature_ready = true;
    g.reset = true;
    LOG("[Scarlet.DLSS] native feature ready %ux%u -> %ux%u quality=%d\n",
        g.input_w, g.input_h, g.output_w, g.output_h, q);
    return true;
}

} // namespace

extern "C" bool scarlet_dlss_requested(void) {
    return selected();
}

extern "C" uint32_t scarlet_dlss_instance_extensions(const char **names, uint32_t capacity) {
    if (!load_bridge()) return 0;
    uint32_t count{};
    const VkExtensionProperties *props{};
    if (!g.api->InstanceExtensions(&count, &props)) return 0;
    return copy_extensions(g.instance_extensions, count, props, names, capacity);
}

extern "C" uint32_t scarlet_dlss_device_extensions(VkInstance instance, VkPhysicalDevice physical,
                                                    const char **names, uint32_t capacity) {
    if (!load_bridge()) return 0;
    uint32_t count{};
    const VkExtensionProperties *props{};
    if (!g.api->DeviceExtensions(instance, physical, &count, &props)) return 0;
    return copy_extensions(g.device_extensions, count, props, names, capacity);
}

extern "C" bool scarlet_dlss_initialize(void) {
    if (!load_bridge() || g.initialized) return g.initialized;
    if (!g.api->Initialize(g_vk.inst, g_vk.phys, g_vk.dev,
                           vkGetInstanceProcAddr, vkGetDeviceProcAddr)) {
        LOG("[Scarlet.DLSS] NGX initialization unavailable on selected GPU/driver\n");
        return false;
    }
    g.initialized = true;
    LOG("[Scarlet.DLSS] NGX initialized on native Scarlet VkDevice\n");
    return true;
}

extern "C" TexBackend *scarlet_dlss_upscale(TexBackend *source) {
    if (!source || !selected() || g.failed || !g.initialized) return source;

    ScarletTemporalImages temporal{};
    if (!scarlet_temporal_get(&temporal)) {
        g.reset = true;
        static unsigned logs;
        if (logs++ < 4) LOG("[Scarlet.DLSS] waiting for native Scarlet depth/motion pair\n");
        return source;
    }

    uint32_t out_w = g_vk.sc_extent.width, out_h = g_vk.sc_extent.height;
    if (!out_w || !out_h) return source;
    // DLSS SR is useful only when the guest renders below the window. DLAA is
    // the explicit exception.
    if (quality() != 0 && source->width >= out_w && source->height >= out_h) return source;

    if (!ensure_feature(source, out_w, out_h) || !ensure_temporal_views(temporal)) return source;

    VkComponentMapping sw = identity_swizzle();
    VkImageView source_view = vk_image_view(source, VK_IMAGE_VIEW_TYPE_2D, source->format,
                                            0, 1, 0, 1, sw, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView output_view = vk_image_view(&g.output, VK_IMAGE_VIEW_TYPE_2D, g.output.format,
                                            0, 1, 0, 1, sw, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!source_view || !output_view) return source;

    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, source->image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cb, temporal.depth_image, depth_only(temporal.depth_aspect),
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cb, temporal.motion_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cb, g.output.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    ScarletDlssEvaluate eval{};
    eval.color = {source->image, source_view, range(VK_IMAGE_ASPECT_COLOR_BIT),
                  source->format, source->width, source->height};
    eval.depth = {temporal.depth_image, g.depth_view, range(depth_only(temporal.depth_aspect)),
                  temporal.depth_format, temporal.depth_width, temporal.depth_height};
    eval.motion = {temporal.motion_image, g.motion_view, range(VK_IMAGE_ASPECT_COLOR_BIT),
                   temporal.motion_format, temporal.motion_width, temporal.motion_height};
    eval.output = {g.output.image, output_view, range(VK_IMAGE_ASPECT_COLOR_BIT),
                   g.output.format, out_w, out_h};
    eval.jitter_x = 0.0f;
    eval.jitter_y = 0.0f;
    eval.reset = (g.reset || g.last_temporal_generation != temporal.generation) ? 1 : 0;
    eval.frame_ms = frame_time_ms();
    eval.sharpness = 0.0f;

    if (!g.api->Evaluate(cb, &eval)) {
        LOG("[Scarlet.DLSS] native evaluation failed; disabling DLSS for this run\n");
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
        LOG("[Scarlet.DLSS] first native true-temporal NGX dispatch recorded\n");
    }
    return &g.output;
}

extern "C" void scarlet_dlss_reset_history(void) {
    g.reset = true;
    g.last_temporal_generation = 0;
}

extern "C" void scarlet_dlss_shutdown(void) {
    if (g.api) {
        release_feature();
        if (g.initialized && g.api->Shutdown) g.api->Shutdown();
    }
    g.initialized = false;
    g.api = nullptr;
#ifdef _WIN32
    if (g.module) {
        FreeLibrary(g.module);
        g.module = nullptr;
    }
#endif
    g.instance_extensions.clear();
    g.device_extensions.clear();
    g.load_tried = false;
    g.failed = false;
}
