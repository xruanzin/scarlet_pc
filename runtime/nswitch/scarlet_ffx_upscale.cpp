// SPDX-License-Identifier: MIT
// Native FidelityFX temporal upscaling for Scarlet PC.
// Uses the official AMD FidelityFX Vulkan runtime, the same runtime family used
// by Zelda-on-PC for its FSR 3 path.

#include "video_core/renderer_vulkan/scarlet_ffx_upscale.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

using CreateFn = PfnFfxCreateContext;
using DestroyFn = PfnFfxDestroyContext;
using DispatchFn = PfnFfxDispatch;

bool FfxUpscaleRequested() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value &&
           (std::strcmp(value, "fsr3") == 0 || std::strcmp(value, "fsr31") == 0);
}

const char* SelectedFsrName() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value && std::strcmp(value, "fsr31") == 0 ? "FSR 3.1" : "FSR 3";
}

float FrameTimeMs() {
    int fps = 60;
    if (const char* value = std::getenv("SCARLET_TARGET_FPS"); value && *value) {
        fps = std::clamp(std::atoi(value), 30, 240);
    }
    return 1000.0f / static_cast<float>(fps);
}

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size);
    return std::filesystem::path(path).parent_path();
#else
    return {};
#endif
}

FfxApiResource Resource(VkImage image, VkFormat format, std::uint32_t width,
                        std::uint32_t height, std::uint32_t state, bool uav) {
    FfxApiResource result{};
    result.resource = reinterpret_cast<void*>(image);
    result.state = state;
    result.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = ffxApiGetSurfaceFormatVK(format);
    result.description.width = width;
    result.description.height = height;
    result.description.depth = 1;
    result.description.mipCount = 1;
    result.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    result.description.usage =
        FFX_API_RESOURCE_USAGE_READ_ONLY | (uav ? FFX_API_RESOURCE_USAGE_UAV : 0);
    return result;
}

VkImageSubresourceRange ColorRange() {
    return {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

void GeneralBarrier(vk::CommandBuffer cmdbuf, VkImage image, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                    VkPipelineStageFlags dst_stage) {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = ColorRange(),
    };
    cmdbuf.PipelineBarrier(src_stage, dst_stage, 0, barrier);
}

} // namespace

struct ScarletFfxUpscalePass::Impl {
#ifdef _WIN32
    HMODULE module{};
#endif
    CreateFn create{};
    DestroyFn destroy{};
    DispatchFn dispatch{};
    ffxContext context{};
    // FidelityFX keeps pointers from the create descriptor chain for the lifetime
    // of the context. Keep the descriptors in Impl rather than on EnsureResources'
    // stack so the backend chain never dangles after creation returns.
    ffxCreateBackendVKDesc backend_desc{};
    ffxCreateContextDescUpscale upscale_desc{};
    bool loaded{};

    bool Load() {
        if (loaded) {
            return true;
        }
#ifdef _WIN32
        const auto path = ExecutableDirectory() / L"amd_fidelityfx_vk.dll";
        module = LoadLibraryW(path.c_str());
        if (!module) {
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] amd_fidelityfx_vk.dll not found");
            return false;
        }
#define SCARLET_FFX_LOAD(member, name)                                                     \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name));         \
        if (!member) {                                                                      \
            LOG_WARNING(Render_Vulkan, "[Scarlet.FSR3] FidelityFX export missing: {}", name); \
            return false;                                                                   \
        }
        SCARLET_FFX_LOAD(create, "ffxCreateContext");
        SCARLET_FFX_LOAD(destroy, "ffxDestroyContext");
        SCARLET_FFX_LOAD(dispatch, "ffxDispatch");
#undef SCARLET_FFX_LOAD
        loaded = true;
        return true;
#else
        return false;
#endif
    }

    void DestroyContext() {
        if (context && destroy) {
            ffxContext local = context;
            destroy(&local, nullptr);
            context = nullptr;
        }
        backend_desc = {};
        upscale_desc = {};
    }

    ~Impl() {
        DestroyContext();
#ifdef _WIN32
        if (module) {
            FreeLibrary(module);
        }
#endif
    }
};

ScarletFfxUpscalePass::ScarletFfxUpscalePass(const Device&, MemoryAllocator& allocator_,
                                             std::size_t image_count_,
                                             VkExtent2D output_extent_)
    : allocator{allocator_}, image_count{image_count_}, output_extent{output_extent_},
      impl{std::make_unique<Impl>()} {
    ready = FfxUpscaleRequested() && impl->Load();
}

ScarletFfxUpscalePass::~ScarletFfxUpscalePass() = default;

bool ScarletFfxUpscalePass::Ready() const {
    return ready && !dispatch_failed;
}

void ScarletFfxUpscalePass::ReleaseResources(Scheduler& scheduler) {
    scheduler.Finish();
    impl->DestroyContext();
    images.clear();
    input_extent = {};
    source_format = VK_FORMAT_UNDEFINED;
    reset_pending = true;
}

