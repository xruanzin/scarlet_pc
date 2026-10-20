// Function-entry profiler. LARECOMP_FNPROF=<from>:<to> counts guest function entries (every generated
// function calls FN_ENTER) between those presented frames, then writes build/fnprof.txt (addr, count, order of
// first entry, guest lr of that first entry; most called first). Diffing two runs shows which game functions a change brings in; counts also show hot code.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"

volatile unsigned g_fn_enter_flags;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; uint64_t first, lr; } slots[NSLOT];
static _Atomic uint64_t first_seq;  // order of first entry (third column), and the guest lr of that entry (fourth)

// LARECOMP_FNPROF_BT=<hex addr>[,...]: also print the host backtrace (guest functions are f_<addr>) the first time
// each listed function is entered inside the window - who calls it
// (LARECOMP_FNPROF_BT_MAX=<n>: the first n entries instead of only the first)
static uint64_t bt_addrs[256];
static int bt_n = -1, bt_max = 1;
static _Atomic uint8_t bt_done[256];

uint64_t g_bt_first_x0;

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n < 0) {
        bt_n = 0;
        const char *e = getenv("LARECOMP_FNPROF_BT");
        if (getenv("LARECOMP_FNPROF_BT_MAX")) bt_max = atoi(getenv("LARECOMP_FNPROF_BT_MAX"));
        for (const char *p = e; p && *p && bt_n < 256;) {
            bt_addrs[bt_n++] = strtoull(p, (char **)&p, 16);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && atomic_fetch_add(&bt_done[i], 1) < bt_max) {
            if (i == 0) g_bt_first_x0 = x0;
            uint64_t vt = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t v100 = vt >= 0x7000000000ull && vt < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(vt + 0x100) : 0;
            uint64_t p8 = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            fprintf(stderr, "[fnprof-bt] %llx entered from guest lr %llx x0 %llx (vtable %llx, +0x100 %llx, [x0+8] %llx):\n",
                    (unsigned long long)a, (unsigned long long)lr, (unsigned long long)x0, (unsigned long long)vt,
                    (unsigned long long)v100, (unsigned long long)p8);
            plat_print_backtrace();  // (execinfo is macOS / glibc only)
        }
}

