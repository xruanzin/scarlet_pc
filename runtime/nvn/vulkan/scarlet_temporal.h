// Scarlet-specific temporal scene inputs retained across post-processing/UI.
//
// The legacy video_core path learned the game's actual scene MRT signature:
// D24S8 depth plus two RGBA16F targets, where RT1 carries velocity.  Keep that
// knowledge in the native NVN renderer rather than depending on video_core.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../nvn.h"

typedef struct ScarletTemporalImages {
    VkImage depth_image;
    VkFormat depth_format;
    VkImageAspectFlags depth_aspect;
    uint32_t depth_width, depth_height;

    VkImage motion_image;
    VkFormat motion_format;
    uint32_t motion_width, motion_height;

    uint64_t generation;
} ScarletTemporalImages;

void scarlet_temporal_observe(NvnTexture *const color[8],
                              const NvnTextureView cview[8],
                              const uint8_t has_cview[8],
                              int ncolor,
                              NvnTexture *depth,
                              const NvnTextureView *dview,
                              bool has_dview);

// Must be called before a native texture object becomes invalid.
void scarlet_temporal_forget(NvnTexture *texture);

// Resolves the retained guest textures to their current native Vulkan images.
// This intentionally survives backend image recreation/resolution changes.
bool scarlet_temporal_get(ScarletTemporalImages *out);

void scarlet_temporal_reset(void);
