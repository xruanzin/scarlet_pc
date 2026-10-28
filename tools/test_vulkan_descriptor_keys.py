#!/usr/bin/env python3
"""Check exact descriptor identity and bounded writes in the production key builder."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('typedef struct {\n    uint32_t binding, type, image;')
end = source.index('static VkDescriptorSet stage_set(', start)
production = source[start:end]
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define MAX_BIND 192
typedef uint64_t VkDescriptorSet, VkDescriptorSetLayout;
typedef struct { uint64_t sampler, imageView; uint32_t imageLayout; } VkDescriptorImageInfo;
typedef struct { uint64_t buffer, offset, range; } VkDescriptorBufferInfo;
typedef struct {
    uint32_t descriptorCount, dstBinding, dstArrayElement, descriptorType;
    const void *pNext, *pTexelBufferView;
    const VkDescriptorImageInfo *pImageInfo;
    const VkDescriptorBufferInfo *pBufferInfo;
} VkWriteDescriptorSet;
'''
checks = r'''
int main(void) {
    ScarletDescriptorKey a[MAX_BIND], b[MAX_BIND];
    memset(a, 0xa5, sizeof a); memset(b, 0x5a, sizeof b);
    VkDescriptorBufferInfo buffer = {1, 256, 64};
    VkWriteDescriptorSet w = {.descriptorCount=1, .dstBinding=4, .descriptorType=6, .pBufferInfo=&buffer};
    assert(stage_descriptor_key(&w, 1, a) && stage_descriptor_key(&w, 1, b));
    assert(!memcmp(a, b, sizeof a[0])); // normalized keys include deterministic padding
    for (unsigned i = sizeof a[0]; i < sizeof a; ++i) assert(((unsigned char *)a)[i] == 0xa5);
    buffer.offset++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    buffer.offset--; buffer.range++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    buffer.range--; buffer.buffer++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    buffer.buffer--; w.dstBinding++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    w.dstBinding--; w.descriptorType++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    w.descriptorCount=2; assert(!stage_descriptor_key(&w, 1, b));
    w.descriptorCount=1; w.pNext=&w; assert(!stage_descriptor_key(&w, 1, b));
    w.pNext=0; w.dstArrayElement=1; assert(!stage_descriptor_key(&w, 1, b));
    w.dstArrayElement=0; w.pTexelBufferView=&w; assert(!stage_descriptor_key(&w, 1, b));
    w.pTexelBufferView=0; w.pBufferInfo=0;
    VkDescriptorImageInfo image = {8, 9, 1}; w.pImageInfo=&image;
    assert(stage_descriptor_key(&w, 1, a));
    image.sampler++; assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    image.sampler--; image.imageView++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    image.imageView--; image.imageLayout++;
    assert(stage_descriptor_key(&w, 1, b) && memcmp(a, b, sizeof a[0]));
    assert(stage_descriptor_key(0, 0, b));
    assert(!stage_descriptor_key(0, -1, b) && !stage_descriptor_key(0, MAX_BIND+1, b));
    stage_descriptors[0].seq=1; assert(stage_descriptors[0].seq==1);
    puts("descriptor keys: exact identity, unsupported bindings and bounded writes passed");
}
'''
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'keys.c'
    exe = Path(directory) / ('keys.exe' if os.name == 'nt' else 'keys')
    c.write_text(fixture + production + checks)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
