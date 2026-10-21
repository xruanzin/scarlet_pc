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

void hle_memset(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[0];
    int value = (int)(uint8_t)c->x[1];
    size_t n = (size_t)c->x[2];
    memset(dst, value, n);
    c->x[0] = (uint64_t)(uintptr_t)dst;
}

void hle_strlen(Cpu *c) {
    const char *s = (const char *)(uintptr_t)c->x[0];
    c->x[0] = (uint64_t)strlen(s);
}

void hle_strcmp(Cpu *c) {
    const char *a = (const char *)(uintptr_t)c->x[0];
    const char *b = (const char *)(uintptr_t)c->x[1];
    // AArch64 returns int in w0; model that zero-extension explicitly.
    c->x[0] = (uint32_t)strcmp(a, b);
}

void hle_strcpy(Cpu *c) {
    char *dst = (char *)(uintptr_t)c->x[0];
    const char *src = (const char *)(uintptr_t)c->x[1];
    strcpy(dst, src);
    c->x[0] = (uint64_t)(uintptr_t)dst;
}

void hle_stpcpy(Cpu *c) {
    char *dst = (char *)(uintptr_t)c->x[0];
    const char *src = (const char *)(uintptr_t)c->x[1];
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    c->x[0] = (uint64_t)(uintptr_t)(dst + n);
}

void hle_strcat(Cpu *c) {
    char *dst = (char *)(uintptr_t)c->x[0];
    const char *src = (const char *)(uintptr_t)c->x[1];
    strcat(dst, src);
    c->x[0] = (uint64_t)(uintptr_t)dst;
}

HLE_FUNC("memcpy", hle_import_memcpy) { hle_memcpy(c); }


HLE_FUNC("strcmp", hle_import_strcmp) { hle_strcmp(c); }



