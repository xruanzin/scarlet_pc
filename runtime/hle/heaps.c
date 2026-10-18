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
    void *ptr;
    uint64_t size;
    uint64_t align;
    struct StdAllocRec *next;
} StdAllocRec;

static StdAllocRec *g_std_recs;
static pthread_mutex_t g_std_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_std_declared_size = 0x10000000ull;
static uint64_t g_std_live_bytes;

static uint64_t std_align(uint64_t a) {
    if (a < 16) a = 16;
    // Nintendo callers use powers of two; be defensive for malformed values.
    if (a & (a - 1)) {
        uint64_t p = 16;
        while (p < a && p < (1ull << 62)) p <<= 1;
        a = p;
    }
    return a;
}

static void *std_host_alloc(uint64_t size, uint64_t align) {
    if (!size) size = 1;
    align = std_align(align);
#ifdef _WIN32
    void *p = _aligned_malloc((size_t)size, (size_t)align);
#else
    void *p = NULL;
    if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) p = NULL;
#endif
    if (!p) return NULL;

    StdAllocRec *r = malloc(sizeof *r);
    if (!r) {
#ifdef _WIN32
        _aligned_free(p);
#else
        free(p);
#endif
        return NULL;
    }
    r->ptr = p; r->size = size; r->align = align;
    pthread_mutex_lock(&g_std_lock);
    r->next = g_std_recs;
    g_std_recs = r;
    g_std_live_bytes += size;
    pthread_mutex_unlock(&g_std_lock);
    return p;
}

static StdAllocRec *std_find_locked(void *p, StdAllocRec ***link_out) {
    StdAllocRec **link = &g_std_recs;
    while (*link) {
        if ((*link)->ptr == p) {
            if (link_out) *link_out = link;
            return *link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static void std_host_free(void *p) {
    if (!p) return;
    uint64_t size = 0;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec **link = NULL;
    StdAllocRec *r = std_find_locked(p, &link);
    if (r) {
        *link = r->next;
        size = r->size;
        if (g_std_live_bytes >= size) g_std_live_bytes -= size;
    }
    pthread_mutex_unlock(&g_std_lock);
    if (!r) {
        LOG("[heaps] StandardAllocator::Free unknown pointer %p (ignored)\n", p);
        return;
    }
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
    free(r);
}

static void *std_host_realloc(void *p, uint64_t size) {
    if (!p) return std_host_alloc(size, 16);
    if (!size) size = 1;

    uint64_t old_size = 0, align = 16;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_find_locked(p, NULL);
    if (r) { old_size = r->size; align = r->align; }
    pthread_mutex_unlock(&g_std_lock);
    if (!r) return NULL;

    void *n = std_host_alloc(size, align);
    if (!n) return NULL;
    memcpy(n, p, (size_t)(old_size < size ? old_size : size));
    std_host_free(p);
    return n;
}

static uint64_t std_host_size(void *p) {
    uint64_t size = 0;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_find_locked(p, NULL);
    if (r) size = r->size;
    pthread_mutex_unlock(&g_std_lock);
    return size;
}

RT_ON_RESET(stdalloc_reset) {
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = g_std_recs;
    g_std_recs = NULL;
    g_std_live_bytes = 0;
    g_std_declared_size = 0x10000000ull;
    pthread_mutex_unlock(&g_std_lock);
    while (r) {
        StdAllocRec *n = r->next;
#ifdef _WIN32
        _aligned_free(r->ptr);
#else
        free(r->ptr);
#endif
        free(r);
        r = n;
    }
}

static void std_init_common(Cpu *c, bool cached) {
    g_std_declared_size = c->x[2] ? c->x[2] : 0x10000000ull;
    LOG("[heaps] StandardAllocator host-backed: arena=%#llx addr=%#llx cached=%d\n",
        (unsigned long long)g_std_declared_size,
        (unsigned long long)c->x[1], cached ? 1 : 0);
    // Initialize is void. Preserve x0 ('this') as normal AArch64 member
    // functions commonly do; no guest SDK call is made here.
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvm", heap_std_init) {
    std_init_common(c, false);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvmb", heap_std_init_b) {
    std_init_common(c, c->x[3] != 0);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEm", heap_std_alloc) {
    uint64_t size = c->x[1], lr = c->x[30];
    c->x[0] = (uint64_t)(uintptr_t)std_host_alloc(size, 16);
    if (!c->x[0] && size) fail_once("standard allocation", lr, size);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEmm", heap_std_alloc_a) {
    uint64_t size = c->x[1], align = c->x[2], lr = c->x[30];
    c->x[0] = (uint64_t)(uintptr_t)std_host_alloc(size, align);
    if (!c->x[0] && size) fail_once("standard allocation (aligned)", lr, size);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator4FreeEPv", heap_std_free) {
    std_host_free((void *)(uintptr_t)c->x[1]);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10ReallocateEPvm", heap_std_realloc) {
    uint64_t size = c->x[2], lr = c->x[30];
    c->x[0] = (uint64_t)(uintptr_t)std_host_realloc((void *)(uintptr_t)c->x[1], size);
    if (!c->x[0] && size) fail_once("standard reallocation", lr, size);
}

HLE_FUNC("_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv", heap_std_sizeof) {
    c->x[0] = std_host_size((void *)(uintptr_t)c->x[1]);
}

HLE_FUNC("_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv", heap_std_free_size) {
    pthread_mutex_lock(&g_std_lock);
    uint64_t live = g_std_live_bytes, total = g_std_declared_size;
    pthread_mutex_unlock(&g_std_lock);
    c->x[0] = total > live ? total - live : 0;
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8FinalizeEv", heap_std_finalize) {
    (void)c;
}
