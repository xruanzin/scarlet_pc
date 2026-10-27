// Execute existing ArmRecomp AOT images on the Scarlet-native kernel/HLE.
//
// This is the transition layer that removes CpuManager/PhysicalCore/KScheduler
// from the normal execution loop without requiring an AOT rebuild. The DLL ABI
// and the LAS-style native runtime use different context layouts, so state is
// marshalled only at host boundaries (SVC/HLE/nested callback), not per block.
#include "aot.h"
#include "aot_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "../rt.h"

typedef struct {
    _Atomic uint64_t calls;
    _Atomic uint64_t slices;
    _Atomic uint64_t blocks;
    _Atomic uint64_t aot_ns;
    _Atomic uint64_t svc_calls;
    _Atomic uint64_t svc_ns;
    _Atomic uint64_t hle_calls;
    _Atomic uint64_t hle_ns;
    _Atomic uint64_t loads;
    _Atomic uint64_t stores;
    _Atomic uint64_t missing_targets;
    _Atomic uint64_t unhandled_halts;
    _Atomic uint64_t las_fallbacks;
    _Atomic uint64_t las_fallback_ns;
} AotProfile;

static AotProfile g_prof;
static _Atomic int g_profile = -1;

static bool profile_enabled(void) {
    int p = atomic_load_explicit(&g_profile, memory_order_relaxed);
    if (p >= 0) return p != 0;
    int value = getenv("SCARLET_NATIVE_PROFILE") ? 1 : 0;
    int expected = -1;
    atomic_compare_exchange_strong_explicit(&g_profile, &expected, value,
                                            memory_order_relaxed, memory_order_relaxed);
    return atomic_load_explicit(&g_profile, memory_order_relaxed) != 0;
}

static bool guest_span(uint64_t address, uint32_t size) {
    address &= UINT64_C(0x0000ffffffffffff);
    uint64_t bytes = size ? size : 4;
    return address >= GUEST_LO && address < GUEST_HI &&
           bytes <= GUEST_HI - address;
}

static uint64_t host_load_fast(void *user, uint64_t address, uint32_t size) {
    (void)user;
    address &= UINT64_C(0x0000ffffffffffff);
    if (!guest_span(address, size)) return 0;

    // Guard-v2 reserves size 0: bit 32 says the mapping is valid and low32 is
    // the instruction word.
    if (size == 0) {
        uint32_t value;
        memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
        return UINT64_C(0x100000000) | value;
    }

    uint64_t value = 0;
    if (size > sizeof value) return 0;
    memcpy(&value, (const void *)(uintptr_t)address, size);
    return value;
}

static void host_store_fast(void *user, uint64_t address, uint32_t size, uint64_t value) {
    (void)user;
    address &= UINT64_C(0x0000ffffffffffff);
    if (!size || size > sizeof value || !guest_span(address, size)) return;
    memcpy((void *)(uintptr_t)address, &value, size);
    // Publish the new generation only after the bytes are visible. A block
    // observing the new generation can then safely verify the new code.
    scarlet_aot_note_store(address, size);
}

static uint64_t host_load_profiled(void *user, uint64_t address, uint32_t size) {
    atomic_fetch_add_explicit(&g_prof.loads, 1, memory_order_relaxed);
    return host_load_fast(user, address, size);
}

static void host_store_profiled(void *user, uint64_t address, uint32_t size, uint64_t value) {
    atomic_fetch_add_explicit(&g_prof.stores, 1, memory_order_relaxed);
    host_store_fast(user, address, size, value);
}

static uint64_t host_excl_load(void *user, uint64_t address, uint32_t size) {
    Cpu *c = (Cpu *)user;
    address &= UINT64_C(0x0000ffffffffffff);
    switch (size) {
    case 1: return ldx8(c, address);
    case 2: return ldx16(c, address);
    case 4: return ldx32(c, address);
    case 8: return ldx64(c, address);
    default: return host_load_fast(user, address, size);
    }
}

