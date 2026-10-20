// Game heaps (engine limits). The game carves its memory into SDK heaps of fixed sizes: nn::mem::StandardAllocator
// (general), nn::lmem unit heaps (fixed-size blocks: object pools) and frame heaps. A heap that runs out returns
// NULL and the game crashes further on (bushsanity: ~450 actors around Link -> null weak_ptr in f_70010d0f00).
// These wrappers call the SDK's own implementation (loader_find_symbol), log heap creation (LARECOMP_LOG_HEAPS=1)
// and every allocation that fails (once per caller), and can grow heaps (heap_scale) - a recomp has memory to spare.
#include <pthread.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

uint64_t loader_find_symbol(const char *name);
void guest_call(Cpu *c, uint64_t addr);

static uint64_t orig(const char *name, uint64_t *cache) {
    if (!*cache) *cache = loader_find_symbol(name);
    if (!*cache) FATAL("heaps: no SDK implementation of %s\n", name);
    return *cache;
}

static void fail_once(const char *what, uint64_t lr, uint64_t size) {
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 64) seen[n++] = lr;
    LOG("[heaps] %s FAILED (size %#llx) from lr %llx\n", what, (unsigned long long)size, (unsigned long long)lr);
}

#define CALL(sym)                                    \
    do {                                             \
        static uint64_t a_;                          \
        uint64_t lr_ = c->x[30];                     \
        guest_call(c, orig(sym, &a_));               \
        c->x[30] = lr_;                              \
    } while (0)

// Unit heaps are fixed pools (e.g. 256 x 0x50-byte objects per loaded area: bushsanity's extra actors ran out at
// ~450 actors around Link). Every unit heap gets UNIT_SCALE times the units in a buffer of ours (the game's own
// buffer stays unused); freed again in DestroyUnitHeap. LARECOMP_UNIT_HEAP_SCALE overrides (1 = the game's sizes).
typedef struct { uint64_t head; void *mem; } UnitBuf;
static UnitBuf g_unit[256];
static pthread_mutex_t g_unit_lock = PTHREAD_MUTEX_INITIALIZER;

// Reboot: our unit-heap buffers (host memory, not in the guest window) go with the game that made them.
RT_ON_RESET(heaps_reset) {
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
        }
}

static int unit_scale(void) {
    static int s = -1;
    if (s < 0) { const char *e = ENV("LARECOMP_UNIT_HEAP_SCALE"); s = e ? atoi(e) : 4; if (s < 1) s = 1; }
    return s;
}

// nn::lmem::CreateUnitHeap(void *addr, size_t size, size_t unit, int align, int opt, HeapHead *head)
HLE_FUNC("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE", heap_create_unit) {
    uint64_t lr = c->x[30], size = c->x[1], unit = c->x[2];
    int k = unit_scale();
    void *mem = NULL;
    if (k > 1 && unit) {
        uint64_t big = size * (uint64_t)k;
#ifdef _WIN32
        mem = _aligned_malloc(big, 0x1000);
        if (mem) {
#else
        if (posix_memalign(&mem, 0x1000, big) == 0) {
#endif
            memset(mem, 0, big);
            c->x[0] = (uint64_t)(uintptr_t)mem;
            c->x[1] = big;
        } else {
            mem = NULL;
        }
    }
    uint64_t head = c->x[5];
    CALL("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE");
    if (mem) {
        pthread_mutex_lock(&g_unit_lock);
        for (int i = 0; i < 256; i++)
            if (!g_unit[i].mem) { g_unit[i].head = c->x[0] ? c->x[0] : head; g_unit[i].mem = mem; break; }
        pthread_mutex_unlock(&g_unit_lock);
    }
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] unit heap %#llx bytes, unit %#llx (%llu units, x%d) from lr %llx -> %llx\n", (unsigned long long)size,
            (unsigned long long)unit, unit ? (unsigned long long)(size / unit) : 0ull, mem ? k : 1, (unsigned long long)lr,
            (unsigned long long)c->x[0]);
}

