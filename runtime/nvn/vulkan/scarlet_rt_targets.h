// Exact NVN render-target equality for preserving an active Vulkan render pass.
// An NVN command may carry unspecified bytes in inactive texture views.
// Byte-comparing the whole record spuriously forces VkEndRendering/VkBeginRendering.
#pragma once
#include <stdbool.h>
#include <string.h>

#include "../nvn.h"

typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} ScarletRtState;

static inline bool scarlet_rt_targets_equal(const ScarletRtState *a, const ScarletRtState *b) {
    if (a->ncolor != b->ncolor || a->depth != b->depth) return false;
    for (int i = 0; i < a->ncolor; ++i) {
        if (a->color[i] != b->color[i]) return false;
        // When there is no attachment, the associated view is irrelevant.
        if (!a->color[i]) continue;
        bool av = a->has_cview[i] != 0;
        bool bv = b->has_cview[i] != 0;
        if (av != bv) return false;
        if (av && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i])) return false;
    }
    if (a->depth) {
        bool av = a->has_dview != 0;
        bool bv = b->has_dview != 0;
        if (av != bv) return false;
        if (av && memcmp(&a->dview, &b->dview, sizeof a->dview)) return false;
    }
    return true;
}
