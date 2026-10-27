#pragma once

#include <stdbool.h>
#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

bool scarlet_fsr3_requested(void);

// Records native FSR 3.1 into the current renderer command buffer. On success
// returns an application-owned output image suitable for vk_present_draw.
// Returns source unchanged when disabled or when true temporal inputs are not
// yet available.
TexBackend *scarlet_fsr3_upscale(TexBackend *source);

void scarlet_fsr3_reset_history(void);
void scarlet_fsr3_shutdown(void);

#ifdef __cplusplus
}
#endif
