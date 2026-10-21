// Scarlet 1.0.0 hot guest leaf functions replaced with direct host C.
//
// These addresses were selected from the slow-scene function-entry profile and
// verified against the exact main Build ID in tools/gen.py. They contain no
// callbacks, allocation or hidden side effects: just loads/arithmetic/constant
// returns. Keeping them here avoids lifted-fragment setup and dispatcher hops.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void hle_scarlet_hot_vec_d0_size(Cpu *c) {
    uint64_t impl = LD64(c->x[0] + 8);
    uint64_t begin = LD64(impl);
    uint64_t end = LD64(impl + 8);
    c->x[0] = (uint32_t)((end - begin) / 0xD0ull);
}

void hle_scarlet_hot_slot_c4(Cpu *c) {
    uint64_t impl = LD64(c->x[0] + 8);
    uint64_t base = LD64(impl);
    int64_t index = (int16_t)c->x[1];
    c->x[0] = LD32(base + index * 0xD0ll + 0xC4);
}

void hle_scarlet_hot_false(Cpu *c) {
    c->x[0] = 0;
}


// -----------------------------------------------------------------------------
// REA + host-RIP validated runtime-only hot paths.
//
// Unlike hle_override(), these patches also catch direct C calls already baked
// into the frozen lasgame.dll. On Windows we keep the original lifted function
// intact, patch its function-table slot, and rewrite every x86 CALL/JMP rel32
// in the DLL that targets the original function to a nearby absolute-jump
// thunk. Slow/edge paths can therefore call the untouched original body with
// no lasgame.dll rebuild.

#include "../gamelib.h"

#ifdef _WIN32
#include <windows.h>
#include <limits.h>

#define SCARLET_VEC_PUSH32  0x70009c184cull
#define SCARLET_REF_ASSIGN  0x700270c4acull

static GuestFn g_orig_vec_push32;
static GuestFn g_orig_ref_assign;
static _Atomic int g_vec_push32_hit;
static _Atomic int g_refassign_hit;

static inline bool scarlet_guest_range(uint64_t p, uint64_t size) {
    return p >= GUEST_LO && p <= GUEST_HI && size <= GUEST_HI - p;
}

static void scarlet_vec_push32(Cpu *c) {
    uint64_t vec = c->x[0], src = c->x[1];
    if (__builtin_expect(!scarlet_guest_range(vec, 0x28) || !scarlet_guest_range(src, 0x20), 0)) {
        g_orig_vec_push32(c);
        return;
    }

    uint64_t begin = LD64(vec + 0x00);
    uint64_t end   = LD64(vec + 0x08);
    uint64_t cap   = LD64(vec + 0x10);

    // REA 0x70009c184c fast branch: the element is a 32-byte movable object.
    // Keep the expensive lifted allocator/growth/destructor path untouched.
    if (__builtin_expect(begin <= end && end < cap &&
                         scarlet_guest_range(end, 0x20) &&
                         ((end - begin) & 0x1f) == 0, 1)) {
        uint64_t q0 = LD64(src + 0x00);
        uint64_t q1 = LD64(src + 0x08);
        uint64_t q2 = LD64(src + 0x10);
        uint64_t q3 = LD64(src + 0x18);

        // Exact move performed by the guest fast path: ownership leaves src.
        ST64(src + 0x00, 0);
        ST64(end + 0x00, q0);
        ST64(end + 0x08, q1);
        ST64(end + 0x10, q2);
        ST64(end + 0x18, q3);
        ST64(vec + 0x08, end + 0x20);

        c->x[0] = 1;
        c->x[1] = (end - begin) >> 5;

        if (__builtin_expect(!atomic_load_explicit(&g_vec_push32_hit, memory_order_relaxed), 0) &&
            !atomic_exchange_explicit(&g_vec_push32_hit, 1, memory_order_relaxed))
            LOG("[scarlet-hot] 70009c184c direct-DLL fast path active\n");
        return;
    }

    g_orig_vec_push32(c);
}

