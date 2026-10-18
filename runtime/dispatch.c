// Guest address -> recompiled function lookup, and host (HLE) function registry.
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamelib.h"
#include "hle.h"

typedef struct {
    const uint64_t *addrs;
    const GuestFn *fns;
    uint64_t count, lo, hi;
    const GameLibResumeRange *resumes;
    uint64_t resume_count;
} FnTable;
static FnTable tables[GAMELIB_MAX_MODULES];
static int ntables;

typedef struct GuestJumpScope {
    jmp_buf native;
    struct GuestJumpScope *prev;
    Cpu *cpu;
    uint64_t sp, pc;
} GuestJumpScope;
static __thread GuestJumpScope *guest_jump_scope;
static __thread unsigned guest_jump_logs;

void rt_guest_setjmp(Cpu *c, GuestFn setup, GuestFn continuation, uint64_t return_pc) {
    GuestJumpScope scope = {.prev = guest_jump_scope, .cpu = c, .sp = c->sp, .pc = return_pc};
    guest_jump_scope = &scope;
    if (!setjmp(scope.native)) {
        c->x[30] = return_pc;
        setup(c);
    }
    // Call the static continuation directly: dispatching this PC would catch
    // our own active scope again. Native frames below this scope remain live.
    c->pc = return_pc;
    continuation(c);
    guest_jump_scope = scope.prev;
}

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("LARECOMP_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

static void hle_unimplemented(Cpu *c);

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

// ---- recent-HLE ring. Shared histories outlive their writer threads (TLS does not).
#define HHIST 128
#define MAX_HH_THREADS 128
typedef struct { const char *name; uint64_t x0, x1, x30; } HhEntry;
typedef struct { HhEntry e[HHIST]; uint64_t n, tid; _Atomic bool busy; } Hh;
typedef struct { HhEntry e[HHIST]; uint64_t n, tid; } HhSnapshot;
static Hh hh_all[MAX_HH_THREADS];
static _Atomic unsigned hh_nall;
static __thread Hh hh_fallback;
static __thread Hh *hh_current;

static void hhist_push(const char *name, Cpu *c) {
    if (!hh_current) {
        unsigned k = atomic_fetch_add_explicit(&hh_nall, 1, memory_order_relaxed);
        hh_current = k < MAX_HH_THREADS ? &hh_all[k] : &hh_fallback;
        // tid is protected by busy, just like the entries. Never publish a pointer to TLS.
    }
    Hh *h = hh_current;
    bool expected = false;
    // Never delay a guest call for diagnostics: a concurrent snapshot may drop one record.
    if (!atomic_compare_exchange_strong_explicit(&h->busy, &expected, true, memory_order_acquire, memory_order_relaxed)) return;
    if (!h->tid) h->tid = plat_register_thread();
    unsigned i = h->n & (HHIST - 1);
    h->e[i] = (HhEntry){name, c->x[0], c->x[1], c->x[30]};
    h->n++;
    atomic_store_explicit(&h->busy, false, memory_order_release);
}
static bool hhist_snapshot(Hh *h, HhSnapshot *out) {
    if (!h) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&h->busy, &expected, true, memory_order_acquire, memory_order_relaxed)) return false;
    memcpy(out->e, h->e, sizeof out->e);
    out->n = h->n;
    out->tid = h->tid;
    atomic_store_explicit(&h->busy, false, memory_order_release);
    return true;
}
static void hhist_print(const HhSnapshot *h, int last) {
    unsigned n = h->n < HHIST ? (unsigned)h->n : HHIST;
    if (last > 0 && n > (unsigned)last) n = (unsigned)last;
    uint64_t start = h->n - n;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (start + k) & (HHIST - 1);
        uint64_t off = 0;
        const char *sym = loader_symbol_at(h->e[i].x30, &off);
        fprintf(stderr, "  %s x0=%llx x1=%llx lr=%llx", h->e[i].name ? h->e[i].name : "?",
                (unsigned long long)h->e[i].x0, (unsigned long long)h->e[i].x1, (unsigned long long)h->e[i].x30);
        if (sym) fprintf(stderr, " (%s+%llx)", sym, (unsigned long long)off);
        fputc('\n', stderr);
    }
}
static void hhist_dump(void) {
    HhSnapshot h;
    if (!hhist_snapshot(hh_current, &h)) return;
    fprintf(stderr, "last %u host calls:\n", h.n < HHIST ? (unsigned)h.n : HHIST);
    hhist_print(&h, 0);
}

