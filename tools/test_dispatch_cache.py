#!/usr/bin/env python3
"""Exercise the production dispatcher with colliding synthetic entries and reboots.
No game files needed. Requires clang, Zig, or the ziglang Python package.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "gamelib.h"
#include "hle.h"
static void one(Cpu *c) { c->x[0] = 1; }
static void two(Cpu *c) { c->x[0] = 2; }
static uint64_t addrs[] = {@A@ull, @B@ull}, count = 2;
static GuestFn funcs[] = {one, two};
static GameLibTables tables = {.nmods=1, .mods={{.name="test", .addrs=addrs, .fns=funcs, .count=&count}}};
const GameLibTables *gamelib_tables(void) { return &tables; }
__attribute__((used, section("hle_impls"))) static const HleImplEntry dummy = {0};
void rt_unwind_point(void) {}
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000000000ull+t.tv_nsec; }
Cpu *kernel_current_cpu(void) { return NULL; }
const char *loader_symbol_at(uint64_t a, uint64_t *o) { *o=0; return NULL; }
void plat_print_backtrace(void) {}
uint64_t plat_register_thread(void) { return 1; }
void plat_dump_other_threads(void) {}
void plat_fatal_notice(void) {}
static void check(unsigned i, unsigned expected) {
    Cpu c = {.pc=addrs[i]}; rt_dispatch(&c); assert(c.x[0]==expected);
}
static void *worker(void *p) {
    unsigned i=(uintptr_t)p;
    for (unsigned n=0;n<2000000;n++) check(i, i+1);
    return NULL;
}
int main(void) {
    dispatch_init();
    pthread_t threads[8];
    uint64_t start=plat_now_ns();
    for (uintptr_t i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(i&1)));
    for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    printf("16M colliding threaded calls: %.3f ms\n",(plat_now_ns()-start)/1e6);
    check(0,1); check(1,2);
    // The host main thread survives a boot; its cached pointer must not.
    funcs[0]=two; funcs[1]=one; dispatch_init(); check(0,2); check(1,1);
    // Register an override after a warm entry, while workers are stopped.
    hle_override(addrs[0],one,"test override"); check(0,1);
    uint64_t host=hle_register("test host",two);
    Cpu c={.pc=host}; rt_dispatch(&c); assert(c.x[0]==2);
    puts("PASS: collisions, reboot invalidation, warm override, HLE dispatch");
}
'''

def main():
    # Find a pair that collides in both the old 18-bit and new 10-bit hashes.
    seen = {}
    for a in range(0x7000000000, 0x7001000000, 4):
        h = (((a >> 2) * 11400714819323198485) & ((1 << 64)-1)) >> 46
        if h in seen:
            first, second = seen[h], a
            break
        seen[h] = a
    cc = shlex.split(os.environ['CC']) if os.environ.get('CC') else (
        ['clang'] if shutil.which('clang') else ['zig', 'cc'] if shutil.which('zig') else [sys.executable, '-m', 'ziglang', 'cc'])
    with tempfile.TemporaryDirectory() as d:
        p = Path(d); src = p/'test.c'; exe = p/'test'
        src.write_text(HARNESS.replace('@A@',hex(first)).replace('@B@',hex(second)))
        subprocess.run(cc + ['-O2', '-UNDEBUG','-ffunction-sections','-fdata-sections','-Wno-format','-I'+str(ROOT/'runtime'), str(src),str(ROOT/'runtime/dispatch.c'),'-Wl,--gc-sections','-pthread','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    main()
