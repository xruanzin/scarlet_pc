// Sampling profiler for recompiled guest code.
//
// Unlike FNPROF this does not require instrumentation in lasgame.dll. On
// Windows it periodically samples the RIP of registered guest threads, maps
// host PCs back to f_<guest address>, and writes a fnprof-compatible file.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "rt.h"

void hostprof_start(void);

#ifdef _WIN32

enum { HOSTPROF_CAP = 1 << 18, HOSTPROF_MAX_PCS = 128 };

typedef struct {
    uint64_t addr;
    uint64_t count;
} HostProfSlot;

typedef struct {
    uint64_t addr;
    uint64_t count;
} HostProfRow;

static HostProfSlot g_hostprof[HOSTPROF_CAP];
static const char *g_hostprof_out;
static int g_hostprof_ms = 5;
static int g_hostprof_started;

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);

static unsigned hostprof_hash(uint64_t a) {
    return (unsigned)(((a >> 2) * 11400714819323198485ull) >> (64 - 18));
}

static void hostprof_hit(uint64_t addr) {
    unsigned h = hostprof_hash(addr);
    for (unsigned probe = 0; probe < HOSTPROF_CAP; probe++) {
        HostProfSlot *s = &g_hostprof[(h + probe) & (HOSTPROF_CAP - 1)];
        if (!s->addr) {
            s->addr = addr;
            s->count = 1;
            return;
        }
        if (s->addr == addr) {
            s->count++;
            return;
        }
    }
}

static int hostprof_cmp(const void *aa, const void *bb) {
    const HostProfRow *a = aa, *b = bb;
    if (a->count < b->count) return 1;
    if (a->count > b->count) return -1;
    return a->addr < b->addr ? -1 : a->addr > b->addr;
}

static void hostprof_flush(void) {
    if (!g_hostprof_out || !*g_hostprof_out) return;

    size_t n = 0;
    for (size_t i = 0; i < HOSTPROF_CAP; i++)
        if (g_hostprof[i].addr && g_hostprof[i].count) n++;

    HostProfRow *rows = n ? malloc(n * sizeof *rows) : NULL;
    if (n && !rows) return;

    size_t k = 0;
    for (size_t i = 0; i < HOSTPROF_CAP; i++) {
        if (!g_hostprof[i].addr || !g_hostprof[i].count) continue;
        rows[k++] = (HostProfRow){g_hostprof[i].addr, g_hostprof[i].count};
    }
    qsort(rows, n, sizeof *rows, hostprof_cmp);

    FILE *f = fopen(g_hostprof_out, "wb");
    if (f) {
        fprintf(f, "# host RIP sampling; interval_ms=%d\n", g_hostprof_ms);
        fprintf(f, "# addr count first lr\n");
        for (size_t i = 0; i < n; i++)
            fprintf(f, "%llx %llu 0 0\n",
                    (unsigned long long)rows[i].addr,
                    (unsigned long long)rows[i].count);
        fclose(f);
    }
    free(rows);
}

static void *hostprof_thread(void *unused) {
    (void)unused;
    uint64_t next_flush = plat_now_ns() + 1000000000ull;
    for (;;) {
        uint64_t pcs[HOSTPROF_MAX_PCS];
        int n = plat_sample_registered_thread_pcs(pcs, HOSTPROF_MAX_PCS);
        for (int i = 0; i < n; i++) {
            uint64_t off = 0;
            const char *name = rt_host_fn_name(pcs[i], &off);
            if (!name || strncmp(name, "f_", 2)) continue;
            unsigned long long addr = 0;
            if (sscanf(name + 2, "%llx", &addr) == 1 && addr)
                hostprof_hit((uint64_t)addr);
        }

        uint64_t now = plat_now_ns();
        if (now >= next_flush) {
            hostprof_flush();
            next_flush = now + 1000000000ull;
        }
        plat_sleep_until_ns(now + (uint64_t)g_hostprof_ms * 1000000ull);
    }
    return NULL;
}

void hostprof_start(void) {
    if (g_hostprof_started) return;
    g_hostprof_out = getenv("LARECOMP_HOSTPROF_OUT");
    if (!g_hostprof_out || !*g_hostprof_out) return;

    const char *ms = getenv("LARECOMP_HOSTPROF_MS");
    if (ms && atoi(ms) >= 1 && atoi(ms) <= 100) g_hostprof_ms = atoi(ms);

    g_hostprof_started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, hostprof_thread, NULL) == 0) {
        pthread_detach(t);
        fprintf(stderr, "[hostprof] sampling guest threads every %d ms -> %s\n",
                g_hostprof_ms, g_hostprof_out);
    } else {
        g_hostprof_started = 0;
        fprintf(stderr, "[hostprof] failed to start sampler thread\n");
    }
}

#else

void hostprof_start(void) {}

#endif