// Runs on a dedicated diagnostic thread, never suspends game threads or blocks the SDL event loop.
void rt_hang_report(void) {
    fprintf(stderr, "\n===== hang report: no frame presented, per-thread state =====\n");
    unsigned count = atomic_load_explicit(&hh_nall, memory_order_relaxed);
    unsigned n = count < MAX_HH_THREADS ? count : MAX_HH_THREADS;
    for (unsigned k = 0; k < n; k++) {
        HhSnapshot h;
        if (!hhist_snapshot(&hh_all[k], &h)) {
            fprintf(stderr, "[hang] history #%u busy; snapshot skipped\n", k);
            continue;
        }
        if (!h.n) continue;
        fprintf(stderr, "[hang] history #%u (os tid %llu): %llu HLE calls, last ones:\n", k,
                (unsigned long long)h.tid, (unsigned long long)h.n);
        hhist_print(&h, 6);
    }
    if (count > MAX_HH_THREADS) fprintf(stderr, "[hang] history capacity reached; later threads omitted\n");
    kernel_dump_guest_threads();
    plat_dump_other_threads();
    fprintf(stderr, "===== end of hang report =====\n");
    fflush(stderr);
}

static __thread uint64_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (x0=%llx x1=%llx lr=%llx) -> returning 0\n", hle_slots[i].name, c->x[0],
            c->x[1], c->x[30]);
    c->x[0] = 0;
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

// Shared direct-mapped cache in front of the binary search.
// Guest address -> lifted function is immutable for a boot, so making this
// per-thread only duplicated memory and forced every Scarlet worker to warm the
// same lookup table again. 262144 entries (~4 MiB total) cover almost the
// complete lifted working set and are shared by loader, jobs, audio and render.
#define DCACHE 262144
typedef struct {
    _Atomic uint64_t addr;
    _Atomic(uintptr_t) fn;
} DCacheEnt;
static DCacheEnt dcache[DCACHE];

static uint64_t hle_next = HLE_HEAP;
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

// Immutable exact-entry lookup built before guest threads start. Scarlet has
// ~271k lifted function entries; a 2^20 open-addressing table stays under 30%
// load, making cold indirect calls O(1) instead of a binary search per module.
#define EXACT_TAB (1u << 20)
typedef struct { uint64_t addr; GuestFn fn; } ExactEnt;
static ExactEnt *exact_tab;

static inline uint32_t exact_hash(uint64_t a) {
    return (uint32_t)(((a >> 2) * 11400714819323198485ull) >> (64 - 20));
}
static void exact_insert(uint64_t addr, GuestFn fn) {
    uint32_t h = exact_hash(addr);
    while (exact_tab[h].addr && exact_tab[h].addr != addr) h = (h + 1) & (EXACT_TAB - 1);
    exact_tab[h] = (ExactEnt){addr, fn};
}
static GuestFn exact_lookup(uint64_t addr) {
    if (!exact_tab) return NULL;
    uint32_t h = exact_hash(addr);
    for (;;) {
        uint64_t a = exact_tab[h].addr;
        if (a == addr) return exact_tab[h].fn;
        if (!a) return NULL;
        h = (h + 1) & (EXACT_TAB - 1);
    }
}

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {  // (after gamelib_load)
    const GameLibTables *g = gamelib_tables();
    memset(dcache, 0, sizeof dcache);
    if (!exact_tab) exact_tab = calloc(EXACT_TAB, sizeof *exact_tab);
    else memset(exact_tab, 0, EXACT_TAB * sizeof *exact_tab);
    if (!exact_tab) FATAL("cannot allocate exact guest dispatch table\n");
    ntables = 0;
    for (uint32_t i = 0; i < g->nmods && i < GAMELIB_MAX_MODULES; i++) {
        const GameLibModule *m = &g->mods[i];
        uint64_t n = *m->count;
        uint64_t nr = m->resume_count ? *m->resume_count : 0;
        if (n) {
            uint64_t lo = m->addrs[0], hi = m->addrs[n - 1];
            if (nr) {
                if (m->resumes[0].lo < lo) lo = m->resumes[0].lo;
                if (m->resumes[nr - 1].hi - 1 > hi) hi = m->resumes[nr - 1].hi - 1;
                LOG("[dispatch] %s: %llu static resume ranges\n", m->name, (unsigned long long)nr);
            }
            tables[ntables++] = (FnTable){m->addrs, m->fns, n, lo, hi, m->resumes, nr};
            for (uint64_t k = 0; k < n; k++) exact_insert(m->addrs[k], m->fns[k]);
        }
    }
}