HLE_FUNC("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE", heap_destroy_unit) {
    uint64_t head = c->x[0];
    CALL("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE");
    pthread_mutex_lock(&g_unit_lock);
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem && g_unit[i].head == head) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
            break;
        }
    pthread_mutex_unlock(&g_unit_lock);
}

HLE_FUNC("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE", heap_alloc_unit) {
    uint64_t lr = c->x[30], head = c->x[0];
    CALL("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE");
    if (!c->x[0]) fail_once("unit heap allocation", lr, head);
}

// nn::lmem::CreateFrameHeap(void *addr, size_t size, int opt)
HLE_FUNC("_ZN2nn4lmem15CreateFrameHeapEPvmi", heap_create_frame) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn4lmem15CreateFrameHeapEPvmi");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] frame heap %#llx bytes from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi", heap_alloc_frame) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi");
    if (!c->x[0]) fail_once("frame heap allocation", lr, size);
}

// nn::mem::StandardAllocator
//
// Scarlet initializes its allocator with addr == NULL and a 0x300000000-byte
// virtual arena. The Nintendo SDK implementation expects console virtual-memory
// plumbing that this host runtime does not expose yet and aborts during boot.
// Recompiled code can directly dereference host pointers, so use a host-backed
// allocator here. This is also the strategy used by other native Switch
// recompilation projects and gets the boot past the SDK allocator without
// committing the advertised 12 GiB arena.

typedef struct StdAllocRec {
    uint64_t addr;
    uint64_t size;
    uint64_t align;
    uint64_t capacity;  // reserved span; size remains the caller-visible requested size
    // all-list is only for reset; hash chain is the hot lookup path.
    struct StdAllocRec *next, *prev, *hnext;
} StdAllocRec;

typedef struct StdFreeRec {
    uint64_t addr;
    uint64_t size;
    struct StdFreeRec *next;
} StdFreeRec;

static StdAllocRec *g_std_recs;
#define STD_REC_HASH (1u << 17)
static StdAllocRec *g_std_hash[STD_REC_HASH];

#define STD_FREE_BINS 32
static StdFreeRec *g_std_free_bins[STD_FREE_BINS];
static uint64_t g_std_free_bytes;
static pthread_mutex_t g_std_lock = PTHREAD_MUTEX_INITIALIZER;

// The old split-span lists accumulate tiny alignment fragments and search them
// under one global lock on every small allocation. Scarlet's HLE profile reports
// seconds in these APIs during customization, including lock waiting. Keep exact
// size/alignment classes, reusing their metadata as well as memory.
// The cache is shared, so cross-thread frees do not strand memory in TLS.
#define STD_SMALL_SIZES 256             // 16-byte classes through 4096 bytes
#define STD_SMALL_ALIGNS 9              // alignments 16, 32, ... 4096
#define STD_SMALL_CLASSES (STD_SMALL_SIZES * STD_SMALL_ALIGNS)
#define STD_SMALL_CLASS_LIMIT 2048
#define STD_SMALL_CACHE_LIMIT (64ull << 20)
static StdAllocRec *g_std_small[STD_SMALL_CLASSES];
static uint16_t g_std_small_count[STD_SMALL_CLASSES];
static uint64_t g_std_small_bytes;

static int std_small_class(uint64_t capacity, uint64_t align) {
    if (!capacity || capacity > 4096 || (capacity & 15) || align < 16 || align > 4096 || (align & (align - 1))) return -1;
    int a = 0;
    for (uint64_t v = align; v > 16; v >>= 1) a++;
    return a * STD_SMALL_SIZES + (int)(capacity / 16) - 1;
}

static inline uint32_t std_rec_hash(uint64_t addr) {
    return (uint32_t)(((addr >> 4) * 11400714819323198485ull) >> (64 - 17));
}

static void std_rec_insert_locked(StdAllocRec *r) {
    r->prev = NULL;
    r->next = g_std_recs;
    if (g_std_recs) g_std_recs->prev = r;
    g_std_recs = r;

    uint32_t h = std_rec_hash(r->addr);
    r->hnext = g_std_hash[h];
    g_std_hash[h] = r;
}

