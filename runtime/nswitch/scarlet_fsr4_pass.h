// SPDX-License-Identifier: GPL-2.0-or-later
// Scarlet PC FSR 4 v07 Vulkan integration.
// Provider design adapted from DestroxIam/gow3_pc-nvidia; provider code is FireBurn/FSR-Vulkan.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

class ScarletFsr4Pass {
public:
    ScarletFsr4Pass(const Device& device, MemoryAllocator& allocator, Scheduler& scheduler,
                    std::size_t image_count, VkExtent2D output_extent);
    ~ScarletFsr4Pass();

    VkImageView Draw(const Device& device, Scheduler& scheduler, std::size_t image_index,
                     VkImage source_image, VkImageView source_view, VkFormat source_format,
                     VkExtent2D input_extent);

    bool Ready() const;

private:
    struct ImageSet {
        vk::Image color;
        vk::ImageView color_view;
        vk::Image output;
        vk::ImageView output_view;
        vk::Image depth;
        vk::ImageView depth_view;
        vk::Image motion;
        vk::ImageView motion_view;
    };
    struct Impl;

    bool EnsureResources(const Device& device, Scheduler& scheduler,
                         VkExtent2D input_extent, VkFormat source_format);
    void ReleaseResources(Scheduler& scheduler);

    MemoryAllocator& allocator;
    Scheduler& scheduler_ref;
    std::size_t image_count{};
    VkExtent2D output_extent{};
    VkExtent2D input_extent{};
    VkFormat source_format{VK_FORMAT_UNDEFINED};
    std::vector<ImageSet> images;
    std::unique_ptr<Impl> impl;
    std::atomic_bool dispatch_failed{false};
    bool ready{};
    bool reset_pending{true};
    std::uint64_t next_frame_id{1};
    std::deque<std::pair<std::uint64_t, std::uint64_t>> in_flight;
};

} // namespace Vulkan
