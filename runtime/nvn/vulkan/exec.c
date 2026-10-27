// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_thread.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../kernel.h"
#include "../../platform.h"
#include "shaders.h"
#include "storage_ranges.h"
#include "../indirect.h"
#include "vk.h"
#include "scarlet_temporal.h"
#include "../renderer.h"
#include "../../settings.h"

static uint64_t g_draws_total;  // (never reset: LARECOMP_COUNTER_LOG)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_tf_merged, stat_tf_barriers, stat_raster_discard_sets, stat_raster_discard_on, stat_a2c_1x;
static uint64_t stat_tf_dep_vbuf, stat_tf_dep_cbuf, stat_tf_dep_sbuf, stat_tf_dep_gmem, stat_tf_dep_op, stat_tf_dep_submit, stat_tf_dep_rt;
static uint64_t stat_barrier_flags[256];
static uint64_t stat_vkbar_full, stat_vkbar_graphics, stat_vkbar_compute, stat_vkbar_transfer, stat_vkbar_upload;
static uint64_t forensic_barriers_since_draw;
static uint32_t forensic_last_barrier_flags;
static uint32_t stat_pass_end_line[4096];
static bool tf_barrier_pending;
static uint64_t tf_pending_addr[64], tf_pending_size[64];
static uint32_t tf_pending_n;
static void tf_flush_pending_barrier(void);
// Skipped draws by reason (logged with the stats): 0 no program bound, 1 program still compiling, 2 program failed,
// 3 no program object, 4 primitive mode, 5 pipeline still building, 6 pipeline failed.
static uint64_t stat_skip_why[8];
uint64_t g_skin_watch;  // (debug, LARECOMP_DUMP_SKIN: a vertex cbuf watched across frames)
// docs/PLANO-3D.md experiments, logged with the stats: [0] palettes replaced by identity, [1] skinned-draw cbufs
// read in place (UBO_DIRECT), [2] skinned-draw cbufs UBO_DIRECT could not place (copied), [3] one-bone shaders.
static uint64_t g_exp_count[4];
#define SKIP(why) (stat_skipped++, stat_skip_why[(why) & 7]++)
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static _Atomic int jobs_pending;  // queued + running jobs, independent of completed-build statistics
bool vk_builds_pending(void) { return atomic_load(&jobs_pending) != 0; }
static _Atomic uint64_t stat_translate_ns, stat_spirv_ns, stat_pipe_ns;  // CPU time translating / compiling programs
static uint64_t stat_shader_wait_ns;  // draws waiting for their program / pipeline (wait_built)
static _Atomic int new_pipes;  // pipelines created since the pipeline cache was last saved
static uint64_t op_counts[CMD_COUNT];
static int op_counts_enabled = -1;
bool g_debug_frame;
static char stats_text[768];
void vk_collect_dead(void);
static void pipe_history_load(void);
void vk_pipe_history_save(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define MAX_QUEUE_LISTS 16
// Two batches of slots: recording a full batch must never wrap to one of its
// already-ended, not-yet-submitted command buffers.
#define SUBS (MAX_QUEUE_LISTS * 2)
#define DESC_BATCH 16
#define DESC_LAYOUT_CACHE 8
typedef struct {
    VkDescriptorSetLayout layout;
    VkDescriptorSet set[DESC_BATCH];
    uint8_t next, count;
} DescBatch;

typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
    DescBatch desc_cache[DESC_LAYOUT_CACHE];
    uint8_t desc_replace;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last physically submitted timeline value
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

// Each list can contribute an upload and a main command buffer.
#define QBATCH_MAX_CBS (MAX_QUEUE_LISTS * 2)
_Static_assert(SUBS > MAX_QUEUE_LISTS, "queue batch must not wrap the slot ring");
static VkCommandBufferSubmitInfo qbatch_cbs[QBATCH_MAX_CBS];
static uint32_t qbatch_ncb, qbatch_lists;
static uint64_t stat_logical_submits, stat_queue_batches;
static uint64_t stat_desc_sets, stat_desc_alloc_calls;

static int queue_batch_limit(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("LARECOMP_QUEUE_BATCH");
        n = e ? atoi(e) : 16;
        if (n < 1) n = 1;
        if (n > MAX_QUEUE_LISTS) n = MAX_QUEUE_LISTS;
        // Keep guest lists as distinct command buffers, but amortize queue submission
        // across sixteen lists. Scarlet still records thousands of logical lists in a
        // short window; batching 8 cut submit count roughly in half on the Ryzen 4600G,
        // so extend that same ordered batching without changing guest command order.
    }
    return n;
}

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        memset(s->desc_cache, 0, sizeof s->desc_cache);
        s->desc_replace = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

static void vk_memory_barrier_scoped(VkCommandBuffer cb,
                                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                     uint64_t *counter) {
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage, .srcAccessMask = src_access,
        .dstStageMask = dst_stage, .dstAccessMask = dst_access
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &mb
    };
    vkCmdPipelineBarrier2(cb, &di);
    if (counter) (*counter)++;
}

void vk_full_barrier(VkCommandBuffer cb) {
    vk_memory_barrier_scoped(cb,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        &stat_vkbar_full);
}

// Pass-to-pass hazards in Scarlet are overwhelmingly attachment/texture/SSBO
// dependencies. Keep transfer/compute producers visible, but do not stall host,
// present, query and unrelated pipeline stages with ALL_COMMANDS -> ALL_COMMANDS.
static void vk_graphics_barrier(VkCommandBuffer cb) {
    VkPipelineStageFlags2 src =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 src_access =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (g_vk.transform_feedback) {
        src |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        src_access |= VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
    }
    VkPipelineStageFlags2 dst =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 dst_access =
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_2_INDEX_READ_BIT |
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_2_UNIFORM_READ_BIT |
        VK_ACCESS_2_SHADER_READ_BIT |
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vk_memory_barrier_scoped(cb, src, src_access, dst, dst_access, &stat_vkbar_graphics);
}

static void vk_compute_barrier(VkCommandBuffer cb) {
    VkPipelineStageFlags2 src =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 src_access =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (g_vk.transform_feedback) {
        src |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        src_access |= VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
    }
    vk_memory_barrier_scoped(cb, src, src_access,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        &stat_vkbar_compute);
}

static void vk_transfer_barrier(VkCommandBuffer cb) {
    VkPipelineStageFlags2 src =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 src_access =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (g_vk.transform_feedback) {
        src |= VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        src_access |= VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
    }
    vk_memory_barrier_scoped(cb, src, src_access,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        &stat_vkbar_transfer);
}

static void vk_upload_visibility_barrier(VkCommandBuffer cb) {
    vk_memory_barrier_scoped(cb,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_2_INDEX_READ_BIT |
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_2_UNIFORM_READ_BIT |
        VK_ACCESS_2_SHADER_READ_BIT |
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_READ_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        &stat_vkbar_upload);
}

static void ring_retire(void);
static void flush_transient_writes(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, 512);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        mkdir("build", 0755);
        FILE *pf = fopen("build/scarlet_gpuprof_latest.txt", "w");
        if (pf) fprintf(pf, "# %d marks, %.3f ms first-to-last\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
            if (pf) fprintf(pf, "%6.3f ms %5.1f%%  %s%s%u\n", ms, 100 * ms / (total > 0 ? total : 1),
                            gp_label[i], gp_draws[i] ? " draws=" : "", gp_draws[i]);
        }
        if (pf) fclose(pf);
    }
    gp_n = 0;
}

// Finish the current list into the queue batch, but do not submit it yet.
// The command buffers stay distinct and are appended in exact guest order:
// upload0, main0, upload1, main1, ...
static bool qbatch_finish_current(bool force_main) {
    // Reserve for the worst-case upload/main pair before writing either entry.
    if (qbatch_ncb > QBATCH_MAX_CBS - 2)
        FATAL("vk: queue batch capacity exhausted before list finalization\n");
    vk_end_pass();

    // Do not force a transform-feedback memory barrier merely because one
    // command buffer/list ends. Commands submitted to the same Vulkan queue
    // remain ordered across submissions, and tf_pending_used_by_draw()/the
    // explicit copy/compute/RT paths insert the visibility barrier at the first
    // real consumer. Flushing here created one barrier per queue batch even when
    // the captured stream was never read before being overwritten again.
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !force_main) return false;

    if (s->upload_begun) {
        vk_upload_visibility_barrier(s->upload);
        VK_CHECK(vkEndCommandBuffer(s->upload));
        if (qbatch_ncb >= QBATCH_MAX_CBS) FATAL("vk: queue batch command-buffer overflow\n");
        qbatch_cbs[qbatch_ncb++] = (VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }

    if (force_main && !s->main_begun) vk_cmd();
    if (s->main_begun) {
        if (s->ts)
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 prof_qp, (uint32_t)cur_sub * 2 + 1);
        VK_CHECK(vkEndCommandBuffer(s->main));
        if (qbatch_ncb >= QBATCH_MAX_CBS) FATAL("vk: queue batch command-buffer overflow\n");
        qbatch_cbs[qbatch_ncb++] = (VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    }

    qbatch_lists++;
    stat_logical_submits++;
    cur_sub = (cur_sub + 1) % SUBS;
    return true;
}

static void qbatch_submit(VkSemaphore wait, VkSemaphore signal) {
    if (!qbatch_ncb && !wait && !signal) return;

    // A semaphore-only submit still needs no command buffer, which Vulkan 1.3
    // permits. Normal present always has the frame command buffer here.
    static _Atomic(uintptr_t) submit_owner;
    static _Atomic(unsigned) submit_active;
    static _Atomic(uint64_t) submit_overlap_count;
    static int submit_diag = -1;
    if (submit_diag < 0) submit_diag = getenv("LARECOMP_VK_THREAD_DIAG") ? 1 : 0;
    uintptr_t submit_me = 0;
    unsigned submit_before = 0;
    if (submit_diag) {
        submit_me = (uintptr_t)pthread_self();
        submit_before = atomic_fetch_add_explicit(&submit_active, 1, memory_order_acq_rel);
        uintptr_t submit_prev = atomic_load_explicit(&submit_owner, memory_order_acquire);
        if (!submit_before) atomic_store_explicit(&submit_owner, submit_me, memory_order_release);
        else if (submit_prev != submit_me) {
            uint64_t n = atomic_fetch_add_explicit(&submit_overlap_count, 1, memory_order_relaxed) + 1;
            if (n <= 64 || !(n & 255))
                LOG("[vk-thread] !!! overlapping queue batch #%llu: tid=%llx owner=%llx active=%u frame=%llu !!!\n",
                    (unsigned long long)n, (unsigned long long)submit_me,
                    (unsigned long long)submit_prev, submit_before + 1,
                    (unsigned long long)vk_frame_counter());
        }
    }

    const uint64_t seq = submit_seq + 1;
    VkSemaphoreSubmitInfo sig[2] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
         .value = seq, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = qbatch_ncb, .pCommandBufferInfos = qbatch_cbs,
        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};

    {
        PROF_T0();
        dl_note("queue submit %llu: %u lists, %u command buffers",
                (unsigned long long)seq, qbatch_lists, qbatch_ncb);
        flush_transient_writes();
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }

    pthread_mutex_lock(&seq_lock);
    submit_seq = seq;
    pthread_mutex_unlock(&seq_lock);
    stat_flushes++;
    stat_queue_batches++;
    cum_submits++;
    qbatch_ncb = qbatch_lists = 0;
    ring_retire();
    vk_collect_dead();

    if (submit_diag) {
        unsigned left = atomic_fetch_sub_explicit(&submit_active, 1, memory_order_acq_rel);
        if (left == 1) atomic_store_explicit(&submit_owner, 0, memory_order_release);
    }
}

// Immediate submit path used by presentation, explicit flushes and CPU-visible
// synchronization. Any deferred list is included before the current work.
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    qbatch_finish_current(wait || signal);
    qbatch_submit(wait, signal);
}

// List-end path: keep each list in its own CB, but amortize the queue call.
static void renderer_submit_list(void) {
    if (!qbatch_finish_current(false)) return;
    if ((int)qbatch_lists >= queue_batch_limit()) qbatch_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void renderer_submit(void) {
    qbatch_finish_current(false);
    qbatch_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    renderer_submit();  // QueueFinish must include deferred list-end work.
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (qbatch_ncb || subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    if (seq > s && (qbatch_ncb || subs[cur_sub].main_begun || subs[cur_sub].upload_begun)) {
        renderer_submit();
        pthread_mutex_lock(&seq_lock);
        s = submit_seq;
        pthread_mutex_unlock(&seq_lock);
    }
    vk_wait_seq(seq > s ? s : seq);
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();

    // Reuse a preallocated set for this layout before entering the Vulkan
    // driver again. Vertex/fragment layouts repeat thousands of times in a
    // Scarlet frame, so batching turns up to 16 vkAllocateDescriptorSets calls
    // into one without changing descriptor contents or lifetime.
    DescBatch *slot = NULL;
    for (int i = 0; i < DESC_LAYOUT_CACHE; i++) {
        DescBatch *d = &s->desc_cache[i];
        if (d->layout == layout) {
            if (d->next < d->count) {
                stat_desc_sets++;
                return d->set[d->next++];
            }
            slot = d;
            break;
        }
        if (!slot && !d->layout) slot = d;
    }
    if (!slot) {
        slot = &s->desc_cache[s->desc_replace++ % DESC_LAYOUT_CACHE];
        slot->layout = VK_NULL_HANDLE;
        slot->next = slot->count = 0;
    }

    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32768}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }

        VkDescriptorSetLayout layouts[DESC_BATCH];
        for (int i = 0; i < DESC_BATCH; i++) layouts[i] = layout;
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = s->pools[s->cur_pool],
            .descriptorSetCount = DESC_BATCH,
            .pSetLayouts = layouts
        };
        stat_desc_alloc_calls++;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, slot->set);
        if (r == VK_SUCCESS) {
            slot->layout = layout;
            slot->next = 1;
            slot->count = DESC_BATCH;
            stat_desc_sets++;
            return slot->set[0];
        }
        // A batch may not fit at the tail of an otherwise usable pool. Move
        // on exactly as the old single-set allocator did on allocation failure.
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// Keep the host-visible staging pool bounded. Character creation can upload hundreds of MB/s;
// letting differently-sized or GPU-backlogged chunks accumulate eventually exhausted the AMD APU's
// host-visible Vulkan heap even though old submissions were already recyclable.
#define CHUNK_SIZE (32ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;
static uint64_t transient_bytes;

static uint64_t transient_cap(void) {
    static uint64_t cap;
    if (!cap) {
        const char *e = getenv("LARECOMP_TRANSIENT_MB");
        long mb = e ? atol(e) : 384;
        if (mb < 128) mb = 128;
        cap = (uint64_t)mb << 20;
    }
    return cap;
}

static void flush_transient_writes(void) {
    uint64_t seq = vk_pending_seq();
    for (int i = 0; i < nchunks; i++)
        if (chunks[i].buf && chunks[i].seq == seq && chunks[i].used)
            vmaFlushAllocation(g_vk.vma, chunks[i].mem, 0, chunks[i].used);
}

static void drop_chunk(int i) {
    Chunk *c = &chunks[i];
    if (!c->buf) return;
    transient_bytes -= c->size;
    vmaDestroyBuffer(g_vk.vma, c->buf, c->mem);
    memset(c, 0, sizeof *c);
    if (active == i) active = -1;
}

static int new_chunk(uint64_t size) {
    int slot = -1;
    for (int i = 0; i < nchunks; i++) if (!chunks[i].buf) { slot = i; break; }
    if (slot < 0) {
        chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
        slot = nchunks++;
    }
    Chunk *c = &chunks[slot];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    if (r != VK_SUCCESS) {
        memset(c, 0, sizeof *c);
        FATAL("transient vmaCreateBuffer %llu MB failed: %d (pool %llu/%llu MB)\n",
              (unsigned long long)(size >> 20), r,
              (unsigned long long)(transient_bytes >> 20),
              (unsigned long long)(transient_cap() >> 20));
    }
    c->ptr = info.pMappedData;
    c->size = size;
    transient_bytes += size;
    return slot;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }

    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].buf && chunks[i].seq <= done && chunks[i].size >= size) {
            pick = i;
            break;
        }

    if (pick < 0) {
        uint64_t alloc = size > CHUNK_SIZE ? size : CHUNK_SIZE;
        uint64_t cap = transient_cap();

        // Completed chunks too small for this request are dead weight. Drop them before growing the pool.
        for (int i = 0; i < nchunks && transient_bytes + alloc > cap; i++)
            if (i != active && chunks[i].buf && chunks[i].seq <= done) drop_chunk(i);

        // If the GPU is behind, wait for the oldest already-submitted chunk instead of allocating forever.
        while (transient_bytes + alloc > cap) {
            int oldest = -1;
            uint64_t oldest_seq = UINT64_MAX;
            for (int i = 0; i < nchunks; i++) {
                if (i == active || !chunks[i].buf) continue;
                if (chunks[i].seq && chunks[i].seq < pending && chunks[i].seq < oldest_seq) {
                    oldest = i;
                    oldest_seq = chunks[i].seq;
                }
            }
            if (oldest < 0) break;  // current submission alone is unusually large; it cannot be recycled yet
            static uint64_t pressure_waits;
            uint64_t n = ++pressure_waits;
            if (n <= 8 || !(n & 255))
                LOG("[vk] transient pressure #%llu: %llu MB resident, waiting seq %llu (cap %llu MB)\n",
                    (unsigned long long)n, (unsigned long long)(transient_bytes >> 20),
                    (unsigned long long)oldest_seq, (unsigned long long)(cap >> 20));
            vk_wait_seq(oldest_seq);
            done = vk_completed_seq();
            for (int i = 0; i < nchunks && transient_bytes + alloc > cap; i++)
                if (i != active && chunks[i].buf && chunks[i].seq <= done) drop_chunk(i);
        }
        pick = new_chunk(alloc);
    }

    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// Builds run on workers. The current bounded-wait policy can defer dependent operations after its limit.
// A first-use atlas/cache write or compute dispatch may not be repeated by the game; reducing compilation
// latency remains necessary, because the caller's skipped operation is not automatically replayed.
// LARECOMP_SYNC_SHADERS=1 builds inline (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    uint64_t tid = plat_register_thread();
    LOG("[shader] worker os tid %llu ready\n", (unsigned long long)tid);
    // Shader compilation is background work. Keeping these workers below the
    // guest/render/audio threads prevents cold-cache compilation from stalling
    // gameplay for seconds on 6c/12t APUs.
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        atomic_fetch_sub(&jobs_pending, 1);
        free(j);
    }
    return NULL;
}

static uint64_t shader_wait_limit_ns(void);

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation runs on the workers too, then compiles on the same worker (translate_program). It had one
// dedicated thread (the translator used a static string ring, now per thread): on a first visit, ~380 programs took
// ~240 ms one after another while the workers idled. Off the game thread either way (up to ~90 ms per frame there).
static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    static int worker_count;
    if (!started) {
        started = true;
        // Leave substantial CPU headroom for the recompiled game. The old
        // cores-4 policy spawned 8 compiler workers on a 12-thread Ryzen 4600G,
        // starving game/audio threads and turning shader bursts into multi-second hitches.
        int cores = SDL_GetNumLogicalCPUCores();
        int n = cores <= 12 ? 2 : 3;  // >=2: one very slow compile must not block every other shader behind it
        for (int i = 0; i < n; i++) {
            pthread_t t;
            int error = pthread_create(&t, NULL, worker, NULL);
            if (!error) { worker_count++; pthread_detach(t); }
            else LOG("[shader] worker creation failed: %d\n", error);
        }
        LOG("[shader] %d low-priority workers on %d logical CPUs\n", worker_count, cores);
        LOG("[shader] wait limit %d ms then skip (LARECOMP_SHADER_WAIT_MS); translated AMD pipelines use fast creation\n", (int)(shader_wait_limit_ns() / 1000000ull));
    }
    if (!worker_count) { fn(arg); return; }  // no job may wait forever if the OS cannot start a worker
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    atomic_fetch_add(&jobs_pending, 1);
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// SDL events are pumped by device.c's separate UI thread. Never re-enter Vulkan presentation here: the current
// command list and its render pass still belong to this thread. Sleep until the build finishes or the wait limit.
static uint64_t shader_wait_limit_ns(void) {
    static uint64_t v;
    if (!v) {
        const char *e = getenv("LARECOMP_SHADER_WAIT_MS");
        long ms = e ? atol(e) : 0;
        v = (ms > 0 ? (uint64_t)ms : 30000ull) * 1000000ull;
    }
    return v;
}