static StdAllocRec *std_rec_find_locked(uint64_t addr) {
    for (StdAllocRec *r = g_std_hash[std_rec_hash(addr)]; r; r = r->hnext)
        if (r->addr == addr) return r;
    return NULL;
}

static void std_rec_remove_locked(StdAllocRec *r) {
    if (r->prev) r->prev->next = r->next;
    else g_std_recs = r->next;
    if (r->next) r->next->prev = r->prev;

    uint32_t h = std_rec_hash(r->addr);
    StdAllocRec **pp = &g_std_hash[h];
    while (*pp && *pp != r) pp = &(*pp)->hnext;
    if (*pp) *pp = r->hnext;
}
static uint64_t g_std_declared_size = 0x10000000ull;
static uint64_t g_std_live_bytes;

// Keep StandardAllocator pointers inside Horizon's 39-bit guest address space.
// Use the runtime's canonical application heap range: it is directly encodable
// in HIPC descriptors and provides up to 3.25 GiB without host-pointer truncation.
#define STD_GUEST_BASE HEAP_BASE
#define STD_GUEST_LIMIT (HEAP_BASE + HEAP_MAX)
// Commit ahead in 16 MiB slabs. This keeps Windows out of the per-allocation
// hot path while guaranteeing that every pointer returned to lifted guest code
// is writable immediately. Freed/reused spans are already below this frontier.
#define STD_COMMIT_GRANULE 0x01000000ull
static uint64_t g_std_next = STD_GUEST_BASE;
static uint64_t g_std_committed_to = STD_GUEST_BASE;

static uint64_t std_align(uint64_t a) {
    if (a < 16) a = 16;
    if (a & (a - 1)) {
        uint64_t p = 16;
        while (p < a && p < (1ull << 62)) p <<= 1;
        a = p;
    }
    return a;
}

static uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

// ------------------------------------------------------------------
// Aggressive small-allocation fast path.
//
// Scarlet allocates/frees millions of small StandardAllocator objects. The old
// path took g_std_lock, searched metadata/free bins and touched the global hash
// on every operation. Keep large/unusual allocations on that exact path, but
// serve <=4 KiB / <=256-byte-aligned allocations from per-host-thread slabs.
//
// A 32-byte header immediately before the returned guest pointer makes free,
// realloc and GetSizeOf lock-free too. Freed payloads are singly-linked in the
// freeing thread's magazine; cross-thread frees are still valid and simply
// migrate the block to that thread. Refill grabs g_std_lock only once for a
// batch, advances the same guest bump frontier and commits ahead as before.
#define STD_FAST_MAGIC 0x534341524c455446ull /* "SCARLETF" */
#define STD_FAST_MAX 4096u
#define STD_FAST_SIZE_CLASSES (STD_FAST_MAX / 16u)
#define STD_FAST_ALIGN_CLASSES 5u /* 16,32,64,128,256 */
#define STD_FAST_CLASSES (STD_FAST_SIZE_CLASSES * STD_FAST_ALIGN_CLASSES)

typedef struct {
    uint64_t magic;
    uint32_t generation;
    uint16_t cls;
    uint8_t align_log2;
    uint8_t reserved8;
    uint32_t requested;
    uint32_t capacity;
    uint64_t reserved64;
} StdFastHdr;

typedef struct {
    uint64_t head[STD_FAST_CLASSES];
    uint32_t generation;
} StdFastTls;

static _Thread_local StdFastTls g_std_fast_tls;
static uint32_t g_std_fast_generation = 1;
static uint64_t g_std_fast_allocs, g_std_fast_frees, g_std_fast_refills, g_std_fast_slow;

static unsigned std_log2_pow2(uint64_t v) {
    unsigned n = 0;
    while ((1ull << n) < v) n++;
    return n;
}

static void std_fast_tls_sync(void) {
    uint32_t g = __atomic_load_n(&g_std_fast_generation, __ATOMIC_ACQUIRE);
    if (g_std_fast_tls.generation == g) return;
    memset(&g_std_fast_tls, 0, sizeof g_std_fast_tls);
    g_std_fast_tls.generation = g;
}

