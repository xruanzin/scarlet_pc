// Scoped dependencies for known image consumers. Images stay in GENERAL.
#pragma once
#include <vulkan/vulkan.h>

static inline VkImageMemoryBarrier2 vk_image_dependency(
    VkImage image, VkImageSubresourceRange range,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    // The producer may be a draw, copy, compute or layout initialization. Keep
    // that execution scope until the backend tracks last writers per resource.
    // ALL_COMMANDS also orders prior reads before a subsequent overwrite (WAR).
    return (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = dst_stage, .dstAccessMask = dst_access,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = range,
    };
}
