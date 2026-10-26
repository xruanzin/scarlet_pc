// SPDX-License-Identifier: MIT
// Native FidelityFX temporal upscaling for Scarlet PC.
//
// The FSR 3.1 path intentionally uses FireBurn/FSR-Vulkan's portable native
// Vulkan backend, matching the integration used by SpartanPort's experimental
// temporal-upscaler releases. It records directly into Scarlet's Vulkan command
// buffer and does not load amd_fidelityfx_vk.dll.

#include "video_core/renderer_vulkan/scarlet_ffx_upscale.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <ffx_vk_portable.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

constexpr VkFormat kFfxInputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kFfxOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

bool PortableFsrInputFormatSupported(VkFormat format) {
    switch (format) {
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

std::atomic_bool g_temporal_disabled_for_run{false};

bool FfxUpscaleRequested() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value &&
           (std::strcmp(value, "fsr3") == 0 || std::strcmp(value, "fsr31") == 0);
}

const char* SelectedFsrName() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value && std::strcmp(value, "fsr31") == 0 ? "FSR 3.1" : "FSR 3";
}

VkExtent2D RequestedOutputExtent() {
    VkExtent2D extent{};
    if (const char* value = std::getenv("SCARLET_WINDOW_WIDTH"); value && *value) {
        extent.width = static_cast<std::uint32_t>(std::max(0, std::atoi(value)));
    }
    if (const char* value = std::getenv("SCARLET_WINDOW_HEIGHT"); value && *value) {
        extent.height = static_cast<std::uint32_t>(std::max(0, std::atoi(value)));
    }
    return extent;
}

bool HasTrueTemporalInputs() {
    const char* value = std::getenv("SCARLET_FSR_TRUE_TEMPORAL_INPUTS");
    return value && *value && *value != '0';
}

float FrameTimeMs() {
    int fps = 60;
    if (const char* value = std::getenv("SCARLET_TARGET_FPS"); value && *value) {
        fps = std::clamp(std::atoi(value), 30, 240);
    }
    return 1000.0f / static_cast<float>(fps);
}

FfxVkPortableImage PortableImage(VkImage image, VkFormat format, std::uint32_t width,
                                 std::uint32_t height, VkImageUsageFlags usage,
                                 VkImageAspectFlags aspect,
                                 FfxVkPortableResourceState state) {
    FfxVkPortableImage result{};
    result.structSize = sizeof(result);
    result.image = image;
    result.format = format;
    result.extent = {width, height};
    result.mipCount = 1;
    result.arrayLayers = 1;
    result.usage = usage;
    result.aspect = aspect;
    result.state = state;
    return result;
}

void LogValidationIssues(const char* stage, std::uint64_t issues) {
    if (!issues) {
        return;
    }
    for (std::uint32_t bit = 0; bit < 64; ++bit) {
        const std::uint64_t flag = 1ull << bit;
        if ((issues & flag) == 0) {
            continue;
        }
        const char* name = ffxVkPortableValidationIssueName(flag);
        LOG_WARNING(Render_Vulkan, "[Scarlet.FSR3] portable {} validation: {}",
                    stage, name ? name : "unknown");
    }
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
    FfxVkPortableUpscaleContext* context{};
    FfxVkPortableUpscaleCreateInfo create_info{};
    std::uint64_t frame_id{};

    void DestroyContext() {
        if (context) {
            ffxVkPortableUpscaleContextDestroy(context);
            context = nullptr;
        }
        create_info = {};
        frame_id = 0;
    }

    ~Impl() {
        DestroyContext();
    }
};

ScarletFfxUpscalePass::ScarletFfxUpscalePass(const Device&, MemoryAllocator& allocator_,
                                             std::size_t image_count_,
                                             VkExtent2D output_extent_)
    : allocator{allocator_}, image_count{image_count_}, output_extent{output_extent_},
      impl{std::make_unique<Impl>()} {
    ready = FfxUpscaleRequested();
    if (ready) {
        const VkExtent2D requested = RequestedOutputExtent();
        if (requested.width && requested.height &&
            (requested.width != output_extent.width ||
             requested.height != output_extent.height)) {
            LOG_INFO(Render_Vulkan,
                     "[Scarlet.FSR3] Deferring FSR layer {}x{} until requested output {}x{} is active",
                     output_extent.width, output_extent.height,
                     requested.width, requested.height);
            ready = false;
        }
    }
}

ScarletFfxUpscalePass::~ScarletFfxUpscalePass() = default;