static int std_fast_class(uint64_t size, uint64_t align, uint32_t *capacity_out, uint64_t *align_out) {
    if (!size) size = 1;
    align = std_align(align);
    if (align > 256 || size > STD_FAST_MAX) return -1;
    uint64_t capacity = align_up_u64(size, 16);
    if (!capacity || capacity > STD_FAST_MAX) return -1;
    unsigned alog = std_log2_pow2(align);
    if (alog < 4 || alog > 8) return -1;
    unsigned aclass = alog - 4;
    unsigned sclass = (unsigned)(capacity / 16) - 1;
    if (capacity_out) *capacity_out = (uint32_t)capacity;
    if (align_out) *align_out = align;
    return (int)(aclass * STD_FAST_SIZE_CLASSES + sclass);
}

static StdFastHdr *std_fast_header(uint64_t addr) {
    if (addr < STD_GUEST_BASE + sizeof(StdFastHdr) || addr >= STD_GUEST_LIMIT) return NULL;
    StdFastHdr *h = (StdFastHdr *)(uintptr_t)(addr - sizeof(StdFastHdr));
    uint32_t gen = __atomic_load_n(&g_std_fast_generation, __ATOMIC_ACQUIRE);
    if (h->magic != STD_FAST_MAGIC || h->generation != gen || h->cls >= STD_FAST_CLASSES ||
        !h->capacity || h->capacity > STD_FAST_MAX || h->align_log2 < 4 || h->align_log2 > 8)
        return NULL;
    return h;
}

static uint64_t std_fast_refill(int cls, uint32_t capacity, uint64_t align, uint64_t requested) {
    unsigned batch = capacity <= 128 ? 128 : capacity <= 512 ? 64 : capacity <= 2048 ? 32 : 16;
    uint64_t prefix = align_up_u64(sizeof(StdFastHdr), align);
    uint64_t stride = align_up_u64(prefix + capacity, align);
    if (!stride || batch > UINT64_MAX / stride) return 0;
    uint64_t total = stride * batch;

    pthread_mutex_lock(&g_std_lock);
    uint64_t base = align_up_u64(g_std_next, align);
    if (base < STD_GUEST_BASE || base > STD_GUEST_LIMIT || total > STD_GUEST_LIMIT - base) {
        pthread_mutex_unlock(&g_std_lock);
        return 0;
    }
    uint64_t end = base + total;
    g_std_next = end;

    if (end > g_std_committed_to) {
        uint64_t want = align_up_u64(end, STD_COMMIT_GRANULE);
        if (want > STD_GUEST_LIMIT) want = STD_GUEST_LIMIT;
        if (want > g_std_committed_to) {
            plat_commit(g_std_committed_to, want - g_std_committed_to);
            g_std_committed_to = want;
        }
    }
    pthread_mutex_unlock(&g_std_lock);

    uint32_t gen = __atomic_load_n(&g_std_fast_generation, __ATOMIC_ACQUIRE);
    unsigned alog = std_log2_pow2(align);
    uint64_t first = 0;
    for (unsigned i = 0; i < batch; i++) {
        uint64_t cell = base + (uint64_t)i * stride;
        uint64_t addr = cell + prefix;
        StdFastHdr *h = (StdFastHdr *)(uintptr_t)(addr - sizeof(StdFastHdr));
        h->magic = STD_FAST_MAGIC;
        h->generation = gen;
        h->cls = (uint16_t)cls;
        h->align_log2 = (uint8_t)alog;
        h->reserved8 = 0;
        h->requested = (uint32_t)(i ? capacity : requested);
        h->capacity = capacity;
        h->reserved64 = 0;
        if (!i) {
            first = addr;
        } else {
            *(uint64_t *)(uintptr_t)addr = g_std_fast_tls.head[cls];
            g_std_fast_tls.head[cls] = addr;
        }
    }
    __atomic_fetch_add(&g_std_fast_refills, 1, __ATOMIC_RELAXED);
    return first;
}

