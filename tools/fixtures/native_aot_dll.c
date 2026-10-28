// Synthetic Win32 loader fixture. Contains no game code.
// Build only for the CI --check-aot smoke test; never package with the game.
#include "../../runtime/scarlet_native/aot_abi.h"
#ifndef FIXTURE_ABI
#define FIXTURE_ABI 6
#endif
#define EXPORT __declspec(dllexport)
static uint64_t module_base;
static uint32_t generation;
static void block(ScarletAotGuestContext *c) { c->pc = 0; }
static ScarletAotBlockFn slots[1] = {block};
EXPORT unsigned recomp_image_abi(void) { return FIXTURE_ABI; }
EXPORT unsigned recomp_image_features(void) { return SCARLET_AOT_FEATURE_SUPPORTED; }
EXPORT unsigned recomp_image_guard_v2(unsigned version) { return version == 2 ? 2 : 0; }
EXPORT uint64_t recomp_image_entry(void) { return 0; }
EXPORT void recomp_image_set_base(uint64_t base) { module_base = base; }
EXPORT ScarletAotBlockFn recomp_image_lookup(uint64_t pc) {
    return pc == module_base ? block : 0;
}
EXPORT void recomp_image_run_slice(ScarletAotGuestContext *c) { block(c); }
EXPORT int recomp_image_index(uint64_t *lo, uint64_t *hi, ScarletAotBlockFn **index) {
    *lo = module_base; *hi = module_base + 4; *index = slots; return 1;
}
EXPORT unsigned recomp_image_fastmem_v1(uint32_t bits, uint32_t stride,
    uint64_t mask, uint32_t table, uint32_t limit) {
    return bits == 12 && stride == 5 && mask == ~(uint64_t)3 &&
           table == offsetof(ScarletAotGuestContext, fm_table) &&
           limit == offsetof(ScarletAotGuestContext, fm_limit);
}
EXPORT uint32_t *recomp_image_guard_gen_v1(uint32_t version, uint64_t *lo,
    uint64_t *hi, const uint64_t **base) {
    *lo = 0; *hi = 4; *base = &module_base;
    return version == 2 ? &generation : 0;
}
EXPORT unsigned recomp_image_fpx_v1(uint32_t fpcr, uint32_t fpsr, uint64_t inhibit) {
    return fpcr == offsetof(ScarletAotGuestContext, fpcr) &&
           fpsr == offsetof(ScarletAotGuestContext, fpsr) &&
           inhibit == SCARLET_AOT_FPX_INHIBIT;
}
