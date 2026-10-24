// SPDX-License-Identifier: GPL-2.0-or-later
// Scarlet PC FSR 4 v07 Vulkan integration.
// Lifecycle/dispatch structure adapted from DestroxIam/gow3_pc-nvidia vk_fsr4.cpp.
// The provider itself comes from FireBurn/FSR-Vulkan at the pinned revision.

#include "video_core/renderer_vulkan/scarlet_fsr4_pass.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <ffx_vk_fsr4_v07.h>
#include <ffx_vk_fsr4_v07_assets.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

constexpr VkFormat kTemporalColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;

bool Fsr4Requested() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value && std::strcmp(value, "fsr4") == 0;
}

int QualityPreset() {
    const char* quality = std::getenv("SCARLET_UPSCALER_QUALITY");
    if (!quality || !*quality || std::strcmp(quality, "quality") == 0) {
        return 1;
    }
    if (std::strcmp(quality, "balanced") == 0) {
        return 2;
    }
    if (std::strcmp(quality, "performance") == 0) {
        return 3;
    }
    if (std::strcmp(quality, "ultra_performance") == 0) {
        return 4;
    }
    return 1;
}

FfxFsr4ModelPreset ModelPreset(int preset) {
    switch (preset) {
    case 0:
        return FFX_FSR4_MODEL_PRESET_NATIVE_AA;
    case 1:
        return FFX_FSR4_MODEL_PRESET_QUALITY;
    case 2:
        return FFX_FSR4_MODEL_PRESET_BALANCED;
    case 3:
        return FFX_FSR4_MODEL_PRESET_PERFORMANCE;
    default:
        return FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE;
    }
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
    if (!size) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path).parent_path();
#else
    return {};
#endif
}

std::filesystem::path AssetDirectory() {
    if (const char* value = std::getenv("SCARLET_FSR4_DIR"); value && *value) {
        return std::filesystem::u8path(value);
    }
    return ExecutableDirectory() / "assets" / "fsr4-v07";
}

bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const auto end = file.tellg();
    if (end <= 0) {
        return false;
    }
    data.resize(static_cast<std::size_t>(end));
    file.seekg(0);
    return static_cast<bool>(
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

VkImageSubresourceRange ColorRange() {
    return VkImageSubresourceRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

void ImageBarrier(vk::CommandBuffer cmdbuf, VkImage image, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = ColorRange(),
    };
    cmdbuf.PipelineBarrier(src_stage, dst_stage, 0, barrier);
}

VkImageBlit FullBlit(VkExtent2D source, VkExtent2D destination) {
    return VkImageBlit{
        .srcSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .srcOffsets =
            {
                {0, 0, 0},
                {static_cast<s32>(source.width), static_cast<s32>(source.height), 1},
            },
        .dstSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .dstOffsets =
            {
                {0, 0, 0},
                {static_cast<s32>(destination.width), static_cast<s32>(destination.height), 1},
            },
    };
}

bool SupportsBlit(const Device& device, VkFormat source) {
    const VkFormatProperties source_props = device.GetPhysical().GetFormatProperties(source);
    const VkFormatProperties temporal_props =
        device.GetPhysical().GetFormatProperties(kTemporalColorFormat);
    return (source_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
           (temporal_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0 &&
           (temporal_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
           (temporal_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

} // namespace

struct ScarletFsr4Pass::Impl {
    std::vector<std::uint8_t> scratch;
    FfxInterface backend{};
    ffxContext context{};
    bool backend_ok{};
    bool context_ok{};
    int preset{-1};
    std::string problem;

    ~Impl() {
        Destroy();
    }

    void Destroy() {
        if (context_ok) {
            ffxFsr4V07DestroyContext(&context, nullptr);
            context = nullptr;
            context_ok = false;
        }
        if (backend_ok) {
            ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(scratch.data()));
            backend = {};
            backend_ok = false;
        }
        scratch.clear();
    }

    void Fail(const std::string& message) {
        if (problem != message) {
            LOG_WARNING(Render_Vulkan, "[Scarlet.FSR4] {}", message);
        }
        problem = message;
    }

    bool Create(const Device& device, VkExtent2D output, int wanted_preset) {
        Destroy();

        FfxFsr4V07AssetSet assets{};
        if (!ffxFsr4V07BuildAssetSet(ModelPreset(wanted_preset), output.width, output.height,
                                     &assets)) {
            Fail("output size is unsupported by the v07 model");
            return false;
        }

        const auto directory = AssetDirectory();
        std::array<std::vector<std::uint8_t>, FFX_FSR4_VK_PASS_COUNT> code;
        std::vector<std::uint8_t> initializer;
        std::vector<std::uint8_t> weights;

        const auto load = [&](const char* name, std::vector<std::uint8_t>& bytes) {
            const auto path = directory / name;
            if (ReadFile(path, bytes)) {
                return true;
            }
            Fail("missing FSR4 asset: " + path.u8string());
            return false;
        };

        if (!load(assets.pre, code[0])) {
            return false;
        }
        for (u32 pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
            if (!load(assets.model[pass], code[1 + pass])) {
                return false;
            }
        }
        if (!load(assets.post, code[13]) || !load(assets.rcas, code[14]) ||
            !load(assets.spdAutoExposure, code[15]) ||
            !load(assets.initializer, initializer) ||
            !load(assets.prePassWeights, weights)) {
            return false;
        }

        if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
            weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
            Fail("FSR4 model weights have an unexpected size");
            return false;
        }

        static const std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries = [] {
            std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names{};
            names.fill("main");
            for (u32 pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
                names[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
            }
            return names;
        }();

        FfxFsr4VkCreateInfo create_info{};
        create_info.device = *device.GetLogical();
        create_info.physicalDevice = device.GetPhysical();
        for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
            if (code[i].empty() || (code[i].size() % sizeof(std::uint32_t)) != 0) {
                Fail("invalid FSR4 SPIR-V asset");
                return false;
            }
            create_info.shaders[i] = {
                reinterpret_cast<const std::uint32_t*>(code[i].data()),
                code[i].size(),
                entries[i].c_str(),
            };
        }
        create_info.modelInitializer = initializer.data();
        create_info.modelInitializerSize = initializer.size();
        create_info.prePassWeights = weights.data();
        create_info.prePassWeightsSize = weights.size();
        scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
        create_info.scratchBuffer = scratch.data();
        create_info.scratchBufferSize = scratch.size();

        const VkResult backend_result = ffxFsr4VkCreateContext(&create_info, &backend);
        if (backend_result != VK_SUCCESS) {
            Fail("Vulkan FSR4 backend creation failed (" +
                 std::to_string(static_cast<int>(backend_result)) + ")");
            backend = {};
            scratch.clear();
            return false;
        }
        backend_ok = true;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                     FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        desc.maxRenderSize = {(output.width + 7U) & ~7U, (output.height + 7U) & ~7U};
        desc.maxUpscaleSize = {(output.width + 7U) & ~7U, (output.height + 7U) & ~7U};

        ffxFsr4V07SetBackendInterface(&backend);
        const ffxReturnCode_t result =
            ffxFsr4V07CreateContext(&context, &desc.header, nullptr);
        ffxFsr4V07SetBackendInterface(nullptr);
        if (result != FFX_API_RETURN_OK) {
            Fail("FSR4 provider context creation failed (" + std::to_string(result) + ")");
            Destroy();
            return false;
        }

        context_ok = true;
        preset = wanted_preset;
        problem.clear();
        LOG_INFO(Render_Vulkan,
                 "[Scarlet.FSR4] v07 INT8 provider ready model={} tier={} output={}x{}",
                 ffxFsr4ModelPresetName(ModelPreset(wanted_preset)), assets.tier,
                 output.width, output.height);
        return true;
    }

    void RetireThrough(std::uint64_t frame_id) {
        if (backend_ok && frame_id != 0) {
            ffxFsr4VkRetireFrame(&backend, frame_id);
        }
    }

    VkResult Register(VkImage image, VkImageView view, VkImageLayout layout,
                      VkPipelineStageFlags stage, VkAccessFlags access,
                      VkPipelineStageFlags restore_stage, VkAccessFlags restore_access) {
        const FfxFsr4VkExternalImageState state{
            .structSize = sizeof(FfxFsr4VkExternalImageState),
            .image = image,
            .view = view,
            .layout = layout,
            .stageMask = stage,
            .accessMask = access,
            .restoreLayout = VK_IMAGE_LAYOUT_GENERAL,
            .restoreStageMask = restore_stage,
            .restoreAccessMask = restore_access,
        };
        return ffxFsr4VkSetExternalImageState(&backend, &state);
    }

    static FfxApiResource Resource(VkImageView view, std::uint32_t format,
                                   std::uint32_t width, std::uint32_t height,
                                   std::uint32_t state) {
        FfxApiResource resource{};
        resource.resource = reinterpret_cast<void*>(view);
        resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
        resource.description.format = format;
        resource.description.width = width;
        resource.description.height = height;
        resource.description.depth = 1;
        resource.description.mipCount = 1;
        resource.state = state;
        return resource;
    }

    bool Dispatch(VkCommandBuffer command, const ImageSet& set, VkExtent2D input,
                  VkExtent2D output, std::uint64_t frame_id,
                  std::uint64_t retire_through, bool reset) {
        if (!context_ok || !backend_ok) {
            return false;
        }

        if (retire_through) {
            RetireThrough(retire_through);
        }

        const VkResult begin = ffxFsr4VkBeginFrame(&backend, frame_id);
        if (begin != VK_SUCCESS) {
            Fail("no free FSR4 provider frame (" +
                 std::to_string(static_cast<int>(begin)) + ")");
            return false;
        }

        constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
        if (Register(*set.color, *set.color_view, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) != VK_SUCCESS ||
            Register(*set.depth, *set.depth_view, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) != VK_SUCCESS ||
            Register(*set.motion, *set.motion_view, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read) != VK_SUCCESS ||
            Register(*set.output, *set.output_view, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, read,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, read) != VK_SUCCESS) {
            Fail("external image registration failed");
            return false;
        }

        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = reinterpret_cast<void*>(command);
        dispatch.color =
            Resource(*set.color_view, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                     input.width, input.height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth =
            Resource(*set.depth_view, FFX_SURFACE_FORMAT_R32_FLOAT,
                     input.width, input.height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors =
            Resource(*set.motion_view, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                     input.width, input.height, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output =
            Resource(*set.output_view, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                     output.width, output.height, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {0.0f, 0.0f};
        dispatch.motionVectorScale = {1.0f, 1.0f};
        dispatch.renderSize = {input.width, input.height};
        dispatch.upscaleSize = {output.width, output.height};
        dispatch.enableSharpening = false;
        dispatch.enableAutoExposure = true;
        dispatch.sharpness = 0.0f;
        dispatch.frameTimeDelta = FrameTimeMs();
        dispatch.preExposure = 1.0f;
        dispatch.reset = reset;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.0471975512f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = 0;

        const ffxReturnCode_t result =
            ffxFsr4V07Dispatch(&context, &dispatch.header);
        if (result != FFX_API_RETURN_OK) {
            Fail("FSR4 dispatch failed (" + std::to_string(result) + ")");
            return false;
        }
        return true;
    }
};

ScarletFsr4Pass::ScarletFsr4Pass(const Device&, MemoryAllocator& allocator_,
                                 Scheduler& scheduler_, std::size_t image_count_,
                                 VkExtent2D output_extent_)
    : allocator{allocator_}, scheduler_ref{scheduler_}, image_count{image_count_},
      output_extent{output_extent_}, impl{std::make_unique<Impl>()} {
    ready = Fsr4Requested();
}

ScarletFsr4Pass::~ScarletFsr4Pass() {
    scheduler_ref.Finish();
    if (impl && !in_flight.empty()) {
        impl->RetireThrough(in_flight.back().first);
    }
    in_flight.clear();
    impl.reset();
}

bool ScarletFsr4Pass::Ready() const {
    return ready && !dispatch_failed.load(std::memory_order_relaxed);
}

void ScarletFsr4Pass::ReleaseResources(Scheduler& scheduler) {
    scheduler.Finish();
    if (impl && !in_flight.empty()) {
        impl->RetireThrough(in_flight.back().first);
    }
    in_flight.clear();
    images.clear();
    input_extent = {};
    source_format = VK_FORMAT_UNDEFINED;
    reset_pending = true;
}

bool ScarletFsr4Pass::EnsureResources(const Device& device, Scheduler& scheduler,
                                      VkExtent2D wanted_input, VkFormat wanted_format) {
    if (!Ready() || wanted_input.width == 0 || wanted_input.height == 0 ||
        output_extent.width == 0 || output_extent.height == 0) {
        return false;
    }
    if (!SupportsBlit(device, wanted_format)) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR4] source/temporal formats do not support the required Vulkan blit");
        dispatch_failed.store(true, std::memory_order_relaxed);
        return false;
    }

    if (!impl->context_ok) {
        scheduler.Finish();
        if (!impl->Create(device, output_extent, QualityPreset())) {
            dispatch_failed.store(true, std::memory_order_relaxed);
            return false;
        }
    }

    if (!images.empty() && input_extent.width == wanted_input.width &&
        input_extent.height == wanted_input.height && source_format == wanted_format) {
        return true;
    }

    ReleaseResources(scheduler);
    input_extent = wanted_input;
    source_format = wanted_format;
    images.resize(image_count);

    for (auto& set : images) {
        set.color = CreateWrappedImage(allocator, input_extent, kTemporalColorFormat);
        set.color_view = CreateWrappedImageView(device, set.color, kTemporalColorFormat);
        set.output = CreateWrappedImage(allocator, output_extent, kTemporalColorFormat);
        set.output_view = CreateWrappedImageView(device, set.output, kTemporalColorFormat);
        set.depth = CreateWrappedImage(allocator, input_extent, kDepthFormat);
        set.depth_view = CreateWrappedImageView(device, set.depth, kDepthFormat);
        set.motion = CreateWrappedImage(allocator, input_extent, kMotionFormat);
        set.motion_view = CreateWrappedImageView(device, set.motion, kMotionFormat);
    }

    scheduler.Record([this](vk::CommandBuffer cmdbuf) {
        const auto range = ColorRange();
        const std::array ranges{range};
        VkClearColorValue zero{};
        VkClearColorValue far_depth{};
        far_depth.float32[0] = 1.0f;

        for (auto& set : images) {
            for (VkImage image : std::array<VkImage, 4>{
                     *set.color, *set.output, *set.depth, *set.motion}) {
                ImageBarrier(cmdbuf, image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            cmdbuf.ClearColorImage(*set.color, VK_IMAGE_LAYOUT_GENERAL, zero, ranges);
            cmdbuf.ClearColorImage(*set.output, VK_IMAGE_LAYOUT_GENERAL, zero, ranges);
            cmdbuf.ClearColorImage(*set.depth, VK_IMAGE_LAYOUT_GENERAL, far_depth, ranges);
            cmdbuf.ClearColorImage(*set.motion, VK_IMAGE_LAYOUT_GENERAL, zero, ranges);
        }
    });
    scheduler.Finish();

    LOG_INFO(Render_Vulkan,
             "[Scarlet.FSR4] compatibility inputs ready {}x{} -> {}x{} "
             "(final color + neutral depth/motion)",
             input_extent.width, input_extent.height,
             output_extent.width, output_extent.height);
    return true;
}

VkImageView ScarletFsr4Pass::Draw(const Device& device, Scheduler& scheduler,
                                  std::size_t image_index, VkImage source_image,
                                  VkImageView source_view, VkFormat format,
                                  VkExtent2D wanted_input) {
    if (!EnsureResources(device, scheduler, wanted_input, format) ||
        image_index >= images.size()) {
        return source_view;
    }

    std::uint64_t retire_through = 0;
    while (!in_flight.empty() && scheduler.IsFree(in_flight.front().second)) {
        retire_through = in_flight.front().first;
        in_flight.pop_front();
    }
    while (in_flight.size() >= 3) {
        scheduler.Wait(in_flight.front().second);
        retire_through = in_flight.front().first;
        in_flight.pop_front();
        while (!in_flight.empty() && scheduler.IsFree(in_flight.front().second)) {
            retire_through = in_flight.front().first;
            in_flight.pop_front();
        }
    }

    const std::uint64_t frame_id = next_frame_id++;
    const bool reset = reset_pending;
    reset_pending = false;
    const VkImageBlit blit = FullBlit(input_extent, input_extent);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, source_image, image_index, blit, frame_id,
                      retire_through, reset](vk::CommandBuffer cmdbuf) {
        auto& set = images[image_index];

        ImageBarrier(cmdbuf, source_image, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        ImageBarrier(cmdbuf, *set.color, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);

        cmdbuf.BlitImage(source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         *set.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blit,
                         VK_FILTER_LINEAR);

        ImageBarrier(cmdbuf, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ImageBarrier(cmdbuf, *set.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        if (!impl->Dispatch(*cmdbuf, set, input_extent, output_extent,
                            frame_id, retire_through, reset)) {
            dispatch_failed.store(true, std::memory_order_relaxed);
        }
    });

    in_flight.emplace_back(frame_id, scheduler.CurrentTick());
    return *images[image_index].output_view;
}

} // namespace Vulkan
