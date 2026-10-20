// Ordered asynchronous NVN/Vulkan execution for CPU-bound Scarlet.
// The game records command lists on its own workers. Queue submission pins those
// host streams, then this thread translates them to Vulkan while the recompiled
// game continues doing CPU work. Presents/fences are jobs in the same queue, so
// host reordering cannot move GPU work across a visible frame boundary.
//
// Default: on. Set LARECOMP_RENDER_THREAD=0 to restore the synchronous path.
// The interpolation research mode replays command lists on the caller and stays
// synchronous until that path is made render-thread aware.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../rt.h"
#include "../nvn.h"

uint64_t renderer_fence_seq(void);
void renderer_submit(void);
void renderer_sync(void);
void renderer_flush(void);
void renderer_wait_seq(uint64_t seq);
void vk_big_lock(void);
void vk_big_unlock(void);
uint64_t backend_dirty_seq(void);
void backend_set_dirty_limit(uint64_t seq);

enum {
    RJ_EXEC,
    RJ_PRESENT,
    RJ_FENCE,
    RJ_GPU_SYNC,
    RJ_GPU_FLUSH,
    RJ_GPU_WAIT,
};

typedef struct {
    int kind;
    uint64_t *handles;
    int nhandles;
    uint64_t dirty;
    uint64_t seq;
    NvnTexture *tex;
    NvnSync *sync;
} RenderJob;

#define RQ_CAP 256
static RenderJob rq[RQ_CAP];
static uint64_t qhead, qtail;
static uint64_t enqueued, finished;
static uint64_t max_depth, full_waits;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qwork = PTHREAD_COND_INITIALIZER;
static pthread_cond_t qdone = PTHREAD_COND_INITIALIZER;
static pthread_t rthread;
static bool started, busy, dropping;

bool renderthread_active(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_RENDER_THREAD");
        on = e ? atoi(e) != 0 : 1;
        if (getenv("LARECOMP_INTERP")) on = 0;
        LOG("[render-thread] %s%s\n", on ? "enabled" : "disabled",
            !on && getenv("LARECOMP_INTERP") ? " (interpolation research mode)" : "");
    }
    return on != 0;
}

static void release_exec(RenderJob *j) {
    if (!j->handles) return;
    for (int i = j->nhandles - 1; i >= 0; i--) nvn_cmdstream_unpin(j->handles[i]);
    free(j->handles);
    j->handles = NULL;
}

static void run_job(RenderJob *j) {
    switch (j->kind) {
    case RJ_EXEC:
        backend_set_dirty_limit(j->dirty);
        for (int i = 0; i < j->nhandles; i++) backend_execute(j->handles[i]);
        backend_set_dirty_limit(0);
        release_exec(j);
        break;
    case RJ_PRESENT:
        backend_present(j->tex);
        break;
    case RJ_FENCE:
        // Make the fence's timeline value physical before publishing it to the
        // guest. renderer_submit only touches renderer state on this thread.
        vk_big_lock();
        renderer_submit();
        j->sync->seq = renderer_fence_seq();
        vk_big_unlock();
        break;
    case RJ_GPU_SYNC:
        vk_big_lock();
        renderer_sync();
        vk_big_unlock();
        break;
    case RJ_GPU_FLUSH:
        vk_big_lock();
        renderer_flush();
        vk_big_unlock();
        break;
    case RJ_GPU_WAIT:
        vk_big_lock();
        renderer_wait_seq(j->seq);
        vk_big_unlock();
        break;
    }
}

static void drop_job(RenderJob *j) {
    if (j->kind == RJ_EXEC) release_exec(j);
}

static void *render_main(void *arg) {
    (void)arg;
    crash_handler_install();
    pthread_mutex_lock(&qlock);
    for (;;) {
        while (qhead == qtail) pthread_cond_wait(&qwork, &qlock);
        RenderJob j = rq[qhead % RQ_CAP];
        busy = true;
        bool drop = dropping;
        pthread_mutex_unlock(&qlock);

        if (drop) drop_job(&j);
        else run_job(&j);

        pthread_mutex_lock(&qlock);
        qhead++;
        finished++;
        busy = false;
        pthread_cond_broadcast(&qdone);
    }
    return NULL;
}

