// Hot C-library entry points implemented directly on the host. Guest virtual
// addresses are host addresses in this runtime, so the standard C operations can
// run without lifting their AArch64 loops instruction by instruction.
#include <string.h>

#include "../hle.h"

static inline uint64_t ret_ptr(const void *p) {
    return (uint64_t)(uintptr_t)p;
}

void hle_memcpy(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[0];
    const void *src = (const void *)(uintptr_t)c->x[1];
    size_t n = (size_t)c->x[2];
    memcpy(dst, src, n);
    c->x[0] = ret_ptr(dst);
}

void hle_memmove(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[0];
    const void *src = (const void *)(uintptr_t)c->x[1];
    size_t n = (size_t)c->x[2];
    memmove(dst, src, n);
    c->x[0] = ret_ptr(dst);
}

void hle_memset(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[0];
    int value = (int)(uint32_t)c->x[1];
    size_t n = (size_t)c->x[2];
    memset(dst, value, n);
    c->x[0] = ret_ptr(dst);
}

void hle_memcmp(Cpu *c) {
    const void *a = (const void *)(uintptr_t)c->x[0];
    const void *b = (const void *)(uintptr_t)c->x[1];
    size_t n = (size_t)c->x[2];
    c->x[0] = (uint32_t)memcmp(a, b, n);
}

void hle_memchr(Cpu *c) {
    const void *s = (const void *)(uintptr_t)c->x[0];
    int ch = (int)(uint32_t)c->x[1];
    size_t n = (size_t)c->x[2];
    c->x[0] = ret_ptr(memchr(s, ch, n));
}

void hle_strlen(Cpu *c) {
    c->x[0] = (uint64_t)strlen((const char *)(uintptr_t)c->x[0]);
}

void hle_strcmp(Cpu *c) {
    const char *a = (const char *)(uintptr_t)c->x[0];
    const char *b = (const char *)(uintptr_t)c->x[1];
    // AArch64 returns int in w0; model that zero-extension explicitly.
    c->x[0] = (uint32_t)strcmp(a, b);
}

void hle_strncmp(Cpu *c) {
    const char *a = (const char *)(uintptr_t)c->x[0];
    const char *b = (const char *)(uintptr_t)c->x[1];
    c->x[0] = (uint32_t)strncmp(a, b, (size_t)c->x[2]);
}

void hle_strcpy(Cpu *c) {
    char *dst = (char *)(uintptr_t)c->x[0];
    const char *src = (const char *)(uintptr_t)c->x[1];
    strcpy(dst, src);
    c->x[0] = ret_ptr(dst);
}

void hle_strncpy(Cpu *c) {
    char *dst = (char *)(uintptr_t)c->x[0];
    const char *src = (const char *)(uintptr_t)c->x[1];
    strncpy(dst, src, (size_t)c->x[2]);
    c->x[0] = ret_ptr(dst);
}

void hle_strchr(Cpu *c) {
    const char *s = (const char *)(uintptr_t)c->x[0];
    c->x[0] = ret_ptr(strchr(s, (int)(uint32_t)c->x[1]));
}

void hle_strrchr(Cpu *c) {
    const char *s = (const char *)(uintptr_t)c->x[0];
    c->x[0] = ret_ptr(strrchr(s, (int)(uint32_t)c->x[1]));
}

#define LIBC_BIND(name) HLE_FUNC(#name, hle_import_##name) { hle_##name(c); }

LIBC_BIND(memcpy)
LIBC_BIND(memmove)
LIBC_BIND(memset)
LIBC_BIND(memcmp)
LIBC_BIND(memchr)
LIBC_BIND(strlen)
LIBC_BIND(strcmp)
LIBC_BIND(strncmp)
LIBC_BIND(strcpy)
LIBC_BIND(strncpy)
LIBC_BIND(strchr)
LIBC_BIND(strrchr)
