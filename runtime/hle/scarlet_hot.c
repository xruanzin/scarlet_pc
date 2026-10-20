// Scarlet 1.0.0 hot guest leaf functions replaced with direct host C.
//
// These addresses were selected from the slow-scene function-entry profile and
// verified against the exact main Build ID in tools/gen.py. They contain no
// callbacks, allocation or hidden side effects: just loads/arithmetic/constant
// returns. Keeping them here avoids lifted-fragment setup and dispatcher hops.
#include "../hle.h"

void hle_scarlet_hot_vec_d0_size(Cpu *c) {
    uint64_t impl = LD64(c->x[0] + 8);
    uint64_t begin = LD64(impl);
    uint64_t end = LD64(impl + 8);
    c->x[0] = (uint32_t)((end - begin) / 0xD0ull);
}

void hle_scarlet_hot_slot_c4(Cpu *c) {
    uint64_t impl = LD64(c->x[0] + 8);
    uint64_t base = LD64(impl);
    int64_t index = (int16_t)c->x[1];
    c->x[0] = LD32(base + index * 0xD0ll + 0xC4);
}

void hle_scarlet_hot_false(Cpu *c) {
    c->x[0] = 0;
}