// A pipeline/program that is still compiling after the limit is NOT fatal: the draw (callers check state != 1) is
// skipped and the game keeps running while the driver finishes in the background. Killing the process here made
// Scarlet "not start": its first large fragment shader (~140k SPIR-V words) takes the AMD driver far longer than
// 8 s, and nothing else ever got to run. LARECOMP_SHADER_TIMEOUT_EXIT=1 restores the old behaviour for debugging.
static void wait_built(_Atomic int *state, const char *kind, uint64_t code0, uint64_t code1) {
    if (atomic_load(state)) return;
    static __thread _Atomic int *given_up[16];
    static __thread unsigned ngiven_up;
    for (unsigned i = 0; i < 16 && i < ngiven_up; i++)
        if (given_up[i] == state) return;  // already waited the full limit once: don't stall every frame again
    uint64_t limit = shader_wait_limit_ns();
    uint64_t start = SDL_GetTicksNS(), report = start + 1000000000ull;
    while (!atomic_load(state)) {
        SDL_Delay(1);
        uint64_t now = SDL_GetTicksNS();
        if (!atomic_load(state) && now - start >= limit) {
            LOG("[shader-slow] %s %llx/%llx still compiling after %.1f s: skipping draws that use it until it is ready\n",
                kind, (unsigned long long)code0, (unsigned long long)code1, (now - start) / 1e9);
            if (getenv("LARECOMP_SHADER_TIMEOUT_EXIT")) {
                fflush(stderr);
                plat_emergency_exit(86);
            }
            given_up[ngiven_up++ & 15] = state;
            stat_shader_wait_ns += now - start;
            return;
        }
        if (now >= report) {
            LOG("[shader-wait] %s %llx/%llx: %.1f s, jobs=%d (waiting for build)\n", kind,
                (unsigned long long)code0, (unsigned long long)code1, (now - start) / 1e9,
                atomic_load(&jobs_pending));
            report = now + 1000000000ull;
        }
    }
    uint64_t elapsed = SDL_GetTicksNS() - start;
    stat_shader_wait_ns += elapsed;
    if (elapsed >= 1000000000ull)
        LOG("[shader-wait] %s ready=%d after %.1f s\n", kind, atomic_load(state) == 1, elapsed / 1e9);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

// Extension commands are not exported by vulkan-1.lib on Windows. Resolve them
// from the device once, as required for device extension entry points.
static PFN_vkCmdBindTransformFeedbackBuffersEXT tf_bind_buffers;
static PFN_vkCmdBeginTransformFeedbackEXT tf_begin;
static PFN_vkCmdEndTransformFeedbackEXT tf_end;
static bool tf_api_ready(void) {
    static int loaded;
    if (!loaded) {
        loaded = 1;
        tf_bind_buffers = (PFN_vkCmdBindTransformFeedbackBuffersEXT)
            vkGetDeviceProcAddr(g_vk.dev, "vkCmdBindTransformFeedbackBuffersEXT");
        tf_begin = (PFN_vkCmdBeginTransformFeedbackEXT)
            vkGetDeviceProcAddr(g_vk.dev, "vkCmdBeginTransformFeedbackEXT");
        tf_end = (PFN_vkCmdEndTransformFeedbackEXT)
            vkGetDeviceProcAddr(g_vk.dev, "vkCmdEndTransformFeedbackEXT");
        if (!tf_bind_buffers || !tf_begin || !tf_end) {
            LOG("[vk] transform feedback entry points unavailable; disabling TF\n");
            g_vk.transform_feedback = false;
        }
    }
    return g_vk.transform_feedback && tf_bind_buffers && tf_begin && tf_end;
}

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16], tfbuf[4];
    bool tf_active, tf_vk_active, tf_resume;
    uint64_t tf_counter;
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    bool raster_discard;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    uint64_t obj;       // frame interpolation: the game object the current draws belong to (CMD_OBJECT), 0 = none
    uint32_t obj_draw;  // draws of that object so far in this run of it
} st;

static bool range_overlap_u64(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    if (!a || !as || !b || !bs) return false;
    return a < b + bs && b < a + as;
}

static void tf_pending_add(uint64_t addr, uint64_t size) {
    if (!addr || !size) return;
    uint64_t lo = addr, hi = addr + size;
    if (hi < lo) hi = UINT64_MAX;

    // Coalesce transitively. A new range can bridge two older TF writes; the
    // previous first-match implementation left the second range behind, which
    // inflated dependency checks/barriers in Scarlet's stream-output bursts.
    for (uint32_t i = 0; i < tf_pending_n;) {
        uint64_t rlo = tf_pending_addr[i], rhi = rlo + tf_pending_size[i];
        if (rhi < rlo) rhi = UINT64_MAX;
        if (lo <= rhi && rlo <= hi) {
            if (rlo < lo) lo = rlo;
            if (rhi > hi) hi = rhi;
            tf_pending_n--;
            tf_pending_addr[i] = tf_pending_addr[tf_pending_n];
            tf_pending_size[i] = tf_pending_size[tf_pending_n];
            continue;
        }
        i++;
    }

    if (tf_pending_n < 64) {
        tf_pending_addr[tf_pending_n] = lo;
        tf_pending_size[tf_pending_n] = hi - lo;
        tf_pending_n++;
    } else {
        // Saturation is rare; collapse everything into one conservative range.
        for (uint32_t i = 0; i < tf_pending_n; i++) {
            uint64_t rlo = tf_pending_addr[i], rhi = rlo + tf_pending_size[i];
            if (rhi < rlo) rhi = UINT64_MAX;
            if (rlo < lo) lo = rlo;
            if (rhi > hi) hi = rhi;
        }
        tf_pending_addr[0] = lo;
        tf_pending_size[0] = hi - lo;
        tf_pending_n = 1;
    }
    tf_barrier_pending = true;
}

static bool tf_pending_overlaps_range(Range r) {
    for (uint32_t i = 0; i < tf_pending_n; i++)
        if (range_overlap_u64(r.addr, r.size, tf_pending_addr[i], tf_pending_size[i])) return true;
    return false;
}

static bool tf_pending_used_by_draw(const ShaderInfo *vi, const ShaderInfo *fi) {
    // Only bindings that the currently translated shaders can actually read
    // create a TF dependency.  The old conservative test scanned every stale
    // NVN binding, so a TF output left bound in an unused vertex slot forced a
    // barrier before virtually every following TF draw.
    uint32_t used_streams = 0;
    if (vi) {
        for (int aidx = 0; aidx < 32; aidx++) {
            if (!(vi->attr_mask & (1u << aidx))) continue;
            if (aidx >= st.nattrib) continue;
            uint32_t a = st.attrib[aidx];
            uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F;
            if (fmt && stream < 16) used_streams |= 1u << stream;
        }
    }
    for (int stream = 0; stream < 16; stream++)
        if ((used_streams & (1u << stream)) && tf_pending_overlaps_range(st.vbuf[stream])) { stat_tf_dep_vbuf++; return true; }

    const ShaderInfo *si[2] = {vi, fi};
    for (int stage = 0; stage < 2; stage++) {
        if (!si[stage]) continue;
        uint32_t cm = si[stage]->cbuf_mask;
        for (int i = 0; i < 18; i++)
            if ((cm & (1u << i)) && tf_pending_overlaps_range(st.ubuf[stage][i])) { stat_tf_dep_cbuf++; return true; }

        // storage_buffers means the translated shader may resolve LDG/STG via
        // the bound NVN storage ranges; check those ranges.  Native imported
        // gmem access is conservatively synchronized because its address is
        // computed dynamically by the guest shader.
        if (si[stage]->storage_buffers) {
            for (int i = 0; i < 16; i++)
                if (tf_pending_overlaps_range(st.sbuf[stage][i])) { stat_tf_dep_sbuf++; return true; }
        }
        if (si[stage]->uses_gmem) { stat_tf_dep_gmem++; return true; }
    }
    return false;
}

static void tf_pending_clear(void) {
    tf_barrier_pending = false;
    tf_pending_n = 0;
}

static void tf_flush_pending_barrier(void) {
    if (!tf_barrier_pending) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    // Stream-output only needs its writes made visible to later consumers.
    // A global ALL_COMMANDS write/read+write barrier serialized unrelated color,
    // depth and transfer work and became very expensive in Scarlet's TF-heavy
    // scenes. Keep the destination broad (the consumer can be vertex/UBO/SSBO/
    // indirect guest memory), but scope the producer to TF writes and do not
    // unnecessarily order unrelated destination writes.
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
        .srcAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb
    };
    vkCmdPipelineBarrier2(cb, &di);
    tf_pending_clear();
    stat_tf_barriers++;
}

// frame interpolation (prototype, LARECOMP_INTERP=1): the hooks below; the rest at the end of this file
static int ip_on = -1;
static bool ip_replaying;     // running a recorded frame again for the in-between image
static uint64_t ip_stats_tagged, ip_stats_untagged;  // draws with / without a game object (CMD_OBJECT)
static uint64_t ip_draw_sig;  // the current draw's identity (draw_)
static bool ip_draw_obj;      // ... and whether it belongs to a game object (CMD_OBJECT): only those are blended
static bool ip_draw_2d;       // ... a 2D draw (no depth test): interface, menus
static bool ip_enabled(void);
bool vk_interp_enabled(void) { return ip_enabled(); }  // (cmdbuf.c: hooks the game's object draw functions)
static bool ip_enabled(void) {
    // The 60 fps mode (runtime/hle/fps60.c): the world at 60 natively; menus and the Pokédex at the game's 30 with the
    // in-between image; battles and captures at 30 without it (tried, left out: docs/HISTORICO.md 46).
    // LARECOMP_INTERP=1 (research): always on.
    if (ip_on == -1) ip_on = getenv("LARECOMP_INTERP") ? atoi(getenv("LARECOMP_INTERP")) : 0;
    extern bool fps60_menu(void);
    return ip_on > 0 || fps60_menu();
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size);
static void ip_note_list(uint64_t handle);
static bool ip_skip_op(uint32_t op) {  // what the replay must not do again: it writes guest memory or signals the game
    switch (op) {
    case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT: case CMD_COPY_BUFFER_TO_BUFFER: case CMD_COPY_TEXTURE_TO_BUFFER:
    case CMD_UPDATE_UNIFORM_BUFFER: case CMD_FENCE_SYNC: case CMD_WAIT_SYNC: case CMD_SIGNAL_EVENT: case CMD_WAIT_EVENT:
    case CMD_REPORT_COUNTER: case CMD_CLEAR_BUFFER:
    case CMD_COPY_BUFFER_TO_TEXTURE:  // (uploads: done when the frame ran; again, they overran scaled images)
        return true;
    default:
        return false;
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();

    // Optional complete frame timeline for the performance bundle. Keep this
    // inside the already opt-in Vulkan profiler so normal play has zero file
    // I/O. Buffered flushing limits diagnostic overhead; a hard crash loses at
    // most roughly the last second of rows.
    static FILE *csv;
    static bool csv_checked;
    static uint64_t csv_flush, previous_frame;
    if (!csv_checked) {
        csv_checked = true;
        const char *path = getenv("LARECOMP_FRAME_CSV");
        if (path && *path) {
            csv = fopen(path, "w");
            if (csv) {
                fprintf(csv, "frame,host_ms,frame_ms,backend_ms,gpu_wait_ms,present_ms,other_ms,draws,submits,shader_jobs\n");
                fflush(csv);
            } else LOG("[profile] cannot open frame CSV: %s\n", path);
        }
    }
    if (frame <= previous_frame) last_t = 0; // in-process reboot
    previous_frame = frame;
    if (csv && last_t) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        fprintf(csv, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%d\n",
                (unsigned long long)frame, now / 1e6, dt / 1e6, ex / 1e6, wt / 1e6,
                present_ns / 1e6, ((double)dt - ex - present_ns) / 1e6,
                (unsigned long long)(cum_draws - l_draws),
                (unsigned long long)(cum_submits - l_subs), atomic_load(&jobs_pending));
        if (now - csv_flush >= 1000000000ull) { fflush(csv); csv_flush = now; }
        if (ferror(csv)) {
            LOG("[profile] frame CSV write failed; collection disabled\n");
            fclose(csv); csv = NULL;
        }
    }

    if (last_t && now - last_t > 50000000) {
        static uint64_t hitch_count;
        static int hitch_all = -1;
        if (hitch_all < 0) hitch_all = getenv("LARECOMP_HITCH_LOG_ALL") != NULL;
        uint64_t hn = ++hitch_count;
        if (hitch_all || hn <= 8 || !(hn % 120)) {
            uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
            LOG("[hitch] #%llu frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
                "%llu submits, shader jobs %d pending\n", (unsigned long long)hn, (unsigned long long)frame,
                dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
                ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws),
                (unsigned long long)(cum_submits - l_subs), atomic_load(&jobs_pending));
        }
    }
    if (last_t) {
        extern void fnprof_note_frame(uint64_t frame, double frame_ms);
        fnprof_note_frame(frame, (now - last_t) / 1e6);
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static void save_pipeline_cache(void *a) {
    (void)a;
    uint64_t t0 = SDL_GetTicksNS();
    vk_pipe_history_save();
    vk_pipeline_cache_save();
    LOG("[vk] pipeline cache saved (%.0f ms)\n", (SDL_GetTicksNS() - t0) / 1e6);
}

const char *renderer_stats(void) {
    if (getenv("LARECOMP_HLE_PROF")) rt_hle_profile_report();
    // The pipeline cache was written only at a clean exit: a crash or a killed process lost every pipeline built in
    // the session, and the next one popped in again. Save it on a worker once a burst of new pipelines settles
    // (no new ones for ~6 s: entering an area builds them for a few seconds).
    static int quiet, last_new;
    int np = atomic_load(&new_pipes);
    if (np && np == last_new) {
        if (++quiet >= 3) { atomic_fetch_sub(&new_pipes, np); np = 0; quiet = 0; run_job(save_pipeline_cache, NULL); }
    } else {
        quiet = 0;
    }
    last_new = np;
    static int dumps;
    if (op_counts_enabled > 0 && dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (translate %.0fms, spirv %.0fms, pipelines %.0fms, draws waited %.0fms), fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_translate_ns, 0) / 1e6, atomic_exchange(&stat_spirv_ns, 0) / 1e6, atomic_exchange(&stat_pipe_ns, 0) / 1e6,
             stat_shader_wait_ns / 1e6,
             (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    if (stat_tf_merged || stat_tf_barriers) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", tfmerge %llu/barrier %llu",
                 (unsigned long long)stat_tf_merged, (unsigned long long)stat_tf_barriers);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " dep[v%llu c%llu s%llu g%llu o%llu q%llu rt%llu]",
                 (unsigned long long)stat_tf_dep_vbuf, (unsigned long long)stat_tf_dep_cbuf,
                 (unsigned long long)stat_tf_dep_sbuf, (unsigned long long)stat_tf_dep_gmem,
                 (unsigned long long)stat_tf_dep_op, (unsigned long long)stat_tf_dep_submit,
                 (unsigned long long)stat_tf_dep_rt);
    }
    if (stat_raster_discard_sets) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", rdiscard %llu/%llu",
                 (unsigned long long)stat_raster_discard_on, (unsigned long long)stat_raster_discard_sets);
    }
    if (stat_a2c_1x) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", a2c1x %llu",
                 (unsigned long long)stat_a2c_1x);
    }
    if (stat_logical_submits || stat_queue_batches) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", qbatch %llu lists/%llu queue",
                 (unsigned long long)stat_logical_submits,
                 (unsigned long long)stat_queue_batches);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " desc %llu/%llu alloc",
                 (unsigned long long)stat_desc_sets,
                 (unsigned long long)stat_desc_alloc_calls);
    }
    {
        uint32_t bf[4] = {0}; uint64_t bc[4] = {0};
        for (uint32_t fl = 0; fl < 256; fl++) {
            uint64_t c = stat_barrier_flags[fl];
            if (!c) continue;
            for (int k = 0; k < 4; k++) if (c > bc[k]) {
                for (int j = 3; j > k; j--) { bc[j] = bc[j - 1]; bf[j] = bf[j - 1]; }
                bc[k] = c; bf[k] = fl; break;
            }
        }
        if (bc[0]) {
            size_t n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " [barrier");
            for (int k = 0; k < 4 && bc[k]; k++) {
                n = strlen(stats_text);
                snprintf(stats_text + n, sizeof stats_text - n, " %#x:%llu", bf[k], (unsigned long long)bc[k]);
            }
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "]");
        }
        memset(stat_barrier_flags, 0, sizeof stat_barrier_flags);
    }
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n,
                 " [vkbar full:%llu gfx:%llu comp:%llu xfer:%llu upload:%llu]",
                 (unsigned long long)stat_vkbar_full,
                 (unsigned long long)stat_vkbar_graphics,
                 (unsigned long long)stat_vkbar_compute,
                 (unsigned long long)stat_vkbar_transfer,
                 (unsigned long long)stat_vkbar_upload);
    }
    {
        uint32_t top_line[5] = {0}, top_count[5] = {0};
        for (uint32_t l = 1; l < 4096; l++) {
            uint32_t c = stat_pass_end_line[l];
            if (!c) continue;
            for (int k = 0; k < 5; k++) if (c > top_count[k]) {
                for (int j = 4; j > k; j--) { top_count[j] = top_count[j - 1]; top_line[j] = top_line[j - 1]; }
                top_count[k] = c; top_line[k] = l; break;
            }
        }
        if (top_count[0]) {
            size_t n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " [pass-end");
            for (int k = 0; k < 5 && top_count[k]; k++) {
                n = strlen(stats_text);
                snprintf(stats_text + n, sizeof stats_text - n, " %u:%u", top_line[k], top_count[k]);
            }
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "]");
        }
        memset(stat_pass_end_line, 0, sizeof stat_pass_end_line);
    }
    stat_feedback_copies = stat_upload_bytes = stat_shader_wait_ns = 0;
    if (g_exp_count[0] | g_exp_count[1] | g_exp_count[2] | g_exp_count[3]) {
        LOG("[exp] skin identity %llu, ubo direct %llu, ubo copied %llu, one-bone shaders %llu\n",
            (unsigned long long)g_exp_count[0], (unsigned long long)g_exp_count[1], (unsigned long long)g_exp_count[2],
            (unsigned long long)g_exp_count[3]);
        g_exp_count[0] = g_exp_count[1] = g_exp_count[2] = 0;  // ([3]: a running total)
    }
    if (g_skin_watch) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
        LOG("[skin] cb3 %llx now: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
    }
    if (stat_skipped) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [skip: noprog %llu compiling %llu failed %llu noobj %llu mode %llu pipe-building %llu pipe-failed %llu]",
                 (unsigned long long)stat_skip_why[0], (unsigned long long)stat_skip_why[1],
                 (unsigned long long)stat_skip_why[2], (unsigned long long)stat_skip_why[3],
                 (unsigned long long)stat_skip_why[4], (unsigned long long)stat_skip_why[5],
                 (unsigned long long)stat_skip_why[6]);
        memset(stat_skip_why, 0, sizeof stat_skip_why);
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n,
                 " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu, watch-skip %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn,
                 (unsigned long long)prof_tex_watch_skips);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = prof_tex_watch_skips = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = stat_tf_merged = stat_tf_barriers = stat_raster_discard_sets = stat_raster_discard_on = stat_a2c_1x = 0;
    stat_vkbar_full = stat_vkbar_graphics = stat_vkbar_compute = stat_vkbar_transfer = stat_vkbar_upload = 0;
    stat_logical_submits = stat_queue_batches = 0;
    stat_desc_sets = stat_desc_alloc_calls = 0;
    stat_tf_dep_vbuf = stat_tf_dep_cbuf = stat_tf_dep_sbuf = stat_tf_dep_gmem = stat_tf_dep_op = stat_tf_dep_submit = stat_tf_dep_rt = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
    pipe_history_load();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;