void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n) bt_check(a, lr, x0);
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) {
            slots[h].first = atomic_fetch_add_explicit(&first_seq, 1, memory_order_relaxed) + 1;
            slots[h].lr = lr;
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
        if (cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
}

static int cmp(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

static void fnprof_clear(void) {
    for (int i = 0; i < NSLOT; i++) {
        atomic_store_explicit(&slots[i].addr, 0, memory_order_relaxed);
        atomic_store_explicit(&slots[i].count, 0, memory_order_relaxed);
        slots[i].first = slots[i].lr = 0;
    }
    atomic_store_explicit(&first_seq, 0, memory_order_relaxed);
}

static void fnprof_write(uint64_t from, uint64_t to, const char *reason) {
    uint64_t (*rows)[4] = malloc(sizeof(uint64_t[4]) * NSLOT);
    if (!rows) {
        LOG("[fnprof] cannot allocate result table\n");
        return;
    }
    int n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; rows[n][2] = slots[i].first; rows[n][3] = slots[i].lr; n++; }
    qsort(rows, n, sizeof rows[0], cmp);
    const char *out = getenv("LARECOMP_FNPROF_OUT") ? getenv("LARECOMP_FNPROF_OUT") : "build/fnprof.txt";
    FILE *fp = fopen(out, "w");
    for (int i = 0; fp && i < n; i++)
        fprintf(fp, "%llx %llu %llu %llx\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1],
                (unsigned long long)rows[i][2], (unsigned long long)rows[i][3]);
    if (fp) fclose(fp);

    LOG("[fnprof] hottest guest entries (count, function, first caller):\n");
    int topn = n < 32 ? n : 32;
    for (int i = 0; i < topn; i++) {
        uint64_t off = 0, coff = 0;
        const char *sym = loader_symbol_at(rows[i][0], &off);
        const char *caller = loader_symbol_at(rows[i][3], &coff);
        LOG("[fnprof-hot] %llu %llx %s%s%llx <- %llx %s%s%llx\n",
            (unsigned long long)rows[i][1],
            (unsigned long long)rows[i][0],
            sym ? sym : "?", sym ? "+" : "", (unsigned long long)(sym ? off : 0),
            (unsigned long long)rows[i][3],
            caller ? caller : "?", caller ? "+" : "", (unsigned long long)(caller ? coff : 0));
    }

    free(rows);
    LOG("[fnprof] %d functions, written %s (%s, presents %llu-%llu)\n", n, out,
        reason ? reason : "capture", (unsigned long long)from, (unsigned long long)to);
}

static int auto_inited, auto_consecutive = 3, auto_frames = 90;
static double auto_ms;
static int auto_slow_run;
static bool auto_pending, auto_active, auto_done;
static uint64_t auto_trigger, auto_from, auto_to;

static void fnprof_auto_init(void) {
    if (auto_inited) return;
    auto_inited = 1;
    const char *e = getenv("LARECOMP_FNPROF_AUTO_MS");
    if (e && *e) auto_ms = strtod(e, NULL);
    e = getenv("LARECOMP_FNPROF_AUTO_CONSEC");
    if (e && *e) auto_consecutive = atoi(e);
    e = getenv("LARECOMP_FNPROF_AUTO_FRAMES");
    if (e && *e) auto_frames = atoi(e);
    if (auto_consecutive < 1) auto_consecutive = 1;
    if (auto_frames < 1) auto_frames = 1;
    if (auto_ms > 0.0)
        LOG("[fnprof-auto] armed: %.1f ms for %d consecutive frames -> capture next %d frames\n",
            auto_ms, auto_consecutive, auto_frames);
}

// Called after a frame's real wall time is known. It does not profile the hitch
// retroactively; persistent slow scenes trigger a capture of the immediately
// following frames, so scene timing/loading no longer needs a hard-coded frame id.
void fnprof_note_frame(uint64_t frame, double frame_ms) {
    fnprof_auto_init();
    if (auto_ms <= 0.0 || auto_done || auto_active || auto_pending) return;
    if (frame_ms >= auto_ms) auto_slow_run++;
    else auto_slow_run = 0;
    if (auto_slow_run < auto_consecutive) return;
    auto_pending = true;
    auto_trigger = frame;
    LOG("[fnprof-auto] slow scene detected at present %llu: %.1f ms (%d consecutive >= %.1f ms)\n",
        (unsigned long long)frame, frame_ms, auto_slow_run, auto_ms);
}

void fnprof_frame(uint64_t frame) {
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_FNPROF");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }

    // Explicit frame ranges still take priority for narrow/reproducible tests.
    if (from >= 0) {
        if ((long)frame == from) {
            fnprof_clear();
            __atomic_fetch_or(&g_fn_enter_flags, FN_FLAG_PROF, __ATOMIC_RELAXED);
            LOG("[fnprof] on at frame %llu\n", (unsigned long long)frame);
        }
        if ((long)frame != to) return;
        __atomic_fetch_and(&g_fn_enter_flags, ~FN_FLAG_PROF, __ATOMIC_RELAXED);
        fnprof_write((uint64_t)from, (uint64_t)to, "fixed frame window");
        from = -1;
        return;
    }

    fnprof_auto_init();
    if (auto_ms <= 0.0 || auto_done) return;
    if (auto_pending) {
        fnprof_clear();
        auto_pending = false;
        auto_active = true;
        auto_from = frame;
        auto_to = frame + (uint64_t)auto_frames;
        __atomic_fetch_or(&g_fn_enter_flags, FN_FLAG_PROF, __ATOMIC_RELAXED);
        LOG("[fnprof-auto] capture on after trigger %llu; profiling next %d frames\n",
            (unsigned long long)auto_trigger, auto_frames);
        return;
    }
    if (auto_active && frame >= auto_to) {
        __atomic_fetch_and(&g_fn_enter_flags, ~FN_FLAG_PROF, __ATOMIC_RELAXED);
        auto_active = false;
        auto_done = true;
        fnprof_write(auto_from, frame, "automatic slow-scene window");
    }
}

// LARECOMP_FNPROF=0:<to>: profile from process start (threads the game starts before the first frame)
__attribute__((constructor)) static void fnprof_early(void) {
    const char *e = getenv("LARECOMP_FNPROF");
    if (e && e[0] == '0' && e[1] == ':') __atomic_fetch_or(&g_fn_enter_flags, FN_FLAG_PROF, __ATOMIC_RELAXED);
}
