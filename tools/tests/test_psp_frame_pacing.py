#!/usr/bin/env python3
"""Run the PSP scheduler with a virtual microsecond clock and task queues."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'src/port/psp/sched_psp.c').read_text()
source = re.sub(r'^#include [^\n]+\n', '', source, flags=re.M)
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef uint8_t u8;
typedef void* OSMesg; typedef int OSPri; typedef int UNK_TYPE; typedef int IrqMgr;
typedef struct { struct { int type; } t; } OSTask;
typedef struct { int updateRate; void* swapBuffer; } Framebuffer;
typedef struct { OSTask list; unsigned flags; Framebuffer* framebuffer; void* msgQueue; OSMesg msg; } OSScTask;
typedef struct { int cmdQueue,interruptQueue,cmdMsgBuf[4],interruptMsgBuf[4]; int retraceCount,isFirstSwap; } Scheduler;
#define OS_MESG_NOBLOCK 0
#define OS_SC_SWAPBUFFER 1
#define M_GFXTASK 1
#define ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))
static u32 now,renderCost;
static int fps=20, sample=1, queued, completed, swapped, rendered;
static OSScTask* pending;
static u32 renderTimes[512];
static float fraction, renderFractions[512];
static u32 sceKernelGetSystemTimeLow(void) { return now; }
static void sceKernelDelayThread(u32 duration) { assert(duration<1000000); now+=duration; }
static int osRecvMesg(void* q, OSMesg* task, int flags) {
    (void)q;(void)flags;
    if (!queued) return -1;
    *task=pending; queued=0; return 0;
}
static void osSendMesg(void* q, OSMesg message, int flags) { (void)q;(void)message;(void)flags;completed++; }
static void osCreateMesgQueue(void* q, void* data, int count) { (void)q;(void)data;(void)count; }
static void osViSwapBuffer(void* buffer) { (void)buffer;swapped++; }
static void OotPspRenderer_Init(void) {}
static int OotPspVideo_GetFrameRate(void) { return fps; }
static int OotPspFrameInterpolation_HasSample(void) { return sample; }
static void OotPspFrameInterpolation_SetFraction(float value) { fraction=value; }
static void OotPspRenderer_RenderTask(const OSTask* task) {
    (void)task;
    renderTimes[rendered]=now; renderFractions[rendered++]=fraction; now+=renderCost;
}
'''
tests = r'''
static Scheduler scheduler;
static Framebuffer fb={3,(void*)1};
static OSScTask task={.list={{M_GFXTASK}},.flags=OS_SC_SWAPBUFFER,.framebuffer=&fb,.msgQueue=(void*)1};
static void reset(int frameRate,u32 clock,u32 cost) {
    fps=frameRate; now=clock; renderCost=cost; rendered=completed=swapped=0; sample=1;
    Sched_Init(&scheduler,NULL,0,0,0,NULL);
}
static void submit(void) { pending=&task; queued=1; Sched_Notify(&scheduler); assert(!queued); }
int main(void) {
    reset(20,0,3000);
    for (int i=0;i<10;i++) { now+=5000; submit(); }
    assert(rendered==10 && completed==10 && swapped==10);
    for (int i=0;i<10;i++) { assert(renderTimes[i]==5000+50000U*i); assert(renderFractions[i]==1); }
    reset(60,0,3000);
    for (int i=0;i<10;i++) { now+=5000; submit(); }
    assert(rendered==30 && completed==10 && swapped==10 && fraction==1);
    for (int i=0;i<30;i++) {
        assert(renderTimes[i]==5000+(1000000U*i)/60U);
        assert(renderFractions[i]==(i%3+1)/3.0f);
    }
    // 30-Hz states replay twice; normal one-VI menus stay at native 60 Hz.
    reset(60,0,1000); fb.updateRate=2; submit(); submit();
    assert(rendered==4 && renderTimes[3]==50000);
    reset(60,0,1000); fb.updateRate=1; submit(); submit();
    assert(rendered==2 && renderTimes[1]==16666);
    // Disabled history (pause/UI) keeps its task cadence without redundant replays.
    reset(60,0,1000); fb.updateRate=3; sample=0; submit(); submit();
    assert(rendered==2 && renderTimes[1]==50000);
    // Non-presenting tasks neither wait nor advance the presentation clock.
    reset(60,0,1000); task.flags=0; submit(); submit();
    assert(rendered==2 && renderTimes[1]==1000 && swapped==0 && completed==2);
    task.flags=OS_SC_SWAPBUFFER;
    // Audio/completion-only tasks do not render.
    task.list.t.type=2; submit(); assert(rendered==2 && completed==3); task.list.t.type=M_GFXTASK;
    // Over-budget rendering never accumulates debt or changes the update rate.
    reset(60,0,25000); submit(); submit();
    assert(rendered==6 && completed==2 && fb.updateRate==3);
    for (int i=0;i<6;i++) assert(renderTimes[i]==25000U*i);
    // Signed timestamp deltas remain correct across the 32-bit timer wrap.
    reset(60,UINT32_MAX-10000,1000); u32 origin=now; submit(); submit();
    assert((u32)(renderTimes[5]-origin)==83333);
    // A HOME/menu stall rebases deadlines, without executing catch-up ticks.
    now+=1000000; int before=rendered; submit();
    assert(rendered==before+3 && (u32)(renderTimes[before+1]-renderTimes[before])==16666);
    // Wake can cross the signed 32-bit comparison window. Reset the old
    // deadline explicitly so sleep never becomes a long pacing delay.
    now+=0x90000000U;before=rendered;u32 wakeTime=now;
    SchedPsp_ResetGfxPacing();submit();
    assert(renderTimes[before]==wakeTime && renderTimes[before+1]-wakeTime==16666);
    puts("Frame pacing: 20/60 Hz, task cadence, completion, overruns, timer wrap and UI fallback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-pacing-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(stubs+source+tests)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