static uint32_t host_excl_store(void *user, uint64_t address, uint32_t size, uint64_t value) {
    Cpu *c = (Cpu *)user;
    address &= UINT64_C(0x0000ffffffffffff);
    switch (size) {
    case 1: return stx8(c, address, value);
    case 2: return stx16(c, address, value);
    case 4: return stx32(c, address, value);
    case 8: return stx64(c, address, value);
    default: return 1;
    }
}

static void host_clear_excl(void *user) {
    Cpu *c = (Cpu *)user;
    c->excl_addr = UINT64_MAX;
}

static uint64_t host_cntpct(void *user) {
    (void)user;
    return rt_ticks();
}

static void host_excl_load_pair(void *user, uint64_t address, uint32_t size,
                                uint64_t *lo, uint64_t *hi) {
    Cpu *c = (Cpu *)user;
    address &= UINT64_C(0x0000ffffffffffff);
    if (size == 4) {
        uint64_t v = ldxp_pair32(c, address);
        *lo = (uint32_t)v;
        *hi = (uint32_t)(v >> 32);
    } else if (size == 8) {
        __uint128_t v = ldxp_pair64(c, address);
        *lo = (uint64_t)v;
        *hi = (uint64_t)(v >> 64);
    } else {
        *lo = host_load_fast(user, address, size);
        *hi = host_load_fast(user, address + size, size);
    }
}

static uint32_t host_excl_store_pair(void *user, uint64_t address, uint32_t size,
                                     uint64_t lo, uint64_t hi) {
    Cpu *c = (Cpu *)user;
    address &= UINT64_C(0x0000ffffffffffff);
    if (size == 4) return stxp_pair32(c, address, lo, hi);
    if (size == 8) return stxp_pair64(c, address, lo, hi);
    return 1;
}

static void setup_host_fp(void) {
#if defined(__x86_64__) || defined(_M_X64)
    // FPX1's x86 contract: RN, masked exceptions, DAZ/FTZ disabled. Guest FPCR
    // and FPSR themselves live in the AOT context and are copied below.
    _mm_setcsr(0x1f80u);
#endif
}

static void cpu_to_aot(const Cpu *c, ScarletAotGuestContext *g) {
    memcpy(g->x, c->x, 31 * sizeof(uint64_t));
    g->x[31] = c->sp;
    g->pc = c->pc;
    g->n = (uint8_t)((c->nzcv >> 31) & 1);
    g->z = (uint8_t)((c->nzcv >> 30) & 1);
    g->c = (uint8_t)((c->nzcv >> 29) & 1);
    g->v = (uint8_t)((c->nzcv >> 28) & 1);
    memcpy(g->vreg, c->v, sizeof g->vreg);
    g->tpidr_el0 = c->tpidr;
    g->tpidrro_el0 = c->tpidrro;
    g->fpcr = c->fpcr;
    g->fpsr = c->fpsr;
}

static void aot_to_cpu(const ScarletAotGuestContext *g, Cpu *c) {
    memcpy(c->x, g->x, 31 * sizeof(uint64_t));
    c->sp = g->x[31];
    c->pc = g->pc;
    c->nzcv = ((uint64_t)(g->n != 0) << 31) |
              ((uint64_t)(g->z != 0) << 30) |
              ((uint64_t)(g->c != 0) << 29) |
              ((uint64_t)(g->v != 0) << 28);
    memcpy(c->v, g->vreg, sizeof g->vreg);
    c->tpidr = g->tpidr_el0;
    c->tpidrro = g->tpidrro_el0;
    c->fpcr = g->fpcr;
    c->fpsr = g->fpsr;
}

static ScarletAotHostMem host_bridge(Cpu *cpu) {
    ScarletAotHostMem h = {0};
    h.user = cpu;
    const bool prof = profile_enabled();
    h.load = prof ? host_load_profiled : host_load_fast;
    h.store = prof ? host_store_profiled : host_store_fast;
    h.excl_load = host_excl_load;
    h.excl_store = host_excl_store;
    h.clear_excl = host_clear_excl;
    h.read_cntpct = host_cntpct;
    h.excl_load_pair = host_excl_load_pair;
    h.excl_store_pair = host_excl_store_pair;
    h.address_space_max = GUEST_HI;
    // page_entries intentionally stays NULL. The native window is identity
    // mapped, for which suyu's "host pointer minus guest VA" page encoding is
    // zero (also its unmapped sentinel). Direct callbacks are therefore the
    // correct ABI-preserving path until AOT is regenerated for identity memory.
    return h;
}

