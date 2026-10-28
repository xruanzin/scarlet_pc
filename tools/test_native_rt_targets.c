// Verifies the semantic NVN render-target equality used by Vulkan pass reuse.
// No game files, Vulkan device, or window needed.
#include <assert.h>
#include <string.h>
#include "../runtime/nvn/vulkan/scarlet_rt_targets.h"

int main(void) {
    NvnTexture first, second, depth;
    ScarletRtState a, b;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    memset(&depth, 0, sizeof depth);
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);

    assert(scarlet_rt_targets_equal(&a, &b));
    a.ncolor = b.ncolor = 1;
    a.color[0] = b.color[0] = &first;
    assert(scarlet_rt_targets_equal(&a, &b));

    // Recorder bytes of an unused view are not a Vulkan attachment state.
    a.cview[0].format = 0x25;
    b.cview[0].format = 0x4e;
    assert(scarlet_rt_targets_equal(&a, &b));

    // A live view flag or attachment change does require a new rendering pass.
    a.has_cview[0] = 1;
    assert(!scarlet_rt_targets_equal(&a, &b));
    b.has_cview[0] = 1;
    assert(!scarlet_rt_targets_equal(&a, &b));
    b.cview[0] = a.cview[0];
    assert(scarlet_rt_targets_equal(&a, &b));
    b.cview[0].base_level++;
    assert(!scarlet_rt_targets_equal(&a, &b));
    b.cview[0] = a.cview[0];
    b.color[0] = &second;
    assert(!scarlet_rt_targets_equal(&a, &b));

    // Unbound attachments cannot make inactive views semantically relevant.
    a.color[0] = b.color[0] = 0;
    a.has_cview[0] = 0;
    b.cview[0].format = 0;
    assert(scarlet_rt_targets_equal(&a, &b));

    // Depth view changes matter only while a depth attachment is bound.
    a.dview.format = 0x35;
    b.dview.format = 0x36;
    assert(scarlet_rt_targets_equal(&a, &b));
    a.depth = &depth;
    assert(!scarlet_rt_targets_equal(&a, &b));
    b.depth = &depth;
    assert(scarlet_rt_targets_equal(&a, &b));
    a.has_dview = b.has_dview = 1;
    assert(!scarlet_rt_targets_equal(&a, &b));
    b.dview = a.dview;
    assert(scarlet_rt_targets_equal(&a, &b));
    b.depth = 0;
    assert(!scarlet_rt_targets_equal(&a, &b));

    return 0;
}