bool ScarletFfxUpscalePass::Ready() const {
    return ready && !dispatch_failed &&
           !g_temporal_disabled_for_run.load(std::memory_order_relaxed);
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

    // The temporal upscaler must receive a genuinely lower-resolution render.
    // Scarlet currently has presentation paths that can still expose the native
    // 1920x1080 framebuffer even when a lower resolution was requested. Never
    // feed those to FSR; one bad layer disables temporal upscaling for the run.
    if (wanted_input.width >= output_extent.width ||
        wanted_input.height >= output_extent.height) {
        LOG_ERROR(Render_Vulkan,
                  "[Scarlet.FSR3] Invalid temporal extent {}x{} -> {}x{}; "
                  "render scale was not applied, disabling portable FSR globally for this run",
                  wanted_input.width, wanted_input.height,
                  output_extent.width, output_extent.height);
        dispatch_failed = true;
        g_temporal_disabled_for_run.store(true, std::memory_order_relaxed);
        return false;
    }

    if (impl->context && !images.empty() &&
        input_extent.width == wanted_input.width &&
        input_extent.height == wanted_input.height && source_format == format) {
        return true;
    }

    LOG_INFO(Render_Vulkan,
             "[Scarlet.FSR3] Preparing portable FSR resources input={}x{} output={}x{} source_format={}",
             wanted_input.width, wanted_input.height, output_extent.width, output_extent.height,
             static_cast<int>(format));

    ReleaseResources(scheduler);
    input_extent = wanted_input;
    source_format = format;
    images.resize(image_count);

    for (auto& set : images) {
        set.input = CreateWrappedImage(allocator, input_extent, kFfxInputFormat);
        set.input_view = CreateWrappedImageView(device, set.input, kFfxInputFormat);
        set.output = CreateWrappedImage(allocator, output_extent, kFfxOutputFormat);
        set.output_view = CreateWrappedImageView(device, set.output, kFfxOutputFormat);
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
            TransitionImageLayout(cmdbuf, *set.input, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
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

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = VK_NULL_HANDLE;
    device_info.physicalDevice = device.GetPhysical();
    device_info.device = *device.GetLogical();
    device_info.getDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;
    device_info.queue = device.GetGraphicsQueue().Handle();
    device_info.queueFamilyIndex = device.GetGraphicsFamily();
    device_info.shaderFloat16Enabled =
        device.IsFloat16Supported() ? VK_TRUE : VK_FALSE;
    device_info.subgroupSizeControlEnabled =
        device.IsExtSubgroupSizeControlSupported() ? VK_TRUE : VK_FALSE;
    device_info.synchronization2Enabled =
        device.HasSynchronization2() ? VK_TRUE : VK_FALSE;

    // suyu creates the logical device from its accepted core feature set.
    // FSR-Vulkan's checked accumulate shaders require formatless storage writes;
    // this is a Vulkan 1.x core feature on the target AMD/NVIDIA desktop path.
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    impl->create_info = {};
    impl->create_info.structSize = sizeof(impl->create_info);
    impl->create_info.flags = FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    impl->create_info.maxRenderSize = {input_extent.width, input_extent.height};
    impl->create_info.maxOutputSize = {output_extent.width, output_extent.height};

    const auto create_issues =
        ffxVkPortableValidateUpscaleCreateInfo(&impl->create_info);
    if (create_issues) {
        LogValidationIssues("create", create_issues);
        dispatch_failed = true;
        g_temporal_disabled_for_run.store(true, std::memory_order_relaxed);
        return false;
    }

    LOG_INFO(Render_Vulkan,
             "[Scarlet.FSR3] Creating FireBurn portable FSR 3.1 Vulkan context");
    const auto create_rc =
        ffxVkPortableUpscaleContextCreate(&device_info, &impl->create_info, &impl->context);
    if (create_rc != FFX_VK_PORTABLE_OK || !impl->context) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR3] Portable {} context creation failed (rc={})",
                    SelectedFsrName(), static_cast<int>(create_rc));
        dispatch_failed = true;
        g_temporal_disabled_for_run.store(true, std::memory_order_relaxed);
        return false;
    }

    reset_pending = true;
    LOG_INFO(Render_Vulkan,
             "[Scarlet.FSR3] Portable {} temporal upscaler ready {}x{} -> {}x{}",
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
    const std::uint64_t frame_id = impl->frame_id++;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, source_image, image_index, reset, frame_id](vk::CommandBuffer cmdbuf) {
        auto& local = images[image_index];

        VkImage fsr_input_image = source_image;
        VkFormat fsr_input_format = source_format;

        // Scarlet's scanout is commonly VK_FORMAT_A8B8G8R8_UNORM_PACK32 (VkFormat 51).
        // AMD FidelityFX 1.1.4 does not map that packed format to an FFX surface format.
        // Normalize unsupported scanout formats into a known FSR-compatible RGBA8 image.
        if (!PortableFsrInputFormatSupported(source_format)) {
            const VkImageSubresourceRange range = ColorRange();
            const std::array pre_blit{
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = source_image,
                    .subresourceRange = range,
                },
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = *local.input,
                    .subresourceRange = range,
                },
            };
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, nullptr, pre_blit);

            const VkImageBlit blit{
                .srcSubresource{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .srcOffsets{
                    VkOffset3D{0, 0, 0},
                    VkOffset3D{static_cast<s32>(input_extent.width),
                               static_cast<s32>(input_extent.height), 1},
                },
                .dstSubresource{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .dstOffsets{
                    VkOffset3D{0, 0, 0},
                    VkOffset3D{static_cast<s32>(input_extent.width),
                               static_cast<s32>(input_extent.height), 1},
                },
            };
            cmdbuf.BlitImage(source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             *local.input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             std::array{blit}, VK_FILTER_NEAREST);

            const std::array post_blit{
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = source_image,
                    .subresourceRange = range,
                },
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = *local.input,
                    .subresourceRange = range,
                },
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   nullptr, nullptr, post_blit);

            fsr_input_image = *local.input;
            fsr_input_format = kFfxInputFormat;

            static std::atomic_bool logged_conversion{false};
            if (!logged_conversion.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO(Render_Vulkan,
                         "[Scarlet.FSR3] Normalizing unsupported scanout format {} to RGBA8 before FSR",
                         static_cast<int>(source_format));
            }
        } else {
            GeneralBarrier(cmdbuf, source_image,
                           VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        FfxVkPortableUpscaleDispatchInfo dispatch{};
        dispatch.structSize = sizeof(dispatch);
        dispatch.commandBuffer = *cmdbuf;
        dispatch.color = PortableImage(
            fsr_input_image, fsr_input_format, input_extent.width, input_extent.height,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        dispatch.depth = PortableImage(
            *local.depth, VK_FORMAT_R32_SFLOAT, input_extent.width, input_extent.height,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        dispatch.motionVectors = PortableImage(
            *local.motion, VK_FORMAT_R16G16_SFLOAT, input_extent.width, input_extent.height,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        dispatch.output = PortableImage(
            *local.output, kFfxOutputFormat, output_extent.width, output_extent.height,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);

        dispatch.exposure.structSize = sizeof(dispatch.exposure);
        dispatch.reactiveMask.structSize = sizeof(dispatch.reactiveMask);
        dispatch.transparencyAndCompositionMask.structSize =
            sizeof(dispatch.transparencyAndCompositionMask);

        const bool true_temporal_inputs = HasTrueTemporalInputs();
        dispatch.jitterOffset = {0.0f, 0.0f};
        // The current presentation bridge has no game motion-vector signal yet.
        // Keep the zero-motion texture in pixel units. Once Scarlet's real scene
        // depth/motion path is wired, the opt-in true-temporal mode can use history.
        dispatch.motionVectorScale = {1.0f, 1.0f};
        dispatch.renderSize = {input_extent.width, input_extent.height};
        dispatch.outputSize = {output_extent.width, output_extent.height};
        dispatch.frameTimeMilliseconds = FrameTimeMs();
        dispatch.preExposure = 1.0f;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraVerticalFovRadians = 1.0471975512f;
        dispatch.viewSpaceToMeters = 1.0f;
        dispatch.sharpness = 0.0f;
        dispatch.enableSharpening = VK_FALSE;
        // Do not accumulate invalid temporal history from placeholder depth/motion.
        // Reset every frame until real scene inputs are supplied, which makes the
        // FSR3 backend behave as a stable per-frame reconstruction/upscale path.
        dispatch.reset = true_temporal_inputs ? (reset ? VK_TRUE : VK_FALSE) : VK_TRUE;
        dispatch.frameId = frame_id;

        const auto issues =
            ffxVkPortableValidateUpscaleDispatchInfo(&impl->create_info, &dispatch);
        if (issues) {
            LogValidationIssues("dispatch", issues);
            dispatch_failed = true;
            g_temporal_disabled_for_run.store(true, std::memory_order_relaxed);
            return;
        }

        const auto rc =
            ffxVkPortableUpscaleContextRecordDispatch(impl->context, &dispatch);
        if (rc != FFX_VK_PORTABLE_OK) {
            dispatch_failed = true;
            g_temporal_disabled_for_run.store(true, std::memory_order_relaxed);
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] Portable {} dispatch failed (rc={}); "
                        "temporal upscaler disabled globally for this run",
                        SelectedFsrName(), static_cast<int>(rc));
            return;
        }

        static std::atomic_bool logged_first_dispatch{false};
        if (!logged_first_dispatch.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Render_Vulkan,
                     "[Scarlet.FSR3] First portable dispatch recorded successfully; mode={}",
                     true_temporal_inputs ? "true-temporal" : "safe-reset-every-frame");
        }

        GeneralBarrier(cmdbuf, *local.output, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });

    return *set.output_view;
}

} // namespace Vulkan