bool ScarletFfxUpscalePass::EnsureResources(const Device& device, Scheduler& scheduler,
                                            VkExtent2D wanted_input, VkFormat format) {
    if (!Ready() || wanted_input.width == 0 || wanted_input.height == 0 ||
        output_extent.width == 0 || output_extent.height == 0) {
        return false;
    }
    if (impl->context && !images.empty() &&
        input_extent.width == wanted_input.width &&
        input_extent.height == wanted_input.height && source_format == format) {
        return true;
    }

    ReleaseResources(scheduler);
    input_extent = wanted_input;
    source_format = format;
    images.resize(image_count);

    for (auto& set : images) {
        set.output = CreateWrappedImage(allocator, output_extent, source_format);
        set.output_view = CreateWrappedImageView(device, set.output, source_format);
        set.depth = CreateWrappedImage(allocator, input_extent, VK_FORMAT_R32_SFLOAT);
        set.depth_view = CreateWrappedImageView(device, set.depth, VK_FORMAT_R32_SFLOAT);
        set.motion = CreateWrappedImage(allocator, input_extent, VK_FORMAT_R16G16_SFLOAT);
        set.motion_view = CreateWrappedImageView(device, set.motion, VK_FORMAT_R16G16_SFLOAT);
    }

    scheduler.Record([this](vk::CommandBuffer cmdbuf) {
        const auto range = ColorRange();
        const std::array ranges{range};
        VkClearColorValue depth{};
        depth.float32[0] = 1.0f;
        VkClearColorValue zero{};

        for (auto& set : images) {
            TransitionImageLayout(cmdbuf, *set.output, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionImageLayout(cmdbuf, *set.depth, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionImageLayout(cmdbuf, *set.motion, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            cmdbuf.ClearColorImage(*set.depth, VK_IMAGE_LAYOUT_GENERAL, depth, ranges);
            cmdbuf.ClearColorImage(*set.motion, VK_IMAGE_LAYOUT_GENERAL, zero, ranges);
        }
    });
    scheduler.Finish();

    impl->backend_desc = {};
    impl->backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    impl->backend_desc.vkDevice = *device.GetLogical();
    impl->backend_desc.vkPhysicalDevice = device.GetPhysical();
    impl->backend_desc.vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;

    impl->upscale_desc = {};
    impl->upscale_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    impl->upscale_desc.header.pNext = &impl->backend_desc.header;
    impl->upscale_desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    impl->upscale_desc.maxRenderSize = {input_extent.width, input_extent.height};
    impl->upscale_desc.maxUpscaleSize = {output_extent.width, output_extent.height};
    impl->upscale_desc.fpMessage = nullptr;

    ffxContext context{};
    const auto create_rc =
        impl->create(&context, &impl->upscale_desc.header, nullptr);
    if (create_rc != FFX_API_RETURN_OK) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR3] {} context creation failed (rc={})",
                    SelectedFsrName(), create_rc);
        dispatch_failed = true;
        return false;
    }
    impl->context = context;
    reset_pending = true;
    LOG_INFO(Render_Vulkan,
             "[Scarlet.FSR3] {} temporal upscaler ready {}x{} -> {}x{}",
             SelectedFsrName(), input_extent.width, input_extent.height,
             output_extent.width, output_extent.height);
    return true;
}

VkImageView ScarletFfxUpscalePass::Draw(const Device& device, Scheduler& scheduler,
                                        std::size_t image_index, VkImage source_image,
                                        VkImageView source_view, VkFormat format,
                                        VkExtent2D wanted_input) {
    if (!EnsureResources(device, scheduler, wanted_input, format) ||
        image_index >= images.size()) {
        return source_view;
    }

    auto& set = images[image_index];
    const bool reset = reset_pending;
    reset_pending = false;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, source_image, image_index, reset](vk::CommandBuffer cmdbuf) {
        auto& local = images[image_index];

        GeneralBarrier(cmdbuf, source_image,
                       VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = reinterpret_cast<void*>(*cmdbuf);
        dispatch.color = Resource(source_image, source_format, input_extent.width,
                                  input_extent.height, FFX_API_RESOURCE_STATE_COMPUTE_READ,
                                  false);
        dispatch.depth = Resource(*local.depth, VK_FORMAT_R32_SFLOAT, input_extent.width,
                                  input_extent.height, FFX_API_RESOURCE_STATE_COMPUTE_READ,
                                  false);
        dispatch.motionVectors =
            Resource(*local.motion, VK_FORMAT_R16G16_SFLOAT, input_extent.width,
                     input_extent.height, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.output =
            Resource(*local.output, source_format, output_extent.width, output_extent.height,
                     FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
        dispatch.jitterOffset = {0.0f, 0.0f};
        dispatch.motionVectorScale = {static_cast<float>(input_extent.width),
                                      static_cast<float>(input_extent.height)};
        dispatch.renderSize = {input_extent.width, input_extent.height};
        dispatch.upscaleSize = {output_extent.width, output_extent.height};
        dispatch.enableSharpening = false;
        dispatch.sharpness = 0.0f;
        dispatch.frameTimeDelta = FrameTimeMs();
        dispatch.preExposure = 1.0f;
        dispatch.reset = reset;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.0471975512f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = 0;

        ffxContext context = impl->context;
        if (impl->dispatch(&context, &dispatch.header) != FFX_API_RETURN_OK) {
            dispatch_failed = true;
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] {} dispatch failed; temporal upscaler disabled",
                        SelectedFsrName());
            return;
        }

        GeneralBarrier(cmdbuf, *local.output, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });

    return *set.output_view;
}

} // namespace Vulkan
