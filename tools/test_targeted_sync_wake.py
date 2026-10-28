#!/usr/bin/env python3
"""Production kernel selective wake and real pthread wait lifecycle regression."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import os, shlex, shutil
def compiler_command():
    return shlex.split(os.environ['CC']) if os.environ.get('CC') else [shutil.which('clang') or shutil.which('cc')]
ROOT=Path(__file__).resolve().parent.parent
HARNESS=r'''
#include <assert.h>
#include <time.h>
#include <errno.h>
#include "kernel.h"
static pthread_cond_t *observed[8];
static int nwake;
static int signal_counted(pthread_cond_t *p) { observed[nwake++ % 8]=p; return pthread_cond_signal(p); }
#define pthread_cond_signal signal_counted
static int counted_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define pthread_cond_timedwait counted_timedwait
#include "kernel.c"
#undef pthread_cond_timedwait
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

static uint32_t arb_word;
static _Atomic int arb_stop;
static unsigned long adds;
static void *update_word(void *arg) {
    (void)arg;
    while (!atomic_load(&arb_stop)) { __atomic_fetch_add(&arb_word, 2, __ATOMIC_SEQ_CST); adds++; }
    return NULL;
}
static uint32_t cv_word, mx_word;
static KThread cv_waiter;
static int late_handoff;
static int counted_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *ts) {
    if (late_handoff && cv == &cv_waiter.cond) {
        // Signal while the mutex is contended, then reach the deadline before ownership handoff.
        // Model the release/reacquire performed by pthread_cond_timedwait.
        late_handoff = 0;
        pthread_mutex_unlock(mutex);
        pthread_mutex_lock(&g_klock); mx_word = 0x222; pthread_mutex_unlock(&g_klock);
        svc_signal_process_wide_key((uintptr_t)&cv_word, 1);
        pthread_mutex_lock(mutex);
        assert(cv_waiter.wait_kind == WAIT_MUTEX);
        mx_word = 0; // owner releases in user space just before timedwait returns
        return ETIMEDOUT;
    }
    return pthread_cond_timedwait(cv, mutex, ts);
}
static void kernel_race_tests(void) {
    pthread_t adder;
    assert(!pthread_create(&adder, NULL, update_word, NULL));
    long delta = 0;
    for (int i = 0; i < 300000; ++i) {
        if (svc_wait_for_address((uintptr_t)&arb_word, ARB_DEC_WAIT_LESS, INT32_MAX, 0) == R_TIMEOUT) --delta;
        int32_t v = (int32_t)__atomic_load_n(&arb_word, __ATOMIC_SEQ_CST);
        if (svc_signal_to_address((uintptr_t)&arb_word, ARB_SIGNAL_INC_EQ, v, 0) == R_OK) ++delta;
    }
    atomic_store(&arb_stop, 1); pthread_join(adder, NULL);
    assert(arb_word == (uint32_t)(2 * adds + delta));
    arb_word = INT32_MAX;
    assert(svc_signal_to_address((uintptr_t)&arb_word, ARB_SIGNAL_INC_EQ, INT32_MAX, 0) == R_OK);
    assert(arb_word == 0x80000000u);
    assert(svc_wait_for_address((uintptr_t)&arb_word, ARB_DEC_WAIT_LESS, 0, 0) == R_TIMEOUT);
    assert(arb_word == INT32_MAX);
    KThread q[3] = {0};
    for (int i = 0; i < 3; ++i) {
        pthread_cond_init(&q[i].cond, NULL); q[i].prio = i == 2 ? 20 : 40;
        q[i].wait_kind = WAIT_ARBITER; q[i].wait_addr = (uintptr_t)&arb_word; waitlist_add(&q[i]);
    }
    assert(arbiter_wake((uintptr_t)&arb_word, 1) == 1 && q[2].wait_kind == WAIT_NONE);
    assert(arbiter_wake((uintptr_t)&arb_word, 1) == 1 && q[0].wait_kind == WAIT_NONE && q[1].wait_kind == WAIT_ARBITER);
    for (int i = 0; i < 3; ++i) { waitlist_remove(&q[i]); pthread_cond_destroy(&q[i].cond); }
    pthread_cond_init(&cv_waiter.cond, NULL); cv_waiter.handle = 0x130; t_cur = &cv_waiter;
    mx_word = cv_waiter.handle;
    assert(svc_wait_process_wide_key((uintptr_t)&mx_word, (uintptr_t)&cv_word, cv_waiter.handle, 0) == R_TIMEOUT);
    assert(mx_word == 0 && !g_waitlist); // true timeout: SDK reacquires
    mx_word = cv_waiter.handle; late_handoff = 1;
    assert(svc_wait_process_wide_key((uintptr_t)&mx_word, (uintptr_t)&cv_word, cv_waiter.handle, 1000000) == R_OK);
    assert(mx_word == cv_waiter.handle && !g_waitlist);
    mutex_release((uintptr_t)&mx_word); pthread_cond_destroy(&cv_waiter.cond);
    puts("arbiter concurrent updates, wraparound, FIFO priority and signaled timeout passed");
}

int main(void) {
    kernel_race_tests();
    nwake = 0; t_cur = NULL;
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
            # This synchronization-only harness discards thread creation/SVC code.
            inc=Path(tmp)/'SDL3'; inc.mkdir()
            (inc/'SDL.h').write_text('extern int SDL_GetNumLogicalCPUCores(void);\n')
            r=subprocess.run(compiler_command()+['-O2','-UNDEBUG','-ffunction-sections','-fdata-sections','-Wno-format','-I'+tmp,'-I'+str(ROOT/'runtime'),str(src),'-Wl,--gc-sections','-pthread','-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main(verbosity=2)
