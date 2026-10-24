// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), gow3 contributors
// SPDX-FileCopyrightText: Copyright 2026 Scarlet PC contributors
// SPDX-License-Identifier: MIT
//
// Scarlet PC's optional C ABI for NVIDIA NGX / DLSS. The game runtime does not
// link NGX directly; it loads ScarletDlssBridge.dll only when DLSS is requested.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCARLET_DLSS_BRIDGE_ABI 1

typedef void (*ScarletDlssLogFn)(int warning, const char* message);

typedef struct ScarletDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} ScarletDlssImage;

typedef struct ScarletDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality;      // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset;      // NGX render preset, 0 = driver default
    int32_t hdr;
} ScarletDlssFeature;

typedef struct ScarletDlssEvaluate {
    ScarletDlssImage color;
    ScarletDlssImage depth;
    ScarletDlssImage motion;
    ScarletDlssImage output;
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness;
} ScarletDlssEvaluate;

typedef struct ScarletDlssApi {
    uint32_t abi;
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         ScarletDlssLogFn log);
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const ScarletDlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const ScarletDlssEvaluate* evaluate);
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
} ScarletDlssApi;

typedef const ScarletDlssApi* (*ScarletDlssGetApiFn)(void);

#ifdef __cplusplus
}
#endif
