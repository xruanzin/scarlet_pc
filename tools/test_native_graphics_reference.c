// CPU-only check of the opt-in Scarlet guest-fidelity reference flag.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include "../runtime/nvn/vulkan/scarlet_fidelity.h"

int main(void) {
    unsetenv("SCARLET_GRAPHICS_REFERENCE");
    assert(!scarlet_graphics_reference());
    setenv("SCARLET_GRAPHICS_REFERENCE", "0", 1);
    assert(!scarlet_graphics_reference());
    setenv("SCARLET_GRAPHICS_REFERENCE", "1", 1);
    assert(scarlet_graphics_reference());
    setenv("SCARLET_GRAPHICS_REFERENCE", "false", 1);
    assert(!scarlet_graphics_reference());
    unsetenv("SCARLET_GRAPHICS_REFERENCE");
    return 0;
}
