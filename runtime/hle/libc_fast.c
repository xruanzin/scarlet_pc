// Hot C-library entry points implemented directly on the host. Guest virtual
// addresses are host addresses in this runtime, so the standard C operations can
// run without lifting their AArch64 loops instruction by instruction.
#include <string.h>

#include "../hle.h"

void hle_memcpy(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[0];
    const void *src = (const void *)(uintptr_t)c->x[1];
    size_t n = (size_t)c->x[2];
    memcpy(dst, src, n);
    c->x[0] = (uint64_t)(uintptr_t)dst;
}

void hle_strcmp(Cpu *c) {
    const char *a = (const char *)(uintptr_t)c->x[0];
    const char *b = (const char *)(uintptr_t)c->x[1];
    // AArch64 returns int in w0; model that zero-extension explicitly.
    c->x[0] = (uint32_t)strcmp(a, b);
}

HLE_FUNC("memcpy", hle_import_memcpy) {
    hle_memcpy(c);
}

HLE_FUNC("strcmp", hle_import_strcmp) {
    hle_strcmp(c);
}
