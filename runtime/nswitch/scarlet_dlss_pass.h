// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <vector>
#include <vulkan/vulkan.h>

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Scheduler;

class ScarletDlssPass {
public:
    ScarletDlssPass(const Device& device, MemoryAllocator& allocator, std::size_t image_count,
                    VkExtent2D output_extent);
    ~ScarletDlssPass();

    VkImageView Draw(const Device& device, Scheduler& scheduler, std::size_t image_index,
                     VkImage source_image, VkImageView source_view, VkFormat source_format,
                     VkExtent2D input_extent);

    bool Ready() const;

private:
    struct Images {
        vk::Image output;
        vk::ImageView output_view;
        vk::Image depth;
        vk::ImageView depth_view;
        vk::Image motion;
        vk::ImageView motion_view;
    };

    bool EnsureResources(const Device& device, Scheduler& scheduler, VkExtent2D input_extent,
                         VkFormat source_format);
    void ReleaseResources(Scheduler& scheduler);

    MemoryAllocator& allocator;
    std::size_t image_count{};
    VkExtent2D output_extent{};
    VkExtent2D input_extent{};
    VkFormat source_format{VK_FORMAT_UNDEFINED};
    std::vector<Images> images;
    bool feature_ready{};
    bool ready{};
};

} // namespace Vulkan
