#include "scarlet_temporal.h"

#include <stdatomic.h>
#include <string.h>

#include "../../rt.h"
#include "vk.h"

typedef struct {
    NvnTexture *depth;
    NvnTexture *motion;
    uint64_t generation;
} TemporalState;

static TemporalState g_temporal;
static _Atomic uint64_t g_generation = 1;

static bool same_extent(const NvnTexture *a, const NvnTexture *b) {
    return a && b && a->width == b->width && a->height == b->height;
}

static bool motion_fallback_format(VkFormat f) {
    return f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT;
}

static void select_pair(NvnTexture *depth, NvnTexture *motion, const char *rule) {
    if (!depth || !motion) return;
    if (g_temporal.depth == depth && g_temporal.motion == motion) return;

    g_temporal.depth = depth;
    g_temporal.motion = motion;
    g_temporal.generation = atomic_fetch_add_explicit(&g_generation, 1, memory_order_relaxed) + 1;

    static unsigned logs;
    if (logs++ < 8)
        LOG("[scarlet-temporal] selected depth=%p motion=%p %ux%u rule=%s gen=%llu\n",
            (void *)depth, (void *)motion, motion->width, motion->height,
            rule, (unsigned long long)g_temporal.generation);
}

void scarlet_temporal_observe(NvnTexture *const color[8],
                              const NvnTextureView cview[8],
                              const uint8_t has_cview[8],
                              int ncolor,
                              NvnTexture *depth,
                              const NvnTextureView *dview,
                              bool has_dview) {
    if (!depth || ncolor <= 0) return;

    const VkFormat df = vk_attachment_format(depth, dview, has_dview);
    if (df != VK_FORMAT_D24_UNORM_S8_UINT &&
        df != VK_FORMAT_D32_SFLOAT &&
        df != VK_FORMAT_D32_SFLOAT_S8_UINT)
        return;

    // Observed Scarlet scene path: D24S8 + RT0/RT1 RGBA16F, same guest
    // extent. RT1 is the temporal/velocity target. Keep this rule ahead of
    // generic RG motion-vector formats because Scarlet's velocity buffer is
    // RGBA16F in the captured 960x540 path.
    if (ncolor >= 2 && color[0] && color[1] &&
        same_extent(depth, color[0]) && same_extent(depth, color[1])) {
        VkFormat f0 = vk_attachment_format(color[0], &cview[0], has_cview[0] != 0);
        VkFormat f1 = vk_attachment_format(color[1], &cview[1], has_cview[1] != 0);
        if (df == VK_FORMAT_D24_UNORM_S8_UINT &&
            f0 == VK_FORMAT_R16G16B16A16_SFLOAT &&
            f1 == VK_FORMAT_R16G16B16A16_SFLOAT) {
            select_pair(depth, color[1], "scarlet-rgba16-rt1");
            return;
        }
    }

    for (int i = 0; i < ncolor && i < 8; ++i) {
        if (!color[i] || !same_extent(depth, color[i])) continue;
        VkFormat f = vk_attachment_format(color[i], &cview[i], has_cview[i] != 0);
        if (motion_fallback_format(f)) {
            select_pair(depth, color[i], "rg-motion-fallback");
            return;
        }
    }
}

void scarlet_temporal_forget(NvnTexture *texture) {
    if (!texture) return;
    if (g_temporal.depth != texture && g_temporal.motion != texture) return;

    if (g_temporal.depth == texture) g_temporal.depth = NULL;
    if (g_temporal.motion == texture) g_temporal.motion = NULL;
    g_temporal.generation = atomic_fetch_add_explicit(&g_generation, 1, memory_order_relaxed) + 1;
}

bool scarlet_temporal_get(ScarletTemporalImages *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);

    NvnTexture *depth = g_temporal.depth;
    NvnTexture *motion = g_temporal.motion;
    if (!depth || !motion) return false;

    TexBackend *db = vk_tex(depth, true);
    TexBackend *mb = vk_tex(motion, true);
    if (!db || !mb || !db->image || !mb->image) return false;

    out->depth_image = db->image;
    out->depth_format = db->format;
    out->depth_aspect = db->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!out->depth_aspect) out->depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    out->depth_width = db->width;
    out->depth_height = db->height;

    out->motion_image = mb->image;
    out->motion_format = mb->format;
    out->motion_width = mb->width;
    out->motion_height = mb->height;
    out->generation = g_temporal.generation;
    return true;
}

void scarlet_temporal_reset(void) {
    memset(&g_temporal, 0, sizeof g_temporal);
    g_temporal.generation = atomic_fetch_add_explicit(&g_generation, 1, memory_order_relaxed) + 1;
}
