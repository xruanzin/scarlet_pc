// F9 CPU-source snapshots of the textures actually bound by the captured frame.
// GPU-owned images are metadata-only: guest backing is not their current pixels.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nvn.h"
#include "../platform.h"

#define CAP_TEXTURES 128
#define CAP_BYTES (128ull << 20)
#define CAP_ONE (32ull << 20)
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t capture_frame = UINT64_MAX, capture_bytes;
static uint64_t capture_seen[CAP_TEXTURES];
static unsigned capture_count;
static char capture_dir[256];

static void capture_begin(uint64_t frame) {
    if (capture_frame == frame) return;
    capture_frame = frame; capture_count = 0; capture_bytes = 0;
    mkdir("build", 0755); mkdir("build/forensics-textures", 0755);
    snprintf(capture_dir, sizeof capture_dir, "build/forensics-textures/frame_%llu_%llu",
             (unsigned long long)frame, (unsigned long long)plat_now_ns());
    mkdir(capture_dir, 0755);
}

const char *nvn_diag_texture_capture_dir(uint64_t frame) {
    pthread_mutex_lock(&capture_lock);
    capture_begin(frame);
    pthread_mutex_unlock(&capture_lock);
    return capture_dir;
}

void nvn_diag_texture_source(const NvnTexture *t, bool gpu_owned, uint64_t frame) {
    if (!t) return;
    pthread_mutex_lock(&capture_lock);
    capture_begin(frame);
    for (unsigned i = 0; i < capture_count; i++)
        if (capture_seen[i] == t->debug_id) { pthread_mutex_unlock(&capture_lock); return; }
    if (capture_count == CAP_TEXTURES) { pthread_mutex_unlock(&capture_lock); return; }
    capture_seen[capture_count++] = t->debug_id;

    char dir[320], path[512];
    snprintf(dir, sizeof dir, "%s/texture_%llu", capture_dir, (unsigned long long)t->debug_id);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/texture.txt", dir);
    FILE *meta = fopen(path, "w");
    if (!meta) { pthread_mutex_unlock(&capture_lock); return; }
    fprintf(meta, "id=%llu\naddr=%llx\nformat=%x\nwidth=%u\nheight=%u\ndepth=%u\nlayers=%u\nlevels=%u\nbh=%u\nbd=%u\nflags=%x\nstride=%llu\ngpu_owned=%d\n",
            (unsigned long long)t->debug_id, (unsigned long long)t->addr, t->format,
            t->width, t->height, t->depth, t->layout.layers, t->levels,
            t->layout.block_height_log2, t->layout.block_depth_log2, t->flags,
            (unsigned long long)t->stride, gpu_owned);
    fprintf(meta, "target=%u\nswizzle=%u,%u,%u,%u\n", t->target,
            t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3]);
    const char *status = "no-valid-pool-range";
    uint64_t written = 0;
    if (gpu_owned) status = "gpu-owned-not-current-in-cpu-backing";
    else if (t->pool && t->pool->storage && t->addr >= t->pool->storage &&
             t->addr - t->pool->storage < t->pool->size && t->layout.fmt && t->layout.fmt->bpb) {
        uint64_t available = t->pool->size - (t->addr - t->pool->storage);
        uint64_t needed = tegra_total_size(&t->layout);
        if (t->flags & 0x10) {
            // Record only the base image's CPU row span for pitch-linear data.
            needed = t->height && t->stride <= UINT64_MAX / t->height ? t->stride * t->height : UINT64_MAX;
        }
        status = "incomplete-pool-range";
        if (needed && needed <= available) {
            // Include padding for alternative level-0 block-height candidates,
            // but never read beyond the actual owning pool or capture budgets.
            TexLayout candidate = t->layout; candidate.block_height_log2 = 5;
            uint64_t padding = tegra_level(&candidate, 1).offset;
            uint64_t size = needed > padding ? needed : padding;
            if (size > available) size = available;
            status = "capture-budget";
            if (size <= CAP_ONE && size <= CAP_BYTES - capture_bytes) {
                snprintf(path, sizeof path, "%s/%llx_%ux%u_f%x_bh%u.bin", dir,
                         (unsigned long long)t->addr, t->width, t->height, t->format,
                         t->layout.block_height_log2);
                FILE *raw = fopen(path, "wb");
                status = "open-failed";
                if (raw) {
                    bool ok = fwrite((const void *)(uintptr_t)t->addr, 1, (size_t)size, raw) == size;
                    if (fclose(raw)) ok = false;
                    if (ok) { written = size; capture_bytes += size; status = "saved-cpu-source"; }
                    else { remove(path); status = "write-failed"; }
                }
            }
        }
        fprintf(meta, "pool_remaining=%llu\nrequired_bytes=%llu\n", (unsigned long long)available, (unsigned long long)needed);
    }
    fprintf(meta, "status=%s\ncaptured_bytes=%llu\n", status, (unsigned long long)written);
    fclose(meta);
    pthread_mutex_unlock(&capture_lock);
}
