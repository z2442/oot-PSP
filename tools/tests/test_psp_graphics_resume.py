#!/usr/bin/env python3
"""Exercise saved GE/renderer restoration without rebuilding live GU resources."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / 'src/port/psp'
backend = (PORT / 'gfx/gfx_scegu.c').read_text()
renderer = (PORT / 'oot_psp_renderer.c').read_text()


def function(source, name):
    start = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M).start()
    return source[start:source.index('\n}', source.index('{', start)) + 2] + '\n'


stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {unsigned context[512];} PspGeContext;
typedef struct {int tvOutput,bufferWidth,displayWidth,displayHeight,viewportWidth,viewportHeight;} OotPspVideoMode;
static PspGeContext sSuspendGeContext __attribute__((aligned(64)));
static bool sSuspendGeContextValid;
static PspGeContext hardware, original;
static OotPspVideoMode activeMode={0,512,480,272,362,272};
static OotPspVideoMode pendingMode={1,768,720,480,720,460};
static int sBufferWidth=512,sColorPixelFormat=3;
#define BUF_WIDTH sBufferWidth
#define LCD_SCREEN_WIDTH 480
#define LCD_SCREEN_HEIGHT 272
#define PSP_DISPLAY_SETBUF_IMMEDIATE 0
#define GU_SYNC_FINISH 0
#define GU_SYNC_WHAT_DONE 0
static void* sDisplayBuffer=(void*)0x40088000;
static void* sDrawBuffer=(void*)0x40000000;
static void* sPendingBuffer;
static unsigned callbacks=1,eventFlags=1,textureCache=0xa12b34,guSoftwareState=0x7351;
static unsigned waits,syncs,saves,restores,routes,framebufs,invalidations,interpolationResets,pacingResets;
static int failSave,failRoute,failRestore,failFramebuf;
static void* front;
static void OotPspVideoThread_Wait(void) {waits++;}
static void sceGuSync(int mode,int what) {assert(mode==0 && what==0);syncs++;}
static void OotPspFrameInterpolation_Reset(void) {interpolationResets++;}
static void SchedPsp_ResetGfxPacing(void) {pacingResets++;}
static void gfx_invalidate_render_state(void) {invalidations++;}
// Calling either of these on ordinary wake breaks boot/resume parity.
void gfx_invalidate_texture_cache(void) {textureCache=0;}
int sceGuInit(void) {callbacks++;eventFlags++;guSoftwareState=0;return 0;}
void sceGuTerm(void) {callbacks--;eventFlags--;}
int OotPspRenderer_ApplyVideoSettings(void) {assert(0);return -1;}
void OotPspVideo_GetMode(OotPspVideoMode* mode) {*mode=pendingMode;assert(0);}
static void OotPspVideo_GetActiveMode(OotPspVideoMode* mode) {*mode=activeMode;}
static int sceGeSaveContext(PspGeContext* p) {
    assert(((uintptr_t)p&63)==0 && waits && syncs);saves++;
    if(failSave) return -1;
    *p=hardware;return 0;
}
static int sceGeRestoreContext(const PspGeContext* p) {
    restores++;if(failRestore) return -2;hardware=*p;return 0;
}
static int sceDisplaySetMode(int mode,int width,int height) {
    assert(!activeMode.tvOutput && mode==0 && width==480 && height==272);routes++;return failRoute ? -3 : 0;
}
static int gfx_scegu_set_hardware_video_mode(const OotPspVideoMode* mode) {
    assert(mode->tvOutput && !memcmp(mode,&activeMode,sizeof(*mode)));routes++;return failRoute ? -3 : 0;
}
static void* gfx_scegu_vram_cpu_addr(const void* p) {return (void*)p;}
static int sceDisplaySetFrameBuf(void* p,int width,int format,int sync) {
    assert(p==sDisplayBuffer && width==sBufferWidth && format==sColorPixelFormat && sync==0);
    framebufs++;if(failFramebuf) return -4;front=p;return 0;
}
'''
functions = ''.join(function(backend, name) for name in (
    'gfx_scegu_save_suspend_context', 'gfx_scegu_restore_suspend_context'))
functions += ''.join(function(renderer, name) for name in (
    'OotPspRenderer_PrepareSuspend', 'OotPspRenderer_Resume'))
tests = r'''
static void reset(void) {
    failSave=failRoute=failRestore=failFramebuf=0;
    for(unsigned i=0;i<512;i++) hardware.context[i]=0x12345678U^(i*9281);
    original=hardware;sPendingBuffer=sDisplayBuffer;front=sDisplayBuffer;
}
static void checkPreserved(void) {
    assert(!memcmp(&hardware,&original,sizeof(hardware)));
    assert(callbacks==1 && eventFlags==1 && textureCache==0xa12b34 && guSoftwareState==0x7351);
    assert(sDrawBuffer==(void*)0x40000000 && sDisplayBuffer==(void*)0x40088000);
    assert(!sPendingBuffer && front==sDisplayBuffer);
    assert(pendingMode.tvOutput==1 && pendingMode.viewportWidth==720);
}
int main(void) {
    // LCD wake must preserve GU objects and warm textures while leaving HOME
    // edits unapplied. Repeating sleep must not leak SDK callback/event IDs.
    for(int i=0;i<50;i++) {
        reset();assert(OotPspRenderer_PrepareSuspend());
        memset(&hardware,0xcd,sizeof(hardware));front=NULL;
        assert(OotPspRenderer_Resume());checkPreserved();
    }
    assert(saves==50 && restores==50 && routes==50 && framebufs==50);
    assert(invalidations==50 && pacingResets==50 && interpolationResets==100);
    // An active TV route is restored without changing buffer geometry either.
    activeMode=(OotPspVideoMode){1,768,720,480,640,448};sBufferWidth=768;
    reset();assert(OotPspRenderer_PrepareSuspend());
    memset(&hardware,0xcd,sizeof(hardware));assert(OotPspRenderer_Resume());checkPreserved();
    // Bad saves prevent sleep. Failed restores remain retryable and must not
    // advertise restored state or reset pacing until restoration succeeds.
    reset();failSave=1;assert(!OotPspRenderer_PrepareSuspend());
    unsigned before=routes;assert(!OotPspRenderer_Resume() && routes==before);
    for(int error=0;error<3;error++) {
        reset();assert(OotPspRenderer_PrepareSuspend());
        failRoute=error==0;failRestore=error==1;failFramebuf=error==2;
        before=invalidations;
        assert(!OotPspRenderer_Resume() && invalidations==before);
        failRoute=failRestore=failFramebuf=0;
        assert(OotPspRenderer_Resume());checkPreserved();
    }
    puts("Graphics resume: 50 cycles preserve GE/GU state, callbacks, textures, active mode, buffers; save/restore failures retry safely");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-ge-resume-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stubs + functions + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
