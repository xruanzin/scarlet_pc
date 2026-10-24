// SPDX-License-Identifier: MIT
// Scarlet PC - optional AMD FidelityFX frame-generation bridge.
//
// This file contains no game code and no AMD binaries. It dynamically loads the
// official FidelityFX Vulkan runtime when SCARLET_FRAME_GENERATION=1.

#pragma once

#include <cstdint>
#include "video_core/vulkan_common/vulkan.h"

namespace ScarletPC::Graphics {

class FidelityFxFrameGeneration {
public:
    static FidelityFxFrameGeneration& Get();

    bool Requested() const;
    bool Load();
    bool IsLoaded() const;

    void SetQueues(VkQueue graphics, std::uint32_t graphics_family,
                   VkQueue present, std::uint32_t present_family);

    bool CreateSwapchain(VkPhysicalDevice physical_device, VkDevice device,
                         VkSwapchainKHR* swapchain,
                         const VkSwapchainCreateInfoKHR& create_info);
    void DestroySwapchain();

    bool OwnsSwapchain() const;
    VkResult GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain,
                                std::uint32_t* count, VkImage* images) const;
    VkResult AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain,
                              std::uint64_t timeout, VkSemaphore semaphore,
                              VkFence fence, std::uint32_t* image_index) const;
    VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) const;

    // Initial Scarlet integration: real FidelityFX frame interpolation with a
    // conservative optical-flow compatibility input set. The final color frame
    // is copied into a shader-readable HUD-less input while depth/motion are
    // neutral. A later game-specific path can replace these with true Scarlet
    // depth and motion vectors without changing the swapchain integration.
    bool PrepareCompatibilityFrame(VkPhysicalDevice physical_device, VkDevice device,
                                   VkCommandBuffer command, VkImage final_color,
                                   VkFormat color_format, VkExtent2D size,
                                   float frame_time_ms);

    const char* Problem() const;

private:
    FidelityFxFrameGeneration() = default;
    ~FidelityFxFrameGeneration();
    FidelityFxFrameGeneration(const FidelityFxFrameGeneration&) = delete;
    FidelityFxFrameGeneration& operator=(const FidelityFxFrameGeneration&) = delete;

    struct Impl;
    Impl* impl{};
};

} // namespace ScarletPC::Graphics
