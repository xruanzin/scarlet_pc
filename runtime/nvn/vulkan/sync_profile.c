// Inclusive wall times, not CPU utilization. Counters can overlap; never sum
// backend-present, queue ticket wait and their nested GPU/acquire/lock waits.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "sync_profile.h"

bool vk_sync_profile_enabled;
static _Atomic uint64_t elapsed[SYNC_METRIC_COUNT], calls[SYNC_METRIC_COUNT];
static const char *const names[SYNC_METRIC_COUNT] = {
    "big-lock", "present-gate", "interp-sleep", "frame-begin",
    "acquire", "queue-present", "backend-present", "guest-pace",
    "present-ticket", "gpu-wait"
};
void vk_sync_profile_init(void) {
    vk_sync_profile_enabled = getenv("LARECOMP_VK_PROF") != NULL;
}
void vk_sync_profile_add(VkSyncMetric metric, uint64_t ns) {
    atomic_fetch_add_explicit(&elapsed[metric], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&calls[metric], 1, memory_order_relaxed);
}
void vk_sync_profile_report(void) {
    if (!vk_sync_profile_enabled) return;
    char line[1024];
    int used = snprintf(line, sizeof line, "[sync-prof] inclusive wall times; overlapping scopes:");
    for (int i = 0; i < SYNC_METRIC_COUNT; i++) {
        uint64_t ns = atomic_exchange_explicit(&elapsed[i], 0, memory_order_relaxed);
        uint64_t n = atomic_exchange_explicit(&calls[i], 0, memory_order_relaxed);
        used += snprintf(line + used, sizeof line - (size_t)used, " %s=%.3fms/%llu",
            names[i], ns / 1e6, (unsigned long long)n);
    }
    // One stdio call prevents other threads' log lines splitting these fields.
    fprintf(stderr, "%s\n", line);
}