static bool debug_mrt_drawn;
// Lightweight always-on pass-break attribution. Scarlet currently creates
// hundreds of rendering instances per frame; keeping just the source line that
// ended each pass lets us remove the dominant break without the much heavier
// LARECOMP_VK_PROF timestamp instrumentation.


static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (line > 0 && line < 4096) stat_pass_end_line[line]++;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    bool save_materials = debug_mrt_drawn;
    debug_mrt_drawn = false;
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
    // G-buffer passes can have RT0 unbound. The old F9 path skipped all their
    // material/normal/emissive outputs, hiding the source of hair colour errors.
    if (g_debug_frame && save_materials) {
        static uint64_t frame = ~0ull;
        static unsigned saved;
        uint64_t now = renderer_present_count();
        if (frame != now) { frame = now; saved = 0; }
        for (int i = 1; i < st.rt.ncolor && saved < 32; i++) {
            NvnTexture *t = st.rt.color[i];
            if (!t || !t->backend || pass_color[i] == VK_FORMAT_UNDEFINED) continue;
            char path[256];
            snprintf(path, sizeof path, "build/pass_mrt_%llu_%llx_rt%d.png", (unsigned long long)now,
                     (unsigned long long)t->addr, i);
            renderer_save_texture_tex(t->backend, path);
            saved++;
        }
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    debug_mrt_drawn = false;
    VkCommandBuffer cb = vk_cmd();
    vk_graphics_barrier(cb);  // scoped visibility for render/texture/SSBO hazards
    if (tf_barrier_pending) { stat_tf_dep_rt++; tf_pending_clear(); stat_tf_barriers++; }
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = vk_attachment_format(t, &st.rt.cview[i], st.rt.has_cview[i]);
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 192
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    const uint8_t *constants[2]; // immutable CB1 sections of these exact NVN stages
    uint32_t constants_size[2];
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    uint16_t prewarm_cursor;
    bool prewarm_done;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void log_shader_flow(const ShaderInfo *si, uint64_t addr, const char *stage) {
    if (si->indirect_tables || si->indirect_fallbacks)
        LOG("[shader-flow] %s %llx: %u instructions, %u cases, BRX tables=%u fallback=%u\n", stage,
            (unsigned long long)addr, si->num_insts, si->dispatch_labels, si->indirect_tables, si->indirect_fallbacks);
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint64_t t0 = SDL_GetTicksNS();
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (sv && getenv("LARECOMP_RENDERDOC_AT")) {  // which guest shader each SPIR-V is: tools/rdc_skin.py (FNV-1a 64)
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < nv * 4; i++) { h ^= ((const uint8_t *)sv)[i]; h *= 0x100000001B3ull; }
        pthread_mutex_lock(&lock);
        FILE *f = fopen("build/rdc-shaders.txt", "a");
        if (f) { fprintf(f, "%016llx vs %llx\n", (unsigned long long)h, (unsigned long long)p->code[0]); fclose(f); }
        pthread_mutex_unlock(&lock);
    }
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    static _Atomic unsigned module_logs;
    if (atomic_fetch_add(&module_logs, 1) < 8)
        LOG("[shader] modules %llx/%llx: vertex=%zu words fragment=%zu words\n",
            (unsigned long long)p->code[0], (unsigned long long)p->code[1], nv, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_spirv_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    uint64_t t0 = SDL_GetTicksNS();
    p->vi = maxwell_translate_with_constants((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX,
        &p->vkey, p->constants[0], p->constants_size[0]);
    // Experiment X4 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_ONEBONE=1: skinned vertex shaders see 100% weight on the
    // first bone (a12 = bone weights) - rigid, but whole, if the first bone's matrix reaches the GPU correctly.
    // Experiment X6, LARECOMP_VK_SKIN_IDX_X=1: every bone slot uses the first bone index (a11.x), weights unchanged.
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_IDX_X") && strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *d = strstr(p->vi->msl, "in uvec4 a11;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            static const char def[] = "#define a11 uvec4(a11_in.x, a11_in.x, a11_in.x, a11_in.x)\n";
            size_t len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in uvec4 a11"), pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + sizeof def), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in uvec4 a11_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, sizeof def - 1); o += sizeof def - 1;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // Experiments X8+, LARECOMP_VK_SKIN_W="<GLSL vec4 expression of a12_in>": the bone weights a skinned vertex shader
    // sees, rewritten (a12_in = the weights as the GPU fetched them).
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_W") && strstr(p->vi->msl, "in vec4 a12;")) {
        const char *d = strstr(p->vi->msl, "in vec4 a12;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            char def[512];
            snprintf(def, sizeof def, "#define a12 (%s)\n", getenv("LARECOMP_VK_SKIN_W"));
            size_t dl = strlen(def), len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in vec4 a12"),
                   pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + dl), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in vec4 a12_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, dl); o += dl;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // (only skinned shaders - the ones with bone indices too: a12 is something else elsewhere.)
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_ONEBONE") && strstr(p->vi->msl, "in vec4 a12;") &&
        strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *m = strstr(p->vi->msl, "void main() {");
        if (m) {
            static const char def[] = "#define a12 vec4(1.0, 0.0, 0.0, 0.0)\n";
            size_t pre = (size_t)(m - p->vi->msl), len = strlen(p->vi->msl);
            char *s = malloc(len + sizeof def);
            memcpy(s, p->vi->msl, pre);
            memcpy(s + pre, def, sizeof def - 1);
            memcpy(s + pre + sizeof def - 1, m, len - pre + 1);
            p->vi->msl = s;  // (the translator's string is kept: it lives as long as the program)
            g_exp_count[3]++;
        }
    }
    if (p->vi->msl && !p->code[1]) {  // vertex-only program (see get_program): an empty fragment shader
        p->fi = calloc(1, sizeof *p->fi);
        p->fi->stage = STAGE_FRAGMENT;
        p->fi->msl = strdup("#version 460\nvoid main() {}\n");
    } else if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate_with_constants((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT,
            &p->fkey, p->constants[1], p->constants_size[1]);
    }
    atomic_fetch_add(&stat_translate_ns, SDL_GetTicksNS() - t0);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s (code %llx)\n",
            p->fi->error, (unsigned long long)p->code[1]);
        atomic_store(&p->state, 2);
        return;
    }
    if (p->vi->surface_fallbacks || p->fi->surface_fallbacks) {
        static _Atomic unsigned nlog;
        unsigned n = atomic_fetch_add(&nlog, 1);
        if (n < 32)
            LOG("[shader] storage-image compatibility: vs=%u fs=%u (vs %llx fs %llx)\n",
                p->vi->surface_fallbacks, p->fi->surface_fallbacks,
                (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    }
    if (p->vi->uses_surface_write || p->vi->uses_surface_atomic ||
        p->fi->uses_surface_write || p->fi->uses_surface_atomic) {
        static _Atomic unsigned native_log;
        unsigned n = atomic_fetch_add(&native_log, 1);
        if (n < 32)
            LOG("[shader] native storage images: vs(write=%d atomic=%d) fs(write=%d atomic=%d) (vs %llx fs %llx)\n",
                p->vi->uses_surface_write, p->vi->uses_surface_atomic,
                p->fi->uses_surface_write, p->fi->uses_surface_atomic,
                (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;

    // A translated graphics vertex stage must eventually export position.
    // Keep this diagnostic tied to the guest code address: large dispatcher
    // shaders previously produced a GLSL file that only contained our default
    // gl_Position initializer, making it impossible to identify the offending
    // Maxwell program from the source-hash filename alone.
    const char *pos_init = strstr(p->vi->msl, "gl_Position = vec4(0, 0, 0, 1);");
    const char *pos_after = pos_init ? strstr(pos_init + strlen("gl_Position = vec4(0, 0, 0, 1);"), "gl_Position") : NULL;
    if (pos_init && !pos_after && !p->vkey.xfb) {
        static _Atomic unsigned no_pos_logs;
        if (atomic_fetch_add(&no_pos_logs, 1) < 32)
            LOG("[shader] VS has no position export: code=%llx source=%zu bytes inst=%u cases=%u\n",
                (unsigned long long)p->code[0], strlen(p->vi->msl),
                p->vi->num_insts, p->vi->dispatch_labels);
        // Keep what is needed to analyse the shader offline: the translated GLSL and the raw Maxwell words, named
        // by guest address (build/shader-diagnostics/nopos_<addr>_vs.{glsl,sass.bin}).
        if (no_pos_logs < 32) {
            char path[256];
            mkdir("build/shader-diagnostics", 0755);
            snprintf(path, sizeof path, "build/shader-diagnostics/nopos_%llx_vs.glsl", (unsigned long long)p->code[0]);
            FILE *f = fopen(path, "w");
            if (f) { fputs(p->vi->msl, f); fclose(f); }
            size_t n = p->cmax[0] < 0x4000 ? p->cmax[0] : 0x4000;
            snprintf(path, sizeof path, "build/shader-diagnostics/nopos_%llx_vs.sass.bin", (unsigned long long)p->code[0]);
            f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)p->code[0], 1, n, f); fclose(f); }
        }
        // Keep the program alive. Some Scarlet programs reach position through control-flow paths that the
        // current static diagnostic does not recognize; rejecting them caused hundreds of valid model draws/frame
        // to disappear. The diagnostic dump remains useful, but it must not change rendering semantics.
    }

    log_shader_flow(p->vi, p->code[0], "vs");
    log_shader_flow(p->fi, p->code[1], "fs");
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    compile_program(p);  // (this worker: queued again it would wait behind the other translations)
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// BindProgram(program, stages): the stages in the mask come from `program` (NULL: unbound), the others stay. A bind
// that leaves one program's stages and nothing else uses that program itself (as before); a mix of stages from
// several programs (Let's Go: vertex and fragment bound separately) uses a combined program. The shader caches are
// keyed by code address, so the combination needs no identity of its own.
static NvnProgram g_comb_prog;
static void bind_program_stages(NvnProgram *p, uint32_t mask) {
    if (!mask) mask = p ? p->stage_mask : 0x3F;  // (no mask recorded: the whole program)
    NvnProgram *cur = st.program;
    NvnProgram next = cur ? *cur : (NvnProgram){0};
    next.backend = NULL;
    for (int i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (p && (p->stage_mask & bit)) next.stages[i] = p->stages[i], next.stage_mask |= bit;
        else memset(&next.stages[i], 0, sizeof next.stages[i]), next.stage_mask &= ~bit;
    }
    if (!next.stage_mask) { st.program = NULL; return; }
    if (p && next.stage_mask == p->stage_mask &&
        !memcmp(next.stages, p->stages, sizeof next.stages)) { st.program = p; return; }
    g_comb_prog = next;
    st.program = &g_comb_prog;
}

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    // A vertex shader without a fragment shader (Let's Go's depth / shadow passes: stages 0x1) gets an empty fragment
    // shader in translate_program - no color output, only depth, as the hardware does. They were all skipped.
    if (!(np->stage_mask & 1)) {
        static uint32_t seen;
        if (np->stage_mask < 32 && !(seen & (1u << np->stage_mask))) {
            seen |= 1u << np->stage_mask;
            LOG("[shader] draw with stages %#x (needs vertex + fragment): skipped\n", np->stage_mask);
        }
        return NULL;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) {
        uint64_t values[] = {np->stages[i].code_addr, np->stages[i].const_offset, np->stages[i].const_size};
        for (size_t j = 0; j < sizeof values / sizeof *values; j++)
            for (int k = 0; k < 8; k++) { h ^= (values[j] >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; }
    }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        if (np->stages[i].const_offset && np->stages[i].const_size) {
            p->constants[i] = (const uint8_t *)(uintptr_t)(p->code[i] + np->stages[i].const_offset);
            p->constants_size[i] = np->stages[i].const_size;
        }
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_job(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (f < 256 && !told[f]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// SSBU-Recomp's production renderer uses a persistent pipeline history so
// expensive variants can be built before their next first-use draw. Scarlet's
// PipeKey contains a process-local Prog*, therefore its raw key cannot be
// persisted. Store stable shader addresses + fixed state with that pointer
// cleared, then restore the current Prog* when a matching program becomes ready.
#define PIPE_HIST_MAGIC 0x48504353u /* "SCPH" */
#define PIPE_HIST_VERSION 1u
#define PIPE_HIST_MAX 8192u
#define PIPE_HIST_SLOTS 16384u

typedef struct {
    uint64_t vs, fs;
    PipeKey key;  // key.prog is always NULL on disk
} PipeHistRec;

typedef struct {
    uint32_t magic, version, key_size, count;
    uint32_t vendor_id, device_id;
    uint32_t reserved[2];
} PipeHistHeader;

static PipeHistRec pipe_hist[PIPE_HIST_MAX];
static uint32_t pipe_hist_slots[PIPE_HIST_SLOTS]; // 1-based record index
static uint32_t pipe_hist_count, pipe_hist_loaded;
static pthread_mutex_t pipe_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pipe_hist_initialized;

static bool pipe_prewarm_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("LARECOMP_PIPE_PREWARM");
        enabled = !e || atoi(e) != 0;
    }
    return enabled != 0;
}

static uint64_t pipe_hist_hash(uint64_t vs, uint64_t fs, const PipeKey *key) {
    PipeKey stable = *key;
    stable.prog = NULL;
    uint64_t h = hash_words(UINT64_C(0xcbf29ce484222325), &vs, sizeof vs);
    h = hash_words(h, &fs, sizeof fs);
    h = hash_words(h, &stable, sizeof stable);
    return h ? h : 1;
}

static bool pipe_hist_equal(const PipeHistRec *r, uint64_t vs, uint64_t fs, const PipeKey *key) {
    if (r->vs != vs || r->fs != fs) return false;
    PipeKey stable = *key;
    stable.prog = NULL;
    return !memcmp(&r->key, &stable, sizeof stable);
}

static void pipe_hist_index_record(uint32_t index) {
    PipeHistRec *r = &pipe_hist[index];
    uint64_t h = pipe_hist_hash(r->vs, r->fs, &r->key);
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PIPE_HIST_SLOTS - 1);
    while (pipe_hist_slots[slot]) slot = (slot + 1) & (PIPE_HIST_SLOTS - 1);
    pipe_hist_slots[slot] = index + 1;
}

static void pipe_history_load(void) {
    if (pipe_hist_initialized) return;
    pipe_hist_initialized = true;
    if (!pipe_prewarm_enabled()) {
        LOG("[vk-prewarm] disabled (LARECOMP_PIPE_PREWARM=0)\n");
        return;
    }

    FILE *f = fopen("cache/scarlet_pipeline_history.bin", "rb");
    if (!f) return;
    PipeHistHeader h = {0};
    bool valid = fread(&h, 1, sizeof h, f) == sizeof h &&
                 h.magic == PIPE_HIST_MAGIC && h.version == PIPE_HIST_VERSION &&
                 h.key_size == sizeof(PipeKey) && h.count <= PIPE_HIST_MAX &&
                 h.vendor_id == g_vk.props.vendorID && h.device_id == g_vk.props.deviceID;
    if (valid && h.count) {
        valid = fread(pipe_hist, sizeof(PipeHistRec), h.count, f) == h.count;
    }
    fclose(f);
    if (!valid) {
        memset(pipe_hist, 0, sizeof pipe_hist);
        LOG("[vk-prewarm] ignored incompatible pipeline history\n");
        return;
    }

    pipe_hist_count = pipe_hist_loaded = h.count;
    for (uint32_t i = 0; i < pipe_hist_count; ++i) {
        pipe_hist[i].key.prog = NULL;
        pipe_hist_index_record(i);
    }
    LOG("[vk-prewarm] loaded %u Scarlet pipeline variants for device %04x:%04x\n",
        pipe_hist_loaded, g_vk.props.vendorID, g_vk.props.deviceID);
}

static void pipe_history_note(const PipeKey *key) {
    if (!pipe_prewarm_enabled() || !key || !key->prog) return;
    uint64_t vs = key->prog->code[0], fs = key->prog->code[1];
    uint64_t h = pipe_hist_hash(vs, fs, key);
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PIPE_HIST_SLOTS - 1);

    pthread_mutex_lock(&pipe_hist_lock);
    while (pipe_hist_slots[slot]) {
        uint32_t index = pipe_hist_slots[slot] - 1;
        if (index < pipe_hist_count && pipe_hist_equal(&pipe_hist[index], vs, fs, key)) {
            pthread_mutex_unlock(&pipe_hist_lock);
            return;
        }
        slot = (slot + 1) & (PIPE_HIST_SLOTS - 1);
    }
    if (pipe_hist_count < PIPE_HIST_MAX) {
        uint32_t index = pipe_hist_count++;
        pipe_hist[index].vs = vs;
        pipe_hist[index].fs = fs;
        pipe_hist[index].key = *key;
        pipe_hist[index].key.prog = NULL;
        pipe_hist_slots[slot] = index + 1;
    }
    pthread_mutex_unlock(&pipe_hist_lock);
}

void vk_pipe_history_save(void) {
    if (!pipe_prewarm_enabled() || !pipe_hist_initialized) return;

    pthread_mutex_lock(&pipe_hist_lock);
    uint32_t count = pipe_hist_count;
    if (!count) {
        pthread_mutex_unlock(&pipe_hist_lock);
        return;
    }

    mkdir("cache", 0755);
    const char *tmp = "cache/scarlet_pipeline_history.bin.tmp";
    FILE *f = fopen(tmp, "wb");
    if (f) {
        PipeHistHeader h = {
            .magic = PIPE_HIST_MAGIC, .version = PIPE_HIST_VERSION,
            .key_size = sizeof(PipeKey), .count = count,
            .vendor_id = g_vk.props.vendorID, .device_id = g_vk.props.deviceID,
        };
        bool ok = fwrite(&h, 1, sizeof h, f) == sizeof h &&
                  fwrite(pipe_hist, sizeof(PipeHistRec), count, f) == count;
        if (fflush(f) != 0) ok = false;
        fclose(f);
        if (ok) {
            remove("cache/scarlet_pipeline_history.bin");
            rename(tmp, "cache/scarlet_pipeline_history.bin");
        } else {
            remove(tmp);
        }
    }
    pthread_mutex_unlock(&pipe_hist_lock);
}

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static VkPipelineCreateFlags translated_pipeline_flags(uint32_t vendor) {
    // Prefer bounded compilation work on the user's AMD APU. This is a driver optimization hint, separate
    // from shaderc optimization and from SPIR-V NoContraction/depth invariance; it does not replace a shader.
    return vendor == 0x1002 ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = VK_FALSE,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = k->prog->vkey.xfb ? 1u : 2u,
                                       .flags = translated_pipeline_flags(g_vk.props.vendorID),
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    uint64_t tp = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    atomic_fetch_add(&stat_pipe_ns, SDL_GetTicksNS() - tp);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// Queue a few historical variants after the current draw's required pipeline
// has already entered the FIFO. This makes prewarm opportunistic on CPU-bound
// APUs instead of allowing it to delay the frame currently being rendered.
static void pipe_history_prewarm(Prog *pr, uint64_t current_hash) {
    if (!pipe_prewarm_enabled() || !pipe_hist_loaded || !pr || pr->prewarm_done) return;

    unsigned queued = 0;
    uint32_t i = pr->prewarm_cursor;
    for (; i < pipe_hist_loaded && queued < 4; ++i) {
        const PipeHistRec *r = &pipe_hist[i];
        if (r->vs != pr->code[0] || r->fs != pr->code[1]) continue;

        PipeKey k = r->key;
        k.prog = pr;
        uint64_t ph = hash_words(UINT64_C(0xcbf29ce484222325), &k, sizeof k);
        if (!ph) ph = 1;
        if (ph == current_hash) continue;

        uint32_t slot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
        while (pipe_tab[slot].key && pipe_tab[slot].key != ph)
            slot = (slot + 1) & (PIPE_TAB - 1);
        if (pipe_tab[slot].pipe) continue;

        Pipe *pp = calloc(1, sizeof *pp);
        if (!pp) break;
        pp->k = k;
        pp->hash = ph;
        pipe_tab[slot].key = ph;
        pipe_tab[slot].pipe = pp;
        run_job(build_pipeline, pp);
        queued++;
    }
    pr->prewarm_cursor = (uint16_t)(i > UINT16_MAX ? UINT16_MAX : i);
    if (i >= pipe_hist_loaded || i >= UINT16_MAX) pr->prewarm_done = true;
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4][2]; // depth/color and stencil views of the same snapshot
static int nfeedback;
static uint64_t g_cur_fs_hash;
uint64_t renderer_present_count(void);
static uint64_t fragment_code_hash(uint64_t fs) {
    static uint64_t previous, hash;
    if (fs != previous) {
        previous = fs;
        hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
    }
    return hash;
}

static bool sharp_dof_variant(uint64_t hash) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("LARECOMP_NO_DOF");
        enabled = e && atoi(e) != 0;
    }
    // Same composite fingerprint in the academy, classroom and statue captures.
    return enabled && hash == 0xfbff97881854688eull;
}
static bool hair_lighting_variant(uint64_t hash) {
    // The G-buffer capture proved this compatibility path restores Scarlet's
    // material-3 hair colour. Keep it on by default for the exact deferred
    // lighting shader; LARECOMP_NO_HAIR_FIX=1 is a diagnostic opt-out.
    static int disabled = -1;
    if (disabled < 0) {
        const char *off = getenv("LARECOMP_NO_HAIR_FIX");
        disabled = off && atoi(off) != 0;
    }
    return !disabled && hash == 0xaa68a5939321c84aull;
}