static void scarlet_ref_assign(Cpu *c) {
    uint64_t owner = c->x[0], source = c->x[1];
    if (__builtin_expect(!scarlet_guest_range(owner, 0x40) ||
                         !scarlet_guest_range(source, 8), 0)) {
        g_orig_ref_assign(c);
        return;
    }

    uint64_t next = LD64(source);
    int32_t *next_rc = NULL;
    if (next) {
        if (__builtin_expect(!scarlet_guest_range(next, 0x2c), 0)) {
            g_orig_ref_assign(c);
            return;
        }
        next_rc = (int32_t *)(uintptr_t)(next + 0x28);
        int32_t before = __atomic_fetch_add(next_rc, 1, __ATOMIC_ACQ_REL);
        if (__builtin_expect(before == -1, 0)) {
            __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            g_orig_ref_assign(c);
            return;
        }
    }

    uint64_t old = LD64(owner + 0x38);
    ST64(owner + 0x38, next);

    if (old) {
        if (__builtin_expect(!scarlet_guest_range(old, 0x2c), 0)) {
            ST64(owner + 0x38, old);
            if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            g_orig_ref_assign(c);
            return;
        }

        int32_t *old_rc = (int32_t *)(uintptr_t)(old + 0x28);
        int32_t before = __atomic_fetch_sub(old_rc, 1, __ATOMIC_ACQ_REL);
        int32_t after = before - 1;
        if (__builtin_expect(before == 0, 0)) {
            __atomic_fetch_add(old_rc, 1, __ATOMIC_ACQ_REL);
            ST64(owner + 0x38, old);
            if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
            g_orig_ref_assign(c);
            return;
        }

        if (after == 0) {
            uint64_t vtbl = LD64(old);
            if (__builtin_expect(!scarlet_guest_range(vtbl, 0x18), 0)) {
                __atomic_fetch_add(old_rc, 1, __ATOMIC_ACQ_REL);
                ST64(owner + 0x38, old);
                if (next_rc) __atomic_fetch_sub(next_rc, 1, __ATOMIC_ACQ_REL);
                g_orig_ref_assign(c);
                return;
            }
            c->x[0] = old;
            c->pc = LD64(vtbl + 0x10);
            rt_dispatch(c);  // original ends in BR x1, preserving x30
            return;
        }
    }

    if (__builtin_expect(!atomic_load_explicit(&g_refassign_hit, memory_order_relaxed), 0) &&
        !atomic_exchange_explicit(&g_refassign_hit, 1, memory_order_relaxed))
        LOG("[scarlet-hot] 700270c4ac direct-DLL fast path active\n");
}

static GuestFn *scarlet_guest_slot(uint64_t addr) {
    const GameLibTables *t = gamelib_tables();
    for (uint32_t m = 0; t && m < t->nmods; m++) {
        const GameLibModule *md = &t->mods[m];
        uint64_t lo = 0, hi = *md->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (md->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *md->count && md->addrs[lo] == addr)
            return (GuestFn *)(uintptr_t)&md->fns[lo];
    }
    return NULL;
}

static void *scarlet_alloc_near(void *target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = (uintptr_t)si.dwAllocationGranularity;
    uintptr_t t = (uintptr_t)target;
    uintptr_t radius = 0x70000000ull;
    uintptr_t lo = t > radius ? t - radius : gran;
    uintptr_t hi = t <= UINTPTR_MAX - radius ? t + radius : UINTPTR_MAX;
    lo = (lo + gran - 1) & ~(gran - 1);

    for (uintptr_t p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t next = base + mbi.RegionSize;
        if (next <= p) break;

        if (mbi.State == MEM_FREE) {
            uintptr_t cand = (base + gran - 1) & ~(gran - 1);
            if (cand >= lo && cand + 0x1000 > cand && cand + 0x1000 <= next && cand < hi) {
                void *mem = VirtualAlloc((void *)cand, 0x1000,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (mem) return mem;
            }
        }
        p = next;
    }
    return NULL;
}

static void *scarlet_make_thunk(void *near_to, GuestFn helper) {
    uint8_t *p = (uint8_t *)scarlet_alloc_near(near_to);
    if (!p) return NULL;

    // mov rax, imm64 ; jmp rax
    p[0] = 0x48; p[1] = 0xB8;
    uint64_t dst = (uint64_t)(uintptr_t)helper;
    memcpy(p + 2, &dst, sizeof dst);
    p[10] = 0xFF; p[11] = 0xE0;

    DWORD old;
    if (!VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old)) return NULL;
    FlushInstructionCache(GetCurrentProcess(), p, 12);
    return p;
}

