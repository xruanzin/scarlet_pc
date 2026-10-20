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
    void hle_##name(Cpu *c) { fp64_ret(c, name(fp64_arg(c, 0))); } \
    HLE_FUNC(#name "f", hle_import_##name##f) { hle_##name##f(c); } \
    HLE_FUNC(#name, hle_import_##name) { hle_##name(c); }

#define MATH_F2(name) \
    void hle_##name##f(Cpu *c) { fp32_ret(c, name##f(fp32_arg(c, 0), fp32_arg(c, 1))); } \
    void hle_##name(Cpu *c) { fp64_ret(c, name(fp64_arg(c, 0), fp64_arg(c, 1))); } \
    HLE_FUNC(#name "f", hle_import_##name##f) { hle_##name##f(c); } \
    HLE_FUNC(#name, hle_import_##name) { hle_##name(c); }

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
