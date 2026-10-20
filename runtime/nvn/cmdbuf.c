// NVN command buffers: commands are recorded into the guest-provided command memory as CmdRec
// records, so the game's own memory management and out-of-memory callbacks work as designed.
// A command handle is the guest address of a list's first record.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

// A no-position vertex shader can be a stream-output preprocessing pass. These
// APIs remain unimplemented; report their actual use once per entry point so
// a missing model is not incorrectly diagnosed as a texture/driver problem.
#define TF_UNIMPLEMENTED(name) NVN(name) { \
    nvn_diag_tf_state(cb_of(A(0)), #name); \
    nvn_diag_tf_call(#name, c); \
    static atomic_bool reported; \
    if (!atomic_exchange_explicit(&reported, true, memory_order_relaxed)) \
        LOG("[nvn-unimplemented] %s cb=%llx args=%llx,%llx,%llx lr=%llx (transform feedback is not executed)\n", \
            #name, (unsigned long long)A(0), (unsigned long long)A(1), (unsigned long long)A(2), \
            (unsigned long long)A(3), (unsigned long long)c->x[30]); \
}

// Expanded host records must not consume the game's real NVN command-memory
// budget. Every recording gets a tiny opaque handle in guest memory and the
// actual CmdRec stream lives in host segments chained from that handle.
#define HOSTSEG_BASE_SIZE (32u << 10)
#define HOSTSEG_MAGIC 0x534547484f53544full /* "HOSTSEGO" */
/*
 * Expanded command streams used to keep all segments in one flat array and
 * linearly scan that array on every BeginRecording, AddCommandMemory, submit
 * pin and unpin. Scarlet records thousands of lists per frame; once the pool
 * reached ~7.7k segments those O(N) scans alone consumed tens of milliseconds
 * of CPU time per frame.
 *
 * Each allocation now has a tiny prefix containing its stable pool index,
 * recordings keep an intrusive chain of their segments, and ordinary 32 KiB
 * segments use an O(1) free list. Guest command-memory ranges contain one
 * fixed-size CMD_JUMP handle per recorded list, so recycling a range can walk
 * those handles and release the exact chains instead of scanning the pool.
 */
typedef struct {
    uint64_t magic;
    uint32_t index;
    uint32_t pad;
} HostSegPrefix;

// Most Scarlet command lists are small. A fixed 128 KB allocation per live list grew to >800 MB in character
// creation. Use 32 KB base segments and allocate a larger segment only when one expanded record actually needs it.
// LARECOMP_HOSTSEG_MB is a safety ceiling for backing allocations, not a target pool size.
static uint64_t hostseg_cap(void) {
    static uint64_t cap;
    if (!cap) {
        const char *e = getenv("LARECOMP_HOSTSEG_MB");
        long mb = e ? atol(e) : 1024;
        cap = (uint64_t)(mb >= 64 ? mb : 1024) << 20;
    }
    return cap;
}
typedef struct {
    uint64_t born;  // hostseg_take count when it was taken (reclaim leaves the newest alone)
    uint8_t *mem;
    uint32_t size;
    bool busy;
    bool pending_release;
    bool on_free32;
    bool chain_head;
    uint32_t refs;
    uint64_t origin_lo, origin_hi;  // guest handle slot that owns this segment chain
    int32_t chain_next;
    int32_t free_next;
} HostSeg;
static HostSeg *g_hostseg;
static int g_nhostseg, g_caphostseg;
static int32_t g_free32 = -1;
// Segment backing is never individually freed, so an append-only exact-pointer
// hash gives safe O(1) lookup without dereferencing an untrusted guest jump.
#define HOSTSEG_HASH_SIZE 65536u
static uint64_t g_hostseg_hash_key[HOSTSEG_HASH_SIZE];
static int32_t g_hostseg_hash_val[HOSTSEG_HASH_SIZE]; // index + 1, zero = empty
static pthread_mutex_t g_hostseg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_take_count;
static uint64_t g_hostseg_bytes;

static uint32_t hostseg_hash_slot(uint64_t mem) {
    uint64_t x = mem >> 4;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x & (HOSTSEG_HASH_SIZE - 1u);
}
static void hostseg_hash_insert_locked(uint64_t mem, int idx) {
    uint32_t h = hostseg_hash_slot(mem);
    for (uint32_t n = 0; n < HOSTSEG_HASH_SIZE; n++, h = (h + 1u) & (HOSTSEG_HASH_SIZE - 1u)) {
        if (!g_hostseg_hash_val[h] || g_hostseg_hash_key[h] == mem) {
            g_hostseg_hash_key[h] = mem;
            g_hostseg_hash_val[h] = idx + 1;
            return;
        }
    }
    FATAL("nvn: host segment pointer hash exhausted\n");
}
static int hostseg_index_from_mem(uint64_t mem) {
    if (!mem) return -1;
    uint32_t h = hostseg_hash_slot(mem);
    for (uint32_t n = 0; n < HOSTSEG_HASH_SIZE; n++, h = (h + 1u) & (HOSTSEG_HASH_SIZE - 1u)) {
        int32_t v = g_hostseg_hash_val[h];
        if (!v) return -1;
        if (g_hostseg_hash_key[h] == mem) {
            int idx = v - 1;
            return idx >= 0 && idx < g_nhostseg && g_hostseg[idx].mem == (uint8_t *)(uintptr_t)mem ? idx : -1;
        }
    }
    return -1;
}

static void hostseg_free32_push_locked(int idx) {
    HostSeg *s = &g_hostseg[idx];
    if (s->size != HOSTSEG_BASE_SIZE || s->on_free32) return;
    s->free_next = g_free32;
    s->on_free32 = true;
    g_free32 = idx;
}

static int hostseg_free32_pop_locked(void) {
    while (g_free32 >= 0) {
        int idx = g_free32;
        HostSeg *s = &g_hostseg[idx];
        g_free32 = s->free_next;
        s->free_next = -1;
        s->on_free32 = false;
        if (!s->busy && !s->refs && s->size == HOSTSEG_BASE_SIZE) return idx;
    }
    return -1;
}

static void hostseg_make_free_locked(int idx) {
    HostSeg *s = &g_hostseg[idx];
    s->busy = false;
    s->pending_release = false;
    s->origin_lo = s->origin_hi = 0;
    s->chain_head = false;
    s->chain_next = -1;
    if (!s->refs) hostseg_free32_push_locked(idx);
}

// Release one exact recording chain. Returns the number of segments touched.
static int hostseg_release_chain_locked(int head) {
    int n = 0;
    for (int idx = head, guard = 0; idx >= 0 && idx < g_nhostseg && guard++ < 65536;) {
        HostSeg *s = &g_hostseg[idx];
        int next = s->chain_next;
        if (!s->busy) break;
        n++;
        if (s->refs) {
            s->pending_release = true;
            static uint64_t deferred;
            uint64_t d = ++deferred;
            if (d <= 16 || !(d & 255))
                LOG("[nvn] deferred host command recycle #%llu: refs=%u origin=%llx..%llx\n",
                    (unsigned long long)d, s->refs,
                    (unsigned long long)s->origin_lo, (unsigned long long)s->origin_hi);
        } else {
            hostseg_make_free_locked(idx);
        }
        idx = next;
    }
    return n;
}

// Resolve a guest command-handle slot to the exact first host segment.
static int hostseg_head_from_slot_locked(uint64_t slot) {
    if (slot < GUEST_LO || slot + sizeof(CmdRec) > GUEST_HI) return -1;
    const CmdRec *r = (const CmdRec *)(uintptr_t)slot;
    if (r->op != CMD_JUMP || r->size != sizeof(CmdRec) || !r->x[0]) return -1;
    int idx = hostseg_index_from_mem(r->x[0]);
    if (idx < 0) return -1;
    HostSeg *s = &g_hostseg[idx];
    if (!s->busy || !s->chain_head || s->origin_lo != slot || slot >= s->origin_hi) return -1;
    return idx;
}

// Pool pressure only (caller holds g_hostseg_lock): free chains whose guest
// handle is gone. Only chain heads are inspected, so this is O(number of lists)
// rather than the old O(segments^2) nested scan.
static bool hostseg_slot_intact(uint64_t lo) {
    return hostseg_head_from_slot_locked(lo) >= 0;
}
static int hostseg_reclaim_dead_locked(void) {
    int freed = 0;
    for (int i = 0; i < g_nhostseg; i++) {
        HostSeg *s = &g_hostseg[i];
        if (!s->busy || !s->chain_head || s->refs || !s->origin_lo || g_take_count - s->born < 4096) continue;
        if (hostseg_slot_intact(s->origin_lo)) continue;
        freed += hostseg_release_chain_locked(i);
    }
    return freed;
}

static void hostseg_release_range(uint64_t lo, uint64_t hi) {
    if (!lo || hi <= lo) return;
    pthread_mutex_lock(&g_hostseg_lock);

    int released = 0;
    // Normal path: every guest handle is one fixed-size CmdRec slot. Walk the
    // supplied NVN command-memory range and release only the chains actually
    // referenced by its jump handles.
    for (uint64_t p = lo; p + sizeof(CmdRec) <= hi; p += sizeof(CmdRec)) {
        int head = hostseg_head_from_slot_locked(p);
        if (head >= 0) released += hostseg_release_chain_locked(head);
    }

    // Compatibility fallback for unusual SDK ranges that do not begin on our
    // handle-slot boundary. This path should be rare; keeping it preserves the
    // old semantics without putting the common path back on an O(N) scan.
    if (!released) {
        for (int i = 0; i < g_nhostseg; i++) {
            HostSeg *s = &g_hostseg[i];
            if (!s->busy || !s->chain_head || s->origin_hi <= lo || s->origin_lo >= hi) continue;
            released += hostseg_release_chain_locked(i);
        }
    }
    pthread_mutex_unlock(&g_hostseg_lock);
}

static uint32_t hostseg_alloc_size(uint32_t need) {
    uint64_t n = need > HOSTSEG_BASE_SIZE ? need : HOSTSEG_BASE_SIZE;
    n = (n + 4095u) & ~4095ull;
    if (n > UINT32_MAX) FATAL("nvn: expanded command segment request too large (%llu bytes)\n",
                              (unsigned long long)n);
    return (uint32_t)n;
}

static int hostseg_best_free_locked(uint32_t need) {
    if (need == HOSTSEG_BASE_SIZE) {
        int idx = hostseg_free32_pop_locked();
        if (idx >= 0) return idx;
    }
    int best = -1;
    for (int i = 0; i < g_nhostseg; i++) {
        HostSeg *s = &g_hostseg[i];
        if (s->busy || s->refs || s->on_free32 || s->size < need) continue;
        if (best < 0 || s->size < g_hostseg[best].size) best = i;
    }
    return best;
}

static void hostseg_chain_append_locked(NvnCmdBuf *cb, int idx) {
    HostSeg *s = &g_hostseg[idx];
    s->chain_next = -1;
    s->chain_head = cb->host_chain_head < 0;
    if (cb->host_chain_tail >= 0 && cb->host_chain_tail < g_nhostseg)
        g_hostseg[cb->host_chain_tail].chain_next = idx;
    else
        cb->host_chain_head = idx;
    cb->host_chain_tail = idx;
}

static uint64_t hostseg_take(NvnCmdBuf *cb, uint32_t need, uint32_t *actual_size, uint64_t replace_slot) {
    static uint64_t total;
    uint8_t *mem = NULL;
    int pick = -1;
    uint32_t want = hostseg_alloc_size(need);
    pthread_mutex_lock(&g_hostseg_lock);

    // BeginRecording overwrites exactly one tiny guest CMD_JUMP slot. Recycle
    // the previous stream for that slot while we already hold the allocator
    // lock, then immediately reuse a free segment below. This replaces two
    // lock acquisitions + a separate range walk on every recorded list.
    if (replace_slot) {
        int old_head = hostseg_head_from_slot_locked(replace_slot);
        if (old_head >= 0) hostseg_release_chain_locked(old_head);
    }

    pick = hostseg_best_free_locked(want);
    // Reclaim stale chains before growing a large backing pool. This is deliberately
    // opportunistic: live/pinned command streams are never reclaimed.
    if (pick < 0 && g_hostseg_bytes >= (256ull << 20)) {
        int freed = hostseg_reclaim_dead_locked();
        if (freed) pick = hostseg_best_free_locked(want);
        static uint64_t proactive;
        if (freed && (++proactive <= 8 || !(proactive & 63)))
            LOG("[nvn] proactive host command reclaim: %d segments, backing %.1f MB\n",
                freed, (double)g_hostseg_bytes / (1024.0 * 1024.0));
    }
    if (pick >= 0) {
        HostSeg *s = &g_hostseg[pick];
        s->busy = true;
        s->pending_release = false;
        s->origin_lo = cb->host_origin_lo;
        s->origin_hi = cb->host_origin_hi;
        s->born = g_take_count + 1;
        s->free_next = -1;
        mem = s->mem;
        *actual_size = s->size;
    }

    if (!mem && g_hostseg_bytes + want > hostseg_cap()) {
        int freed = hostseg_reclaim_dead_locked();
        pick = hostseg_best_free_locked(want);
        static uint64_t reclaims;
        if (++reclaims <= 8 || !(reclaims & 63))
            LOG("[nvn] host segment backing at cap (%.1f/%llu MB): reclaimed %d dead segments (#%llu)\n",
                (double)g_hostseg_bytes / (1024.0 * 1024.0),
                (unsigned long long)(hostseg_cap() >> 20), freed, (unsigned long long)reclaims);
        if (pick >= 0) {
            HostSeg *s = &g_hostseg[pick];
            s->busy = true;
            s->pending_release = false;
            s->origin_lo = cb->host_origin_lo;
            s->origin_hi = cb->host_origin_hi;
            s->born = g_take_count + 1;
            s->free_next = -1;
            mem = s->mem;
            *actual_size = s->size;
        }
    }

    if (!mem && g_hostseg_bytes + want <= hostseg_cap()) {
        if (g_nhostseg == g_caphostseg) {
            int nc = g_caphostseg ? g_caphostseg * 2 : 64;
            HostSeg *p = realloc(g_hostseg, (size_t)nc * sizeof *p);
            if (p) { g_hostseg = p; g_caphostseg = nc; }
        }
        if (g_nhostseg < g_caphostseg) {
            int idx = g_nhostseg;
            HostSegPrefix *raw = malloc(sizeof *raw + want);
            if (raw) {
                raw->magic = HOSTSEG_MAGIC;
                raw->index = (uint32_t)idx;
                raw->pad = 0;
                mem = (uint8_t *)(raw + 1);
                g_hostseg[g_nhostseg++] = (HostSeg){
                    .born = g_take_count + 1, .mem = mem, .size = want,
                    .busy = true, .pending_release = false, .on_free32 = false,
                    .chain_head = false, .refs = 0,
                    .origin_lo = cb->host_origin_lo, .origin_hi = cb->host_origin_hi,
                    .chain_next = -1, .free_next = -1
                };
                hostseg_hash_insert_locked((uint64_t)(uintptr_t)mem, idx);
                pick = idx;
                g_hostseg_bytes += want;
                *actual_size = want;
            }
        }
    }

    if (pick >= 0) hostseg_chain_append_locked(cb, pick);
    uint64_t n = ++total;
    g_take_count++;
    int pool = g_nhostseg;
    uint64_t bytes = g_hostseg_bytes;
    pthread_mutex_unlock(&g_hostseg_lock);
    if (!mem) FATAL("nvn: host command segment pool exhausted (%llu MB)\n",
                    (unsigned long long)(hostseg_cap() >> 20));
    // This counter advances hundreds of thousands of times. Logging every
    // 1024 takes creates measurable console/file I/O in CPU-bound scenes.
    if (n == 1 || !(n & 16383))
        LOG("[nvn] expanded command records -> host segments (take %llu, pool %d, backing %.1f MB, segment %u KB)\n",
            (unsigned long long)n, pool, (double)bytes / (1024.0 * 1024.0), *actual_size >> 10);
    return (uint64_t)(uintptr_t)mem;
}

uint64_t nvn_cmdstream_pin(uint64_t handle) {
    if (!handle) return 0;
    uint64_t resolved = handle;
    pthread_mutex_lock(&g_hostseg_lock);

    int head = -1;
    if (handle >= GUEST_LO && handle + sizeof(CmdRec) <= GUEST_HI) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)handle;
        if (r->op == CMD_JUMP && r->size == sizeof(CmdRec) && r->x[0]) {
            resolved = r->x[0];
            head = hostseg_index_from_mem(resolved);
            if (head >= 0) {
                HostSeg *s = &g_hostseg[head];
                if (!s->busy || !s->chain_head || s->origin_lo != handle) head = -1;
            }
        }
    } else {
        head = hostseg_index_from_mem(handle);
        if (head >= 0) {
            resolved = handle;
            // Host-only fallback recordings also start at a chain head.
            if (!g_hostseg[head].busy) head = -1;
        }
    }

    if (head >= 0) {
        for (int idx = head, guard = 0; idx >= 0 && idx < g_nhostseg && guard++ < 65536; idx = g_hostseg[idx].chain_next)
            g_hostseg[idx].refs++;
    } else {
        // Safety fallback for an old/unusual handle. This retains the old
        // behaviour but should disappear from ordinary Scarlet submissions.
        for (int i = 0; i < g_nhostseg; i++) {
            HostSeg *s = &g_hostseg[i];
            if (!s->busy) continue;
            if (s->origin_lo && handle >= s->origin_lo && handle < s->origin_hi) {
                s->refs++;
                if (s->chain_head) resolved = (uint64_t)(uintptr_t)s->mem;
            } else {
                uint64_t lo = (uint64_t)(uintptr_t)s->mem, hi = lo + s->size;
                if (!s->origin_lo && handle >= lo && handle < hi) s->refs++;
            }
        }
    }

    pthread_mutex_unlock(&g_hostseg_lock);
    return resolved;
}

