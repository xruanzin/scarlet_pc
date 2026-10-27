// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "gamesetup.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "ipc.h"
#include "scarlet_native/aot.h"

const char *g_romfs_dir;
uint64_t g_boot_started_ns;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// Nintendo's module DT_INIT calls __nnmusl_init_dso and stores its status in a
// module-local word. If that status is 1, constructors were deliberately left
// for rtld/the loader instead of being run by DT_INIT. Locate that word from
// DT_INIT's initial ADRP ... LDR W8 sequence (SDK 16 uses this same pattern).
static uint64_t module_init_status_addr(Module *m) {
    if (!m->init) return 0;
    uint64_t page[32] = {0};
    for (uint64_t pc = m->init; pc < m->init + 0x40; pc += 4) {
        uint32_t w = *(uint32_t *)(uintptr_t)pc;
        if ((w & 0x9F000000u) == 0x90000000u) {  // ADRP
            uint32_t rd = w & 31u;
            uint64_t immlo = (w >> 29) & 3u;
            uint64_t immhi = (w >> 5) & 0x7FFFFu;
            int64_t imm = (int64_t)((immhi << 2) | immlo);
            if (imm & (1ll << 20)) imm -= 1ll << 21;
            page[rd] = (pc & ~0xFFFull) + (uint64_t)(imm * 0x1000ll);
        } else if ((w & 0xFFC0001Fu) == 0xB9400008u) {  // LDR W8,[Xn,#imm]
            uint32_t rn = (w >> 5) & 31u;
            if (page[rn])
                return page[rn] + (uint64_t)((w >> 10) & 0xFFFu) * 4u;
        }
    }
    return 0;
}

// nn::init::Start invokes the loader init callback after nninitStartup, just
// before nnMain. At that point the game's allocator exists, so initialize every
// module in dependency order: SDK, subsdk(s), then main.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n",
        m->name, (unsigned long long)m->init,
        (unsigned long long)(m->init_array_sz / 8));

    if (!m->init) {
        run_array(c, m->init_array, m->init_array_sz);
        LOG("[boot] %s constructors: %llu (no DT_INIT)\n",
            m->name, (unsigned long long)(m->init_array_sz / 8));
        return;
    }

    guest_call(c, m->init);

    uint64_t status_addr = module_init_status_addr(m);
    if (!status_addr) {
        FATAL("%s: could not locate DT_INIT status word\n", m->name);
    }

    uint32_t status = *(uint32_t *)(uintptr_t)status_addr;
    LOG("[boot] %s DT_INIT status=%u @ %llx\n",
        m->name, status, (unsigned long long)status_addr);

    if (status == 1) {
        run_array(c, m->init_array, m->init_array_sz);
        LOG("[boot] %s constructors run by loader: %llu\n",
            m->name, (unsigned long long)(m->init_array_sz / 8));
    }
}

static void rtld_init_sdk(Cpu *c) {
    for (int i = 1; i < g_nmodules; i++) module_init(c, &g_modules[i]);
    if (g_nmodules > 0) module_init(c, &g_modules[0]);
}

// The second callback is rtld's companion/fini hook, not a second constructor
// phase. Running main constructors here is too late (nnMain is already active).
static void rtld_init_main(Cpu *c) { (void)c; }
static void rtld_mark(Cpu *c) { (void)c; }  // extra callback used by newer nn::init::Start ABIs

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<seed | vanilla | title>[,...] (test): at that presented frame, switch to the seed /
// vanilla in place, or go back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    rt_request_reboot(RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void renderthread_reset(void);          // renderer: drop/drain queued command batches before backend teardown
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);

    // Native gaps are migration blockers, not normal diagnostics. Their counters
    // are touched only on a real gap, so keeping this report enabled has no hot
    // path cost. A clean gameplay run should end with no native-gap lines.
    uint64_t hle_gaps = rt_hle_gap_report();
    uint64_t ipc_gaps = ipc_gap_report();
    if (hle_gaps || ipc_gaps)
        LOG("[native-gap] run total: %llu unresolved native call(s)\n",
            (unsigned long long)(hle_gaps + ipc_gaps));
    scarlet_aot_profile_report();
    rt_hle_profile_report();
    renderthread_reset();
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void game_exefs_path(const char *root, char *out, size_t size) {
    const char *mounted = getenv("LARECOMP_EXEFS_DIR");
    if (mounted && *mounted) snprintf(out, size, "%s", mounted);
    else snprintf(out, size, "%s/exefs", root);
}

static void game_romfs_path(const char *root, char *out, size_t size) {
    const char *mounted = getenv("LARECOMP_ROMFS_DIR");
    if (mounted && *mounted) snprintf(out, size, "%s", mounted);
    else snprintf(out, size, "%s/romfs", root);
}

