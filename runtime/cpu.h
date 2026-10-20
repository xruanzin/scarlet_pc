// Guest CPU state and helpers used by recompiled code.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((aligned(16))) Cpu {
    uint64_t x[32];          // x0-x30 (x[31] unused; sp is separate)
    uint64_t sp;             // 256
    uint64_t pc;             // 264: target for indirect dispatch
    uint64_t nzcv;           // 272: flags in bits 31..28
    uint64_t tpidr;          // 280
    uint64_t tpidrro;        // 288
    uint64_t excl_addr;      // 296
    uint64_t excl_val;       // 304
    uint64_t excl_val2;      // 312: second lane for LDXP/STXP
    void *thread;            // 320: runtime thread object
    uint64_t fpcr;           // 328: guest FPCR/FPSR (portable FP/SIMD lowering only; the asm path uses the host's)
    uint64_t fpsr;           // 336
    uint8_t _pad[512 - 344];
    __uint128_t v[32];       // 512
} Cpu;

_Static_assert(offsetof(Cpu, sp) == 256, "sp");
_Static_assert(offsetof(Cpu, pc) == 264, "pc");
_Static_assert(offsetof(Cpu, nzcv) == 272, "nzcv");
_Static_assert(offsetof(Cpu, fpcr) == 328, "fpcr");
_Static_assert(offsetof(Cpu, fpsr) == 336, "fpsr");
_Static_assert(offsetof(Cpu, v) == 512, "v");

typedef void (*GuestFn)(Cpu *);

// ---- runtime services (runtime/*.c)
void rt_dispatch(Cpu *c);                 // call function at c->pc
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) __attribute__((noreturn));
void rt_udf(Cpu *c, uint64_t pc) __attribute__((noreturn));
uint64_t rt_ticks(void);

#if defined(__aarch64__)
static inline uint64_t rt_get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline uint64_t rt_get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void rt_set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline void rt_set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }
#else
// Host FP state is not the guest's here: code lifted with --simd=c keeps FPCR/FPSR in Cpu (c->fpcr, zeroed
// with the Cpu), so the host-side reset in thread start-up has nothing to do.
static inline uint64_t rt_get_fpcr(void) { return 0; }
static inline uint64_t rt_get_fpsr(void) { return 0; }
static inline void rt_set_fpcr(uint64_t v) { (void)v; }
static inline void rt_set_fpsr(uint64_t v) { (void)v; }
#endif

// ---- memory: guest addresses are host addresses
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint8_t __attribute__((may_alias)) u8a;

// Function-entry profiler (runtime/fnprof.c, LARECOMP_FNPROF): one predictable branch per call when off.
// Function hooks (runtime/fnhook.c): while any is registered, rt_fnhook may handle the call (returns 1: the guest
// function returns at once, with the registers the hook left).
enum { FN_FLAG_PROF = 1u, FN_FLAG_HOOK = 2u };
extern volatile unsigned g_fn_enter_flags;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
int rt_fnhook(Cpu *c, uint64_t addr);
#define FN_ENTER(a) do {                                                                 \
        /* Scarlet can enter ~180k lifted functions per frame in crowded scenes.          \
           Keep diagnostics at one relaxed flag load/branch when both profilers/hooks      \
           are off instead of two volatile global loads on every guest call. */            \
        unsigned _ff = __atomic_load_n(&g_fn_enter_flags, __ATOMIC_RELAXED);               \
        if (__builtin_expect(_ff != 0, 0)) {                                               \
            if (_ff & FN_FLAG_PROF) fnprof_hit(a, c->x[30], c->x[0]);                     \
            if ((_ff & FN_FLAG_HOOK) && rt_fnhook(c, a)) return;                          \
        }                                                                                  \
    } while (0)