void nvn_cmdstream_unpin(uint64_t resolved) {
    if (!resolved) return;
    pthread_mutex_lock(&g_hostseg_lock);

    int head = hostseg_index_from_mem(resolved);
    if (head >= 0) {
        for (int idx = head, guard = 0; idx >= 0 && idx < g_nhostseg && guard++ < 65536;) {
            HostSeg *s = &g_hostseg[idx];
            int next = s->chain_next;
            if (s->refs) s->refs--;
            if (!s->refs && s->pending_release) hostseg_make_free_locked(idx);
            idx = next;
        }
    } else {
        // Fallback for a non-prefixed legacy/fallback pointer.
        for (int i = 0; i < g_nhostseg; i++) {
            HostSeg *s = &g_hostseg[i];
            uint64_t lo = (uint64_t)(uintptr_t)s->mem, hi = lo + s->size;
            if (resolved < lo || resolved >= hi) continue;
            if (s->refs) s->refs--;
            if (!s->refs && s->pending_release) hostseg_make_free_locked(i);
            break;
        }
    }

    pthread_mutex_unlock(&g_hostseg_lock);
}

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t size = A(3), mem = 0;
    if (A(1) && size) mem = HOST(NvnPool, A(1))->storage + A(2);

    if (mem && size) {
        // Do not scan/recycle the whole guest command-memory grant here.
        // Scarlet calls AddCommandMemory over a million times in a few minutes
        // and only one 80-byte handle slot is actually overwritten by each
        // BeginRecording. BeginRecording now recycles that exact slot under the
        // same allocator lock used to take its replacement segment. Intact old
        // handles remain valid until their slot is really overwritten.
        cb->guest_mem = mem;
        cb->guest_size = size;
        cb->used = 0;
        if (!cb->recording) {
            cb->mem = mem;
            cb->size = size;
            cb->chunk_used = 0;
            cb->host_segment = false;
        }
    } else {
        // Some SDK callbacks can report an empty grant. Internal recording does
        // not depend on it; BeginRecording will use a host-only opaque handle
        // until the game supplies command memory again.
        static int once;
        if (!once++) LOG("[nvn] empty command-memory grant; using host recording fallback\n");
    }
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->guest_size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->guest_size > cb->used ? cb->guest_size - cb->used : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Reserve one expanded host record. Real NVN commands are much smaller than
// CmdRec, so exhausting a host segment must never call the game's command-memory
// callback: that callback budgets for Maxwell command bytes, not our structures.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    (void)c;
    uint64_t total = sizeof(CmdRec) + (uint64_t)extra + 7;
    if (total > UINT32_MAX) FATAL("nvn: expanded command record too large (%llu bytes)\n",
                                  (unsigned long long)total);
    uint32_t size = (uint32_t)total & ~7u;

    if (!cb->mem || !cb->host_segment || cb->chunk_used + size + JUMP_RESERVE > cb->size) {
        uint32_t seg_size = 0;
        uint64_t next = hostseg_take(cb, size + JUMP_RESERVE, &seg_size, 0);
        if (cb->mem && cb->host_segment) {
            CmdRec *j = raw_rec(cb->mem + cb->chunk_used, CMD_JUMP, sizeof(CmdRec));
            j->x[0] = next;
        }
        cb->mem = next;
        cb->size = seg_size;
        cb->chunk_used = 0;
        cb->host_segment = true;
    }

    CmdRec *r = raw_rec(cb->mem + cb->chunk_used, op, size);
    cb->chunk_used += size;
    return r;
}