void scarlet_aot_profile_report(void) {
    if (!profile_enabled()) return;
#define TAKE(name) atomic_exchange_explicit(&g_prof.name, 0, memory_order_relaxed)
    uint64_t calls = TAKE(calls), slices = TAKE(slices), blocks = TAKE(blocks);
    uint64_t aot_ns = TAKE(aot_ns), svc = TAKE(svc_calls), svc_ns = TAKE(svc_ns);
    uint64_t hle = TAKE(hle_calls), hle_ns = TAKE(hle_ns);
    uint64_t loads = TAKE(loads), stores = TAKE(stores);
    uint64_t miss = TAKE(missing_targets), gaps = TAKE(unhandled_halts);
    uint64_t las = TAKE(las_fallbacks), las_ns = TAKE(las_fallback_ns);
#undef TAKE
    LOG("[native-prof] calls=%llu slices=%llu blocks=%llu aot=%.3fms "
        "svc=%llu/%.3fms hle=%llu/%.3fms mem=%lluR/%lluW miss=%llu gaps=%llu "
        "las-fallback=%llu/%.3fms\n",
        (unsigned long long)calls, (unsigned long long)slices,
        (unsigned long long)blocks, aot_ns / 1e6,
        (unsigned long long)svc, svc_ns / 1e6,
        (unsigned long long)hle, hle_ns / 1e6,
        (unsigned long long)loads, (unsigned long long)stores,
        (unsigned long long)miss, (unsigned long long)gaps,
        (unsigned long long)las, las_ns / 1e6);
}

static bool try_las_fallback(ScarletAotGuestContext *g, Cpu *cpu, bool prof) {
    // dispatch_lookup is populated only when a compatible lasgame is already
    // present and was loaded at boot. Normal native execution never touches
    // this table; it is strictly a migration fallback for uncovered AOT PCs.
    if (!dispatch_lookup(g->pc)) return false;

    aot_to_cpu(g, cpu);
    cpu->pc = g->pc;
    uint64_t t0 = prof ? plat_now_ns() : 0;
    rt_dispatch(cpu);
    if (prof) {
        atomic_fetch_add_explicit(&g_prof.las_fallbacks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_prof.las_fallback_ns,
                                  plat_now_ns() - t0, memory_order_relaxed);
    }
    cpu_to_aot(cpu, g);
    g->halted = SCARLET_AOT_HALT_NONE;
    g->pending_svc = SCARLET_AOT_NO_SVC;
    return true;
}

