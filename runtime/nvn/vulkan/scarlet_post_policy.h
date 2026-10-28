// Scarlet native Vulkan post-processing policy.
// Temporal outputs already include reconstruction/anti-aliasing. Never apply
// the spatial AA/upscaler again to a successfully reconstructed frame.
// Failed/unavailable temporal frames retain the user's spatial fallback.
#pragma once
#include <stdbool.h>

typedef struct {
    bool smaa;
    bool fsr1;
} ScarletPostPasses;

static inline ScarletPostPasses scarlet_post_passes(bool temporal_output,
                                                    bool smaa_requested,
                                                    bool fsr1_requested,
                                                    bool upscale_needed) {
    ScarletPostPasses result = {
        .smaa = !temporal_output && smaa_requested,
        .fsr1 = !temporal_output && fsr1_requested && upscale_needed,
    };
    return result;
}