// ---- the game object being drawn (frame interpolation, docs/PESQUISA-60FPS.md)
// The game draws each object from a few functions, called once per object (and pass) with the object in x0: hooked
// while interpolation is on, they name the object around the command lists it calls and the draws it records, so the
// in-between frame pairs each draw with the same object's draw in the previous frame (the game reorders objects every
// frame: pairing by order made ghosts).
void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
static __thread uint64_t g_obj_tag;
static const uint64_t k_obj_fns[] = {
    0x70006237b0,  // one per object (Viridian: 22 a frame), called by the draw loop f_70006453a0
    0x7000641190,  // objects drawn in two passes (shadow / colour), via f_7000642990
};
static int obj_hook(Cpu *c, uint64_t addr) {
    uint64_t saved = g_obj_tag;
    g_obj_tag = c->x[0];
    rt_call_original(c, addr);
    g_obj_tag = saved;
    return 1;  // (the original ran)
}
// Before a draw or a command list call: a CMD_OBJECT when the object changed since the list's last one.
static void obj_mark(Cpu *c, NvnCmdBuf *cb) {
    if (cb->obj_tag == g_obj_tag) return;
    CmdRec *r = rec(c, cb, CMD_OBJECT, 0);
    r->x[0] = g_obj_tag;
    cb->obj_tag = g_obj_tag;
}