static uint64_t schoolyard_grade_frame = ~0ull;
static void scarlet_scene_markers(uint64_t fs_addr) {
    // These fragment programs occur in the opening academy schoolyard/court
    // capture and are absent from the classroom capture. Guest code addresses
    // are more robust here than hashing a translated shader variant.
    switch (fs_addr) {
    case 0x72715cc300ull:
    case 0x727164aa00ull:
    case 0x7271748900ull:
    case 0x727191fe00ull:
    case 0x72723c3600ull:
    case 0x7272433000ull:
    case 0x727355e900ull:
    case 0x7273570800ull:
    case 0x72735f6800ull:
    case 0x7273605d00ull:
    case 0x728d023900ull:
    case 0x728d026500ull:
    case 0x728d02ba00ull:
    case 0x728d26d900ull:
    case 0x728d270500ull:
    case 0x728d275a00ull:
        schoolyard_grade_frame = renderer_present_count();
        break;
    default:
        break;
    }
}
static bool schoolyard_grade_variant(uint64_t fs_addr) {
    static int disabled = -1;
    if (disabled < 0) {
        const char *off = getenv("LARECOMP_NO_RING_GRADE_FIX");
        disabled = off && atoi(off) != 0;
    }
    if (disabled) return false;
    if (schoolyard_grade_frame != renderer_present_count()) return false;
    // Tone-map variants observed in the two opening captures.
    return fs_addr == 0x7273317200ull || fs_addr == 0x7273318200ull;
}

static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

// Snapshots include both depth and stencil: material IDs may change even when
// the depth plane is read-only (the old depth-only predicate missed this).
static bool depth_stencil_writes(void) {
    if (!st.rt.depth) return false;
    if (st.ds[0] && st.ds[1]) return true;
    if (!st.ds[3] || !renderer_format_has_stencil(st.rt.depth->format)) return false;
    uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
    for (int f = 0; f < 2; f++) {
        if (!(st.stencil_mask[f] & 0xFF)) continue;
        for (int i = 0; i < 3; i++)
            if (stencil_op((ops >> (12 * f + 4 * i)) & 0xF) != VK_STENCIL_OP_KEEP) return true;
    }
    return false;
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexEntry *shader_texture_entry(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return &((NvnTexEntry *)st.tpool->entries)[tic];
}

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    NvnTexEntry *e = shader_texture_entry(stage, st_);
    return e ? e->tex : NULL;
}

static uint32_t stencil_texture_bindings(int stage) {
    uint32_t mask = 0;
    if (!st.tpool) return mask;
    NvnTexEntry *entries = st.tpool->entries;
    for (int slot = 0; slot < 32; slot++) {
        uint64_t h = st.tex[stage][slot];
        uint32_t tic = h & 0xFFFFF;
        if (h && tic < (uint32_t)st.tpool->count &&
            nvn_texture_samples_stencil(entries[tic].tex, &entries[tic].view, entries[tic].has_view))
            mask |= 1u << slot;
    }
    return mask;
}

static uint32_t feedback_copy_regions(const TexBackend *src, VkImageCopy regions[2]) {
    uint32_t n = 0;
    for (uint32_t aspect = 1; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
        if (!(src->aspect & aspect)) continue;
        regions[n++] = (VkImageCopy){.srcSubresource = {aspect, 0, 0, 1},
            .dstSubresource = {aspect, 0, 0, 1}, .extent = {src->width, src->height, 1}};
    }
    return n;
}

static void feedback_set_views(int index, TexBackend *b) {
    static const VkComponentMapping id = {0};
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    feedback_view[index][0] = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, aspect);
    feedback_view[index][1] = b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ?
        vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT) : VK_NULL_HANDLE;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// Preserve exact stencil bytes, not a normalized visualization: lighting uses
// bits 3..5 to distinguish emission from the hair/tangent material path.
static void debug_save_stencil(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n);
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P5 %u %u 255\n", b->width, b->height);
    fwrite(out.ptr, 1, n, fp);
    fclose(fp);
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            if (!nosnap && si >= 0 && snap_ok[si] && existing) {
                feedback_tex[nfeedback] = t;
                feedback_set_views(nfeedback++, &existing->copy);
                continue;
            }
            stat_feedback_copies++;
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_transfer_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            // VkImageSubresourceLayers permits only one aspect per copy region.
            // Both planes must survive: the lighting pass reads stencil material
            // IDs and float depth through two views of this same attachment.
            VkImageCopy regions[2];
            uint32_t nregions = feedback_copy_regions(src, regions);
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, nregions, regions);
            if (g_debug_frame && getenv("LARECOMP_FORENSICS_DEPTH") && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                static uint64_t saved_frame = ~0ull;
                static unsigned saved;
                uint64_t frame = renderer_present_count();
                if (frame != saved_frame) { saved_frame = frame; saved = 0; }
                if (saved < 8) {
                    char path[128];
                    snprintf(path, sizeof path, "build/fbdepth_stencil_%llu_%u_src.pgm", (unsigned long long)frame, saved);
                    debug_save_stencil(src, path);
                    snprintf(path, sizeof path, "build/fbdepth_stencil_%llu_%u_snap.pgm", (unsigned long long)frame, saved++);
                    debug_save_stencil(dst, path);
                    cb = vk_cmd();
                }
            }
            feedback_tex[nfeedback] = t;
            feedback_set_views(nfeedback++, dst);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// True for fragment programs that currently sample a small BC7-sRGB texture.
// Scarlet character materials use this pattern for their base colour; keeping
// the diagnostic scoped here avoids touching sky/UI/full-screen passes.
static bool scarlet_character_material(ShaderInfo *si) {
    if (!si) return false;
    for (int i = 0; i < si->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &si->tex[i]);
        if (t && t->format == 0x4E && t->width <= 512 && t->height <= 512)
            return true;
    }
    return false;
}

static bool tint_like_vec4(const float *v) {
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(v[3])) return false;
    if (v[3] < 0.90f || v[3] > 1.10f) return false;
    for (int i = 0; i < 3; i++) if (v[i] < 0.0f || v[i] > 1.25f) return false;
    float lo = fminf(v[0], fminf(v[1], v[2]));
    float hi = fmaxf(v[0], fmaxf(v[1], v[2]));
    return hi - lo > 0.12f;
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    bool skinned_vs = stage == 0 && ((si->attr_mask >> 11) & 1);  // (Let's Go: a11 = bone indices)
    // Experiment X2 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_IDENTITY=1: a skinned vertex shader's dynamically indexed
    // cbuf (its bone palette) reads identity matrices - models in their bind pose if the rest of the chain is right.
    static int skin_identity = -1;
    if (skin_identity < 0) skin_identity = getenv("LARECOMP_VK_SKIN_IDENTITY") != NULL;
    if (skin_identity && skinned_vs && (si->cbuf_dynamic & (1u << k))) {
        static float ident[0x10000 / 4];
        if (ident[0] != 1.0f)
            for (int m = 0; m < 0x10000 / 64; m++) ident[m * 16] = ident[m * 16 + 5] = ident[m * 16 + 10] = ident[m * 16 + 15] = 1.0f;
        g_exp_count[0]++;
        return ubo_span(vk_transient_copy(ident, size, 256), size);
    }
    // Diagnostic A/B for Scarlet's cyan/purple character colours.
    // It does not modify guest memory.  For character-material fragment draws,
    // copy the selected guest cbuffer and neutralize only vec4 values that look
    // like RGB tints (alpha ~= 1 and visibly chromatic RGB).  Value "1" scans
    // all guest material cbuffers; a numeric value 3..17 restricts it to that
    // shader cbuffer.  This is intentionally opt-in until the exact guest field
    // is identified from the accompanying material-draw trace.
    const char *neutral_tint = getenv("LARECOMP_NEUTRAL_HAIR_TINT");
    if (neutral_tint && stage == 1 && k >= 3 && scarlet_character_material(si)) {
        int only = atoi(neutral_tint);
        if (only <= 1 || only == k) {
            const char *off_s = getenv("LARECOMP_NEUTRAL_HAIR_OFF");
            uint64_t only_off = off_s && *off_s ? strtoull(off_s, NULL, 0) : UINT64_MAX;
            uint8_t *tmp = malloc((size_t)size);
            if (tmp) {
                memcpy(tmp, (const void *)(uintptr_t)r.addr, (size_t)size);
                unsigned changed = 0;
                for (uint64_t off = 0; off + 16 <= size; off += 16) {
                    float *v = (float *)(void *)(tmp + off);
                    if (!tint_like_vec4(v)) continue;
                    if (only_off != UINT64_MAX && off != only_off) continue;
                    float r0 = v[0], g0 = v[1], b0 = v[2], a0 = v[3];
                    uint32_t raw[4]; memcpy(raw, v, sizeof raw);
                    static unsigned candidate_logs;
                    if (candidate_logs++ < 128)
                        LOG("[material-tint-candidate] cb%d(u%d) off=%#llx rgba=%g,%g,%g,%g raw=%08x,%08x,%08x,%08x\n",
                            k, k - 3, (unsigned long long)off, r0, g0, b0, a0,
                            raw[0], raw[1], raw[2], raw[3]);
                    v[0] = v[1] = v[2] = 1.0f;
                    changed++;
                }
                if (changed) {
                    static unsigned logs;
                    if (logs++ < 64)
                        LOG("[material-tint] neutralized %u candidate vec4(s) in shader cb%d (NVN u%d, %llu bytes%s)\n",
                            changed, k, k - 3, (unsigned long long)size,
                            only_off != UINT64_MAX ? ", offset-filtered" : "");
                    VkDescriptorBufferInfo out = ubo_span(vk_transient_copy(tmp, size, 256), size);
                    free(tmp);
                    return out;
                }
                free(tmp);
            }
        }
    }

    // LARECOMP_VK_UBO_DIRECT=1 (experiment X3): bind the range in place in the imported guest heap, read when the GPU
    // runs - as on the console - in case compute shaders write it after this point. Counted: direct vs copied.
    static int direct = -1;
    if (direct < 0) direct = getenv("LARECOMP_VK_UBO_DIRECT") ? atoi(getenv("LARECOMP_VK_UBO_DIRECT")) : 0;
    if (direct) {
        extern bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
        VkBuffer gb;
        VkDeviceSize goff;
        uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        if (!(r.addr % (align ? align : 256)) && vk_guest_direct(r.addr, size, &gb, &goff)) {
            if (skinned_vs) g_exp_count[1]++;
            return ubo_span((VkSpan){.buf = gb, .off = goff}, size);
        }
        if (skinned_vs) g_exp_count[2]++;
    }
    if (ip_enabled()) {  // frame interpolation: recorded, or the blended copy in the replay
        const void *d = ip_cbuf(stage, k, r.addr, (uint32_t)size);
        if (d) return ubo_span(vk_transient_copy(d, size, 256), size);
    }
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      (g_vk.transform_feedback ? (VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
                                                                   VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT) : 0) |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};  // (uniforms read in place: cbuf_info)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static uint8_t storage_buffer_count(void) {
    if (g_vk.host_import) return 0;
    uint32_t n = g_vk.props.limits.maxPerStageDescriptorStorageBuffers;
    if (n) n--;  // leave room for the storage-image handle map
    return (uint8_t)(n < 16 ? n : 16);
}

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || !size || addr < HEAP_BASE) return false;

    uint64_t rel = addr - HEAP_BASE;
    if (rel >= HEAP_MAX || size > HEAP_MAX - rel) return false;

    /*
     * Scarlet allocates important GPU work buffers (notably transform-feedback
     * output) from its guest-backed StandardAllocator without first growing the
     * Horizon SetHeapSize region.  kernel_heap_size() is therefore still zero
     * while valid TF buffers already live hundreds of MiB above HEAP_BASE.
     *
     * The old direct path only imported kernel_heap_size(), so those buffers
     * could never become VkBuffers: every TF begin logged nbuf=0/direct0=0 and
     * the stream-output pass silently produced no geometry.
     *
     * Import enough of the reserved guest window to cover the actual binding.
     * Grow geometrically to avoid reallocating the external-memory buffer for
     * every nearby allocation.  Windows reserves this whole guest window but
     * commits it lazily, so make the range real before importing it.
     */
    uint64_t need = (rel + size + 0xFFFFull) & ~0xFFFFull;
    if (need > gmem_size) {
        uint64_t known = kernel_heap_size();
        if (known > need) need = (known + 0xFFFFull) & ~0xFFFFull;

        uint64_t want = need;
        uint64_t grow = gmem_size ? gmem_size * 2 : (256ull << 20);
        if (grow < gmem_size || grow > HEAP_MAX) grow = HEAP_MAX;
        if (want < grow) want = grow;
        if (want > HEAP_MAX) want = HEAP_MAX;

        if (want != gmem_tried) {
            gmem_tried = want;
#ifdef _WIN32
            plat_commit(HEAP_BASE, want);
#endif
            if (!gmem_import(want)) {
                static _Atomic int import_fail_logged;
                if (!atomic_exchange(&import_fail_logged, 1))
                    LOG("[vk] guest direct import could not cover %llx+%llx (want %llu MB); using copy paths where possible\n",
                        (unsigned long long)addr, (unsigned long long)size,
                        (unsigned long long)(want >> 20));
            }
        }
    }

    if (!gmem_buf || rel + size > gmem_size) return false;
    *buf = gmem_buf;
    *off = rel;
    return true;
}
bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {  // (cbuf_info, above)
    return guest_direct(addr, size, buf, off);
}

static int g_cs_n;  // (compute program table below)
static void surface_cache_reset(void);

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    vk_pipe_history_save();
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    surface_cache_reset();
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k][nvn_texture_samples_stencil(e->tex, &e->view, e->has_view) ? 1 : 0];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

typedef struct {
    NvnDescPool *pool;
    uint32_t generation;
    VkBuffer map_buf;
    VmaAllocation map_mem;
    uint32_t *map_ptr; // uvec2 per TIC: x write slot, y atomic slot
    uint32_t map_count;
    VkDescriptorImageInfo write[MAXWELL_BINDLESS_IMAGES];
    VkDescriptorImageInfo atomic[MAXWELL_BINDLESS_IMAGES];
    NvnTexture *write_tex[MAXWELL_BINDLESS_IMAGES], *atomic_tex[MAXWELL_BINDLESS_IMAGES];
} SurfaceBindlessCache;

static SurfaceBindlessCache surface_cache[8];

static void surface_cache_reset(void) { memset(surface_cache, 0, sizeof surface_cache); }

static SurfaceBindlessCache *surface_cache_get(NvnDescPool *pool) {
    SurfaceBindlessCache *c = NULL;
    for (int i = 0; i < 8; i++) {
        if (surface_cache[i].pool == pool && surface_cache[i].map_buf) { c = &surface_cache[i]; break; }
        if (!c && !surface_cache[i].map_buf) c = &surface_cache[i];
    }
    if (!c) c = &surface_cache[0];

    uint32_t count = pool && pool->count > 0 ? (uint32_t)pool->count : 1;
    if (!c->map_buf || c->pool != pool || c->map_count < count) {
        // Keep old allocations alive for the process lifetime rather than destroying
        // a buffer that an in-flight descriptor set may still reference.
        c = NULL;
        for (int i = 0; i < 8; i++) if (!surface_cache[i].map_buf) { c = &surface_cache[i]; break; }
        if (!c) c = &surface_cache[0];
        memset(c, 0, sizeof *c);
        c->pool = pool;
        c->map_count = count;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = (VkDeviceSize)count * 8,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo inf;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->map_buf, &c->map_mem, &inf));
        c->map_ptr = inf.pMappedData;
    }

    uint32_t gen = pool ? pool->generation : 0;
    uint32_t token = gen + 1u;
    if (c->generation != token) {
        // Descriptor sets recorded before this registration change may still be
        // in the pending/in-flight command buffers. Never rewrite their map in
        // place: snapshot a fresh buffer for the new generation. The old VMA
        // allocation intentionally stays alive for this run (small and rare,
        // registration-time only), exactly like the immutable descriptor sets.
        if (c->generation) {
            VkBufferCreateInfo mbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = (VkDeviceSize)c->map_count * 8,
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
            VmaAllocationCreateInfo mai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                    VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo minf;
            VkBuffer new_buf;
            VmaAllocation new_mem;
            VK_CHECK(vmaCreateBuffer(g_vk.vma, &mbi, &mai, &new_buf, &new_mem, &minf));
            c->map_buf = new_buf;
            c->map_mem = new_mem;
            c->map_ptr = minf.pMappedData;
        }
        for (uint32_t i = 0; i < c->map_count; i++) {
            c->map_ptr[i * 2 + 0] = UINT32_MAX;
            c->map_ptr[i * 2 + 1] = UINT32_MAX;
        }
        for (int i = 0; i < MAXWELL_BINDLESS_IMAGES; i++) {
            c->write_tex[i] = c->atomic_tex[i] = NULL;
            c->write[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, vk_dummy_storage_view(false), VK_IMAGE_LAYOUT_GENERAL};
            c->atomic[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, vk_dummy_storage_view(true), VK_IMAGE_LAYOUT_GENERAL};
        }

        int nw = 0, na = 0;
        int slot_cap = g_vk.bindless_storage_slots ? g_vk.bindless_storage_slots : 1;
        if (slot_cap > MAXWELL_BINDLESS_IMAGES) slot_cap = MAXWELL_BINDLESS_IMAGES;
        if (pool) {
            NvnTexEntry *e = pool->entries;
            // Most recently registered images first. This keeps the compact window
            // focused on the render targets the current Scarlet scene actually uses.
            uint32_t before = UINT32_MAX;
            for (int pass = 0; pass < pool->count && (nw < slot_cap || na < slot_cap); pass++) {
                int best = -1;
                uint32_t best_seq = 0;
                for (int i = 0; i < pool->count; i++)
                    if (e[i].is_image && e[i].tex && e[i].image_seq < before && e[i].image_seq > best_seq) {
                        best = i; best_seq = e[i].image_seq;
                    }
                if (best < 0) break;
                before = best_seq;
                if (nw < slot_cap) {
                    VkImageView v = vk_storage_write_view(e[best].tex, &e[best].view, e[best].has_view);
                    if (v) {
                        c->map_ptr[best * 2 + 0] = (uint32_t)nw;
                        c->write_tex[nw] = e[best].tex;
                        c->write[nw++] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
                    }
                }
                if (na < slot_cap) {
                    VkImageView v = vk_storage_atomic_u32_view(e[best].tex, &e[best].view, e[best].has_view);
                    if (v) {
                        c->map_ptr[best * 2 + 1] = (uint32_t)na;
                        c->atomic_tex[na] = e[best].tex;
                        c->atomic[na++] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
                    }
                }
            }
        }
        vmaFlushAllocation(g_vk.vma, c->map_mem, 0, VK_WHOLE_SIZE);
        c->generation = token;
        static unsigned nlog;
        if (nlog++ < 24)
            LOG("[shader] bindless image map: pool=%p count=%u generation=%u write=%d atomic=%d\n",
                (void *)pool, count, gen, nw, na);
    }
    // CPU updates may change image contents without changing descriptor registration. Refresh only compatible
    // views; vk_tex leaves existing GPU-owned data intact and notices explicit memory-pool invalidations.
    for (int i = 0; i < MAXWELL_BINDLESS_IMAGES; i++) {
        if (c->write_tex[i]) vk_tex(c->write_tex[i], false);
        if (c->atomic_tex[i]) vk_tex(c->atomic_tex[i], false);
    }
    return c;
}

