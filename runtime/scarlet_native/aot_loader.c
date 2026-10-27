// Loader for the existing ArmRecomp AOT images.
//
// The native runtime deliberately consumes the generated-image ABI directly.
// It does not include or link any suyu core/kernel/video headers.
#include "aot.h"
#include "aot_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../paths.h"
#include "../rt.h"

static ScarletAotModule g_aot[SCARLET_AOT_MODULE_COUNT] = {
    {.name = "rtld",       .filename = "scarlet-rtld.dll",       .guest_module = NULL},
    {.name = "main",       .filename = "scarlet-main.dll",       .guest_module = "main"},
    {.name = "multimedia", .filename = "scarlet-multimedia.dll", .guest_module = "subsdk0"},
    {.name = "sdk",        .filename = "scarlet-sdk.dll",        .guest_module = "sdk"},
};
static bool g_loaded;

ScarletAotModule *scarlet_aot_modules(void) { return g_aot; }

static void set_error(char *why, size_t n, const char *fmt, const char *a) {
    if (why && n) snprintf(why, n, fmt, a ? a : "");
}

#ifdef _WIN32

static FARPROC symbol(ScarletAotLibrary lib, const char *name) {
    return GetProcAddress(lib, name);
}

static bool file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool find_image(const char *game_root, const char *filename, char *out, size_t n) {
    const char *dir = getenv("SCARLET_AOT_DIR");
    if (dir && *dir) {
        snprintf(out, n, "%s/%s", dir, filename);
        if (file_exists(out)) return true;
    }

    char rel[256];
    snprintf(rel, sizeof rel, "aot/%s", filename);
    if (rt_resource(rel, NULL, out, n) && file_exists(out)) return true;

    if (game_root && *game_root) {
        snprintf(out, n, "%s/aot/%s", game_root, filename);
        if (file_exists(out)) return true;
    }

    snprintf(out, n, "aot/%s", filename);
    return file_exists(out);
}

#define LOAD_REQUIRED(m, field, type, symname) do {                                      \
    (m)->field = (type)(void *)symbol((m)->library, (symname));                           \
    if (!(m)->field) {                                                                     \
        snprintf(why, why_size, "%s: missing AOT export %s", (m)->filename, (symname));   \
        return false;                                                                      \
    }                                                                                      \
} while (0)

#define LOAD_OPTIONAL(m, field, type, symname) \
    ((m)->field = (type)(void *)symbol((m)->library, (symname)))

