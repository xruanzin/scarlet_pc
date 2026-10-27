#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

bool scarlet_dlss_requested(void);

// Called before VkInstance/VkDevice creation. Returned names remain valid for
// the process lifetime; the caller copies only the pointers into Vulkan create
// infos used immediately.
uint32_t scarlet_dlss_instance_extensions(const char **names, uint32_t capacity);
uint32_t scarlet_dlss_device_extensions(VkInstance instance, VkPhysicalDevice physical,
                                        const char **names, uint32_t capacity);

// Called after the native renderer has created its VkDevice.
bool scarlet_dlss_initialize(void);

// Same presentation contract as the native FSR passes: returns source unchanged
// when DLSS is unavailable or temporal inputs are not ready.
TexBackend *scarlet_dlss_upscale(TexBackend *source);
void scarlet_dlss_reset_history(void);
void scarlet_dlss_shutdown(void);

#ifdef __cplusplus
}
#endif