static void surface_finish_writes(const ShaderInfo *si) {
    if (!si || (!si->uses_surface_write && !si->uses_surface_atomic)) return;
    for (int c = 0; c < 8; c++) {
        SurfaceBindlessCache *s = &surface_cache[c];
        if (s->pool != st.tpool || !s->map_buf) continue;
        // A dynamic image handle can select any bound slot. Mark ownership only after an operation executes,
        // never while probing the pool or waiting for compilation. No incompatible image is included here.
        for (int i = 0; i < MAXWELL_BINDLESS_IMAGES; i++) {
            if (si->uses_surface_write && s->write_tex[i]) renderer_mark_gpu_owned(s->write_tex[i]);
            if ((si->uses_surface_atomic || si->uses_surface_write) && s->atomic_tex[i])
                renderer_mark_gpu_owned(s->atomic_tex[i]);
        }
        break;
    }
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
typedef struct { uint64_t addr, size; VkSpan span; } StorageWriteback;
static StorageWriteback storage_writeback[32];
static int storage_nwriteback;

static void storage_finish_writes(void) {
    if (!storage_nwriteback) return;
    vk_end_pass();
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &host};
    vkCmdPipelineBarrier2(vk_cmd(), &dependency);
    // A conservative fallback for drivers without external_memory_host. GPU writes must reach guest memory
    // before vertex/index/uniform/texture snapshots or the guest CPU can consume the result.
    renderer_flush();
    for (int i = 0; i < storage_nwriteback; i++) {
        StorageWriteback *w = &storage_writeback[i];
        for (int c = 0; c < nchunks; c++) if (chunks[c].buf == w->span.buf) {
            vmaInvalidateAllocation(g_vk.vma, chunks[c].mem, w->span.off, w->size); break;
        }
        memcpy((void *)(uintptr_t)w->addr, w->span.ptr, w->size);
        backend_pool_written(w->addr, w->size);
    }
    storage_nwriteback = 0;
    guest_copy_flush();
}

static void storage_stage_buffers(int stage, ShaderInfo *si, VkDescriptorBufferInfo buffers[16], uint32_t map[16][4]) {
    StorageRange input[32], merged[32];
    int ni = 0;
    // Merge aliases across both graphics stages. A shader can read through one binding and write through another.
    for (int s = 0; s < 6; s++) {
        if (stage == 5 ? s != 5 : s != 0 && s != 1) continue;
        for (int i = 0; i < 16; i++) input[ni++] = (StorageRange){st.sbuf[s][i].addr, st.sbuf[s][i].size};
    }
    uint64_t align = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (align < 256) align = 256;
    size_t nm = storage_merge_ranges(input, (size_t)ni, align, merged, 32);
    VkSpan spans[32] = {0};
    memset(map, 0, 16 * 4 * sizeof(uint32_t));
    for (int i = 0; i < si->storage_buffers; i++) {
        Range r = st.sbuf[stage][i];
        VkSpan span = {0};
        uint64_t range_size = 16, delta = 0;
        for (size_t m = 0; m < nm && r.addr && r.size; m++) {
            if (r.addr < merged[m].addr || r.addr - merged[m].addr > merged[m].size ||
                r.size > merged[m].size - (r.addr - merged[m].addr)) continue;
            if (merged[m].size > g_vk.props.limits.maxStorageBufferRange) {
                FATAL("NVN storage buffer exceeds Vulkan maxStorageBufferRange (%llu bytes)\n", (unsigned long long)merged[m].size);
            }
            if (!spans[m].buf) spans[m] = guest_copy(merged[m].addr, merged[m].size);
            span = spans[m]; range_size = merged[m].size; delta = r.addr - merged[m].addr;
            map[i][0] = (uint32_t)r.addr; map[i][1] = (uint32_t)(r.addr >> 32);
            map[i][2] = (uint32_t)r.size; map[i][3] = (uint32_t)delta;
            if (si->writes_gmem) {
                bool found = false;
                for (int j = 0; j < storage_nwriteback; j++)
                    if (storage_writeback[j].addr == r.addr && storage_writeback[j].size == r.size) found = true;
                if (!found && storage_nwriteback < 32) storage_writeback[storage_nwriteback++] = (StorageWriteback){r.addr, r.size,
                    {.buf = span.buf, .off = span.off + delta, .ptr = (uint8_t *)span.ptr + delta}};
            }
            break;
        }
        if (!span.buf) { static const uint32_t zero[4]; span = vk_transient_copy(zero, sizeof zero, 256); }
        buffers[i] = (VkDescriptorBufferInfo){span.buf, span.off, range_size};
    }
    static int told;
    if (told++ < 8) LOG("[gmem] native NVN storage buffers: stage=%d slots=%u ranges=%zu writes=%d (no host import)\n",
                        stage, si->storage_buffers, nm, si->writes_gmem);
}

static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    VkDescriptorBufferInfo storage[16];
    uint32_t storage_map[16][4];
    if (si->storage_buffers) storage_stage_buffers(stage, si, storage, storage_map);
    SurfaceBindlessCache *surf = (si->uses_surface_write || si->uses_surface_atomic) ? surface_cache_get(st.tpool) : NULL;
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[65][4] = {{1, 1, 0, 0}};
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = b->count ? b->count : 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && b->binding == 120 && surf) {
            bi[n] = (VkDescriptorBufferInfo){surf->map_buf, 0, (VkDeviceSize)surf->map_count * 8};
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding == 121 && surf) {
            w[n].descriptorCount = b->count ? b->count : g_vk.bindless_storage_slots;
            w[n].pImageInfo = surf->write;
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding == 122 && surf) {
            w[n].descriptorCount = b->count ? b->count : g_vk.bindless_storage_slots;
            w[n].pImageInfo = surf->atomic;
        } else if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (si->storage_buffers && b->binding == 17 && b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            bi[n] = ubo_span(vk_transient_copy(storage_map, si->storage_buffers * 16, 256), si->storage_buffers * 16);
            w[n].pBufferInfo = &bi[n];
        } else if (si->storage_buffers && b->binding >= 64 && b->binding < 64u + si->storage_buffers && b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = storage[b->binding - 64];
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .flags = translated_pipeline_flags(g_vk.props.vendorID),
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; uint32_t const_offset, const_size; CsProg *cp; } g_cs_tab[256];

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr && tab[i].const_offset == ss->const_offset &&
        tab[i].const_size == ss->const_size) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) {
        tab[n] = (struct CsEnt){ss->code_addr, ss->const_offset, ss->const_size, cp}; g_cs_n = n + 1;
    }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    k.bindless_storage = g_vk.bindless_storage_slots;
    k.storage_buffers = storage_buffer_count();
    const uint8_t *constants = ss->const_offset ? (const uint8_t *)(uintptr_t)(ss->code_addr + ss->const_offset) : NULL;
    cp->si = maxwell_translate_with_constants((const uint8_t *)(uintptr_t)ss->code_addr,
        ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k, constants, ss->const_size);
    cp->p.vi = cp->si;
    log_shader_flow(cp->si, ss->code_addr, "cs");
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    if (cp->si->surface_fallbacks)
        LOG("[compute] storage-image compatibility: %u ops (code %llx)\n",
            cp->si->surface_fallbacks, (unsigned long long)ss->code_addr);
    if (cp->si->uses_surface_write || cp->si->uses_surface_atomic)
        LOG("[compute] native storage images: write=%d atomic=%d (code %llx)\n",
            cp->si->uses_surface_write, cp->si->uses_surface_atomic,
            (unsigned long long)ss->code_addr);
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    storage_nwriteback = 0;
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (cp) wait_built(&cp->p.state, "compute", st.program->stages[5].code_addr, 0);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_compute_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    surface_finish_writes(cp->si);
    stat_dispatches++;
    storage_finish_writes();
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
// Write directly into the mapped transient ring: the old path malloc'ed a CPU
// array, filled it, copied it into Vulkan staging, then freed it for every such
// draw.
static VkSpan expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype,
                             uint32_t first, uint32_t *out_count, uint32_t *out_max) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    VkSpan span = vk_transient((uint64_t)tris * 3 * 4 + 4, 4);
    uint32_t *out = span.ptr;
    uint32_t n = 0, mx = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
#define EMIT(v) do { uint32_t _v = (v); out[n++] = _v; if (_v > mx) mx = _v; } while (0)
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            EMIT(IDX(q)); EMIT(IDX(q + 1)); EMIT(IDX(q + 2));
            EMIT(IDX(q)); EMIT(IDX(q + 2)); EMIT(IDX(q + 3));
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) {
            EMIT(IDX(0)); EMIT(IDX(i)); EMIT(IDX(i + 1));
        }
    }
#undef EMIT
#undef IDX
    *out_count = n;
    if (out_max) *out_max = mx;
    return span;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static bool lut_slice_active;
// Execute the guest colour-grading shader, one 2D attachment slice at a time.
// This avoids the layered 3D rendering path that faulted on Windows AMD, while
// retaining the game's actual curve, exposure and colour-grading constants.
static bool draw_lut_slices(CmdRec *r, bool indexed) {
    NvnTexture *t = st.rt.ncolor > 0 ? st.rt.color[0] : NULL;
    if (!t || t->target != 2 || t->format != 0x3D || t->width != 32 || t->height != 32 || t->depth != 32)
        return false;
    if (st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) && st.rt.cview[0].num_layers == 1)
        return false; // the recursive single-slice draw, or an already explicit guest slice
    if (st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) &&
        (st.rt.cview[0].min_layer || (st.rt.cview[0].num_layers && st.rt.cview[0].num_layers != t->depth)))
        return false;
    for (int i = 1; i < st.rt.ncolor; i++) if (st.rt.color[i]) return false;
    uint32_t base = (uint32_t)r->x[indexed ? 5 : 3];
    uint32_t count = (uint32_t)r->x[indexed ? 6 : 4];
    const char *compat = getenv("LARECOMP_LUT_IDENTITY");
    /*
     * Keep the game's own colour cube by default.  The identity cube is only an
     * A/B diagnostic: captures proved that replacing the guest LUT makes Scarlet
     * substantially darker without fixing the blue/purple material colours.
     */
    bool force_identity = compat && atoi(compat);
    if (force_identity || base != 0 || count != t->depth || st.rt.depth ||
        (st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) && st.rt.cview[0].base_level != 0)) {
        bool filled = renderer_fill_identity_lut_3d(t);
        if (filled) {
            stat_draws++; g_draws_total++;
            static int identity_logged;
            if (!identity_logged++)
                LOG("[vk] Scarlet colour LUT: diagnostic neutral identity cube (LARECOMP_LUT_IDENTITY=1)\n");
        }
        return filled;
    }
    RtState saved = st.rt;
    bool saved_slice_active = lut_slice_active;
    lut_slice_active = true;
    bool complete = true;
    uint64_t issued_before = g_draws_total;
    for (uint32_t slice = 0; slice < t->depth; slice++) {
        CmdRec one;
        memcpy(&one, r, sizeof one);
        one.x[indexed ? 5 : 3] = slice;
        one.x[indexed ? 6 : 4] = 1;
        st.rt.cview[0].setmask |= 2;
        st.rt.cview[0].min_layer = (uint16_t)slice;
        st.rt.cview[0].num_layers = 1;
        st.rt.has_cview[0] = true;
        st.rt_dirty = true;
        uint64_t before = g_draws_total;
        draw_(&one, indexed);
        if (g_draws_total == before) { complete = false; break; }
    }
    vk_end_pass();
    st.rt = saved;
    lut_slice_active = saved_slice_active;
    st.rt_dirty = true;
    if (!complete) {
        // A rejected shader/pipeline must not leave an uninitialized LUT.
        // Finish any partial draws before the upload-CB compatibility copy.
        if (g_draws_total != issued_before) renderer_flush();
        renderer_fill_identity_lut_3d(t);
        static int failed_logged;
        if (!failed_logged++) LOG("[vk] Scarlet colour LUT: slice draw rejected; retaining identity compatibility fallback\n");
        return true;
    }
    static int logged;
    if (!logged++) LOG("[vk] Scarlet colour LUT: guest shader rendered into 32 individual depth slices\n");
    return true;
}