static uint64_t std_fast_alloc(uint64_t size, uint64_t align) {
    uint32_t capacity;
    uint64_t norm_align;
    int cls = std_fast_class(size, align, &capacity, &norm_align);
    if (cls < 0) return 0;

    std_fast_tls_sync();
    uint64_t addr = g_std_fast_tls.head[cls];
    if (addr) {
        g_std_fast_tls.head[cls] = *(uint64_t *)(uintptr_t)addr;
        StdFastHdr *h = (StdFastHdr *)(uintptr_t)(addr - sizeof(StdFastHdr));
        h->requested = (uint32_t)(size ? size : 1);
        __atomic_fetch_add(&g_std_fast_allocs, 1, __ATOMIC_RELAXED);
        return addr;
    }

    addr = std_fast_refill(cls, capacity, norm_align, size ? size : 1);
    if (addr) __atomic_fetch_add(&g_std_fast_allocs, 1, __ATOMIC_RELAXED);
    return addr;
}

static bool std_fast_free(uint64_t addr) {
    StdFastHdr *h = std_fast_header(addr);
    if (!h) return false;
    std_fast_tls_sync();
    *(uint64_t *)(uintptr_t)addr = g_std_fast_tls.head[h->cls];
    g_std_fast_tls.head[h->cls] = addr;
    __atomic_fetch_add(&g_std_fast_frees, 1, __ATOMIC_RELAXED);
    return true;
}

static int std_free_bin(uint64_t size) {
    if (size <= 16) return 0;
    uint64_t v = size - 1;
    int lg = 0;
    while (v >>= 1) lg++;
    int b = lg - 3;  // (16,32] -> 1, (32,64] -> 2, ...
    if (b < 0) b = 0;
    if (b >= STD_FREE_BINS) b = STD_FREE_BINS - 1;
    return b;
}

static void std_free_insert_locked(uint64_t addr, uint64_t size) {
    if (!size) return;
    StdFreeRec *n = malloc(sizeof *n);
    if (!n) return;
    n->addr = addr;
    n->size = size;
    int b = std_free_bin(size);
    n->next = g_std_free_bins[b];
    g_std_free_bins[b] = n;
    g_std_free_bytes += size;
}

