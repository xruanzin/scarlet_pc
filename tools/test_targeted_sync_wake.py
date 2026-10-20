#!/usr/bin/env python3
"""Production kernel selective wake and real pthread wait lifecycle regression."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_indirect_resume import compiler_command
ROOT=Path(__file__).resolve().parent.parent
HARNESS=r'''
#include <assert.h>
#include <time.h>
#include "kernel.h"
static pthread_cond_t *observed[8];
static int nwake;
static int signal_counted(pthread_cond_t *p) { observed[nwake++ % 8]=p; return pthread_cond_signal(p); }
#define pthread_cond_signal signal_counted
#include "kernel.c"
#undef pthread_cond_signal
void rt_fatal_trace(void) { abort(); }
void plat_print_backtrace(void) {}
uint32_t vi_vsync_handle(void) { return 0; }
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static KThread waiter;
static uint32_t wh[2], result;
static int32_t result_index;
static int64_t wait_timeout;
static void *run_wait(void *unused) {
    t_cur=&waiter;
    result=svc_wait_synchronization((uintptr_t)wh,2,wait_timeout,&result_index);
    return NULL;
}
static void await_link(void) {
    for (;;) {
        pthread_mutex_lock(&g_klock);
        bool ready=waiter.in_sync_wait;
        pthread_mutex_unlock(&g_klock);
        if (ready) return;
        struct timespec t={0,100000}; nanosleep(&t,NULL);
    }
}
int main(void) {
    KObject a={0}, b={0};
    uint32_t ha=handle_alloc(&a), hb=handle_alloc(&b);
    uint32_t ah[]={ha}, bh[]={hb}, alias[]={handle_alloc(&a)}, zero[]={0};
    KThread ts[5]={0};
    for(int i=0;i<5;i++) { pthread_cond_init(&ts[i].cond,NULL); if(i<4) ts[i].snext=&ts[i+1]; }
    ts[0].sync_hs=ah; ts[1].sync_hs=bh; ts[2].sync_hs=alias; ts[3].sync_hs=zero;
    for(int i=0;i<4;i++) ts[i].sync_n=1;
    g_syncwaiters=ts;
    kernel_signal(&a);
    assert(a.signaled && nwake==3);
    assert(observed[0]==&ts[0].cond && observed[1]==&ts[2].cond && observed[2]==&ts[4].cond);
    g_syncwaiters=NULL;
    pthread_cond_init(&waiter.cond,NULL);
    wh[0]=hb; wh[1]=ha; a.signaled=b.signaled=false;
    wait_timeout=1000000000;
    pthread_t thread; assert(!pthread_create(&thread,NULL,run_wait,NULL));
    await_link(); pthread_mutex_lock(&g_klock); kernel_signal(&a); pthread_mutex_unlock(&g_klock);
    pthread_join(thread,NULL);
    assert(result==R_OK && result_index==1 && !g_syncwaiters && !waiter.sync_hs && !waiter.in_sync_wait);
    a.signaled=false; wait_timeout=1000000;
    run_wait(NULL);
    assert(result==R_TIMEOUT && !waiter.sync_hs && !g_syncwaiters);
    wait_timeout=1000000000;
    assert(!pthread_create(&thread,NULL,run_wait,NULL)); await_link();
    pthread_mutex_lock(&g_klock); waiter.cancel=true; wake(&waiter); pthread_mutex_unlock(&g_klock);
    pthread_join(thread,NULL);
    assert(result==R_CANCELLED && !waiter.sync_hs && !g_syncwaiters);
    puts("selective wake, aliases, signal, timeout and cancellation passed");
}
'''
class TargetedSyncWakeTests(unittest.TestCase):
    def test_wait_lifecycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'test.c'; exe=Path(tmp)/'test'
            src.write_text(HARNESS)
            r=subprocess.run(compiler_command()+['-O2','-UNDEBUG','-ffunction-sections','-fdata-sections','-Wno-format','-I'+str(ROOT/'runtime'),str(src),'-Wl,--gc-sections','-pthread','-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main(verbosity=2)