static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    storage_nwriteback = 0;
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIP(0); return; }
    if (draw_lut_slices(r, indexed)) return;
    if (ip_enabled()) {  // (frame interpolation: what identifies this draw from one frame to the next)
        // what the draw is (program, vertex buffer, parameters), and which game object it belongs to: the same mesh
        // drawn for two objects (bushes, Pidgeys) is told apart by the object, not by the order the game drew them in
        uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uintptr_t)st.program ^ ((uint64_t)indexed << 63);
        h = (h ^ st.vbuf[0].addr) * 0x100000001B3ull;
        for (int i = 0; i < 8; i++) h = (h ^ r->x[i]) * 0x100000001B3ull;
        if (st.obj) h = (h ^ st.obj) * 0x100000001B3ull, ip_stats_tagged++;
        else ip_stats_untagged++;
        ip_draw_sig = h;
        // 3D draws (a depth target with the depth test on) are blended, with or without a game object: the ground and the
        // scenery have none, and they must take the same (blended) light matrices as the shadow map the objects are drawn
        // into, or shadows slide away from their casters. Interface and full-screen passes don't test depth: frame N.
        ip_draw_obj = st.rt.depth != NULL && st.ds[0];
        // In menus (the Pokédex, the bag...) the 2D draws are blended too: lists and panels slide there. A 2D draw takes the
        // strict jump test in ip_cbuf (a recycled row or glyph moves far: it keeps frame N).
        ip_draw_2d = !ip_draw_obj;
        extern bool fps60_menu(void);
        if (fps60_menu()) ip_draw_obj = true;
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    uint64_t fs_addr = st.program->stages[1].code_addr;
    uint64_t fs_hash = fragment_code_hash(fs_addr);
    scarlet_scene_markers(fs_addr);
    fkey.sharp_scene = sharp_dof_variant(fs_hash);
    fkey.hair_isotropic = hair_lighting_variant(fs_hash);
    fkey.ring_neutral_grade = schoolyard_grade_variant(fs_addr);
    if (fkey.hair_isotropic) {
        static bool reported;
        if (!reported) { reported = true; LOG("[vk] Scarlet hair compatibility enabled by default (material 3 isotropic + albedo chroma preservation)\n"); }
    }
    if (fkey.ring_neutral_grade) {
        static bool grade_reported;
        if (!grade_reported) { grade_reported = true; LOG("[vk] Scarlet opening schoolyard atmosphere/chroma correction enabled\n"); }
    }
    vkey.layer_varying = fkey.layer_varying = lut_slice_active;
    vkey.bindless_storage = fkey.bindless_storage = g_vk.bindless_storage_slots;
    vkey.storage_buffers = fkey.storage_buffers = storage_buffer_count();
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    vkey.xfb = st.tf_active && g_vk.transform_feedback;
    if (vkey.xfb && st.program) {
        const NvnShaderStage *vs = &st.program->stages[0];
        // Scarlet's stream-output programs captured in graphics diagnostics all
        // carry this control signature. It identifies three streams, but NOT
        // fixed 12:4:4 byte widths: the preceding morph/copy passes allocate
        // 12:12:12 and export xyz, then skinning writes packed 12:4:4 streams.
        // Derive each width from the shader's stores instead of dropping yz.
        uint32_t tf0 = vs->tf_meta0, tf1 = vs->tf_meta1;
        if (tf0 == 0x00010101u && tf1 == 0x00000103u) {
            vkey.xfb_layout = 1;
            vkey.xfb_comp[0] = 4;
            vkey.xfb_comp[1] = 4;
            vkey.xfb_comp[2] = 4;
            static _Atomic int tf_layout_logged;
            if (!atomic_exchange(&tf_layout_logged, 1))
                LOG("[vk] Scarlet TF layout: control 10101/103 -> shader export widths (12:12:12 or 12:4:4; packed scalar streams raw-u32)\n");
        } else {
            static _Atomic unsigned tf_unknown_logs;
            if (atomic_fetch_add(&tf_unknown_logs, 1) < 8)
                LOG("[vk] unknown TF control layout: vs=%llx meta=%08x/%08x; using inferred outputs\n",
                    (unsigned long long)vs->code_addr, tf0, tf1);
        }
    }
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    vkey.stencil_tex_mask = stencil_texture_bindings(0);
    fkey.stencil_tex_mask = stencil_texture_bindings(1);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (pr) wait_built(&pr->state, "program", pr->code[0], pr->code[1]);
    if (!pr || atomic_load(&pr->state) != 1) { SKIP(pr ? 1 + (atomic_load(&pr->state) > 1) : 3); return; }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        uint64_t last_hash = fs_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    {   // (research, LARECOMP_COUNT_TEX=<w>x<h>: per frame, the draws and instances sampling a fragment texture of that size)
        static int tw = -1, th;
        if (tw < 0) { const char *e = getenv("LARECOMP_COUNT_TEX"); tw = 0; if (e) sscanf(e, "%dx%d", &tw, &th); }
        if (tw > 0) {
            static uint64_t frame = ~0ull;
            static unsigned nd, ni;
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull) LOG("[counttex] frame %llu: %u draws %u instances\n", (unsigned long long)frame, nd, ni);
                frame = renderer_present_count(), nd = ni = 0;
            }
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                if (t && (int)t->width == tw && (int)t->height == th) { nd++; ni += (unsigned)(indexed ? r->x[6] : r->x[4]); break; }
            }
        }
    }
    // Targeted Scarlet material trace. Character materials in the academy
    // consistently bind BC7-sRGB albedo together with BC5/BC7/BC4 auxiliary
    // maps. Their image-view swizzles are correct, so capture the fragment
    // program and the first words of the actually-used fragment constant
    // buffers to locate a bad tint/material parameter rather than changing
    // texture formats globally.
    static int log_material_draws = -1;
    if (log_material_draws < 0) log_material_draws = getenv("LARECOMP_LOG_MATERIAL_DRAWS") != NULL;
    if (log_material_draws) {
        NvnTexture *albedo = NULL;
        int albedo_slot = -1;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            if (t && t->format == 0x4E && t->width <= 512 && t->height <= 512) {
                albedo = t; albedo_slot = i; break;
            }
        }
        if (albedo) {
            static unsigned material_draw_logs;
            if (material_draw_logs++ < 512) {
                LOG("[material-draw] fs=%llx hash=%016llx albedo=t%d id=%llu addr=%llx %ux%u",
                    (unsigned long long)st.program->stages[1].code_addr,
                    (unsigned long long)fs_hash, albedo_slot,
                    (unsigned long long)albedo->debug_id,
                    (unsigned long long)albedo->addr, albedo->width, albedo->height);
                uint32_t mask = pr->fi->cbuf_mask;
                for (int bi = 0; bi < 18; bi++) {
                    if (!(mask & (1u << bi))) continue;
                    // Maxwell cbuf 0/1 are driver/compiler-owned. Guest NVN
                    // uniform-buffer slot N backs shader cbuf N+3.
                    if (bi < 3) {
                        LOG(" cb%d=special", bi);
                        continue;
                    }
                    int ui = bi - 3;
                    Range rb = st.ubuf[1][ui];
                    if (!rb.addr || rb.size < 16) {
                        LOG(" cb%d(u%d)=none", bi, ui);
                        continue;
                    }
                    const uint32_t *p = (const uint32_t *)(uintptr_t)rb.addr;
                    uint64_t words = rb.size / 4;
                    if (words > 16) words = 16;
                    LOG(" cb%d(u%d)@%llx[%llu]=", bi, ui,
                        (unsigned long long)rb.addr, (unsigned long long)rb.size);
                    for (uint64_t wi = 0; wi < words; wi++)
                        LOG("%s%08x", wi ? "," : "", p[wi]);
                }
                LOG("\n");
            }
        }
    }

    if (tf_barrier_pending && (!st.tf_active || st.tf_resume || tf_pending_used_by_draw(pr->vi, pr->fi)))
        tf_flush_pending_barrier();

    if (g_debug_frame) debug_draw(pr, r, indexed);
    else if ((pr->vi->attr_mask >> 11) & 1) {  // skinned draws (a11: bone indices)
        static int n;
        static int64_t dump_frame = -2;  // LARECOMP_DUMP_SKIN_FRAME: dump the first one from that frame on
        if (dump_frame == -2) dump_frame = getenv("LARECOMP_DUMP_SKIN_FRAME") ? atoll(getenv("LARECOMP_DUMP_SKIN_FRAME")) : -1;
        if (dump_frame >= 0 && renderer_present_count() >= (uint64_t)dump_frame) {
            debug_draw(pr, r, indexed);  // (debug_draw writes build/skin/ once)
            dump_frame = -1;
        } else {
            static int log_skin = -1;
            if (log_skin < 0) log_skin = getenv("LARECOMP_LOG_SKIN") != NULL;
            if (log_skin && n++ % 50 == 0 && n < 1000) {
                debug_draw(pr, r, indexed);
            }
        }
    }
    bool vertex_only_xfb = vkey.xfb;
    // XFB pipelines are vertex-only and rasterization-discarded. They cannot
    // sample in a fragment stage or modify color/depth attachments, so avoid
    // framebuffer feedback copies and attachment invalidation on this very hot
    // Scarlet path.
    if (!vertex_only_xfb) {
        resolve_feedback(pr);
        snap_reason = "draw-depth-stencil-write";
        if (depth_stencil_writes()) snapshots_invalidate(st.rt.depth);
        snap_reason = "draw-colorwrite";
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    }
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    if (st.tf_active && g_vk.transform_feedback && !st.tf_vk_active) {
        VkBuffer bufs[4], counter = VK_NULL_HANDLE;
        VkDeviceSize offs[4], sizes[4], coff = 0;
        uint32_t nbuf = 0, ncounter = 0;
        for (uint32_t i = 0; i < 4; i++) {
            if (!st.tfbuf[i].addr || !st.tfbuf[i].size) continue;
            VkBuffer b; VkDeviceSize o;
            if (i != nbuf || !guest_direct(st.tfbuf[i].addr, st.tfbuf[i].size, &b, &o)) break;
            bufs[nbuf] = b; offs[nbuf] = o; sizes[nbuf] = st.tfbuf[i].size; nbuf++;
        }
        {
            static int tf_begin_logs;
            if (tf_begin_logs < 24) {
                tf_begin_logs++;
                VkBuffer db; VkDeviceSize doff;
                bool d0 = st.tfbuf[0].addr && guest_direct(st.tfbuf[0].addr, st.tfbuf[0].size, &db, &doff);
                LOG("[tf-begin] #%d nbuf=%u tfbuf0=%llx+%llu tfbuf1=%llx+%llu tfbuf2=%llx+%llu direct0=%d heap=%llx..%llx\n",
                    tf_begin_logs, nbuf, (unsigned long long)st.tfbuf[0].addr, (unsigned long long)st.tfbuf[0].size,
                    (unsigned long long)st.tfbuf[1].addr, (unsigned long long)st.tfbuf[1].size,
                    (unsigned long long)st.tfbuf[2].addr, (unsigned long long)st.tfbuf[2].size, d0,
                    (unsigned long long)HEAP_BASE, (unsigned long long)(HEAP_BASE + gmem_size));
            }
        }
        if (nbuf) {
            if (!tf_api_ready()) { st.tf_active = false; }
            else tf_bind_buffers(cb, 0, nbuf, bufs, offs, sizes);
            if (st.tf_resume && st.tf_counter &&
                guest_direct(st.tf_counter, 4, &counter, &coff)) ncounter = 1;
            if (st.tf_active && tf_api_ready()) {
                tf_begin(cb, 0, ncounter, ncounter ? &counter : NULL, ncounter ? &coff : NULL);
                st.tf_vk_active = true;
            }
        }
    }

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(4); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    memcpy(k.color, pass_color, sizeof k.color);
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    // Alpha-to-coverage only has meaningful coverage with multisampling.
    // Scarlet leaves this state enabled on some 1x passes; forwarding it to
    // Vulkan turns alpha into single-sample coverage decisions (visible as the
    // heavy stipple/checker pattern on AMD). Preserve A2C for real MSAA, but
    // disable it for 1-sample targets.
    if (st.ms[0xC] && pass_samples == 1) stat_a2c_1x++;
    k.a2c = st.ms[0xC] != 0 && pass_samples > 1;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pp->hash = ph;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        pipe_history_note(&k);
        run_job(build_pipeline, pp); // required by this draw: always FIFO-first
    }
    pipe_history_prewarm(pr, ph);
    wait_built(&pp->state, "pipeline", pr->code[0], pr->code[1]);
    if (atomic_load(&pp->state) != 1) { SKIP(5 + (atomic_load(&pp->state) > 1)); return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    vkCmdSetDepthTestEnable(cb, dt);
    vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    vkCmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                              compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF));
            vkCmdSetStencilCompareMask(cb, face, st.stencil_vmask[f] & 0xFF);
            vkCmdSetStencilWriteMask(cb, face, st.stencil_mask[f] & 0xFF);
            vkCmdSetStencilReference(cb, face, st.stencil_ref[f]);
        }
    }
    vkCmdSetBlendConstants(cb, st.blend_color);

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    vkCmdSetCullMode(cb, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    vkCmdSetFrontFace(cb, front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cb, st.prim_restart && native && (mode == 3 || mode == 5));
    vkCmdSetRasterizerDiscardEnable(cb, (st.raster_discard || vkey.xfb) ? VK_TRUE : VK_FALSE);
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        vkCmdSetDepthBiasEnable(cb, on);
        vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // (the per-draw tint the translator can apply to colour target 0: none)
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    if (!vertex_only_xfb) {
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
            float sc[2]; uint32_t wh[2];
            texture_info(1, pr->fi, i, &fii[i], sc, wh);
            tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
        }
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (!vertex_only_xfb && fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // (research, LARECOMP_COUNT_FS=<fs hash>: draws with that fragment shader per frame, logged when the count changes)
        static uint64_t want = 1, frame = ~0ull;
        static unsigned n, last = ~0u;
        if (want == 1) { const char *e = getenv("LARECOMP_COUNT_FS"); want = e ? strtoull(e, NULL, 16) : 0; }
        if (want) {
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull && n != last) LOG("[countfs] frame %llu: %u\n", (unsigned long long)frame, n);
                if (frame != ~0ull) last = n;
                frame = renderer_present_count(), n = 0;
            }
            if (g_cur_fs_hash == want) n++;
        }
    }
    {
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    if (vertex_only_xfb) {
        // Pipeline stageCount is one for XFB, so set 1 is not statically used.
        // Binding only the vertex set avoids thousands of fragment descriptor
        // allocations/updates per second in Scarlet's stream-output passes.
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 1, &vset, 0, NULL);
    } else {
        VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
        VkDescriptorSet sets[2] = {vset, fset};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);
    }
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            ib = expand_indices(mode, count, NULL, 0, first, &icount, NULL);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            ib = expand_indices(mode, count, src, (int)itype, 0, &icount, &max_index);
        }
        if (!scan_src) {
            // Vertex fetch for indexed draws addresses index + baseVertex.  A
            // negative baseVertex does not increase the upper bound, while a
            // positive one does.  Keep the bound in signed space so malformed
            // guest data cannot wrap a negative base into a multi-GB copy.
            uint64_t upper = (uint64_t)max_index + 1u;
            if (base_vertex > 0) upper += (uint32_t)base_vertex;
            max_index = upper > UINT32_MAX ? UINT32_MAX : (uint32_t)upper;
        }
    }
    if (use_index) vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                uint64_t upper = (uint64_t)max_index + 1u;
                if (base_vertex > 0) upper += (uint32_t)base_vertex;
                max_index = upper > UINT32_MAX ? UINT32_MAX : (uint32_t)upper;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    if (g_debug_frame && !vertex_only_xfb && pr->vi->attr_mask && st.rt.depth && st.rt.ncolor > 1)
        debug_mrt_drawn = true;
    surface_finish_writes(pr->vi);
    if (!vertex_only_xfb) surface_finish_writes(pr->fi);
    bool surface_write = pr->vi->uses_surface_write || pr->vi->uses_surface_atomic ||
                         (!vertex_only_xfb && (pr->fi->uses_surface_write || pr->fi->uses_surface_atomic));
    stat_draws++;
    g_draws_total++;
    cum_draws++;
    // Storage-image writes are side effects outside the color/depth attachments.
    // End dynamic rendering so the next operation takes begin_pass()/copy/compute's
    // normal full barrier and observes the SUST/SURED result.
    if (surface_write) vk_end_pass();
    storage_finish_writes();
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static FILE *forensic_fp;
static uint64_t forensic_frame = ~0ull, forensic_draw_no;
static uint64_t forensic_saved_shader[2048];
static uint32_t forensic_saved_shader_n;