static int scarlet_patch_direct_refs(GuestFn original, GuestFn helper) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)original, &mbi, sizeof mbi)) return -1;
    uint8_t *image = (uint8_t *)mbi.AllocationBase;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return -1;

    uint8_t *thunk = (uint8_t *)scarlet_make_thunk(image, helper);
    if (!thunk) return -1;

    int patched = 0;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *beg = image + sec[s].VirtualAddress;
        size_t n = sec[s].Misc.VirtualSize;
        if (n < 5) continue;

        for (size_t i = 0; i + 5 <= n; i++) {
            uint8_t op = beg[i];
            if (op != 0xE8 && op != 0xE9) continue;  // CALL/JMP rel32
            int32_t old_rel;
            memcpy(&old_rel, beg + i + 1, sizeof old_rel);
            uint8_t *old_dst = beg + i + 5 + old_rel;
            if (old_dst != (uint8_t *)(uintptr_t)original) continue;

            intptr_t delta = thunk - (beg + i + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) continue;
            int32_t new_rel = (int32_t)delta;

            DWORD old_prot;
            if (!VirtualProtect(beg + i, 5, PAGE_EXECUTE_READWRITE, &old_prot))
                continue;
            memcpy(beg + i + 1, &new_rel, sizeof new_rel);
            FlushInstructionCache(GetCurrentProcess(), beg + i, 5);
            DWORD ignored;
            VirtualProtect(beg + i, 5, old_prot, &ignored);
            patched++;
            i += 4;
        }
    }
    return patched;
}

static bool scarlet_patch_guest(uint64_t addr, GuestFn helper, GuestFn *original_out) {
    GuestFn *slot = scarlet_guest_slot(addr);
    if (!slot || !*slot) {
        LOG("[scarlet-hot] %llx: lifted function slot not found\n",
            (unsigned long long)addr);
        return false;
    }

    GuestFn original = *slot;
    int direct = scarlet_patch_direct_refs(original, helper);
    if (direct < 0) {
        LOG("[scarlet-hot] %llx: direct-call patch setup failed\n",
            (unsigned long long)addr);
        return false;
    }

    DWORD old_prot;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old_prot)) {
        LOG("[scarlet-hot] %llx: function-table VirtualProtect failed (%lu)\n",
            (unsigned long long)addr, GetLastError());
        return false;
    }
    *slot = helper;
    DWORD ignored;
    VirtualProtect(slot, sizeof *slot, old_prot, &ignored);

    *original_out = original;
    LOG("[scarlet-hot] %llx patched: %d direct CALL/JMP sites + function table\n",
        (unsigned long long)addr, direct);
    return true;
}
#endif

void scarlet_hot_configure(void) {
    static bool configured;
    if (configured) return;
    configured = true;

    const char *e = getenv("LARECOMP_SCARLET_HOT");
    if (e && (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "false"))) {
        LOG("[scarlet-hot] disabled by LARECOMP_SCARLET_HOT=%s\n", e);
        return;
    }

#ifdef _WIN32
    scarlet_patch_guest(SCARLET_VEC_PUSH32, scarlet_vec_push32, &g_orig_vec_push32);
    scarlet_patch_guest(SCARLET_REF_ASSIGN, scarlet_ref_assign, &g_orig_ref_assign);
#else
    LOG("[scarlet-hot] direct DLL call-site patching is Windows-only\n");
#endif
}
