// CPU-only regression for the native Scarlet present path. No Vulkan,
// GPU, game dumps, AOT modules or external dependencies required.
#include <assert.h>
#include <stdbool.h>
#include "../runtime/nvn/vulkan/scarlet_post_policy.h"

int main(void) {
    ScarletPostPasses a = scarlet_post_passes(false, true, true, true);
    assert(a.smaa && a.fsr1); // existing default spatial path

    ScarletPostPasses b = scarlet_post_passes(true, true, true, true);
    assert(!b.smaa && !b.fsr1); // temporal reconstruction wins

    ScarletPostPasses c = scarlet_post_passes(false, true, true, false);
    assert(c.smaa && !c.fsr1); // no needless FSR1 at native resolution

    ScarletPostPasses d = scarlet_post_passes(false, false, true, true);
    assert(!d.smaa && d.fsr1); // user disabled SMAA

    ScarletPostPasses e = scarlet_post_passes(true, false, true, false);
    assert(!e.smaa && !e.fsr1); // exact-size temporal output

    ScarletPostPasses f = scarlet_post_passes(false, false, false, true);
    assert(!f.smaa && !f.fsr1); // no post-processing requested
    return 0;
}
