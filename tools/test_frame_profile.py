"""Compile the production frame-profile function with synthetic timing inputs."""
from pathlib import Path
import csv
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parent.parent
source=(root/'runtime/nvn/vulkan/exec.c').read_text()
function=source[source.index('void vk_prof_frame('):source.index('\nstatic void save_pipeline_cache(')]
pre=r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static int prof=1;
static uint64_t cum_exec,cum_wait,cum_draws,cum_submits,ticks;
static atomic_int jobs_pending;
uint64_t SDL_GetTicksNS(void) { return ticks; }
'''
post=r'''
int main(int argc,char **argv) {
 setenv("LARECOMP_FRAME_CSV",argv[1],1);
 ticks=1000000000; vk_prof_frame(1,0);
 ticks+=20000000; cum_exec+=8000000; cum_wait+=3000000; cum_draws+=12; cum_submits+=2; vk_prof_frame(2,1000000);
 // Reboot must not turn a frame discontinuity into a huge duration.
 ticks+=3000000000; vk_prof_frame(1,0);
 ticks+=30000000; cum_exec+=10000000; cum_wait+=4000000; vk_prof_frame(2,2000000);
 return 0;
}
'''
cc=shlex.split(os.environ['CC']) if os.environ.get('CC') else (['clang'] if shutil.which('clang') else [sys.executable,'-m','ziglang','cc'])
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp); (p/'test.c').write_text(pre+function+post)
 subprocess.run(cc+['-O2',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'frames.csv')],check=True)
 with (p/'frames.csv').open() as f: rows=list(csv.DictReader(f))
 assert len(rows)==2,rows
 assert [float(r['frame_ms']) for r in rows]==[20,30]
 assert [float(r['other_ms']) for r in rows]==[11,18]
 assert float(rows[0]['gpu_wait_ms'])==3
 print('PASS: frame CSV timing, overlapping waits, reboot boundary, exit flush')