void scarlet_aot_guest_call(Cpu *cpu, uint64_t address) {
    if (!scarlet_aot_ready()) FATAL("native AOT guest call before module bind\n");

    const bool prof = profile_enabled();
    if (prof) atomic_fetch_add_explicit(&g_prof.calls, 1, memory_order_relaxed);

    setup_host_fp();

    ScarletAotHostMem bridge = host_bridge(cpu);
    ScarletAotGuestContext g;
    memset(&g, 0, sizeof g);
    cpu->pc = address;
    cpu_to_aot(cpu, &g);
    g.mem = (uint8_t *)(uintptr_t)GUEST_LO;
    g.mem_base_vaddr = GUEST_LO;
    g.mem_size = GUEST_HI - GUEST_LO;
    g.host_mem = &bridge;
    g.pending_svc = SCARLET_AOT_NO_SVC;
    g.fm_table = NULL;
    g.fm_limit = 0;

    uint64_t last_report = prof ? plat_now_ns() : 0;

    while (g.pc) {
        rt_unwind_point();

        // Existing native imports and overrides remain authoritative. This is
        // checked at host boundaries; module-local AOT chaining stays hot.
        aot_to_cpu(&g, cpu);
        GuestFn host = rt_dispatch_host_lookup(cpu, g.pc);
        if (host) {
            uint64_t t0 = prof ? plat_now_ns() : 0;
            cpu->pc = g.pc;
            host(cpu);
            if (prof) {
                atomic_fetch_add_explicit(&g_prof.hle_calls, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_prof.hle_ns, plat_now_ns() - t0, memory_order_relaxed);
            }
            cpu_to_aot(cpu, &g);
            g.pc = g.x[30];  // native HLE implements a guest function call
            continue;
        }

        ScarletAotModule *m = scarlet_aot_find_pc(g.pc);
        if (!m) {
            if (prof) atomic_fetch_add_explicit(&g_prof.missing_targets, 1, memory_order_relaxed);
            if (try_las_fallback(&g, cpu, prof)) continue;
            aot_to_cpu(&g, cpu);
            uint64_t off = 0;
            const char *sym = loader_symbol_at(g.x[30], &off);
            FATAL("native AOT has no target at %llx (lr=%llx %s+%llx); "
                  "no LAS fallback covers it\n",
                  (unsigned long long)g.pc, (unsigned long long)g.x[30],
                  sym ? sym : "?", (unsigned long long)off);
        }

        g.halted = SCARLET_AOT_HALT_NONE;
        g.pending_svc = SCARLET_AOT_NO_SVC;
        const int budget = 1024;
        g.chain_budget = budget;
        uint64_t t0 = prof ? plat_now_ns() : 0;
        m->run_slice(&g);
        if (prof) {
            uint64_t spent = g.chain_budget < budget ? (uint64_t)(budget - g.chain_budget) : 0;
            uint64_t ran = 1 + spent;
            if (g.chain_budget == 0 && ran) --ran; // final budget check parks before execution
            atomic_fetch_add_explicit(&g_prof.slices, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_prof.blocks, ran, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_prof.aot_ns, plat_now_ns() - t0, memory_order_relaxed);
        }

        if (g.halted == SCARLET_AOT_HALT_IC_IVAU) {
            uint64_t target = g.pending_svc;
            g.pending_svc = SCARLET_AOT_NO_SVC;
            scarlet_aot_note_store(target, 64);
            g.pc += 4;
            g.halted = SCARLET_AOT_HALT_NONE;
            continue;
        }

        if (g.halted == SCARLET_AOT_HALT_UNHANDLED) {
            if (prof) atomic_fetch_add_explicit(&g_prof.unhandled_halts, 1, memory_order_relaxed);
            if (try_las_fallback(&g, cpu, prof)) continue;
            aot_to_cpu(&g, cpu);
            uint32_t op = guest_span(g.pc, 4) ? *(const uint32_t *)(uintptr_t)g.pc : 0;
            FATAL("native AOT uncovered instruction %08x at %llx and no LAS "
                  "resume/function covers that PC\n",
                  op, (unsigned long long)g.pc);
        }
        if (g.halted == SCARLET_AOT_HALT_BREAKPOINT)
            FATAL("native AOT guest breakpoint at %llx\n", (unsigned long long)g.pc);
        if (g.halted && g.halted != SCARLET_AOT_HALT_EXIT)
            FATAL("native AOT halt=%d at %llx\n", g.halted, (unsigned long long)g.pc);
        if (g.halted == SCARLET_AOT_HALT_EXIT) break;

        if (g.pending_svc != SCARLET_AOT_NO_SVC) {
            uint32_t imm = (uint32_t)g.pending_svc;
            g.pending_svc = SCARLET_AOT_NO_SVC;
            aot_to_cpu(&g, cpu);
            uint64_t st = prof ? plat_now_ns() : 0;
            rt_svc(cpu, imm, g.pc);
            if (prof) {
                atomic_fetch_add_explicit(&g_prof.svc_calls, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_prof.svc_ns, plat_now_ns() - st, memory_order_relaxed);
            }
            cpu_to_aot(cpu, &g);
            g.pending_svc = SCARLET_AOT_NO_SVC;
        }

        if (prof) {
            uint64_t now = plat_now_ns();
            if (now - last_report >= UINT64_C(2000000000)) {
                last_report = now;
                scarlet_aot_profile_report();
                rt_hle_profile_report();
            }
        }
    }

    aot_to_cpu(&g, cpu);
}
