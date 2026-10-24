// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

#include "video_core/renderer_vulkan/scarlet_dlss_bridge.h"

namespace ScarletPC::Graphics {

class DlssClient {
public:
    static DlssClient& Get();

    bool Requested() const;
    bool Load();
    bool IsLoaded() const;
    bool IsInitialized() const;

    std::span<const VkExtensionProperties> InstanceExtensions();
    std::span<const VkExtensionProperties> DeviceExtensions(VkInstance instance,
                                                            VkPhysicalDevice physical);

    bool Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    PFN_vkGetInstanceProcAddr get_instance_proc,
                    PFN_vkGetDeviceProcAddr get_device_proc);

    bool CreateFeature(VkCommandBuffer command, const ScarletDlssFeature& feature);
    bool Evaluate(VkCommandBuffer command, const ScarletDlssEvaluate& evaluate);
    void ReleaseFeature();
    void Shutdown();

    const char* Problem() const;

private:
    DlssClient() = default;
    ~DlssClient();
    DlssClient(const DlssClient&) = delete;
    DlssClient& operator=(const DlssClient&) = delete;

    struct Impl;
    Impl* impl{};
};

} // namespace ScarletPC::Graphics