static bool ensure_started_locked(void) {
    if (started) return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16ull << 20);
    int err = pthread_create(&rthread, &attr, render_main, NULL);
    pthread_attr_destroy(&attr);
    if (err) {
        LOG("[render-thread] pthread_create failed: %d; using synchronous renderer\n", err);
        return false;
    }
    started = true;
    return true;
}

static uint64_t push(RenderJob j) {
    pthread_mutex_lock(&qlock);
    if (!ensure_started_locked()) {
        pthread_mutex_unlock(&qlock);
        return 0;
    }
    while (qtail - qhead >= RQ_CAP) {
        full_waits++;
        pthread_cond_wait(&qdone, &qlock);
    }
    rq[qtail % RQ_CAP] = j;
    qtail++;
    uint64_t depth = qtail - qhead;
    if (depth > max_depth) max_depth = depth;
    uint64_t ticket = ++enqueued;
    pthread_cond_signal(&qwork);
    pthread_mutex_unlock(&qlock);
    return ticket;
}

bool renderthread_submit_batch(const uint64_t *resolved, int n) {
    if (!renderthread_active() || n <= 0) return false;
    uint64_t *copy = malloc((size_t)n * sizeof *copy);
    if (!copy) return false;
    for (int i = 0; i < n; i++) copy[i] = resolved[i];
    RenderJob j = {
        .kind = RJ_EXEC,
        .handles = copy,
        .nhandles = n,
        .dirty = backend_dirty_seq(),
    };
    uint64_t ticket = push(j);
    if (!ticket) {
        // Ownership of the pins never moved if the job was not queued.
        free(copy);
        return false;
    }
    return true;
}

uint64_t renderthread_queue_fence(NvnSync *sync) {
    if (!renderthread_active() || !sync) return 0;
    RenderJob j = {.kind = RJ_FENCE, .sync = sync};
    return push(j);
}

void renderthread_wait_ticket(uint64_t ticket) {
    if (!ticket || !renderthread_active()) return;
    pthread_mutex_lock(&qlock);
    while (finished < ticket) pthread_cond_wait(&qdone, &qlock);
    pthread_mutex_unlock(&qlock);
}

void renderthread_drain(void) {
    if (!renderthread_active() || !started) return;
    pthread_mutex_lock(&qlock);
    uint64_t target = enqueued;
    while (finished < target || busy) pthread_cond_wait(&qdone, &qlock);
    pthread_mutex_unlock(&qlock);
}

static void queue_and_wait(RenderJob j) {
    if (!renderthread_active()) {
        run_job(&j);
        return;
    }
    uint64_t ticket = push(j);
    if (ticket) renderthread_wait_ticket(ticket);
    else run_job(&j);
}

void renderthread_present_sync(NvnTexture *tex) {
    if (!tex) return;
    queue_and_wait((RenderJob){.kind = RJ_PRESENT, .tex = tex});
}

void renderthread_gpu_sync(void) {
    queue_and_wait((RenderJob){.kind = RJ_GPU_SYNC});
}

void renderthread_gpu_flush(void) {
    queue_and_wait((RenderJob){.kind = RJ_GPU_FLUSH});
}

void renderthread_gpu_wait_seq(uint64_t seq) {
    if (!seq) return;
    queue_and_wait((RenderJob){.kind = RJ_GPU_WAIT, .seq = seq});
}

void renderthread_reset(void) {
    if (!started) return;
    pthread_mutex_lock(&qlock);
    dropping = true;
    while (qhead != qtail || busy) pthread_cond_wait(&qdone, &qlock);
    dropping = false;
    LOG("[render-thread] reset: jobs=%llu max-depth=%llu queue-full-waits=%llu\n",
        (unsigned long long)finished, (unsigned long long)max_depth,
        (unsigned long long)full_waits);
    pthread_mutex_unlock(&qlock);
}