static uint64_t std_guest_alloc(uint64_t size, uint64_t align) {
    if (!size) size = 1;
    if (size > UINT64_MAX - 15) return 0;
    uint64_t capacity = (size + 15) & ~15ull;
    align = std_align(align);

    uint64_t fast = std_fast_alloc(size, align);
    if (fast) return fast;
    __atomic_fetch_add(&g_std_fast_slow, 1, __ATOMIC_RELAXED);

    pthread_mutex_lock(&g_std_lock);

    uint64_t addr = 0;
    StdAllocRec *r = NULL;
    int small = std_small_class(capacity, align);
    if (small >= 0 && g_std_small[small]) {
        r = g_std_small[small];
        g_std_small[small] = r->next;
        g_std_small_count[small]--;
        g_std_small_bytes -= capacity;
        g_std_free_bytes -= capacity;
        addr = r->addr;
    }
    // Cache misses and large objects use the general free spans. These searches
    // remain linear within a bin; check alignment on each candidate span.
    int first_bin = std_free_bin(capacity);
    for (int b = first_bin; b < STD_FREE_BINS && !addr; b++) {
        StdFreeRec **pp = &g_std_free_bins[b];
        while (*pp) {
            StdFreeRec *f = *pp;
            uint64_t a = align_up_u64(f->addr, align);
            uint64_t prefix = a - f->addr;
            if (prefix <= f->size && capacity <= f->size - prefix) {
                uint64_t old_size = f->size;
                uint64_t old_end = f->addr + f->size;
                *pp = f->next;
                free(f);
                if (g_std_free_bytes >= old_size) g_std_free_bytes -= old_size;

                uint64_t suffix_addr = a + capacity;
                uint64_t suffix = old_end - suffix_addr;
                if (prefix) std_free_insert_locked(a - prefix, prefix);
                if (suffix) std_free_insert_locked(suffix_addr, suffix);
                addr = a;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    if (!addr) {
        uint64_t a = align_up_u64(g_std_next, align);
        if (a < STD_GUEST_BASE || a > STD_GUEST_LIMIT || capacity > STD_GUEST_LIMIT - a) {
            pthread_mutex_unlock(&g_std_lock);
            return 0;
        }
        addr = a;
        g_std_next = a + capacity;
    }

    if (!r) r = malloc(sizeof *r);
    if (!r) {
        std_free_insert_locked(addr, capacity);
        pthread_mutex_unlock(&g_std_lock);
        return 0;
    }
    r->addr = addr;
    r->size = size;
    r->align = align;
    r->capacity = capacity;
    std_rec_insert_locked(r);
    g_std_live_bytes += size;

    // Never return an uncommitted guest pointer. The previous pure lazy-commit
    // path could miss a lifted-code write before the Windows handler serviced it
    // (Scarlet crashed constructing an object at returned_ptr + 0x10).
    //
    // Do this while holding g_std_lock: another guest thread must not observe an
    // advanced commit frontier until VirtualAlloc has actually completed.
    uint64_t end = addr + capacity;
    if (end > g_std_committed_to) {
        uint64_t want = align_up_u64(end, STD_COMMIT_GRANULE);
        if (want > STD_GUEST_LIMIT) want = STD_GUEST_LIMIT;
        if (want > g_std_committed_to) {
            plat_commit(g_std_committed_to, want - g_std_committed_to);
            g_std_committed_to = want;
        }
    }
    pthread_mutex_unlock(&g_std_lock);
    return addr;
}

static void std_guest_free(uint64_t addr) {
    if (!addr) return;
    if (std_fast_free(addr)) return;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_rec_find_locked(addr);
    if (!r) {
        pthread_mutex_unlock(&g_std_lock);
        LOG("[heaps] StandardAllocator::Free unknown guest pointer %#llx (ignored)\n",
            (unsigned long long)addr);
        return;
    }
    std_rec_remove_locked(r);
    if (g_std_live_bytes >= r->size) g_std_live_bytes -= r->size;
    int small = std_small_class(r->capacity, r->align);
    if (small >= 0 && g_std_small_count[small] < STD_SMALL_CLASS_LIMIT &&
        r->capacity <= STD_SMALL_CACHE_LIMIT - g_std_small_bytes) {
        r->next = g_std_small[small];
        g_std_small[small] = r;
        g_std_small_count[small]++;
        g_std_small_bytes += r->capacity;
        g_std_free_bytes += r->capacity;
        pthread_mutex_unlock(&g_std_lock);
        return;
    }
    std_free_insert_locked(r->addr, r->capacity);
    pthread_mutex_unlock(&g_std_lock);
    free(r);
}

static uint64_t std_guest_realloc(uint64_t addr, uint64_t size) {
    if (!addr) return std_guest_alloc(size, 16);
    if (!size) size = 1;

    StdFastHdr *fh = std_fast_header(addr);
    if (fh) {
        uint64_t old_size = fh->requested;
        uint64_t align = 1ull << fh->align_log2;
        if (size <= fh->capacity) {
            fh->requested = (uint32_t)size;
            return addr;
        }
        uint64_t n = std_guest_alloc(size, align);
        if (!n) return 0;
        memcpy((void *)(uintptr_t)n, (void *)(uintptr_t)addr,
               (size_t)(old_size < size ? old_size : size));
        std_fast_free(addr);
        return n;
    }

    uint64_t old_size = 0, align = 16;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_rec_find_locked(addr);
    if (r) {
        old_size = r->size;
        align = r->align;
        if (size <= r->capacity) {
            g_std_live_bytes -= r->size;
            g_std_live_bytes += size;
            r->size = size;
            pthread_mutex_unlock(&g_std_lock);
            return addr;
        }
    }
    pthread_mutex_unlock(&g_std_lock);
    if (!r) return 0;

    uint64_t n = std_guest_alloc(size, align);
    if (!n) return 0;
    memcpy((void *)(uintptr_t)n, (void *)(uintptr_t)addr,
           (size_t)(old_size < size ? old_size : size));
    std_guest_free(addr);
    return n;
}

static uint64_t std_guest_size(uint64_t addr) {
    StdFastHdr *fh = std_fast_header(addr);
    if (fh) return fh->requested;
    uint64_t size = 0;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_rec_find_locked(addr);
    if (r) size = r->size;
    pthread_mutex_unlock(&g_std_lock);
    return size;
}

RT_ON_RESET(stdalloc_reset) {
    __atomic_add_fetch(&g_std_fast_generation, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = g_std_recs;
    StdFreeRec *free_bins[STD_FREE_BINS];
    StdAllocRec *small_bins[STD_SMALL_CLASSES];
    memcpy(free_bins, g_std_free_bins, sizeof free_bins);
    memcpy(small_bins, g_std_small, sizeof small_bins);
    g_std_recs = NULL;
    memset(g_std_hash, 0, sizeof g_std_hash);
    memset(g_std_free_bins, 0, sizeof g_std_free_bins);
    memset(g_std_small, 0, sizeof g_std_small);
    memset(g_std_small_count, 0, sizeof g_std_small_count);
    g_std_small_bytes = 0;
    g_std_free_bytes = 0;
    g_std_live_bytes = 0;
    g_std_declared_size = 0x10000000ull;
    g_std_next = STD_GUEST_BASE;
    g_std_committed_to = STD_GUEST_BASE;
    pthread_mutex_unlock(&g_std_lock);
    while (r) {
        StdAllocRec *n = r->next;
        free(r);
        r = n;
    }
    for (int b = 0; b < STD_FREE_BINS; b++) {
        StdFreeRec *f = free_bins[b];
        while (f) {
            StdFreeRec *n = f->next;
            free(f);
            f = n;
        }
    }
    for (int b = 0; b < STD_SMALL_CLASSES; b++) {
        StdAllocRec *cached = small_bins[b];
        while (cached) {
            StdAllocRec *next = cached->next;
            free(cached);
            cached = next;
        }
    }
}

static void std_init_common(Cpu *c, bool cached) {
    g_std_declared_size = c->x[2] ? c->x[2] : 0x10000000ull;
    LOG("[heaps] StandardAllocator guest-backed: requested=%#llx pool=%#llx..%#llx cached=%d fast<=%uB/alignment<=256\n",
        (unsigned long long)g_std_declared_size,
        (unsigned long long)STD_GUEST_BASE,
        (unsigned long long)STD_GUEST_LIMIT,
        cached ? 1 : 0, STD_FAST_MAX);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvm", heap_std_init) {
    std_init_common(c, false);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvmb", heap_std_init_b) {
    std_init_common(c, c->x[3] != 0);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEm", heap_std_alloc) {
    uint64_t size = c->x[1], lr = c->x[30];
    c->x[0] = std_guest_alloc(size, 16);
    if (!c->x[0] && size) fail_once("standard guest allocation", lr, size);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEmm", heap_std_alloc_a) {
    uint64_t size = c->x[1], align = c->x[2], lr = c->x[30];
    c->x[0] = std_guest_alloc(size, align);
    if (!c->x[0] && size) fail_once("standard guest allocation (aligned)", lr, size);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator4FreeEPv", heap_std_free) {
    std_guest_free(c->x[1]);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10ReallocateEPvm", heap_std_realloc) {
    uint64_t size = c->x[2], lr = c->x[30];
    c->x[0] = std_guest_realloc(c->x[1], size);
    if (!c->x[0] && size) fail_once("standard guest reallocation", lr, size);
}

HLE_FUNC("_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv", heap_std_sizeof) {
    c->x[0] = std_guest_size(c->x[1]);
}

HLE_FUNC("_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv", heap_std_free_size) {
    pthread_mutex_lock(&g_std_lock);
    uint64_t used = g_std_next - STD_GUEST_BASE;
    uint64_t cap = STD_GUEST_LIMIT - STD_GUEST_BASE;
    uint64_t free_bytes = g_std_free_bytes;
    pthread_mutex_unlock(&g_std_lock);
    uint64_t tail = cap > used ? cap - used : 0;
    c->x[0] = tail + free_bytes;
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8FinalizeEv", heap_std_finalize) {
    (void)c;
}
