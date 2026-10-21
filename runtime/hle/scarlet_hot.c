// Scarlet 1.0.0 hot guest leaf functions replaced with direct host C.
//
// These addresses were selected from the slow-scene function-entry profile and
// verified against the exact main Build ID in tools/gen.py. They contain no
// callbacks, allocation or hidden side effects: just loads/arithmetic/constant
// returns. Keeping them here avoids lifted-fragment setup and dispatcher hops.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

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


// -----------------------------------------------------------------------------
// REA-validated runtime-only hot path.
//
// Guest 0x700270c4ac (Scarlet 1.0.0) is an intrusive/shared pointer assignment:
//   new = *x1
//   if (new) ++new->refcount(+0x28)
//   old = *(x0+0x38); *(x0+0x38) = new
//   if (old && --old->refcount == 0) tail-call old->vtable[+0x10]
//
// The guest uses LDAXR/STLXR; one ACQ_REL atomic RMW has the same ordering for
// the successful update and avoids the lifted exclusive-monitor loop. Invalid
// refcount states are rolled back and delegated to the exact lifted function.
#define SCARLET_REF_ASSIGN 0x700270c4acull

static _Atomic int g_refassign_first_hit;

static inline bool scarlet_guest_range(uint64_t p, uint64_t size) {
    return p >= GUEST_LO && p <= GUEST_HI && size <= GUEST_HI - p;
}

static void scarlet_ref_assign(Cpu *c) {
    uint64_t owner = c->x[0];
    uint64_t source = c->x[1];

    if (!scarlet_guest_range(owner, 0x40) || !scarlet_guest_range(source, 8)) {
        rt_call_original(c, SCARLET_REF_ASSIGN);
        return;
    }

    if (__builtin_expect(!atomic_load_explicit(&g_refassign_first_hit, memory_order_relaxed), 0) &&
        !atomic_exchange_explicit(&g_refassign_first_hit, 1, memory_order_relaxed))
        LOG("[scarlet-hot] 700270c4ac native ref-assign fast path active\n");

    uint64_t next = LD64(source);
    int32_t *next_rc = NULL;

    if (next) {
        if (!scarlet_guest_range(next, 0x2c)) {
            rt_call_original(c, SCARLET_REF_ASSIGN);
            return;
        }
        next_rc = (int32_t *)(uintptr_t)(next + 0x28);
        int32_t before = __atomic_fetch_add(next_rc, 1, __ATOMIC_ACQ_REL);
        if (__builtin_expect(before == -1, 0)) {
            __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            rt_call_original(c, SCARLET_REF_ASSIGN);
            return;
        }
    }

    uint64_t old = LD64(owner + 0x38);
    ST64(owner + 0x38, next);

    if (old) {
        if (!scarlet_guest_range(old, 0x2c)) {
            ST64(owner + 0x38, old);
            if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            rt_call_original(c, SCARLET_REF_ASSIGN);
            return;
        }

        int32_t *old_rc = (int32_t *)(uintptr_t)(old + 0x28);
        int32_t before = __atomic_fetch_sub(old_rc, 1, __ATOMIC_ACQ_REL);
        int32_t after = before - 1;

        if (__builtin_expect(before == 0, 0)) {
            // Put the state back exactly as it was before this fast path so the
            // lifted guest reproduces its own fatal/refcount handling.
            __atomic_fetch_add(old_rc, 1, __ATOMIC_ACQ_REL);
            ST64(owner + 0x38, old);
            if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            rt_call_original(c, SCARLET_REF_ASSIGN);
            return;
        }

        if (after == 0) {
            uint64_t vtbl = LD64(old);
            if (!scarlet_guest_range(vtbl, 0x18)) {
                // Restore the complete pre-call state before exact fallback.
                __atomic_fetch_add(old_rc, 1, __ATOMIC_ACQ_REL);
                ST64(owner + 0x38, old);
                if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
                rt_call_original(c, SCARLET_REF_ASSIGN);
                return;
            }
            uint64_t destroy = LD64(vtbl + 0x10);
            c->x[0] = old;
            c->pc = destroy;
            rt_dispatch(c);  // guest BR x1; preserve x30
            return;
        }
    }
}

void scarlet_hot_configure(void) {
    static bool configured;
    if (configured) return;
    configured = true;

    const char *e = getenv("LARECOMP_SCARLET_HOT");
    if (e && (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "false"))) {
        LOG("[scarlet-hot] disabled by LARECOMP_SCARLET_HOT=%s\n", e);
        return;
    }

    hle_override(SCARLET_REF_ASSIGN, scarlet_ref_assign, "scarlet_ref_assign");
    LOG("[scarlet-hot] registered 700270c4ac runtime-only override\n");
}