static void boot(const char *root) {
    g_boot_started_ns = plat_now_ns();
    LOG("[boot-time] boot start\n");
    char exefs[1024], romfs[1024], npdm_path[1024];
    game_exefs_path(root, exefs, sizeof exefs);
    game_romfs_path(root, romfs, sizeof romfs);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);
    if (getenv("LARECOMP_PACKAGE_STREAM"))
        LOG("[package] direct container: ExeFS=%s RomFS=%s image=%s\n", exefs, romfs,
            getenv("LARECOMP_ROMFS_IMAGE") ? getenv("LARECOMP_ROMFS_IMAGE") : "(default)");

    const char *runtime_mode = getenv("SCARLET_RUNTIME");
    bool native_aot = !runtime_mode || !*runtime_mode || !strcmp(runtime_mode, "native");
    bool las_runtime = runtime_mode && !strcmp(runtime_mode, "las");

    char why[512];
    if (native_aot) {
        if (!scarlet_aot_load(root, why, sizeof why)) FATAL("%s\n", why);
        LOG("[runtime] Scarlet Native Runtime (existing AOT ABI)\n");
    } else if (las_runtime) {
        if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
        LOG("[runtime] LAS-generated comparison backend\n");
    } else if (runtime_mode && !strcmp(runtime_mode, "legacy")) {
        FATAL("SCARLET_RUNTIME=legacy is the suyu fallback and must be launched via "
              "tools/scarlet_native_run.ps1; this executable is the native runtime\n");
    } else {
        FATAL("unknown SCARLET_RUNTIME=%s (use native, las, or legacy via the run script)\n",
              runtime_mode ? runtime_mode : "");
    }

    loader_load(exefs);
    if (native_aot) {
        if (!scarlet_aot_bind_loaded_modules(why, sizeof why)) FATAL("%s\n", why);
        rt_set_guest_call_backend(scarlet_aot_guest_call);
        scarlet_aot_log_status();
    } else {
        rt_set_guest_call_backend(NULL);
    }

    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
            uint8_t aci[0x18];  // ACI0 (offset at META+0x70): the program id at +0x10 (svcGetInfo 18)
            if (!fseek(f, *(uint32_t *)(meta + 0x70), SEEK_SET) && fread(aci, 1, sizeof aci, f) == sizeof aci &&
                !memcmp(aci, "ACI0", 4))
                g_program_id = *(uint64_t *)(aci + 0x10);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x, program id %016llx\n", prio, stack_size, (unsigned long long)g_program_id);

    // Nintendo SDKs use two known nn::init::Start ABIs. Older SDKs take
    // (thread, args, init, fini); newer ones add an rtld "mark" callback:
    // (thread, args, mark, init, fini). Scarlet's SDK is newer than the
    // Let's Go baseline, so accept both instead of hardcoding the old ABI.
    const char *start_old = "_ZN2nn4init5StartEmmPFvvES2_";
    const char *start_new = "_ZN2nn4init5StartEmmPFvvES2_S2_";
    uint64_t start = loader_find_symbol(start_old);
    bool modern_start = false;
    if (!start) {
        start = loader_find_symbol(start_new);
        modern_start = start != 0;
    }
    if (!start) {
        loader_log_symbols_prefix("_ZN2nn4init5Start");
        FATAL("nn::init::Start not found (old/new ABI)\n");
    }

    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t mark_fn = hle_register("<rtld_mark>", rtld_mark);
    LOG("[boot] nn::init::Start %s ABI at %llx\n",
        modern_start ? "5-arg" : "4-arg", (unsigned long long)start);

    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    if (modern_start)
        kernel_run_main(t, t->handle, 0, mark_fn, init_fn, fini_fn);
    else
        kernel_run_main(t, t->handle, 0, init_fn, fini_fn, 0);
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    bool skip_launcher = getenv("LARECOMP_NO_LAUNCHER") || getenv("LARECOMP_INPUT");  // tests boot straight in
    static char arg_root[1024];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    for (bool title = !skip_launcher;;) {
        if (title) {  // start screen: resolution, fullscreen, anti-aliasing, translation, keys (runtime/ui/launcher_lg.mm)
            extern void launcher_lg_run(const char *root);
            launcher_lg_run(root);
        }
        {  // (runtime/fnhook.c: the runtime's hooks, in the address tables of the known builds)
            extern void rt_fnhook_set_build(int build);
            char exefs[1100];
            game_exefs_path(root, exefs, sizeof exefs);
            int build = game_build(exefs);
            rt_fnhook_set_build(build);
            extern const char *g_game_version;  // (runtime/services/am.c)
            g_game_version = game_version(exefs);
            if (build == GAME_BUILD_OTHER) LOG("[boot] %s: a build without address tables, 60 fps and wild battles stay off\n", exefs);
        }
        { extern void fps60_configure(void); fps60_configure(); }  // (runtime/hle/fps60.c: the launcher's 60 fps option)
        { extern void wildbattle_configure(void); wildbattle_configure(); }  // (runtime/hle/wildbattle.c: the wild battles option)
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