static void forensic_save_shader(uint64_t addr, const char *stage, const ShaderInfo *si) {
    if (!addr || !si || !si->msl) return;
    for (uint32_t i = 0; i < forensic_saved_shader_n; i++)
        if (forensic_saved_shader[i] == (addr ^ (stage[0] == 'f' ? 1ull : 0ull))) return;
    if (forensic_saved_shader_n < 2048)
        forensic_saved_shader[forensic_saved_shader_n++] = addr ^ (stage[0] == 'f' ? 1ull : 0ull);
    mkdir("build/forensics-shaders", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/forensics-shaders/%llx_%s.glsl", (unsigned long long)addr, stage);
    FILE *sf = fopen(path, "w");
    if (sf) { fputs(si->msl, sf); fclose(sf); }
}

static void forensic_draw_record(Prog *pr, CmdRec *r, bool indexed) {
    if (!g_debug_frame) return; // skin logging alone must not snapshot texture assets
    uint64_t frame = renderer_present_count();
    if (frame != forensic_frame) {
        if (forensic_fp) fclose(forensic_fp);
        forensic_fp = NULL;
        forensic_frame = frame;
        forensic_draw_no = 0;
        forensic_saved_shader_n = 0;
        mkdir("build", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/scarlet_forensics_%llu.txt", (unsigned long long)frame);
        forensic_fp = fopen(path, "w");
        if (forensic_fp) {
            fprintf(forensic_fp, "# capture_started_unix_s=%lld\n", (long long)time(NULL));
            fprintf(forensic_fp, "# texture_sources=%s\n", nvn_diag_texture_capture_dir(frame));
            fprintf(forensic_fp,
                "# Scarlet graphics forensics frame %llu\n"
                "# barrier bits: 1 ORDER_PRIMITIVES, 2 ORDER_FRAGMENTS, 4 ORDER_FRAGMENTS_TILED, "
                "8 ORDER_INDIRECT_DATA, 10 INVALIDATE_TEXTURE, 20 INVALIDATE_SHADER, "
                "40 INVALIDATE_TEXTURE_DESCRIPTOR, 80 INVALIDATE_ZCULL\n"
                "# columns: draw pass type mode count inst vs fs rt0 depth samples tf rdiscard a2c attrs "
                "barriers_since_prev last_barrier textures...\n",
                (unsigned long long)frame);
        }
    }
    if (!forensic_fp) return;
    uint64_t vs = st.program ? st.program->stages[0].code_addr : 0;
    uint64_t fs = st.program ? st.program->stages[1].code_addr : 0;
    uint64_t inst = indexed ? r->x[6] : r->x[4];
    uint32_t rt_fmt = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0;
    uint32_t rt_w = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0;
    uint32_t rt_h = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0;
    uint32_t d_fmt = st.rt.depth ? st.rt.depth->format : 0;
    fprintf(forensic_fp,
        "%llu pass=%d %c mode=%llu count=%llu inst=%llu vs=%llx fs=%llx "
        "rt0=%ux%u/f%#x depth=%#x samples=%u tf=%u discard=%u a2c=%u attrs=%#x "
        "barriers=%llu last=%#x",
        (unsigned long long)forensic_draw_no++, debug_pass_no, indexed ? 'E' : 'A',
        (unsigned long long)r->x[0], (unsigned long long)r->x[2], (unsigned long long)inst,
        (unsigned long long)vs, (unsigned long long)fs,
        rt_w, rt_h, rt_fmt, d_fmt, pass_samples, st.tf_active ? 1u : 0u,
        st.raster_discard ? 1u : 0u, st.ms[0xC] ? 1u : 0u, pr->vi ? pr->vi->attr_mask : 0,
        (unsigned long long)forensic_barriers_since_draw, forensic_last_barrier_flags);
    if (st.rt.ncolor > 0 && st.rt.color[0] && st.rt.has_cview[0]) {
        const NvnTextureView *rv = &st.rt.cview[0];
        fprintf(forensic_fp, " rtview=set%#x", rv->setmask);
        if (rv->setmask & 4) fprintf(forensic_fp, "/f%#x", rv->format);
        if (rv->setmask & 1) fprintf(forensic_fp, "/lv%u+%u", rv->base_level, rv->num_levels);
        if (rv->setmask & 2) fprintf(forensic_fp, "/ly%u+%u", rv->min_layer, rv->num_layers);
    }
    fprintf(forensic_fp, " ds=%02x%02x%02x/st%u/ops%02x%02x%02x/ref%02x,%02x/mask%02x,%02x",
            st.ds[0], st.ds[1], st.ds[2], st.ds[3], st.ds[5], st.ds[6], st.ds[7],
            st.stencil_ref[0], st.stencil_ref[1], st.stencil_mask[0], st.stencil_mask[1]);
    for (int i = 1; i < st.rt.ncolor; i++) {
        const NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        fprintf(forensic_fp, " rt%d=%llx/%ux%u/f%#x", i, (unsigned long long)t->addr,
                t->width, t->height, t->format);
        if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4)) fprintf(forensic_fp, "/viewf%#x", st.rt.cview[i].format);
    }
    if (st.vbuf[0].addr)
        fprintf(forensic_fp, " v0=%llx+%llu/stride%u", (unsigned long long)st.vbuf[0].addr,
                (unsigned long long)st.vbuf[0].size, st.nstream > 0 ? st.stream[0][0] : 0);
    if (st.tf_active) {
        for (int ti = 0; ti < 4; ti++) if (st.tfbuf[ti].addr)
            fprintf(forensic_fp, " tf%d=%llx+%llu", ti, (unsigned long long)st.tfbuf[ti].addr,
                    (unsigned long long)st.tfbuf[ti].size);
    }
    if (pr->vi) {
        fprintf(forensic_fp, " vcb=%#x", pr->vi->cbuf_dynamic);

        // Bone indices/weights are Scarlet a12/a13. Record the real stream and
        // raw values so one capture can distinguish bad input data from bad
        // palette binding/translation.
        for (int ai = 11; ai <= 13; ai++) if (pr->vi->attr_mask & (1u << ai)) {
            uint32_t av = ai < st.nattrib ? st.attrib[ai] : 0;
            uint32_t stream = (av >> 8) & 0x1f, off = av >> 13;
            fprintf(forensic_fp, " a%d=f%#x/s%u/+%u", ai, av & 0xff, stream, off);
            if (stream < 16 && st.vbuf[stream].addr) {
                fprintf(forensic_fp, "@%llx+%llu/stride%u", (unsigned long long)st.vbuf[stream].addr,
                        (unsigned long long)st.vbuf[stream].size,
                        stream < (uint32_t)st.nstream ? st.stream[stream][0] : 0);
            }
        }

        // Shader CB k maps to NVN uniform slot k-3. The previous forensic text
        // printed st.ubuf[k] directly, which mislabeled the palette.
        int dumped_cb = 0;
        for (int k = 0; k < 18 && dumped_cb < 8; k++) {
            if (!(pr->vi->cbuf_mask & (1u << k))) continue;
            if (k == 0) { fprintf(forensic_fp, " CB0=driver"); dumped_cb++; continue; }
            if (k == 1) {
                NvnShaderStage *ss = st.program ? &st.program->stages[0] : NULL;
                fprintf(forensic_fp, " CB1=code+%u/%u", ss ? ss->const_offset : 0, ss ? ss->const_size : 0);
                dumped_cb++; continue;
            }
            if (k < 3) continue;
            int ui = k - 3;
            Range cr = ui >= 0 && ui < 18 ? st.ubuf[0][ui] : (Range){0,0};
            fprintf(forensic_fp, " CB%d=%llx+%llu%s", k, (unsigned long long)cr.addr,
                    (unsigned long long)cr.size, (pr->vi->cbuf_dynamic & (1u << k)) ? "/dyn" : "");
            dumped_cb++;
        }

        // For skinning TF draws, dump the first four raw index/weight tuples.
        if (st.tf_active && (pr->vi->attr_mask & (1u << 12)) && (pr->vi->attr_mask & (1u << 13))) {
            for (int vi = 0; vi < 4; vi++) {
                fprintf(forensic_fp, " skin%d", vi);
                for (int ai = 12; ai <= 13; ai++) {
                    uint32_t av = ai < st.nattrib ? st.attrib[ai] : 0;
                    uint32_t stream = (av >> 8) & 0x1f, off = av >> 13;
                    uint32_t stride = stream < (uint32_t)st.nstream ? st.stream[stream][0] : 0;
                    if (stream >= 16 || !st.vbuf[stream].addr || !stride) { fprintf(forensic_fp, "/a%d=NA", ai); continue; }
                    uint64_t at = st.vbuf[stream].addr + (uint64_t)vi * stride + off;
                    if (at + 16 > st.vbuf[stream].addr + st.vbuf[stream].size) { fprintf(forensic_fp, "/a%d=OOB", ai); continue; }
                    const uint8_t *q = (const uint8_t *)(uintptr_t)at;
                    fprintf(forensic_fp, "/a%d=%02x%02x%02x%02x_%02x%02x%02x%02x", ai,
                            q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]);
                }
            }
        }
    }
    int nt = pr->fi ? pr->fi->ntex : 0;
    if (nt > 12) nt = 12;
    for (int i = 0; i < nt; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { fprintf(forensic_fp, " t%d=none", i); continue; }
        nvn_diag_texture_source(t, b && b->gpu_owned, frame);
        NvnTexEntry *entry = shader_texture_entry(1, &pr->fi->tex[i]);
        fprintf(forensic_fp, " t%d=%llx/%ux%u/f%#x/target%u%s", i,
                (unsigned long long)t->addr, t->width, t->height, t->format, t->target,
                b && b->gpu_owned ? "/gpu" : "");
        if (entry && entry->has_view) {
            const NvnTextureView *tv = &entry->view;
            if (tv->setmask & 4) fprintf(forensic_fp, "/viewf%#x", tv->format);
            if (tv->setmask & 1) fprintf(forensic_fp, "/lv%u+%u", tv->base_level, tv->num_levels);
            if (tv->setmask & 2) fprintf(forensic_fp, "/ly%u+%u", tv->min_layer, tv->num_layers);
        }
        if (entry && nvn_texture_samples_stencil(t, &entry->view, entry->has_view)) fprintf(forensic_fp, "/stencil");
        if (b && getenv("LARECOMP_FORENSICS_DEPTH") && renderer_format_is_depth(t->format)) {
            static NvnTexture *saved_depth_tex[8];
            static uint32_t saved_depth_n;
            bool seen = false;
            for (uint32_t di = 0; di < saved_depth_n; di++) if (saved_depth_tex[di] == t) seen = true;
            if (!seen && saved_depth_n < 8) {
                char dp[128];
                saved_depth_tex[saved_depth_n] = t;
                snprintf(dp, sizeof dp, "build/fbdepth_sample_%03u.pgm", saved_depth_n++);
                debug_save_depth((TexBackend *)b, dp);
            }
        }
    }
    int nvt = pr->vi ? pr->vi->ntex : 0;
    if (nvt > 12) nvt = 12;
    for (int i = 0; i < nvt; i++) {
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { fprintf(forensic_fp, " vt%d=none", i); continue; }
        nvn_diag_texture_source(t, b && b->gpu_owned, frame);
        fprintf(forensic_fp, " vt%d=%llx/%ux%u/f%#x", i,
                (unsigned long long)t->addr, t->width, t->height, t->format);
    }
    fputc('\n', forensic_fp);
    fflush(forensic_fp);  // process exits with _exit on some paths; never lose the tail of the forensic frame
    forensic_barriers_since_draw = 0;
    forensic_last_barrier_flags = 0;
    forensic_save_shader(vs, "vs", pr->vi);
    forensic_save_shader(fs, "fs", pr->fi);
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    forensic_draw_record(pr, r, indexed);
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    LOG("[draw]   attribs:");  // format / stream / offset per attribute, and the streams' strides
    for (int i = 0; i < st.nattrib; i++)
        if ((pr->vi->attr_mask >> i) & 1)
            LOG(" a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int s = 0; s < st.nstream && s < 4; s++) LOG(" | stream%d stride %u", s, st.stream[s][0]);
    LOG("\n");
    if (getenv("LARECOMP_DUMP_SKIN") && ((pr->vi->attr_mask >> 11) & 1)) {  // one skinned draw's inputs -> build/skin/
        static int dumped;
        const char *at_frame = getenv("LARECOMP_DUMP_SKIN_FRAME");  // (the first one from that frame on)
        if (at_frame ? renderer_present_count() >= (uint64_t)atoll(at_frame) && !dumped++
                     : ++dumped == (atoi(getenv("LARECOMP_DUMP_SKIN")) > 1 ? atoi(getenv("LARECOMP_DUMP_SKIN")) : 1)) {  // (or the Nth skinned draw)
            mkdir("build/skin", 0755);
            char path[256];
            FILE *f;
            snprintf(path, sizeof path, "build/skin/info.txt");
            if ((f = fopen(path, "w"))) {
                fprintf(f, "vs %llx stride %u\n", (unsigned long long)st.program->stages[0].code_addr, st.stream[0][0]);
                for (int i = 0; i < st.nattrib; i++)
                    if ((pr->vi->attr_mask >> i) & 1)
                        fprintf(f, "attr %d fmt %#x off %u\n", i, st.attrib[i] & 0xFF, st.attrib[i] >> 13);
                for (int c = 0; c < 18; c++)
                    if ((pr->vi->cbuf_mask >> (c + 3)) & 1)
                        fprintf(f, "cb %d size %u\n", c + 3, (unsigned)st.ubuf[0][c].size);
                fclose(f);
            }
            uint64_t vsz = (uint64_t)st.stream[0][0] * 64;
            if (st.vbuf[0].addr && (f = fopen("build/skin/verts.bin", "wb"))) { fwrite((void *)(uintptr_t)st.vbuf[0].addr, 1, vsz < st.vbuf[0].size ? vsz : st.vbuf[0].size, f); fclose(f); }
            for (int c = 0; c < 18; c++) {
                if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
                snprintf(path, sizeof path, "build/skin/cb%d.bin", c + 3);
                if ((f = fopen(path, "wb"))) { fwrite((void *)(uintptr_t)st.ubuf[0][c].addr, 1, st.ubuf[0][c].size, f); fclose(f); }
            }
            {   // compiler constants (cb1) live after the shader code
                NvnShaderStage *ss = &st.program->stages[0];
                if (ss->const_offset && (f = fopen("build/skin/cb1.bin", "wb"))) {
                    fwrite((void *)(uintptr_t)(ss->code_addr + ss->const_offset), 1, ss->const_size ? ss->const_size : 256, f);
                    fclose(f);
                }
            }
            LOG("[skin] dumped draw inputs of vs %llx to build/skin/\n", (unsigned long long)st.program->stages[0].code_addr);
            extern uint64_t g_skin_watch;  // (renderer_stats: is this cbuf filled later, by the GPU?)
            g_skin_watch = st.ubuf[0][0].addr;
            const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
            LOG("[skin] cb3 %llx at draw: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
        }
    }
    if (getenv("LARECOMP_DEBUG_VSCB"))
        LOG("[draw]   vbuf0 %llx+%llu vbuf1 %llx+%llu\n", (unsigned long long)st.vbuf[0].addr,
            (unsigned long long)st.vbuf[0].size, (unsigned long long)st.vbuf[1].addr, (unsigned long long)st.vbuf[1].size);
    if (getenv("LARECOMP_DEBUG_VSCB") && st.vbuf[0].addr) {  // first vertices: bytes of attrs 11 / 12
        const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
        uint32_t stride = st.stream[0][0];
        for (int i = 0; i < 4 && stride; i++) {
            const uint8_t *p = v + (uint64_t)i * stride;
            LOG("[draw]   v%d pos %.3f %.3f %.3f", i, ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
            for (int a = 11; a <= 12; a++) {
                if (!((pr->vi->attr_mask >> a) & 1)) continue;
                const uint8_t *q = p + (st.attrib[a] >> 13);
                LOG(" a%d %u %u %u %u", a, q[0], q[1], q[2], q[3]);
            }
            LOG("\n");
        }
    }
    for (int c = 0; c < 18 && getenv("LARECOMP_DEBUG_VSCB"); c++) {  // dynamically indexed vertex cbufs (bone palettes)
        if (!((pr->vi->cbuf_dynamic >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
        LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
        int n = (int)(st.ubuf[0][c].size / 4), bad = 0, ident = 0;
        float mx = 0;
        for (int k = 0; k < n; k++) {
            float f = u[k];
            if (f != f || f > 1e6f || f < -1e6f) bad++;
            else if (fabsf(f) > mx) mx = fabsf(f);
        }
        for (int m = 0; m + 16 <= n; m += 16) {
            bool id = true;
            for (int k = 0; k < 16; k++) if (u[m + k] != ((k % 5) == 0 ? 1.0f : 0.0f)) id = false;
            ident += id;
        }
        LOG(" floats %d, non-finite/huge %d, max |x| %.4g, identity 4x4 %d of %d | m1:", n, bad, mx, ident, n / 16);
        for (int k = 16; k < 32 && k < n; k++) LOG(" %.4g", u[k]);
        LOG(" | m5:");
        for (int k = 80; k < 96 && k < n; k++) LOG(" %.4g", u[k]);
        LOG("\n");
    }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s | target %u flags %#x layers %u bh %u addr %llx "
            "stride %llu%s%s '%s'\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "",
            t->target, t->flags, t->layout.layers, t->layout.block_height_log2, (unsigned long long)t->addr,
            (unsigned long long)t->stride, b && b->gpu_owned ? " gpu-owned" : "", b && b->dynamic ? " dynamic" : "", t->label);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (getenv("LARECOMP_LOG_BUF2TEX") && (t->format == 0x1 || t->format == 0xd)) {  // (movie planes)
        static int n;
        if (n++ < 12) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)src;
            unsigned sum = 0;
            for (int i = 0; i < 4096; i++) sum += p[(uint64_t)i * 97 % (row * (uint64_t)rg[4] ? row * (uint64_t)rg[4] : 1)];
            LOG("[vk] buf2tex src %llx -> %ux%u fmt %#x region %d,%d %dx%d row %llu sample-sum %u\n",
                (unsigned long long)src, t->width, t->height, t->format, rg[0], rg[1], rg[3], rg[4],
                (unsigned long long)row, sum);
        }
    }
    extern void vk_tex_unscale(NvnTexture *t);
    vk_tex_unscale(t);  // the copied data is full size: no output-resolution scaling for this texture
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_transfer_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_graphics_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA && getenv("LARECOMP_NO_SMAA") == NULL;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_graphics_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_graphics_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_transfer_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    // QueueSubmitCommands already pins every top-level list in the whole batch
    // before execution starts and passes us its stable host-chain pointer.
    // Called lists still arrive as guest handles and must be pinned locally.
    // Avoiding a second top-level pin removes two mutex/chain walks per submit
    // and, more importantly, keeps refcounts balanced when guest slots recycle.
    uint64_t pin_handle = 0;
    uint64_t p = handle;
    if (depth > 0) {
        p = nvn_cmdstream_pin(handle);
        pin_handle = p;
    }
    if (depth == 0) st.obj = 0, st.obj_draw = 0;  // (a submitted list starts with no object; its called lists inherit)
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (__builtin_expect(op_counts_enabled > 0, 0) && r->op < CMD_COUNT) {
            op_counts[r->op]++;
        }
        if (ip_replaying && ip_skip_op(r->op)) { p += r->size; continue; }
        // TF writes can remain inside one rendering instance across state-only
        // commands and independent TF draws. Operations that can observe memory
        // must retain the old full-barrier semantics.
        if (tf_barrier_pending) {
            switch (r->op) {
            case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT:
            case CMD_COPY_BUFFER_TO_BUFFER: case CMD_COPY_BUFFER_TO_TEXTURE:
            case CMD_COPY_TEXTURE_TO_BUFFER: case CMD_COPY_TEXTURE_TO_TEXTURE:
            case CMD_DOWNSAMPLE: case CMD_UPDATE_UNIFORM_BUFFER:
            case CMD_CLEAR_BUFFER:
                stat_tf_dep_op++;
                tf_flush_pending_barrier();
                break;
            default:
                break;
            }
        }
        switch (r->op) {
        case CMD_END:
            if (pin_handle) nvn_cmdstream_unpin(pin_handle);
            return;
        case CMD_OBJECT: st.obj = r->x[0]; st.obj_draw = 0; { static int d; if (getenv("LARECOMP_OBJDBG") && vk_frame_counter() == 1450 && d++ < 80) LOG("[objdbg] depth %d object %llx\n", depth,(unsigned long long)r->x[0]); } break;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            uint64_t obj = st.obj;  // (a called list names objects of its own: they end with it)
            uint32_t obj_draw = st.obj_draw;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                st.obj = obj, st.obj_draw = obj_draw;
                exec_list(h[i], depth + 1);
            }
            st.obj = obj, st.obj_draw = obj_draw;
            break;
        }
        case CMD_BIND_PROGRAM: bind_program_stages((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 4; i++)
                memcpy(&st.tfbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BEGIN_TRANSFORM_FEEDBACK:
        case CMD_RESUME_TRANSFORM_FEEDBACK:
            if (g_vk.transform_feedback) {
                st.tf_active = true;
                st.tf_vk_active = false;
                st.tf_resume = r->op == CMD_RESUME_TRANSFORM_FEEDBACK;
                st.tf_counter = r->x[0];
            }
            break;
        case CMD_END_TRANSFORM_FEEDBACK:
        case CMD_PAUSE_TRANSFORM_FEEDBACK: {
            if (!g_vk.transform_feedback || !st.tf_active) break;
            if (!st.tf_vk_active) {
                static int tf_idle_ends;
                if (tf_idle_ends++ < 12)
                    LOG("[tf-end] #%d End/Pause with no TF draw issued since Begin (no draw reached the TF block)\n", tf_idle_ends);
            }
            if (st.tf_vk_active) {
                // EXT_transform_feedback requires Begin/End inside the same rendering instance.
                VkCommandBuffer cb = vk_cmd();
                VkBuffer counter = VK_NULL_HANDLE;
                VkDeviceSize coff = 0;
                uint32_t ncounter = 0;
                if (r->x[0] && guest_direct(r->x[0], 4, &counter, &coff)) ncounter = 1;
                if (tf_api_ready())
                    tf_end(cb, 0, ncounter, ncounter ? &counter : NULL, ncounter ? &coff : NULL);
                st.tf_vk_active = false;
                for (int bi = 0; bi < 4; bi++) tf_pending_add(st.tfbuf[bi].addr, st.tfbuf[bi].size);
                stat_tf_merged++;
                // Keep the dynamic-rendering instance open across independent TF passes. A barrier is
                // inserted lazily before a consumer, a resume counter read, a render-target change, or submission.
                //
                // The old unconditional [tf-check] path flushed and waited for the whole GPU on the first 12 TF
                // passes. That was useful while bringing stream output up, but it stalls Scarlet exactly when the
                // character/world geometry begins using TF. Keep the expensive readback strictly diagnostic.
                static int tf_checks;
                static int tf_check_enabled = -1;
                if (tf_check_enabled < 0) tf_check_enabled = getenv("LARECOMP_TF_CHECK") != NULL;
                if (tf_check_enabled && tf_checks < 12) {
                    tf_checks++;
                    uint64_t sq = vk_pending_seq();
                    renderer_flush();
                    vk_wait_seq(sq);
                    for (int bi = 0; bi < 3; bi++) {
                        if (!st.tfbuf[bi].addr || !st.tfbuf[bi].size) continue;
                        const float *f = (const float *)(uintptr_t)st.tfbuf[bi].addr;
                        uint64_t n = st.tfbuf[bi].size / 4, nan = 0, huge = 0, zero = 0;
                        for (uint64_t i = 0; i < n; i++) {
                            float v = f[i];
                            if (v != v) nan++;
                            else if (v > 1e6f || v < -1e6f) huge++;
                            else if (v == 0.0f) zero++;
                        }
                        LOG("[tf-check] #%d buf%d addr=%llx size=%llu words=%llu nan=%llu huge=%llu zero=%llu first: %g %g %g %g %g %g\n",
                            tf_checks, bi, (unsigned long long)st.tfbuf[bi].addr, (unsigned long long)st.tfbuf[bi].size,
                            (unsigned long long)n, (unsigned long long)nan, (unsigned long long)huge, (unsigned long long)zero,
                            n > 0 ? f[0] : 0, n > 1 ? f[1] : 0, n > 2 ? f[2] : 0, n > 3 ? f[3] : 0, n > 4 ? f[4] : 0, n > 5 ? f[5] : 0);
                    }
                }
            }
            st.tf_active = false;
            st.tf_resume = false;
            st.tf_counter = 0;
            break;
        }
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;

            // Capture Scarlet's true temporal scene pair in the native renderer.
            // It remains retained across later post-processing/UI passes just as
            // in the verified legacy bridge, but no video_core object is involved.
            scarlet_temporal_observe(st.rt.color, st.rt.cview, st.rt.has_cview,
                                     st.rt.ncolor, st.rt.depth, &st.rt.dview,
                                     st.rt.has_dview != 0);

            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET:
            for (int i = 0; i < 3; i++) {
                uint32_t bits = (uint32_t)r->x[4 + i];
                memcpy(&st.poly_offset[i], &bits, sizeof bits);
            }
            break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_RASTERIZER_DISCARD:
            st.raster_discard = r->x[0] != 0;
            stat_raster_discard_sets++;
            if (st.raster_discard) stat_raster_discard_on++;
            break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x color %.3f %.3f %.3f %.3f\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    ((const float *)x)[0], ((const float *)x)[1], ((const float *)x)[2], ((const float *)x)[3]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                uint32_t depth_bits = (uint32_t)r->x[6];
                float depth_value;
                memcpy(&depth_value, &depth_bits, sizeof depth_value);
                clear(-1, 0, NULL, 0, r->x[0] != 0, depth_value, has_stencil && r->x[2] != 0,
                      (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH_INDIRECT: {
            renderer_flush();  // the argument buffer may have been produced by compute
            if (r->x[0]) { uint32_t groups[3]; memcpy(groups, (const void *)(uintptr_t)r->x[0], sizeof groups);
                dispatch_compute(groups[0], groups[1], groups[2]); }
            break;
        }
        case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT:
        case CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT: case CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT: {
            bool indexed = r->op == CMD_DRAW_ELEMENTS_INDIRECT || r->op == CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT;
            bool multi = r->op == CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT || r->op == CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT;
            renderer_flush();
            uint64_t address = r->x[indexed ? 3 : 1];
            uint32_t count = 1, stride = indexed ? 20 : 16;
            if (multi) {
                uint64_t count_addr = r->x[indexed ? 4 : 2];
                uint32_t max_count = (uint32_t)r->x[indexed ? 5 : 3];
                uint32_t requested_stride = (uint32_t)r->x[indexed ? 6 : 4];
                if (!count_addr || !max_count || (int32_t)max_count < 0) break;
                memcpy(&count, (const void *)(uintptr_t)count_addr, sizeof count);
                if (count > max_count) count = max_count;
                if (requested_stride) { if (requested_stride < stride || requested_stride % 4) break; stride = requested_stride; }
            }
            for (uint32_t i = 0; address && i < count; i++) {
                CmdRec direct;
                if (nvn_indirect_draw(r, (const void *)(uintptr_t)address, indexed, &direct)) draw(&direct, indexed);
                if (address > UINT64_MAX - stride) break;
                address += stride;
            }
            break;
        }
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            static int frozen = -1;  // 1: frozen; 2: ideal (LARECOMP_VK_COUNTER=ideal: a 33.3 ms frame, 1 ms per counter in it)
            if (frozen < 0) { const char *e = getenv("LARECOMP_VK_COUNTER"); frozen = !e ? 0 : !strcmp(e, "frozen") ? 1 : !strcmp(e, "ideal") ? 2 : 0; }
            static uint64_t frame_seen = ~0ull, frame_ts, in_frame;
            extern uint64_t vk_frame_counter(void);
            if (vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); in_frame = 0; }
            uint64_t v = frozen == 1 ? frame_ts : frozen == 2 ? frame_seen * 33333333ull + in_frame * 1000000ull : SDL_GetTicksNS();
            in_frame++;
            if (out) { out[0] = 0; out[1] = v; }
            static int clog = -1;  // LARECOMP_COUNTER_LOG=1: each counter, with the draws recorded since the previous one
            if (clog < 0) clog = getenv("LARECOMP_COUNTER_LOG") != NULL;
            static uint64_t last_draws;
            if (clog) LOG("[counter] frame %llu #%llu value %llu draws %llu\n", (unsigned long long)frame_seen, (unsigned long long)in_frame,
                          (unsigned long long)v, (unsigned long long)(g_draws_total - last_draws));
            last_draws = g_draws_total;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        case CMD_BARRIER: {
            uint32_t flags = (uint32_t)r->x[0];
            stat_barrier_flags[flags & 0xFF]++;
            forensic_barriers_since_draw++;
            forensic_last_barrier_flags = flags;
            /*
             * NVN barrier bits are scoped: ORDER_FRAGMENTS(_TILED) and
             * ORDER_INDIRECT_DATA do not make transform-feedback output a
             * vertex/shader resource. The old code ended rendering and issued a
             * full Vulkan barrier for every barrier opcode, which is why
             * Scarlet had tfmerge == barrier for thousands of tiny TF passes.
             *
             * Keep TF pending across pure ordering barriers. Actual consumers
             * are synchronized by the draw binding overlap checks, indirect
             * draws already renderer_flush(), and copy/compute/update paths
             * flush above. Shader/texture invalidation is conservatively kept
             * because those consumers can be address-derived and not visible in
             * the fixed NVN binding ranges.
             */
            const uint32_t INVALIDATE_TEXTURE = 0x10u;
            const uint32_t INVALIDATE_SHADER  = 0x20u;
            if (tf_barrier_pending && (flags & (INVALIDATE_TEXTURE | INVALIDATE_SHADER))) {
                stat_tf_dep_op++;
                tf_flush_pending_barrier();
            }
            break;
        }
        default: break;
        }
        p += r->size;
    }
    nvn_cmdstream_unpin(pin_handle);
}

// Scarlet can submit NVN work from more than one guest thread. The Vulkan
// backend was inherited from a runtime where command execution was effectively
// single-threaded, and almost all renderer state below is global. Diagnose real
// overlap at the executor boundary instead of merely remembering the last thread
// that called vk_cmd() (which would produce false positives after ownership ended).
static _Atomic(uintptr_t) vk_exec_owner;
static _Atomic(unsigned) vk_exec_active;
static _Atomic(uint64_t) vk_exec_overlap_count;

static bool vk_exec_diag_enter(uint64_t handle) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LARECOMP_VK_THREAD_DIAG") ? 1 : 0;
    if (!enabled) return false;
    uintptr_t me = (uintptr_t)pthread_self();
    unsigned before = atomic_fetch_add_explicit(&vk_exec_active, 1, memory_order_acq_rel);
    uintptr_t owner = atomic_load_explicit(&vk_exec_owner, memory_order_acquire);
    if (!before) {
        atomic_store_explicit(&vk_exec_owner, me, memory_order_release);
        return true;
    }
    if (owner != me) {
        uint64_t n = atomic_fetch_add_explicit(&vk_exec_overlap_count, 1, memory_order_relaxed) + 1;
        if (n <= 64 || !(n & 255))
            LOG("[vk-thread] !!! overlapping backend_execute #%llu: tid=%llx owner=%llx active=%u handle=%llx frame=%llu !!!\n",
                (unsigned long long)n, (unsigned long long)me, (unsigned long long)owner,
                before + 1, (unsigned long long)handle,
                (unsigned long long)vk_frame_counter());
    }
    return true;
}

static void vk_exec_diag_leave(bool enabled) {
    if (!enabled) return;
    unsigned before = atomic_fetch_sub_explicit(&vk_exec_active, 1, memory_order_acq_rel);
    if (before == 1)
        atomic_store_explicit(&vk_exec_owner, 0, memory_order_release);
}

// One renderer lock for every thread that records or presents (guest render threads in backend_execute, the guest's
// present, and the UI thread's re-present, device.c backend_represent_now). The recording state above (current Sub,
// command buffer, pass, descriptor pools, ...) is global: Scarlet's overlapping backend_execute / present calls
// interleaved commands into one VkCommandBuffer and crashed inside the driver (amdvlk64 null access in
// vk_frame_begin, called from the UI thread while a render thread was recording). Recursive: present can run
// exec code. device.c serializes presents with g_present_lock before taking this renderer lock; no other
// renderer path takes g_present_lock.
static pthread_mutex_t g_vk_big;
static pthread_once_t g_vk_big_once = PTHREAD_ONCE_INIT;
static void vk_big_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_vk_big, &a);
    pthread_mutexattr_destroy(&a);
}
void vk_big_lock(void) {
    pthread_once(&g_vk_big_once, vk_big_init);
    pthread_mutex_lock(&g_vk_big);
}
void vk_big_unlock(void) { pthread_mutex_unlock(&g_vk_big); }

