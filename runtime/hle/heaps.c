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
    struct StdAllocRec *next;
} StdAllocRec;

typedef struct StdFreeRec {
    uint64_t addr;
    uint64_t size;
    struct StdFreeRec *next;
} StdFreeRec;

static StdAllocRec *g_std_recs;
static StdFreeRec *g_std_free;
static pthread_mutex_t g_std_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_std_declared_size = 0x10000000ull;
static uint64_t g_std_live_bytes;

// Keep StandardAllocator pointers inside Horizon's 39-bit guest address space.
// HLE_HEAP ends at 0x70a0000000; the range up to ALIAS_BASE is otherwise
// unused by the runtime and gives Scarlet a 1.5 GiB guest-visible arena.
#define STD_GUEST_BASE (HLE_HEAP + HLE_HEAP_SIZE)
#define STD_GUEST_LIMIT ALIAS_BASE
static uint64_t g_std_next = STD_GUEST_BASE;

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

static void std_free_insert_locked(uint64_t addr, uint64_t size) {
    if (!size) return;
    StdFreeRec *n = malloc(sizeof *n);
    if (!n) return;
    n->addr = addr;
    n->size = size;
    StdFreeRec **pp = &g_std_free;
    while (*pp && (*pp)->addr < addr) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;

    // Coalesce adjacent free spans.
    StdFreeRec *p = g_std_free;
    while (p && p->next) {
        if (p->addr + p->size == p->next->addr) {
            StdFreeRec *q = p->next;
            p->size += q->size;
            p->next = q->next;
            free(q);
        } else {
            p = p->next;
        }
    }
}

static uint64_t std_guest_alloc(uint64_t size, uint64_t align) {
    if (!size) size = 1;
    align = std_align(align);

    pthread_mutex_lock(&g_std_lock);

    uint64_t addr = 0;
    StdFreeRec **pp = &g_std_free;
    while (*pp) {
        StdFreeRec *f = *pp;
        uint64_t a = align_up_u64(f->addr, align);
        uint64_t prefix = a - f->addr;
        if (prefix <= f->size && size <= f->size - prefix) {
            uint64_t suffix_addr = a + size;
            uint64_t suffix = (f->addr + f->size) - suffix_addr;
            if (prefix && suffix) {
                f->size = prefix;
                StdFreeRec *tail = malloc(sizeof *tail);
                if (tail) {
                    tail->addr = suffix_addr;
                    tail->size = suffix;
                    tail->next = f->next;
                    f->next = tail;
                }
            } else if (prefix) {
                f->size = prefix;
            } else if (suffix) {
                f->addr = suffix_addr;
                f->size = suffix;
            } else {
                *pp = f->next;
                free(f);
            }
            addr = a;
            break;
        }
        pp = &(*pp)->next;
    }

    if (!addr) {
        uint64_t a = align_up_u64(g_std_next, align);
        if (a < STD_GUEST_BASE || a > STD_GUEST_LIMIT || size > STD_GUEST_LIMIT - a) {
            pthread_mutex_unlock(&g_std_lock);
            return 0;
        }
        addr = a;
        g_std_next = a + size;
    }

    StdAllocRec *r = malloc(sizeof *r);
    if (!r) {
        std_free_insert_locked(addr, size);
        pthread_mutex_unlock(&g_std_lock);
        return 0;
    }
    r->addr = addr;
    r->size = size;
    r->align = align;
    r->next = g_std_recs;
    g_std_recs = r;
    g_std_live_bytes += size;
    pthread_mutex_unlock(&g_std_lock);

    plat_commit(addr, size);
    return addr;
}

static StdAllocRec *std_find_locked(uint64_t addr, StdAllocRec ***link_out) {
    StdAllocRec **link = &g_std_recs;
    while (*link) {
        if ((*link)->addr == addr) {
            if (link_out) *link_out = link;
            return *link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static void std_guest_free(uint64_t addr) {
    if (!addr) return;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec **link = NULL;
    StdAllocRec *r = std_find_locked(addr, &link);
    if (!r) {
        pthread_mutex_unlock(&g_std_lock);
        LOG("[heaps] StandardAllocator::Free unknown guest pointer %#llx (ignored)\n",
            (unsigned long long)addr);
        return;
    }
    *link = r->next;
    if (g_std_live_bytes >= r->size) g_std_live_bytes -= r->size;
    std_free_insert_locked(r->addr, r->size);
    pthread_mutex_unlock(&g_std_lock);
    free(r);
}

static uint64_t std_guest_realloc(uint64_t addr, uint64_t size) {
    if (!addr) return std_guest_alloc(size, 16);
    if (!size) size = 1;

    uint64_t old_size = 0, align = 16;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_find_locked(addr, NULL);
    if (r) {
        old_size = r->size;
        align = r->align;
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
    uint64_t size = 0;
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = std_find_locked(addr, NULL);
    if (r) size = r->size;
    pthread_mutex_unlock(&g_std_lock);
    return size;
}

RT_ON_RESET(stdalloc_reset) {
    pthread_mutex_lock(&g_std_lock);
    StdAllocRec *r = g_std_recs;
    StdFreeRec *f = g_std_free;
    g_std_recs = NULL;
    g_std_free = NULL;
    g_std_live_bytes = 0;
    g_std_declared_size = 0x10000000ull;
    g_std_next = STD_GUEST_BASE;
    pthread_mutex_unlock(&g_std_lock);
    while (r) {
        StdAllocRec *n = r->next;
        free(r);
        r = n;
    }
    while (f) {
        StdFreeRec *n = f->next;
        free(f);
        f = n;
    }
}

static void std_init_common(Cpu *c, bool cached) {
    g_std_declared_size = c->x[2] ? c->x[2] : 0x10000000ull;
    LOG("[heaps] StandardAllocator guest-backed: requested=%#llx pool=%#llx..%#llx cached=%d\n",
        (unsigned long long)g_std_declared_size,
        (unsigned long long)STD_GUEST_BASE,
        (unsigned long long)STD_GUEST_LIMIT,
        cached ? 1 : 0);
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
    pthread_mutex_unlock(&g_std_lock);
    c->x[0] = cap > used ? cap - used : 0;
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8FinalizeEv", heap_std_finalize) {
    (void)c;
}