#define LD8(a)  (*(const u8a *)(uintptr_t)(a))
#define LD16(a) (*(const u16u *)(uintptr_t)(a))
#define LD32(a) (*(const u32u *)(uintptr_t)(a))
#define LD64(a) (*(const u64u *)(uintptr_t)(a))
#define ST8(a, v)  (*(u8a *)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(u16u *)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(u32u *)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(u64u *)(uintptr_t)(a) = (uint64_t)(v))
// Research build (gen.py --trace-stores, -DSTORE_TRACE): the generated stores are ST<N>T(<guest pc>, a, v) and report
// (pc, address, old, new) to the runtime (runtime/storetrace.c); otherwise exactly ST<N>.
#ifdef STORE_TRACE
void rt_trace_store(uint64_t pc, uint64_t addr, uint64_t old, uint64_t nw, int size);
#define ST_TRACED(T, pc, a, v, N) do { T _o = *(T *)(uintptr_t)(a); T _n = (T)(v); *(T *)(uintptr_t)(a) = _n; if (_o != _n) rt_trace_store((pc), (uint64_t)(a), _o, _n, N); } while (0)
#define ST8T(pc, a, v)  ST_TRACED(uint8_t, pc, a, v, 1)
#define ST16T(pc, a, v) ST_TRACED(uint16_t, pc, a, v, 2)
#define ST32T(pc, a, v) ST_TRACED(uint32_t, pc, a, v, 4)
#define ST64T(pc, a, v) ST_TRACED(uint64_t, pc, a, v, 8)
#else
#define ST8T(pc, a, v)  ST8(a, v)
#define ST16T(pc, a, v) ST16(a, v)
#define ST32T(pc, a, v) ST32(a, v)
#define ST64T(pc, a, v) ST64(a, v)
#endif

// ---- exclusive monitor (emulated with compare-and-swap)
#define EXCL(N, T)                                                                         \
    static inline uint64_t ldx##N(Cpu *c, uint64_t a) {                                    \
        T v = __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                        \
        c->excl_addr = a;                                                                  \
        c->excl_val = v;                                                                   \
        return v;                                                                          \
    }                                                                                      \
    static inline uint32_t stx##N(Cpu *c, uint64_t a, uint64_t v) {                        \
        if (c->excl_addr != a) return 1;                                                   \
        c->excl_addr = ~0ull;                                                              \
        T expected = (T)c->excl_val;                                                       \
        return !__atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, (T)v, 0,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);           \
    }                                                                                      \
    static inline uint64_t lda##N(uint64_t a) {                                            \
        return __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                       \
    }                                                                                      \
    static inline void stl##N(uint64_t a, uint64_t v) {                                    \
        __atomic_store_n((T *)(uintptr_t)a, (T)v, __ATOMIC_RELEASE);                       \
    }
EXCL(8, uint8_t)
EXCL(16, uint16_t)
EXCL(32, uint32_t)
EXCL(64, uint64_t)

