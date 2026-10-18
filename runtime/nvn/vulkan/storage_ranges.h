// Merge overlapping storage bindings before uploading: aliases must share the same GPU allocation.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint64_t addr, size; } StorageRange;
static size_t storage_merge_ranges(const StorageRange *input, size_t count, uint64_t align,
                                   StorageRange *out, size_t capacity) {
    size_t n = 0;
    if (!align || (align & (align - 1))) align = 256;
    for (size_t i = 0; i < count; i++) {
        uint64_t a = input[i].addr, sz = input[i].size;
        if (!a || !sz || sz > UINT64_MAX - a || a + sz > UINT64_MAX - 3) continue;
        uint64_t end = (a + sz + 3) & ~3ull;
        a &= ~(align - 1);
        size_t j = 0;
        while (j < n && out[j].addr + out[j].size < a) j++;
        size_t k = j;
        while (k < n && out[k].addr <= end) {
            if (out[k].addr < a) a = out[k].addr;
            if (out[k].addr + out[k].size > end) end = out[k].addr + out[k].size;
            k++;
        }
        if (k == j && n == capacity) return 0;
        if (k == j) { for (size_t q = n; q > j; q--) out[q] = out[q - 1]; n++; }
        else { for (size_t q = k; q < n; q++) out[j + 1 + q - k] = out[q]; n -= k - j - 1; }
        out[j] = (StorageRange){a, end - a};
    }
    return n;
}
