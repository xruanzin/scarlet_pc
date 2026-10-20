// Host-native scalar libm entry points for hot SDK/libc math helpers.
// The Windows x86-64 CRT/libm implementations are far cheaper than executing
// an AArch64 math routine instruction-by-instruction through lifted C.
#include <math.h>
#include <string.h>

#include "../hle.h"

static inline float fp32_arg(Cpu *c, int i) {
    uint32_t u = (uint32_t)c->v[i];
    float v;
    memcpy(&v, &u, sizeof v);
    return v;
}
static inline double fp64_arg(Cpu *c, int i) {
    uint64_t u = (uint64_t)c->v[i];
    double v;
    memcpy(&v, &u, sizeof v);
    return v;
}
static inline void fp32_ret(Cpu *c, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    c->v[0] = (__uint128_t)u;
}
static inline void fp64_ret(Cpu *c, double v) {
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    c->v[0] = (__uint128_t)u;
}

#define MATH_F1(name) \
    void hle_##name##f(Cpu *c) { fp32_ret(c, name##f(fp32_arg(c, 0))); } \
    void hle_##name(Cpu *c) { fp64_ret(c, name(fp64_arg(c, 0))); }

#define MATH_F2(name) \
    void hle_##name##f(Cpu *c) { fp32_ret(c, name##f(fp32_arg(c, 0), fp32_arg(c, 1))); } \
    void hle_##name(Cpu *c) { fp64_ret(c, name(fp64_arg(c, 0), fp64_arg(c, 1))); }

#define REG_MATH_F(name) HLE_FUNC(#name "f", hle_import_##name##f) { hle_##name##f(c); }
#define REG_MATH_D(name) HLE_FUNC(#name, hle_import_##name) { hle_##name(c); }

MATH_F1(sqrt)
MATH_F1(fabs)
MATH_F1(floor)
MATH_F1(ceil)
MATH_F1(trunc)
MATH_F1(round)
MATH_F1(sin)
MATH_F1(cos)
MATH_F1(tan)
MATH_F1(asin)
MATH_F1(acos)
MATH_F1(atan)
MATH_F1(exp)
MATH_F1(log)

MATH_F2(atan2)
MATH_F2(pow)
MATH_F2(fmod)
MATH_F2(fmin)
MATH_F2(fmax)
MATH_F2(copysign)

// Keep every HLE_FUNC expansion on its own source line: HLE_FUNC uses __LINE__
// to build the static registration identifier.
REG_MATH_F(sqrt)
REG_MATH_D(sqrt)
REG_MATH_F(fabs)
REG_MATH_D(fabs)
REG_MATH_F(floor)
REG_MATH_D(floor)
REG_MATH_F(ceil)
REG_MATH_D(ceil)
REG_MATH_F(trunc)
REG_MATH_D(trunc)
REG_MATH_F(round)
REG_MATH_D(round)
REG_MATH_F(sin)
REG_MATH_D(sin)
REG_MATH_F(cos)
REG_MATH_D(cos)
REG_MATH_F(tan)
REG_MATH_D(tan)
REG_MATH_F(asin)
REG_MATH_D(asin)
REG_MATH_F(acos)
REG_MATH_D(acos)
REG_MATH_F(atan)
REG_MATH_D(atan)
REG_MATH_F(exp)
REG_MATH_D(exp)
REG_MATH_F(log)
REG_MATH_D(log)
REG_MATH_F(atan2)
REG_MATH_D(atan2)
REG_MATH_F(pow)
REG_MATH_D(pow)
REG_MATH_F(fmod)
REG_MATH_D(fmod)
REG_MATH_F(fmin)
REG_MATH_D(fmin)
REG_MATH_F(fmax)
REG_MATH_D(fmax)
REG_MATH_F(copysign)
REG_MATH_D(copysign)