// ---- exclusive pairs and LSE compare-and-swap
//
// These helpers intentionally use stronger host memory ordering (SEQ_CST) than
// some A64 variants require. That is conservative for correctness and keeps the
// generated code independent of the host architecture's weaker ordering.
//
// Pair-exclusive accesses are naturally aligned by valid A64 code. x86-64-v3
// (the generated-code target on Windows) includes CMPXCHG16B, so Clang can
// lower the 128-bit compare/exchange used by the X-register pair directly.
static inline uint64_t ldxp_pair32(Cpu *c, uint64_t a) {
    uint64_t v = __atomic_load_n((uint64_t *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    c->excl_addr = a;
    c->excl_val = (uint32_t)v;
    c->excl_val2 = (uint32_t)(v >> 32);
    return v;
}
static inline uint32_t stxp_pair32(Cpu *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull;
    uint64_t expected = (uint32_t)c->excl_val | ((uint64_t)(uint32_t)c->excl_val2 << 32);
    uint64_t desired = (uint32_t)lo | ((uint64_t)(uint32_t)hi << 32);
    return !__atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &expected, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline __uint128_t ldxp_pair64(Cpu *c, uint64_t a) {
    __uint128_t v = __atomic_load_n((__uint128_t *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    c->excl_addr = a;
    c->excl_val = (uint64_t)v;
    c->excl_val2 = (uint64_t)(v >> 64);
    return v;
}
static inline uint32_t stxp_pair64(Cpu *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull;
    __uint128_t expected = (__uint128_t)c->excl_val | ((__uint128_t)c->excl_val2 << 64);
    __uint128_t desired = (__uint128_t)lo | ((__uint128_t)hi << 64);
    return !__atomic_compare_exchange_n((__uint128_t *)(uintptr_t)a, &expected, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#define CAS_SINGLE(N, T)                                                                    \
    static inline uint64_t cas##N(uint64_t a, uint64_t expected, uint64_t desired) {         \
        T e = (T)expected;                                                                   \
        __atomic_compare_exchange_n((T *)(uintptr_t)a, &e, (T)desired, 0,                   \
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);                     \
        return e;                                                                            \
    }
CAS_SINGLE(8, uint8_t)
CAS_SINGLE(16, uint16_t)
CAS_SINGLE(32, uint32_t)
CAS_SINGLE(64, uint64_t)

static inline uint64_t casp_pair32(uint64_t a, uint64_t exp0, uint64_t exp1,
                                   uint64_t new0, uint64_t new1) {
    uint64_t expected = (uint32_t)exp0 | ((uint64_t)(uint32_t)exp1 << 32);
    uint64_t desired = (uint32_t)new0 | ((uint64_t)(uint32_t)new1 << 32);
    __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &expected, desired, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline __uint128_t casp_pair64(uint64_t a, uint64_t exp0, uint64_t exp1,
                                     uint64_t new0, uint64_t new1) {
    __uint128_t expected = (__uint128_t)exp0 | ((__uint128_t)exp1 << 64);
    __uint128_t desired = (__uint128_t)new0 | ((__uint128_t)new1 << 64);
    __atomic_compare_exchange_n((__uint128_t *)(uintptr_t)a, &expected, desired, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

// ---- flags (packed NZCV in bits 31..28)
#define FLAG_N(c) (((c)->nzcv >> 31) & 1)
#define FLAG_Z(c) (((c)->nzcv >> 30) & 1)
#define FLAG_C(c) (((c)->nzcv >> 29) & 1)
#define FLAG_V(c) (((c)->nzcv >> 28) & 1)
#define COND_EQ(c) (FLAG_Z(c))
#define COND_NE(c) (!FLAG_Z(c))
#define COND_CS(c) (FLAG_C(c))
#define COND_CC(c) (!FLAG_C(c))
#define COND_MI(c) (FLAG_N(c))
#define COND_PL(c) (!FLAG_N(c))
#define COND_VS(c) (FLAG_V(c))
#define COND_VC(c) (!FLAG_V(c))
#define COND_HI(c) (FLAG_C(c) && !FLAG_Z(c))
#define COND_LS(c) (!(FLAG_C(c) && !FLAG_Z(c)))
#define COND_GE(c) (FLAG_N(c) == FLAG_V(c))
#define COND_LT(c) (FLAG_N(c) != FLAG_V(c))
#define COND_GT(c) (!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c))
#define COND_LE(c) (!(!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c)))

#define MKFLAGS(n, z, cy, v) \
    (((uint64_t)(n) << 31) | ((uint64_t)(z) << 30) | ((uint64_t)(cy) << 29) | ((uint64_t)(v) << 28))

static inline uint64_t adds64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint64_t subs64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}
static inline uint32_t subs32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 31);
    return r;
}
static inline uint64_t logic64(Cpu *c, uint64_t r) { c->nzcv = MKFLAGS(r >> 63, r == 0, 0, 0); return r; }
static inline uint32_t logic32(Cpu *c, uint32_t r) { c->nzcv = MKFLAGS(r >> 31, r == 0, 0, 0); return r; }

#define ADC_IMPL(N, S)                                                                     \
    static inline uint##N##_t adc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a + b + (uint##N##_t)FLAG_C(c);                                             \
    }                                                                                      \
    static inline uint##N##_t sbc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a - b - 1 + (uint##N##_t)FLAG_C(c);                                         \
    }                                                                                      \
    static inline uint##N##_t adcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        uint##N##_t cin = FLAG_C(c), r = a + b + cin;                                      \
        int carry = cin ? r <= a : r < a;                                                  \
        c->nzcv = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));    \
        return r;                                                                          \
    }                                                                                      \
    static inline uint##N##_t sbcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        return adcs##N(c, a, (uint##N##_t)~b);                                             \
    }
ADC_IMPL(64, )
ADC_IMPL(32, )

// ---- bit ops
#define ROR64(v, r) ((uint64_t)(v) >> ((r) & 63) | (uint64_t)(v) << ((64 - ((r) & 63)) & 63))
#define ROR32(v, r) ((uint32_t)(v) >> ((r) & 31) | (uint32_t)(v) << ((32 - ((r) & 31)) & 31))
static inline uint64_t clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }
static inline uint64_t rbit64(uint64_t v) { return __builtin_bitreverse64(v); }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull);
}
static inline uint64_t rev32_64(uint64_t v) {
    return ((uint64_t)__builtin_bswap32((uint32_t)v)) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32);
}
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if ((int64_t)a == INT64_MIN && (int64_t)b == -1) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

// Portable FP/SIMD helpers for code lifted with --simd=c (default off AArch64; gen.py includes simd.h itself
// in that mode, so AArch64 hosts can build it too).
#if !defined(__cplusplus) && !defined(__aarch64__)
#include "simd.h"
#endif