static bool load_one(ScarletAotModule *m, const char *game_root, char *why, size_t why_size) {
    char path[1200];
    if (!find_image(game_root, m->filename, path, sizeof path)) {
        snprintf(why, why_size, "AOT image not found: %s (set SCARLET_AOT_DIR or place it in aot/)",
                 m->filename);
        return false;
    }

    m->library = LoadLibraryA(path);
    if (!m->library) {
        snprintf(why, why_size, "cannot load %s (Win32 error %lu)",
                 path, (unsigned long)GetLastError());
        return false;
    }

    LOAD_REQUIRED(m, lookup, ScarletAotLookupFn, "recomp_image_lookup");
    LOAD_REQUIRED(m, set_base, ScarletAotSetBaseFn, "recomp_image_set_base");
    LOAD_REQUIRED(m, run_slice, ScarletAotRunSliceFn, "recomp_image_run_slice");
    LOAD_REQUIRED(m, abi_fn, ScarletAotAbiFn, "recomp_image_abi");
    LOAD_REQUIRED(m, guard_v2, ScarletAotGuardV2Fn, "recomp_image_guard_v2");
    LOAD_REQUIRED(m, index_fn, ScarletAotIndexFn, "recomp_image_index");
    LOAD_REQUIRED(m, entry_fn, ScarletAotEntryFn, "recomp_image_entry");

    m->abi = m->abi_fn();
    if (m->abi < SCARLET_AOT_ABI_MIN || m->abi > SCARLET_AOT_ABI_MAX) {
        snprintf(why, why_size, "%s: unsupported AOT ABI %u", m->filename, m->abi);
        return false;
    }

    if (m->guard_v2(2) != 2) {
        snprintf(why, why_size, "%s: guard-v2 handshake failed", m->filename);
        return false;
    }

    LOAD_OPTIONAL(m, features_fn, ScarletAotFeaturesFn, "recomp_image_features");
    m->features = m->features_fn ? m->features_fn() : 0;
    if (m->features & ~SCARLET_AOT_FEATURE_SUPPORTED) {
        snprintf(why, why_size, "%s: unsupported AOT feature bits 0x%x",
                 m->filename, m->features & ~SCARLET_AOT_FEATURE_SUPPORTED);
        return false;
    }

    if (m->features & SCARLET_AOT_FEATURE_FASTMEM_PT1) {
        LOAD_REQUIRED(m, fastmem_v1, ScarletAotFastmemV1Fn, "recomp_image_fastmem_v1");
        // The current native address space is identity-mapped. We deliberately
        // leave GuestContext.fm_limit=0 instead of fabricating suyu's page-entry
        // array, so the image takes its ABI5-compatible direct flat-memory path.
        // The handshake still proves that the shared context layout matches.
        if (!m->fastmem_v1(12, 5, ~(uint64_t)3,
                           (uint32_t)offsetof(ScarletAotGuestContext, fm_table),
                           (uint32_t)offsetof(ScarletAotGuestContext, fm_limit))) {
            snprintf(why, why_size, "%s: FM1 layout handshake failed", m->filename);
            return false;
        }
    }

    if (m->features & SCARLET_AOT_FEATURE_GUARD_GEN1) {
        LOAD_REQUIRED(m, guard_gen_v1, ScarletAotGuardGenV1Fn, "recomp_image_guard_gen_v1");
        uint64_t lo = 0, hi = 0;
        const uint64_t *base = NULL;
        m->guard_generation = m->guard_gen_v1(2, &lo, &hi, &base);
        if (!m->guard_generation) {
            snprintf(why, why_size, "%s: GG1 handshake failed", m->filename);
            return false;
        }
        // Keep VERIFY_ALWAYS until the native loader has its own code-page
        // generation tracking. This preserves correctness without importing
        // suyu's page table or invalidation machinery.
    }

    if (m->features & SCARLET_AOT_FEATURE_FPX1) {
        LOAD_REQUIRED(m, fpx_v1, ScarletAotFpxV1Fn, "recomp_image_fpx_v1");
        if (!m->fpx_v1((uint32_t)offsetof(ScarletAotGuestContext, fpcr),
                       (uint32_t)offsetof(ScarletAotGuestContext, fpsr),
                       SCARLET_AOT_FPX_INHIBIT)) {
            snprintf(why, why_size, "%s: FPX1 layout handshake failed", m->filename);
            return false;
        }
    }

    LOG("[native-aot] loaded %-10s ABI %u features=0x%x entry=%llx from %s\n",
        m->name, m->abi, m->features,
        (unsigned long long)m->entry_fn(), path);
    return true;
}

#else

static bool load_one(ScarletAotModule *m, const char *game_root, char *why, size_t why_size) {
    (void)m; (void)game_root;
    set_error(why, why_size, "%s", "ArmRecomp AOT DLL loading is currently supported on Windows only");
    return false;
}

#endif

bool scarlet_aot_load(const char *game_root, char *why, size_t why_size) {
    if (g_loaded) return true;
    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i)
        if (!load_one(&g_aot[i], game_root, why, why_size)) return false;
    g_loaded = true;
    return true;
}

static Module *guest_module(const char *name) {
    for (int i = 0; i < g_nmodules; ++i)
        if (g_modules[i].name && !strcmp(g_modules[i].name, name)) return &g_modules[i];
    return NULL;
}

