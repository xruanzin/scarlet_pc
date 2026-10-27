#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aot_abi.h"

#ifdef _WIN32
#include <windows.h>
typedef HMODULE ScarletAotLibrary;
#else
typedef void *ScarletAotLibrary;
#endif

typedef struct ScarletAotModule {
    const char *name;
    const char *filename;
    const char *guest_module;
    ScarletAotLibrary library;

    ScarletAotLookupFn lookup;
    ScarletAotSetBaseFn set_base;
    ScarletAotRunSliceFn run_slice;
    ScarletAotAbiFn abi_fn;
    ScarletAotFeaturesFn features_fn;
    ScarletAotGuardV2Fn guard_v2;
    ScarletAotFastmemV1Fn fastmem_v1;
    ScarletAotGuardGenV1Fn guard_gen_v1;
    ScarletAotFpxV1Fn fpx_v1;
    ScarletAotIndexFn index_fn;
    ScarletAotEntryFn entry_fn;

    unsigned abi;
    unsigned features;
    uint64_t guest_base;
    uint64_t guest_size;
    uint64_t index_lo;
    uint64_t index_hi;
    ScarletAotBlockFn *index;
    uint32_t *guard_generation;
    bool bound;
} ScarletAotModule;

enum { SCARLET_AOT_MODULE_COUNT = 4 };

ScarletAotModule *scarlet_aot_modules(void);
ScarletAotModule *scarlet_aot_find_pc(uint64_t pc);
