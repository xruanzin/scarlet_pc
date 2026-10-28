// Native fidelity reference mode for image parity tests against suyu.
// Only disables optional, port-authored image transformations. It does not
// disable original guest shaders, LUT grading or the guest's own DOF.
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool scarlet_graphics_reference(void) {
    const char *value = getenv("SCARLET_GRAPHICS_REFERENCE");
    return value && !strcmp(value, "1");
}
