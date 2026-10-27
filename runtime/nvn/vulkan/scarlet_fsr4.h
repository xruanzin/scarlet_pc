#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "vk.h"

bool scarlet_fsr4_requested(void);
TexBackend *scarlet_fsr4_upscale(TexBackend *source);
void scarlet_fsr4_reset_history(void);
void scarlet_fsr4_shutdown(void);

#ifdef __cplusplus
}
#endif
