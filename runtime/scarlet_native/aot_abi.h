// Scarlet AOT ABI bridge for the existing ArmRecomp-generated DLLs.
// This file models the generated-image ABI; it is intentionally independent
// from suyu headers so the native runtime does not link against the emulator.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The preserved Scarlet images use ABI 6. ABI 5 has a shorter context tail
// (save_dir starts where ABI 6 adds FM1 fields), so pretending one struct fits
// both would silently corrupt standalone-helper state.
#define SCARLET_AOT_ABI_MIN 6u
#define SCARLET_AOT_ABI_MAX 6u

#define SCARLET_AOT_FEATURE_FASTMEM_PT1 (1u << 0)
#define SCARLET_AOT_FEATURE_GUARD_GEN1  (1u << 1)
#define SCARLET_AOT_FEATURE_FPX1        (1u << 2)
#define SCARLET_AOT_FEATURE_SUPPORTED   (SCARLET_AOT_FEATURE_FASTMEM_PT1 | \
                                         SCARLET_AOT_FEATURE_GUARD_GEN1 | \
                                         SCARLET_AOT_FEATURE_FPX1)

#define SCARLET_AOT_FPX_INHIBIT (UINT64_C(1) << 32)
#define SCARLET_AOT_NO_SVC UINT64_MAX

enum {
    SCARLET_AOT_HALT_NONE = 0,
    SCARLET_AOT_HALT_EXIT = 1,
    SCARLET_AOT_HALT_UNHANDLED = 2,
    SCARLET_AOT_HALT_BREAKPOINT = 3,
    SCARLET_AOT_HALT_IC_IVAU = 4,
};

typedef struct ScarletAotHostMem {
    void *user;
    uint64_t (*load)(void *user, uint64_t va, uint32_t size);
    void (*store)(void *user, uint64_t va, uint32_t size, uint64_t value);
    uint64_t (*excl_load)(void *user, uint64_t va, uint32_t size);
    uint32_t (*excl_store)(void *user, uint64_t va, uint32_t size, uint64_t value);
    void (*clear_excl)(void *user);
    uint64_t (*read_cntpct)(void *user);
    void (*excl_load_pair)(void *user, uint64_t va, uint32_t size, uint64_t *lo, uint64_t *hi);
    uint32_t (*excl_store_pair)(void *user, uint64_t va, uint32_t size, uint64_t lo, uint64_t hi);
    const void *page_entries;
    uint64_t page_entry_stride;
    uint64_t page_bits;
    uint64_t pointer_mask;
    uint64_t address_space_max;
    const uint64_t *guard_generation;
} ScarletAotHostMem;

typedef struct ScarletAotGuestContext {
    uint64_t x[32];
    uint64_t pc;
    uint8_t n, z, c, v;
    uint8_t *mem;
    uint64_t mem_size;
    uint64_t mem_base_vaddr;
    int halted;
    uint64_t pending_svc;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    const ScarletAotHostMem *host_mem;
    uint64_t tpidrro_el0;
    uint64_t fpcr;
    uint64_t fpsr;
    int chain_budget;
    uint32_t fm_reserved;
    const unsigned char *fm_table;
    uint64_t fm_limit;

    // The generated ABI has standalone-runtime bookkeeping after the shared
    // prefix. Keep enough storage here so helpers compiled into an AOT DLL can
    // never run past our object even if a diagnostic path touches those fields.
    char save_dir[512];
    uint64_t heap_base, heap_end, heap_cur;
    uint32_t ipc_cmd[64];
    void *save_handles[16];
    int save_handle_count;
} ScarletAotGuestContext;

_Static_assert(offsetof(ScarletAotGuestContext, pc) == 256, "AOT pc ABI");
_Static_assert(offsetof(ScarletAotGuestContext, pending_svc) == 304, "AOT svc ABI");
_Static_assert(offsetof(ScarletAotGuestContext, vreg) == 312, "AOT SIMD ABI");
_Static_assert(offsetof(ScarletAotGuestContext, tpidr_el0) == 824, "AOT TPIDR ABI");
_Static_assert(offsetof(ScarletAotGuestContext, host_mem) == 832, "AOT host-memory ABI");
_Static_assert(offsetof(ScarletAotGuestContext, tpidrro_el0) == 840, "AOT TPIDRRO ABI");
_Static_assert(offsetof(ScarletAotGuestContext, fpcr) == 848, "AOT FPCR ABI");
_Static_assert(offsetof(ScarletAotGuestContext, fpsr) == 856, "AOT FPSR ABI");
_Static_assert(offsetof(ScarletAotGuestContext, chain_budget) == 864, "AOT chain ABI");
_Static_assert(offsetof(ScarletAotGuestContext, fm_table) == 872, "AOT FM1 table ABI");
_Static_assert(offsetof(ScarletAotGuestContext, fm_limit) == 880, "AOT FM1 limit ABI");
_Static_assert(offsetof(ScarletAotHostMem, page_entries) == 72, "AOT host page-table ABI");
_Static_assert(offsetof(ScarletAotHostMem, guard_generation) == 112, "AOT host guard ABI");
_Static_assert(sizeof(ScarletAotHostMem) == 120, "AOT host-memory size ABI");

typedef void (*ScarletAotBlockFn)(ScarletAotGuestContext *);
typedef ScarletAotBlockFn (*ScarletAotLookupFn)(uint64_t pc);
typedef void (*ScarletAotSetBaseFn)(uint64_t base);
typedef void (*ScarletAotRunSliceFn)(ScarletAotGuestContext *);
typedef unsigned (*ScarletAotAbiFn)(void);
typedef unsigned (*ScarletAotFeaturesFn)(void);
typedef unsigned (*ScarletAotGuardV2Fn)(unsigned host_version);
typedef unsigned (*ScarletAotFastmemV1Fn)(uint32_t page_bits, uint32_t stride_log2,
                                          uint64_t pointer_mask, uint32_t off_table,
                                          uint32_t off_limit);
typedef uint32_t *(*ScarletAotGuardGenV1Fn)(uint32_t host_version, uint64_t *code_lo,
                                            uint64_t *code_end, const uint64_t **base);
typedef unsigned (*ScarletAotFpxV1Fn)(uint32_t off_fpcr, uint32_t off_fpsr,
                                      uint64_t inhibit_bit);
typedef int (*ScarletAotIndexFn)(uint64_t *lo, uint64_t *hi, ScarletAotBlockFn **index);
typedef uint64_t (*ScarletAotEntryFn)(void);
