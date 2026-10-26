// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "video_core/vulkan_common/vulkan.h"

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Scheduler;

struct ScarletFfxTemporalInputs {
    VkImage depth_image{};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags depth_aspect{VK_IMAGE_ASPECT_DEPTH_BIT};
    VkExtent2D depth_extent{};

    VkImage motion_image{};
    VkFormat motion_format{VK_FORMAT_UNDEFINED};
    VkExtent2D motion_extent{};
};

class ScarletFfxUpscalePass {
public:
    ScarletFfxUpscalePass(const Device& device, MemoryAllocator& allocator,
                          std::size_t image_count, VkExtent2D output_extent);
    ~ScarletFfxUpscalePass();

    VkImageView Draw(const Device& device, Scheduler& scheduler, std::size_t image_index,
                     VkImage source_image, VkImageView source_view, VkFormat source_format,
                     VkExtent2D input_extent,
                     const ScarletFfxTemporalInputs& temporal_inputs = {});
    bool Ready() const;

private:
    struct ImageSet {
        vk::Image input;
        vk::ImageView input_view;
        vk::Image output;
        vk::ImageView output_view;
        vk::Image depth;
        vk::ImageView depth_view;
        vk::Image motion;
        vk::ImageView motion_view;
    };
    struct Impl;

    bool EnsureResources(const Device& device, Scheduler& scheduler,
                         VkExtent2D input_extent, VkFormat format);
    void ReleaseResources(Scheduler& scheduler);

    MemoryAllocator& allocator;
    std::size_t image_count{};
    VkExtent2D output_extent{};
    VkExtent2D input_extent{};
    VkFormat source_format{VK_FORMAT_UNDEFINED};
    std::vector<ImageSet> images;
    std::unique_ptr<Impl> impl;
    bool ready{};
    bool reset_pending{true};
    bool temporal_history_valid{};
    bool dispatch_failed{};
};

} // namespace Vulkan
