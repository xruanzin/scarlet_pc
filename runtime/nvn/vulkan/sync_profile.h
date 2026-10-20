#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_timer.h>

typedef enum {
    SYNC_BIG_LOCK, SYNC_PRESENT_GATE, SYNC_INTERP_SLEEP, SYNC_FRAME_BEGIN,
    SYNC_ACQUIRE, SYNC_QUEUE_PRESENT, SYNC_BACKEND_PRESENT, SYNC_GUEST_PACE,
    SYNC_PRESENT_TICKET, SYNC_GPU_WAIT, SYNC_METRIC_COUNT
} VkSyncMetric;
extern bool vk_sync_profile_enabled;
void vk_sync_profile_init(void);
void vk_sync_profile_add(VkSyncMetric metric, uint64_t ns);
void vk_sync_profile_report(void);
static inline uint64_t vk_sync_profile_begin(void) {
    return vk_sync_profile_enabled ? SDL_GetTicksNS() : 0;
}
static inline void vk_sync_profile_end(VkSyncMetric metric, uint64_t start) {
    if (start) vk_sync_profile_add(metric, SDL_GetTicksNS() - start);
}