void backend_execute_batch(const uint64_t *handles, int n) {
    if (!handles || n <= 0) return;

    vk_big_lock();
    bool thread_diag = vk_exec_diag_enter(handles[0]);
    PROF_T0();

    if (__builtin_expect(op_counts_enabled < 0, 0)) {
        op_counts_enabled = getenv("LARECOMP_COMMAND_COUNTS") ? 1 : 0;
    }

    // One QueueSubmitCommands is one ordered batch. Snapshot guest-written
    // resources once, execute every already-pinned top-level list in order, and
    // close/queue the Vulkan command buffer once at the end. The old path did
    // all three operations once per list, multiplying recursive mutex traffic,
    // guest-copy scans and command-buffer end/begin work.
    guest_copy_flush();
    const bool interp = ip_enabled();
    for (int i = 0; i < n; i++) {
        if (interp) ip_note_list(handles[i]);
        exec_list(handles[i], 0);
    }

    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;

    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit_list();

    vk_exec_diag_leave(thread_diag);
    vk_big_unlock();
}

void backend_execute(uint64_t handle) {
    backend_execute_batch(&handle, 1);
}

// ================================================================== frame interpolation (prototype)
// LARECOMP_INTERP=1: 60 fps from the game's 30 by drawing an in-between frame (docs/PESQUISA-60FPS.md). The game's
// logic advances one step per frame, so the frame rate can't simply be raised; instead, like RT64 (Zelda64Recomp)
// and Ship of Harkinian, frame N's draws are run a second time with their constant buffers (camera, world, bone
// matrices) blended halfway between N-1 and N, and that image is shown half a frame before N.
//  - recording: the command lists submitted in a frame (and a hash of their records), the exec state at the frame's
//    start, and a copy of every vertex / fragment constant buffer a draw binds (cbuf 3 and up), keyed by stage,
//    binding, the program, vertex buffer 0, the draw's parameters and the occurrence of that key in the frame;
//  - replay (vk_interp_render, at the present): the same lists from the same start state, skipping what writes guest
//    memory (compute, copies to buffers, inline uniform updates, syncs, counters); each constant buffer is the blend of
//    its two copies, word by word, for normal floats close enough (other words, and draws without a match, keep N);
//  - a list whose records changed since it ran (the game reused its memory) cancels that frame's in-between image.
typedef struct { uint64_t key; uint32_t off, size; uint64_t obj; } IpEntry;  // (obj: the game object of the draw)
typedef struct {
    IpEntry *e;
    uint32_t n, cap;
    uint8_t *data;
    uint64_t used, dcap;
    uint32_t *tab;  // open addressing: key -> e index + 1
    uint64_t lists[256], hashes[256];
    int nlists;
    bool overflow, valid;
    uint64_t destroyed;  // g_nvn_destroyed when the frame started
    __typeof__(st) start;
} IpFrame;
extern uint64_t g_nvn_destroyed;  // (objects.c: textures, pools, programs the game finalized)
#define IP_TAB (1u << 16)
static IpFrame ipf[2];
static int ipc;  // ipf[ipc]: the frame being recorded; ipf[ipc ^ 1]: the one before
static float ip_t = 0.5f;
static uint32_t ip_occ[IP_TAB];  // occurrences of each base key in this frame / replay (by slot)
static uint64_t ip_occ_key[IP_TAB];
static uint64_t ip_stats[4];  // blended, kept (no match), words kept, frames replayed
// LARECOMP_INTERP_DIAG=1: what the in-between image is made of (docs/PESQUISA-60FPS.md, "Diagnóstico da interpolação")
typedef struct { int stage, k; uint32_t size; uint64_t draws, nomatch, tagged, words, changed, blended, jumps, nonfloat; } IpDiag;
static IpDiag ip_diag[96];
static int ip_ndiag;
static uint64_t ip_rej[8];  // frames without an in-between image: no previous frame, overflow, destroyed, format, list changed
static IpDiag *ip_diag_for(int stage, int k, uint32_t size) {
    for (int i = 0; i < ip_ndiag; i++)
        if (ip_diag[i].stage == stage && ip_diag[i].k == k && ip_diag[i].size == size) return &ip_diag[i];
    if (ip_ndiag >= 96) return &ip_diag[95];
    ip_diag[ip_ndiag] = (IpDiag){.stage = stage, .k = k, .size = size};
    return &ip_diag[ip_ndiag++];
}
static void ip_diag_dump(void) {
    LOG("[interp-diag] rejected frames: no-prev %llu overflow %llu destroyed %llu format %llu list-changed %llu; replayed %llu\n",
        (unsigned long long)ip_rej[0], (unsigned long long)ip_rej[1], (unsigned long long)ip_rej[2], (unsigned long long)ip_rej[3],
        (unsigned long long)ip_rej[4], (unsigned long long)ip_stats[3]);
    for (int i = 0; i < ip_ndiag; i++) {
        const IpDiag *d = &ip_diag[i];
        LOG("[interp-diag] stage %d cbuf %d size %u: binds %llu (tagged %llu, no match %llu) words %llu changed %llu blended %llu jumps %llu nonfloat %llu\n",
            d->stage, d->k, d->size, (unsigned long long)d->draws, (unsigned long long)d->tagged, (unsigned long long)d->nomatch,
            (unsigned long long)d->words, (unsigned long long)d->changed, (unsigned long long)d->blended, (unsigned long long)d->jumps,
            (unsigned long long)d->nonfloat);
    }
    memset(ip_diag, 0, sizeof ip_diag);
    ip_ndiag = 0;
    memset(ip_rej, 0, sizeof ip_rej);
}

static uint64_t ip_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }

static void ip_occ_reset(void) { memset(ip_occ_key, 0, sizeof ip_occ_key); }

static uint64_t ip_key(int stage, int k) {
    uint64_t base = ip_mix(ip_mix(ip_draw_sig, (uint64_t)stage), (uint64_t)k);
    if (!base) base = 1;
    uint32_t s = (uint32_t)(base ^ (base >> 29)) & (IP_TAB - 1);
    while (ip_occ_key[s] && ip_occ_key[s] != base) s = (s + 1) & (IP_TAB - 1);
    if (!ip_occ_key[s]) { ip_occ_key[s] = base; ip_occ[s] = 0; }
    return ip_mix(base, ip_occ[s]++);
}

static const IpEntry *ip_find(const IpFrame *f, uint64_t key) {
    if (!f->tab) return NULL;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) {
        const IpEntry *e = &f->e[f->tab[s] - 1];
        if (e->key == key) return e;
        s = (s + 1) & (IP_TAB - 1);
    }
    return NULL;
}

static void ip_frame_clear(IpFrame *f) {
    f->n = 0; f->used = 0; f->nlists = 0; f->overflow = false; f->valid = false;
    if (f->tab) memset(f->tab, 0, IP_TAB * sizeof *f->tab);
}

static void ip_record(uint64_t key, const void *data, uint32_t size) {
    IpFrame *f = &ipf[ipc];
    if (!f->tab) f->tab = calloc(IP_TAB, sizeof *f->tab);
    if (f->n >= IP_TAB / 2) { f->overflow = true; return; }
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4096; f->e = realloc(f->e, f->cap * sizeof *f->e); }
    if (f->used + size > f->dcap) { f->dcap = (f->used + size) * 2 + (1 << 20); f->data = realloc(f->data, f->dcap); }
    memcpy(f->data + f->used, data, size);
    f->e[f->n] = (IpEntry){key, (uint32_t)f->used, size, st.obj};
    f->used += size;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) s = (s + 1) & (IP_TAB - 1);
    f->tab[s] = ++f->n;
}

// cbuf_info, constant buffers 3+ of the vertex / fragment stage: recording copies them (returns NULL: bind as usual);
// the replay returns the blended data to bind.
// Objects whose buffers changed too much between N-1 and N to be blended (vk_interp_render's check, before the replay):
// a camera cut, a model swapped (another level of detail, another skeleton) or two draws paired wrongly. Blending them
// mixes unrelated poses - characters stretched across the screen, Pokémon torn apart. They keep frame N's pose.
#define IP_REJ 1024
static uint64_t ip_rej_obj[IP_REJ];
static bool ip_obj_rejected(uint64_t obj) {
    uint32_t s = (uint32_t)((obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
    while (ip_rej_obj[s]) { if (ip_rej_obj[s] == obj) return true; s = (s + 1) & (IP_REJ - 1); }
    return false;
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size) {
    if (stage > 1) return NULL;
    uint64_t key = ip_key(stage, k);
    if (!ip_replaying) { ip_record(key, (const void *)(uintptr_t)addr, size); return NULL; }
    const IpEntry *n = ip_find(&ipf[ipc], key), *o = ip_find(&ipf[ipc ^ 1], key);
    static int diag = -1;
    if (diag < 0) diag = getenv("LARECOMP_INTERP_DIAG") ? 1 : 0;
    IpDiag *dg = diag ? ip_diag_for(stage, k, size) : NULL;
    if (dg) { dg->draws++; if (ip_draw_obj) dg->tagged++; if (!o) dg->nomatch++; }
    if (!n) return NULL;
    const uint8_t *nd = ipf[ipc].data + n->off;
    if (!ip_draw_obj || ip_obj_rejected(st.obj ? st.obj : 1)) { ip_stats[1]++; return nd; }  // interface and other draws no object owns: their order and text change between frames, a blend smears them
    if (!o || o->size != n->size) { ip_stats[1]++; return nd; }
    static uint32_t buf[0x10000 / 4];
    const uint32_t *a = (const uint32_t *)(ipf[ipc ^ 1].data + o->off), *b = (const uint32_t *)nd;
    uint32_t nw = n->size / 4;
    // A jump anywhere in the buffer (a bone, a matrix row) keeps all of it at N: blending the rest would pull part of a
    // skeleton or of a matrix halfway and leave the other part, which stretches the model.
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (!ea || !eb || ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        bool jump = ip_draw_2d ? fabsf(fb - fa) > 0.3f * fmaxf(fabsf(fa), fabsf(fb)) && fabsf(fb - fa) > 0.02f  // (2D: a big move is a recycled row or glyph, not motion)
                               : fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f;
        if (jump) {
            if (dg) dg->jumps += nw;
            ip_stats[1]++;
            return nd;
        }
    }
    // Blended a vec4 at a time, all or nothing: a matrix row with some components blended and others kept (one of them
    // crossing zero, or an integer in a float's place) is not a transformation any more - it shears and shrinks the model.
    // Words that are equal need no test; the others must be finite floats (0 counts) without a jump.
    for (uint32_t g = 0; g < nw; g += 4) {
        uint32_t m = nw - g < 4 ? nw - g : 4;
        int bad = 0, changed = 0, jump = 0;
        for (uint32_t i = g; i < g + m; i++) {
            if (a[i] == b[i]) continue;
            changed++;
            uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
            bool fa_ok = ea != 0xFF && (ea || !(a[i] & 0x7FFFFF)), fb_ok = eb != 0xFF && (eb || !(b[i] & 0x7FFFFF));
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            if (!fa_ok || !fb_ok) bad++;
            else if (fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f) jump++;
        }
        if (dg) { dg->words += m; dg->changed += changed; }
        if (!bad && !jump) {
            for (uint32_t i = g; i < g + m; i++) {
                if (a[i] == b[i]) { buf[i] = b[i]; continue; }
                float fa, fb, v;
                memcpy(&fa, &a[i], 4);
                memcpy(&fb, &b[i], 4);
                v = fa + (fb - fa) * ip_t;
                memcpy(&buf[i], &v, 4);
            }
            if (dg) dg->blended += changed;
        } else {
            for (uint32_t i = g; i < g + m; i++) buf[i] = b[i];
            ip_stats[2] += changed;
            if (dg) { dg->jumps += jump ? changed : 0; dg->nonfloat += bad ? changed : 0; }
        }
    }
    if (n->size > nw * 4) memcpy((uint8_t *)buf + nw * 4, nd + nw * 4, n->size - nw * 4);
    ip_stats[0]++;
    return buf;
}

// hash of a command list's records (JUMPs and CALLs followed), to tell whether its memory still holds what ran
static uint64_t ip_list_hash(uint64_t handle, int depth) {
    uint64_t h = 0xcbf29ce484222325ull, p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size > (1u << 20)) return 0;
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < r->size / 8; i++) h = ip_mix(h, w[i]);
        if (r->op == CMD_END) return h;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hs = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) h = ip_mix(h, ip_list_hash(hs[i], depth + 1));
        }
        p += r->size;
    }
    return 0;
}

static void ip_note_list(uint64_t handle) {
    IpFrame *f = &ipf[ipc];
    if (f->nlists >= 256) { f->overflow = true; return; }
    f->hashes[f->nlists] = ip_list_hash(handle, 0);
    f->lists[f->nlists++] = handle;
}

// The hold image: frame N's presented texture, kept while the replay overwrites it with the in-between image.
static TexBackend ip_hold;
static TexBackend *ip_hold_copy(TexBackend *src) {
    TexBackend *d = &ip_hold;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {src->width, src->height, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    d->sx = src->sx; d->sy = src->sy; d->scalable = src->scalable;
    VkCommandBuffer cb = vk_cmd();
    vk_transfer_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_graphics_barrier(cb);
    return d;
}

// At the present of frame N (presented texture b): when frame N-1 was recorded too and N's lists are intact, renders
// the in-between image into the game's textures (b then holds it) and returns frame N's own image in *hold; false:
// nothing to show before N. Either way, recording moves on to the next frame.
bool vk_interp_render(TexBackend *b, TexBackend **hold) {
    if (!ip_enabled()) {
        // Off (the world, a battle): what was recorded the last time it was on is stale - a menu opened later blended
        // with that old scene (a flash when the menu opened while walking)
        ipf[0].valid = ipf[1].valid = false;
        return false;
    }
    IpFrame *cur = &ipf[ipc], *prev = &ipf[ipc ^ 1];
    bool ok = prev->valid && cur->valid && cur->nlists > 0 && !cur->overflow && !prev->overflow && b && b->image &&
              b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->samples <= 1 && cur->destroyed == g_nvn_destroyed;
    bool structural = ok;
    for (int i = 0; ok && i < cur->nlists; i++)
        if (!cur->hashes[i] || ip_list_hash(cur->lists[i], 0) != cur->hashes[i]) ok = false;
    if (!ok) {
        if (!prev->valid || !cur->valid || cur->nlists == 0) ip_rej[0]++;
        else if (cur->overflow || prev->overflow) ip_rej[1]++;
        else if (cur->destroyed != g_nvn_destroyed) ip_rej[2]++;
        else if (!structural) ip_rej[3]++;
        else ip_rej[4]++;
    }
    if (ok) {
        // Per object, before the replay: the share of its changed float words that changed by a large step. Above
        // LARECOMP_INTERP_OBJ_LIMIT (0.2) the object is not blended (ip_obj_rejected); when the rejected objects hold
        // most of the frame's changes (a camera cut), there is no in-between image at all.
        typedef struct { uint64_t obj, changed, big; } ObjAcc;
        static ObjAcc acc[IP_REJ];
        memset(acc, 0, sizeof acc);
        memset(ip_rej_obj, 0, sizeof ip_rej_obj);
        uint64_t changed = 0, big = 0, rej_changed = 0;
        int nrej = 0, nobj = 0;
        for (uint32_t i = 0; i < cur->n; i++) {
            const IpEntry *n = &cur->e[i], *o = ip_find(prev, n->key);
            if (!o || o->size != n->size) continue;
            uint64_t gobj = n->obj ? n->obj : 1;  // (draws without an object: one group)
            const float *fa = (const float *)(prev->data + o->off), *fb = (const float *)(cur->data + n->off);
            uint64_t c = 0, bg = 0;
            for (uint32_t w = 0; w < n->size / 4; w++) {
                float a = fa[w], v = fb[w];
                if (a == v || !isfinite(a) || !isfinite(v)) continue;
                float m = fmaxf(fabsf(a), fabsf(v));
                if (m < 1e-3f || m > 1e7f) continue;
                c++;
                if (fabsf(v - a) > 0.3f * m && fabsf(v - a) > 0.1f) bg++;
            }
            if (!c) continue;
            uint32_t sl = (uint32_t)((gobj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
            while (acc[sl].obj && acc[sl].obj != gobj) sl = (sl + 1) & (IP_REJ - 1);
            if (!acc[sl].obj) { acc[sl].obj = gobj; nobj++; }
            acc[sl].changed += c, acc[sl].big += bg;
            changed += c, big += bg;
        }
        extern bool fps60_menu(void);
        bool menu = fps60_menu();  // (menus: the cut checks are for battles; a menu screen changes by large steps all the time)
        static double obj_limit = -1;
        if (obj_limit < 0) obj_limit = getenv("LARECOMP_INTERP_OBJ_LIMIT") ? atof(getenv("LARECOMP_INTERP_OBJ_LIMIT")) : 0.2;
        for (int i = 0; i < IP_REJ; i++)
            if (!menu && acc[i].obj && acc[i].changed >= 8 && (double)acc[i].big > obj_limit * (double)acc[i].changed) {
                uint32_t sl = (uint32_t)((acc[i].obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
                while (ip_rej_obj[sl]) sl = (sl + 1) & (IP_REJ - 1);
                ip_rej_obj[sl] = acc[i].obj;
                nrej++;
                rej_changed += acc[i].changed;
            }
        if (getenv("LARECOMP_INTERP_DIAG"))
            LOG("[interp-cut] present %llu: objects %d rejected %d (%llu of %llu changed words; big %llu)\n",
                (unsigned long long)renderer_present_count(), nobj, nrej, (unsigned long long)rej_changed, (unsigned long long)changed,
                (unsigned long long)big);
        if (!menu && changed && rej_changed * 10 > changed * 6) { ok = false; ip_rej[5]++; }
    }
    if (ok) {
        vk_end_pass();
        *hold = ip_hold_copy(b);
        __typeof__(st) end = st;
        st = cur->start;
        st.rt_dirty = true;
        ip_replaying = true;
        ip_occ_reset();
        for (int i = 0; i < cur->nlists; i++) exec_list(cur->lists[i], 0);
        vk_end_pass();
        ip_replaying = false;
        st = end;
        st.rt_dirty = true;
        ip_stats[3]++;
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] frame %llu: %llu blended, %llu without a match, %llu words kept (replays %llu)\n",
                (unsigned long long)vk_frame_counter(), (unsigned long long)ip_stats[0], (unsigned long long)ip_stats[1],
                (unsigned long long)ip_stats[2], (unsigned long long)ip_stats[3]);
        if (ip_stats[3] % 300 == 0 && getenv("LARECOMP_INTERP_DIAG")) ip_diag_dump();
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] draws with an object %llu, without %llu\n", (unsigned long long)ip_stats_tagged, (unsigned long long)ip_stats_untagged);
    }
    // the next frame records into the other slot, from the state this one ended with
    ipc ^= 1;
    ip_frame_clear(&ipf[ipc]);
    ipf[ipc].start = st;
    ipf[ipc].destroyed = g_nvn_destroyed;
    ipf[ipc].valid = true;
    ip_occ_reset();
    return ok;
}
