#!/usr/bin/env python3
"""Exercise the production completion watermark with concurrent publishers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parent.parent
CODE = r'''
#include <assert.h>
#include <pthread.h>
#include "completion_cache.h"
static ScarletCompletionCache cache;
static void *publish(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 1; i <= 10000; ++i)
        scarlet_completion_publish(&cache, i * 4 + id);
    return 0;
}
int main(void) {
    scarlet_completion_reset(&cache);
    assert(scarlet_completion_covers(&cache, 0));
    assert(!scarlet_completion_covers(&cache, 1));
    pthread_t workers[4];
    for (uintptr_t i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], 0, publish, (void *)i));
    for (int i = 0; i < 4; ++i) assert(!pthread_join(workers[i], 0));
    assert(scarlet_completion_covers(&cache, 40003));
    assert(!scarlet_completion_covers(&cache, 40004));
    scarlet_completion_publish(&cache, 12);
    assert(scarlet_completion_covers(&cache, 40003));
    scarlet_completion_reset(&cache);
    assert(!scarlet_completion_covers(&cache, 1));
}
'''
class CompletionTest(unittest.TestCase):
    def test_concurrent_monotonic_completion(self):
        cc = shutil.which(os.environ.get("CC", "clang")) or shutil.which("gcc")
        if not cc:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/"test.c"
            src.write_text(CODE)
            exe=Path(tmp)/"test"
            subprocess.run([cc,"-std=c11","-O2","-pthread","-I"+str(ROOT/"runtime/nvn/vulkan"),str(src),"-o",str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__ == "__main__":
    unittest.main()