bool scarlet_aot_bind_loaded_modules(char *why, size_t why_size) {
    if (!g_loaded) {
        set_error(why, why_size, "%s", "AOT modules have not been loaded");
        return false;
    }

    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i) {
        ScarletAotModule *a = &g_aot[i];
        a->bound = false;
        a->guest_base = a->guest_size = 0;
        a->guest_text_lo = a->guest_text_hi = 0;
        a->index_lo = a->index_hi = 0;
        a->index = NULL;

        if (!a->guest_module) {
            LOG("[native-aot] rtld resident; native NSO loader owns relocations/TLS bootstrap\n");
            continue;
        }

        Module *m = guest_module(a->guest_module);
        if (!m) {
            snprintf(why, why_size, "%s: guest module %s was not loaded",
                     a->filename, a->guest_module);
            return false;
        }

        a->guest_base = m->base;
        a->guest_size = m->size;
        a->guest_text_lo = m->base + m->text_off;
        a->guest_text_hi = a->guest_text_lo + m->text_size;
        a->set_base(m->base);

        uint64_t lo = 0, hi = 0;
        ScarletAotBlockFn *index = NULL;
        if (!a->index_fn(&lo, &hi, &index) || !index || hi <= lo) {
            snprintf(why, why_size, "%s: direct AOT index unavailable after set_base",
                     a->filename);
            return false;
        }
        a->index_lo = lo;
        a->index_hi = hi;
        a->index = index;
        a->bound = true;

        // GG1 starts in VERIFY_ALWAYS after negotiation. Native AOT stores all
        // pass through aot_execute.c, so we can move to a real generation and
        // bump it whenever guest code writes an NSO text range. Each block then
        // verifies once per generation instead of re-reading its instructions
        // on every entry.
        if (a->guard_generation)
            __atomic_store_n(a->guard_generation, 1u, __ATOMIC_RELEASE);

        LOG("[native-aot] bound %-10s guest=%s base=%llx size=%llx index=[%llx,%llx)\n",
            a->name, a->guest_module, (unsigned long long)a->guest_base,
            (unsigned long long)a->guest_size, (unsigned long long)lo,
            (unsigned long long)hi);
    }
    return true;
}

ScarletAotModule *scarlet_aot_find_pc(uint64_t pc) {
    // Four modules only. The range test is cheaper than hashing and the direct
    // block index gives O(1) exact-target validation.
    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i) {
        ScarletAotModule *m = &g_aot[i];
        if (!m->bound || pc < m->guest_base || pc >= m->guest_base + m->guest_size)
            continue;
        if (pc >= m->index_lo && pc < m->index_hi && !(pc & 3)) {
            size_t slot = (size_t)((pc - m->index_lo) >> 2);
            if (m->index[slot]) return m;
        }
        if (m->lookup(pc)) return m;
    }
    return NULL;
}

bool scarlet_aot_ready(void) {
    if (!g_loaded) return false;
    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i)
        if (g_aot[i].guest_module && !g_aot[i].bound) return false;
    return true;
}

void scarlet_aot_log_status(void) {
    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i) {
        const ScarletAotModule *m = &g_aot[i];
        LOG("[native-aot] %-10s loaded=%d bound=%d ABI=%u features=0x%x base=%llx\n",
            m->name, m->library != 0, m->bound, m->abi, m->features,
            (unsigned long long)m->guest_base);
    }
}

void scarlet_aot_note_store(uint64_t address, uint32_t size) {
    if (!size) return;
    uint64_t end = address + size;
    if (end < address) end = UINT64_MAX;
    for (int i = 0; i < SCARLET_AOT_MODULE_COUNT; ++i) {
        ScarletAotModule *m = &g_aot[i];
        if (!m->bound || !m->guard_generation || !m->guest_text_hi) continue;
        if (end <= m->guest_text_lo || address >= m->guest_text_hi) continue;

        uint32_t old = __atomic_load_n(m->guard_generation, __ATOMIC_RELAXED);
        uint32_t next = (old == 0 || old >= 0xfffffffeu) ? 1u : old + 1u;
        __atomic_store_n(m->guard_generation, next, __ATOMIC_RELEASE);
    }
}
