// Monotonic completion watermark; reset only while the Vulkan backend is idle.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
typedef struct { _Atomic uint64_t value; } ScarletCompletionCache;
static inline void scarlet_completion_reset(ScarletCompletionCache *c) {
    atomic_store_explicit(&c->value, 0, memory_order_relaxed);
}
static inline int scarlet_completion_covers(ScarletCompletionCache *c, uint64_t seq) {
    return seq <= atomic_load_explicit(&c->value, memory_order_acquire);
}
static inline void scarlet_completion_publish(ScarletCompletionCache *c, uint64_t seq) {
    uint64_t old = atomic_load_explicit(&c->value, memory_order_relaxed);
    while (old < seq && !atomic_compare_exchange_weak_explicit(
               &c->value, &old, seq, memory_order_release, memory_order_relaxed)) {}
}
