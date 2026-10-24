// SPDX-License-Identifier: MIT
#include "video_core/renderer_vulkan/scarlet_dlss_pass.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/scarlet_dlss_client.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

int DlssQuality() {
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

float FrameTimeMs() {
    int fps = 60;
    if (const char* value = std::getenv("SCARLET_TARGET_FPS"); value && *value) {
        fps = std::clamp(std::atoi(value), 30, 240);
    }
    return 1000.0f / static_cast<float>(fps);
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

void GeneralBarrier(vk::CommandBuffer cmdbuf, VkImage image,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
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

ScarletDlssPass::ScarletDlssPass(const Device&, MemoryAllocator& allocator_,
                                 std::size_t image_count_, VkExtent2D output_extent_)
    : allocator{allocator_}, image_count{image_count_}, output_extent{output_extent_} {
    ready = ScarletPC::Graphics::DlssClient::Get().IsInitialized();
    if (!ready) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.DLSS] NGX was not initialized; DLSS pass will stay inactive");
    }
}

ScarletDlssPass::~ScarletDlssPass() {
    ScarletPC::Graphics::DlssClient::Get().ReleaseFeature();
}

bool ScarletDlssPass::Ready() const {
    return ready;
}

void ScarletDlssPass::ReleaseResources(Scheduler& scheduler) {
    scheduler.Finish();
    ScarletPC::Graphics::DlssClient::Get().ReleaseFeature();
    feature_ready = false;
    images.clear();
    input_extent = {};
    source_format = VK_FORMAT_UNDEFINED;
}

bool ScarletDlssPass::EnsureResources(const Device& device, Scheduler& scheduler,
                                      VkExtent2D wanted_input, VkFormat wanted_format) {
    if (!ready || wanted_input.width == 0 || wanted_input.height == 0 ||
        output_extent.width == 0 || output_extent.height == 0) {
        return false;
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
        set.output = CreateWrappedImage(allocator, output_extent, source_format);
        set.output_view = CreateWrappedImageView(device, set.output, source_format);
        set.depth = CreateWrappedImage(allocator, input_extent, VK_FORMAT_R32_SFLOAT);
        set.depth_view = CreateWrappedImageView(device, set.depth, VK_FORMAT_R32_SFLOAT);
        set.motion = CreateWrappedImage(allocator, input_extent, VK_FORMAT_R16G16_SFLOAT);
        set.motion_view = CreateWrappedImageView(device, set.motion, VK_FORMAT_R16G16_SFLOAT);
    }

    scheduler.Record([this](vk::CommandBuffer cmdbuf) {
        const auto range = ColorRange();
        VkClearColorValue depth_clear{};
        depth_clear.float32[0] = 1.0f;
        VkClearColorValue zero{};

        for (auto& set : images) {
            TransitionImageLayout(cmdbuf, *set.output, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionImageLayout(cmdbuf, *set.depth, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionImageLayout(cmdbuf, *set.motion, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_UNDEFINED);
            cmdbuf.ClearColorImage(*set.depth, VK_IMAGE_LAYOUT_GENERAL, depth_clear, range);
            cmdbuf.ClearColorImage(*set.motion, VK_IMAGE_LAYOUT_GENERAL, zero, range);
        }
    });
    scheduler.Finish();

    LOG_INFO(Render_Vulkan, "[Scarlet.DLSS] Resources {}x{} -> {}x{} quality={}",
             input_extent.width, input_extent.height, output_extent.width, output_extent.height,
             DlssQuality());
    return true;
}

VkImageView ScarletDlssPass::Draw(const Device& device, Scheduler& scheduler,
                                  std::size_t image_index, VkImage source_image,
                                  VkImageView source_view, VkFormat format,
                                  VkExtent2D wanted_input) {
    if (!EnsureResources(device, scheduler, wanted_input, format) ||
        image_index >= images.size()) {
        return source_view;
    }

    Images& set = images[image_index];
    auto& dlss = ScarletPC::Graphics::DlssClient::Get();

    const ScarletDlssFeature feature{
        .input_width = input_extent.width,
        .input_height = input_extent.height,
        .output_width = output_extent.width,
        .output_height = output_extent.height,
        .quality = DlssQuality(),
        .depth_inverted = 0,
        .preset = 0,
        .hdr = 0,
    };

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, &dlss, feature, source_image, source_view, image_index](
                         vk::CommandBuffer cmdbuf) {
        Images& local = images[image_index];

        GeneralBarrier(cmdbuf, source_image,
                       VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        if (!feature_ready) {
            if (!dlss.CreateFeature(*cmdbuf, feature)) {
                LOG_WARNING(Render_Vulkan,
                            "[Scarlet.DLSS] Feature creation failed; native presentation retained");
                return;
            }
            feature_ready = true;
        }

        const ScarletDlssEvaluate evaluate{
            .color =
                {
                    .image = source_image,
                    .view = source_view,
                    .range = ColorRange(),
                    .format = source_format,
                    .width = input_extent.width,
                    .height = input_extent.height,
                },
            .depth =
                {
                    .image = *local.depth,
                    .view = *local.depth_view,
                    .range = ColorRange(),
                    .format = VK_FORMAT_R32_SFLOAT,
                    .width = input_extent.width,
                    .height = input_extent.height,
                },
            .motion =
                {
                    .image = *local.motion,
                    .view = *local.motion_view,
                    .range = ColorRange(),
                    .format = VK_FORMAT_R16G16_SFLOAT,
                    .width = input_extent.width,
                    .height = input_extent.height,
                },
            .output =
                {
                    .image = *local.output,
                    .view = *local.output_view,
                    .range = ColorRange(),
                    .format = source_format,
                    .width = output_extent.width,
                    .height = output_extent.height,
                },
            .jitter_x = 0.0f,
            .jitter_y = 0.0f,
            .reset = 0,
            .frame_ms = FrameTimeMs(),
            .sharpness = 0.0f,
        };

        if (!dlss.Evaluate(*cmdbuf, evaluate)) {
            LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] NGX evaluation failed");
            return;
        }

        GeneralBarrier(cmdbuf, *local.output,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });

    return *set.output_view;
}

} // namespace Vulkan