static void draw_chain(Cpu *c, const char *kind, uint64_t count);
static uint64_t g_trace_cb[16], g_trace_list[64];  // (LARECOMP_TRACE_COUNT, below)
static int g_ntrace_cb, g_ntrace_list;

// The expanded command stream lives in host segments, but every returned NVN
// command handle still needs one tiny stable guest slot. When that handle ring
// is full, ask the game's normal NVN memory callback for a fresh guest chunk.
// This is the part the letsgo-recomp path relies on: the game's allocator owns
// command-ring lifetime, so reusing a range lets hostseg_release_range recycle
// all expanded segments associated with the old handles.
static bool command_handle_slot(Cpu *c, NvnCmdBuf *cb) {
    for (int tries = 0; tries < 2; tries++) {
        if (cb->guest_mem && cb->used + sizeof(CmdRec) <= cb->guest_size) return true;
        if (!cb->callback) break;

        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = sizeof(CmdRec) + JUMP_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    return cb->guest_mem && cb->used + sizeof(CmdRec) <= cb->guest_size;
}

NVN(nvnCommandBufferBeginRecording) {
    draw_chain(c, "begin", 0);
    static bool hooked;  // (hooks cost every game function a lookup: only with interpolation on)
    if (!hooked) {
        hooked = true;
        extern bool vk_interp_enabled(void), fps60_enabled(void);
        // (the 60 fps mode turns interpolation on only inside battles and captures: hooked from the start all the same)
        if ((vk_interp_enabled() || fps60_enabled()) && !getenv("LARECOMP_INTERP_NOOBJ"))  // (NOOBJ: pair by order, the old prototype)
            for (size_t i = 0; i < sizeof k_obj_fns / sizeof k_obj_fns[0]; i++) rt_register_fnhook(k_obj_fns[i], obj_hook);
    }
    NvnCmdBuf *cb = cb_of(A(0));
    cb->obj_tag = 0;
    static int diag_state = -1;
    if (diag_state < 0)
        diag_state = getenv("LARECOMP_TF_DIAG_DIR") || getenv("LARECOMP_DRAW_CHAINS") ||
                     getenv("LARECOMP_TRACE_COUNT") ? 1 : 0;
    if (__builtin_expect(diag_state, 0)) {
        memset(cb->diag_programs, 0, sizeof cb->diag_programs);
        cb->diag_tf_active = false;
    }
    cb->host_origin_lo = cb->host_origin_hi = 0;
    cb->host_chain_head = cb->host_chain_tail = -1;

    // Request the next ring chunk before marking the buffer recording. That
    // lets nvnCommandBufferAddCommandMemory safely recycle an old guest range
    // and its host segment chain.
    bool have_slot = command_handle_slot(c, cb);
    cb->recording = true;

    // Keep only one opaque jump record in guest command memory. The expanded
    // stream begins in host memory, so the game's ring usage reflects a stable
    // per-list handle instead of our much larger CmdRec representation.
    if (have_slot) {
        uint64_t slot = cb->guest_mem + cb->used;
        cb->host_origin_lo = slot;
        cb->host_origin_hi = slot + sizeof(CmdRec);
        uint32_t host_size = 0;
        uint64_t host = hostseg_take(cb, JUMP_RESERVE, &host_size, slot);
        CmdRec *j = raw_rec(slot, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = host;
        cb->used += sizeof(CmdRec);
        cb->start = slot;
        cb->mem = host;
        cb->size = host_size;
        cb->chunk_used = 0;
        cb->host_segment = true;
    } else {
        // Last-resort fallback only (missing/broken game callback). Normal
        // Scarlet recording should never stay here frame after frame: doing so
        // is what previously exhausted the 256 MB host-segment pool.
        if (cb->guest_mem) cb->used = cb->guest_size;
        uint32_t host_size = 0;
        uint64_t host = hostseg_take(cb, JUMP_RESERVE, &host_size, 0);
        cb->start = host;
        cb->mem = host;
        cb->size = host_size;
        cb->chunk_used = 0;
        cb->host_segment = true;
        static uint64_t fallbacks;
        if (++fallbacks == 1 || !(fallbacks & 255))
            LOG("[nvn] command handle host fallback %llu (guest free %llu bytes)\n",
                (unsigned long long)fallbacks,
                (unsigned long long)(cb->guest_size > cb->used ? cb->guest_size - cb->used : 0));
    }
    rec(c, cb, CMD_BARRIER, 0);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    for (int i = 0; i < g_ntrace_cb; i++)
        if (g_trace_cb[i] == A(0)) {  // (LARECOMP_TRACE_COUNT: this list holds the traced draw)
            g_trace_cb[i] = g_trace_cb[--g_ntrace_cb];
            if (g_ntrace_list < 64) g_trace_list[g_ntrace_list++] = cb->start;
            LOG("[trace] list %llx holds the traced draw\n", (unsigned long long)cb->start);
            break;
        }
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    uint64_t handle = cb->start;
    // The next recording should consume the next tiny slot in the game's
    // command-memory ring, not continue inside the host segment.
    cb->mem = cb->guest_mem;
    cb->size = cb->guest_size;
    cb->chunk_used = cb->used;
    cb->host_segment = false;
    cb->host_origin_lo = cb->host_origin_hi = 0;
    RET(handle);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
// (cmd, program, stages): only the stages in the mask are bound. Let's Go binds separate programs per stage (a
// vertex program, then a fragment program) and unbinds stages with a NULL program: the executor (exec.c) keeps the
// combination - binding the whole program each time left draws with half a program, all skipped (no 3D).
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;  // host program (0: unbind the stages)
    r->x[1] = s.x[2];                                                      // NVNshaderStageBits
    nvn_diag_bind_program(cb_of(s.x[0]), (NvnProgram *)(uintptr_t)r->x[0], (uint32_t)s.x[2]);
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) {
    nvn_diag_vertex_buffers(A(0), (uint32_t)A(1), (uint32_t)A(2), (const uint64_t *)AP(3));
    record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16);
}
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    uint64_t range[2] = {s.x[2], s.x[3]};
    nvn_diag_vertex_buffers(s.x[0], (uint32_t)s.x[1], 1, range);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, range, 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) {
    nvn_diag_tf_call("nvnCommandBufferBindTransformFeedbackBuffers", c);
    record_copy(c, CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS, 3, (uint32_t)A(2) * 16);
}
NVN(nvnCommandBufferBindTransformFeedbackBuffer) {
    nvn_diag_tf_call("nvnCommandBufferBindTransformFeedbackBuffer", c);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
NVN(nvnCommandBufferSetDepthClamp) { (void)c; }
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_RASTERIZER_DISCARD)
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { draw_chain(c, "arrays", c->x[3]); Snap s = snap(c); obj_mark(c, cb_of(s.x[0])); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { draw_chain(c, "arrays_inst", c->x[3]); Snap s = snap(c); obj_mark(c, cb_of(s.x[0])); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
// LARECOMP_TRACE_COUNT: the command lists that hold the traced draw (recorded once, called every frame): their
// handles, so nvnCommandBufferCallCommands can log who calls them and in which present.
static bool trace_list(uint64_t h) { for (int i = 0; i < g_ntrace_list; i++) if (g_trace_list[i] == h) return true; return false; }
static void trace_chain(Cpu *c) {
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 4000) return;
    if (g_ntrace_cb < 16) g_trace_cb[g_ntrace_cb++] = c->x[0];
    extern uint64_t renderer_present_count(void);
    LOG("[trace] f%llu draw count %llu lr %llx chain:", (unsigned long long)renderer_present_count(), (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

// Debug: LARECOMP_DRAW_CHAINS=<frame>[-<last>] logs every draw recorded during those presents: the present, its
// order, count and the guest call chain (return addresses up the frame records), plus command list recordings and
// calls - to find which game function draws each object, and whether lists are recorded every frame or reused.
static bool chains_on(void) {
    static long from = -2, to = -2;
    if (from == -2) {
        const char *e = getenv("LARECOMP_DRAW_CHAINS");
        from = e ? atol(e) : -1;
        to = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : from;
    }
    // Normal execution never enables this diagnostic. Do not call into the
    // renderer/present counter from every draw just to rediscover "disabled".
    if (__builtin_expect(from < 0, 1)) return false;
    extern uint64_t renderer_present_count(void);
    long f = (long)renderer_present_count();
    return f >= from && f <= to;
}
static void draw_chain(Cpu *c, const char *kind, uint64_t count) {
    if (!chains_on()) return;
    extern uint64_t renderer_present_count(void);
    static int n;
    char line[512];
    int len = snprintf(line, sizeof line, "[chain] f%llu %d %s %llu cb %llx tag %llx:", (unsigned long long)renderer_present_count(),
                       n++, kind, (unsigned long long)count, (unsigned long long)c->x[0], (unsigned long long)g_obj_tag);
    uint64_t fp = c->x[29];
    len += snprintf(line + len, sizeof line - len, " %llx", (unsigned long long)c->x[30]);
    for (int i = 0; i < 16 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        if (len < (int)sizeof line - 20) len += snprintf(line + len, sizeof line - len, " %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("%s\n", line);
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    draw_chain(c, "elements", c->x[3]);
    Snap s = snap(c);
    obj_mark(c, cb_of(s.x[0]));
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    draw_chain(c, "elements_bv", c->x[3]);
    Snap s = snap(c);
    obj_mark(c, cb_of(s.x[0]));
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); draw_chain(c, "elements_inst", c->x[3]); Snap s = snap(c); obj_mark(c, cb_of(s.x[0])); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
PLAIN(nvnCommandBufferMultiDrawArraysIndirectCount, CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT)
PLAIN(nvnCommandBufferMultiDrawElementsIndirectCount, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT)
TF_UNIMPLEMENTED(nvnCommandBufferDrawTransformFeedback)
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) {
    nvn_diag_tf_call("nvnCommandBufferBeginTransformFeedback", c);
    nvn_diag_tf_state(cb_of(A(0)), "nvnCommandBufferBeginTransformFeedback");
    Snap s = snap(c); CmdRec *r = record(c, CMD_BEGIN_TRANSFORM_FEEDBACK, 0, &s); r->x[0] = s.x[1];
}
NVN(nvnCommandBufferEndTransformFeedback) {
    nvn_diag_tf_call("nvnCommandBufferEndTransformFeedback", c);
    nvn_diag_tf_state(cb_of(A(0)), "nvnCommandBufferEndTransformFeedback");
    Snap s = snap(c); CmdRec *r = record(c, CMD_END_TRANSFORM_FEEDBACK, 0, &s); r->x[0] = s.x[1];
}
NVN(nvnCommandBufferPauseTransformFeedback) {
    nvn_diag_tf_call("nvnCommandBufferPauseTransformFeedback", c);
    nvn_diag_tf_state(cb_of(A(0)), "nvnCommandBufferPauseTransformFeedback");
    Snap s = snap(c); CmdRec *r = record(c, CMD_PAUSE_TRANSFORM_FEEDBACK, 0, &s); r->x[0] = s.x[1];
}
NVN(nvnCommandBufferResumeTransformFeedback) {
    nvn_diag_tf_call("nvnCommandBufferResumeTransformFeedback", c);
    nvn_diag_tf_state(cb_of(A(0)), "nvnCommandBufferResumeTransformFeedback");
    Snap s = snap(c); CmdRec *r = record(c, CMD_RESUME_TRANSFORM_FEEDBACK, 0, &s); r->x[0] = s.x[1];
}

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
NVN(nvnCommandBufferCallCommands) {
    if (g_ntrace_list) {
        const uint64_t *h = (const uint64_t *)(uintptr_t)A(2);
        for (uint64_t i = 0; i < A(1); i++)
            if (trace_list(h[i])) {
                extern uint64_t renderer_present_count(void);
                LOG("[trace] f%llu call list %llx lr %llx chain:", (unsigned long long)renderer_present_count(), (unsigned long long)h[i],
                    (unsigned long long)c->x[30]);
                trace_chain(c);
            }
    }
    draw_chain(c, "call", A(1));
    obj_mark(c, cb_of(A(0)));
    record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8);
}
NVN(nvnCommandBufferCopyCommands) {
    obj_mark(c, cb_of(A(0)));
    record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8);
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