GuestFn dispatch_lookup(uint64_t addr) {
    GuestFn exact = exact_lookup(addr);
    if (exact) return exact;

    // Only computed jumps into the middle of a lifted fragment need the slower
    // resume-range search. Normal indirect calls never reach this loop now.
    if (addr & 3) return NULL;
    for (int t = 0; t < ntables; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi || !ft->resume_count) continue;
        uint64_t lo = 0, hi = ft->resume_count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->resumes[mid].lo <= addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo && addr < ft->resumes[lo - 1].hi) return ft->resumes[lo - 1].fn;
    }
    return NULL;
}

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();  // (HLE calls aren't cached below, so every one passes here: a reboot's safe point)
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            hhist_push(hle_slots[i].name, c);
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f) {
        uint64_t off;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", pc, c->x[30], s ? s : "?", off);
    }
    unsigned h = (unsigned)(((pc >> 2) * 11400714819323198485ull) >> (64 - 18));
    atomic_store_explicit(&dcache[h].fn, (uintptr_t)f, memory_order_relaxed);
    atomic_store_explicit(&dcache[h].addr, pc, memory_order_release);
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    if (__builtin_expect(guest_jump_scope != NULL, 0) && c->pc == c->x[30]) {
        for (GuestJumpScope *j = guest_jump_scope; j; j = j->prev) {
            if (j->cpu == c && j->pc == c->pc && j->sp == c->sp) {
                // SDK longjmp has restored guest registers/SP already. Discard
                // the abandoned native frames and any intervening jump scopes.
                guest_jump_scope = j;
                if (guest_jump_logs++ < 4)
                    LOG("[dispatch] guest longjmp -> pc=%llx sp=%llx value=%llu\n",
                        (unsigned long long)c->pc, (unsigned long long)c->sp,
                        (unsigned long long)c->x[0]);
                longjmp(j->native, 1);
            }
        }
    }
    unsigned h = (unsigned)(((c->pc >> 2) * 11400714819323198485ull) >> (64 - 18));
    uint64_t cached = atomic_load_explicit(&dcache[h].addr, memory_order_acquire);
    GuestFn f = cached == c->pc
        ? (GuestFn)(uintptr_t)atomic_load_explicit(&dcache[h].fn, memory_order_relaxed)
        : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < ntables; t++) total += tables[t].count + tables[t].resume_count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < ntables; t++) {
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
            for (uint64_t i = 0; i < tables[t].resume_count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].resumes[i].fn;
                v[k++][1] = tables[t].resumes[i].lo;
            }
        }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    const char *s = plat_symbol(pc, offset);
    return s ? s : rt_host_fn_name(pc, offset);
}

void rt_fatal_trace(void) {
    hhist_dump();
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    Cpu *c = kernel_current_cpu();
    if (c) {
        fprintf(stderr, "guest: sp=%llx lr=%llx tpidr=%llx tpidrro=%llx\n",
                c->sp, c->x[30], c->tpidr, c->tpidrro);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoint: the page is protected; accesses in [lo, hi) are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the next re-arm.
uint64_t g_watch_lo, g_watch_hi;
static uint64_t watch_page;
static void *watch_thread(void *a) {
    (void)a;
    int r = plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE);
    fprintf(stderr, "[watch] page %llx protect -> %d\n", (unsigned long long)watch_page, r);
    for (;;) { plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE); usleep(100); }
    return NULL;
}
void watch_arm(void) {
    if (!g_watch_lo || watch_page) return;
    watch_page = g_watch_lo & ~(PLAT_PAGE - 1);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    if (!watch_page || fa < watch_page || fa >= watch_page + PLAT_PAGE) return false;
    if (fa >= g_watch_lo && fa < g_watch_hi) {
        static char seen[64][48];
        static int nseen;
        const char *nm = host_sym(pc, NULL);
        if (!nm) nm = "?";
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= !strcmp(seen[i], nm);
        if (!found && nseen < 64) {
            snprintf(seen[nseen++], 48, "%s", nm);
            fprintf(stderr, "[watch] +%#llx by %s\n", fa - g_watch_lo, nm);
        }
    }
    plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }

