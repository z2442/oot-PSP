#!/usr/bin/env python3
"""Check actual video config, HOME menu input and frame presentation with host stubs."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
video = (ROOT / "src/port/psp/oot_psp_video.c").read_text()
menu = (ROOT / "src/port/psp/oot_psp_home_menu.c").read_text()
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()


def function(source, name):
    start = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
int OotPspDve_GetModel(void) { return 0; }
int OotPspDve_IsAvailable(void) { return 0; }
int OotPspDve_GetCable(void) { return 0; }
int OotPspDve_RefreshCable(void) { return 0; }
static int sVideoSelectedIndex,sScreen,applies;
static char status[64];
#define OOT_PSP_HOME_MENU_SCREEN_MAIN 0
static void OotPspHomeMenu_SetStatus(const char* message) { snprintf(status,sizeof(status),"%s",message); }
static int OotPspRenderer_ApplyVideoSettings(void) { ++applies; return 0; }
#define GU_SYNC_FINISH 0
#define GU_SYNC_WHAT_DONE 0
static bool sInterlacedOutput;
static void* sDrawBuffer=(void*)1;
static void* sDisplayBuffer=(void*)2;
#define PSP_DISPLAY_SETBUF_IMMEDIATE 0
#define PSP_DISPLAY_SETBUF_NEXTFRAME 1
static int sBufferWidth=512,sScreenHeight=272;
#define BUF_WIDTH sBufferWidth
#define SCR_HEIGHT sScreenHeight
#define TV_BUFFER_WIDTH 768
#define TV_INTERLACED_BUFFER_HEIGHT 503
#define GU_PSM_4444 2
#define GU_PSM_8888 3
static void *sPrimaryBuffer,*sSecondaryBuffer,*sDepthBuffer,*sShadowMaskBuffer;
static unsigned staticOffset;
static void* getStaticVramBuffer(unsigned w,unsigned h,unsigned format) {
    void* result=(void*)(uintptr_t)(staticOffset|0x40000000U);
    staticOffset+=w*h*(format==GU_PSM_4444 ? 2 : 4);
    return result;
}
static int sColorPixelFormat;
static void* sColorBuffers[3]={(void*)1,(void*)2,(void*)3};
static void* sPendingBuffer;
static int sColorBufferCount=2;
static void* scanout=(void*)2;
static void* queued;
static int waits;
static void* gfx_scegu_vram_cpu_addr(const void* address) { return (void*)address; }
static char trace[64];
static unsigned traceLength;
static void record(char event) { trace[traceLength++]=event; trace[traceLength]=0; }
/* Exercise presentation policy synchronously here; the real asynchronous
 * handoff and ownership fence have their own video-thread test. */
static int sFrameVSync;
static void OotPspVideoThread_Submit(void (*complete)(void)) { complete(); }
static void sceGuFinish(void) { record('F'); }
static void sceGuSync(int a,int b) { (void)a;(void)b;record('S'); }
static void sceDisplayWaitVblankStart(void) {
    record('W'); waits++;
    if (queued) { scanout=queued; queued=NULL; }
}
static int sceDisplayGetFrameBuf(void** address,int* width,int* format,int sync) {
    (void)width;(void)format;(void)sync;
    // Exercise the alias returned by some display drivers.
    *address=(void*)((uintptr_t)scanout|0x40000000U); return 0;
}
static int sceDisplaySetFrameBuf(void* address,int width,int format,int sync) {
    (void)width;(void)format;
    if (sync==PSP_DISPLAY_SETBUF_NEXTFRAME) { queued=address; record('Q'); }
    else { scanout=address; queued=NULL; record('I'); }
    return 0;
}
static void gfx_scegu_present_interlaced(void) { record('P'); }
static void* sceGuSwapBuffers(void) { record('B');return sDisplayBuffer; }
'''
tests = r'''
static void config(const char* text) {
    FILE* f=fopen("video.ini","w"); assert(f); fputs(text,f); fclose(f);
    OotPspVideo_Init("EBOOT.PBP");
}
int main(void) {
    OotPspVideo_Init("EBOOT.PBP"); // No configuration: VSync is on.
    assert(OotPspVideo_IsVSyncEnabled());
    assert(OotPspVideo_GetFrameRate()==20);
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_FPS;
    OotPspHomeMenu_AdjustVideoRow(1); assert(OotPspVideo_GetFrameRate()==60);
    OotPspHomeMenu_ActivateVideoRow(); assert(OotPspVideo_GetFrameRate()==20);
    OotPspVideo_SetFrameRate(60);
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_VSYNC;
    OotPspHomeMenu_AdjustVideoRow(-1); assert(!OotPspVideo_IsVSyncEnabled());
    OotPspHomeMenu_AdjustVideoRow(1); assert(OotPspVideo_IsVSyncEnabled());
    OotPspHomeMenu_ActivateVideoRow(); assert(!OotPspVideo_IsVSyncEnabled());
    assert(applies==0); // VSync changes immediately without applying a display mode.
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_SAVE;
    OotPspHomeMenu_ActivateVideoRow(); assert(strcmp(status,"Saved video.ini")==0);
    OotPspVideo_SetVSyncEnabled(1); OotPspVideo_Init("EBOOT.PBP");
    assert(!OotPspVideo_IsVSyncEnabled() && OotPspVideo_GetFrameRate()==60); // Saved settings round-trip.
    const char* off[]={"off","FALSE","no","0"};
    for (unsigned i=0;i<4;i++) {
        char text[80]; snprintf(text,sizeof(text),"[video]\r\n VSYNC = %s ; comment\r\n",off[i]);
        config(text); assert(!OotPspVideo_IsVSyncEnabled());
    }
    config("[video]\noutput = lcd\naspect = 4:3\n"); // Older files retain the new default.
    assert(OotPspVideo_IsVSyncEnabled() && strcmp(OotPspVideo_GetAspectName(),"4:3")==0);
    assert(OotPspVideo_GetFrameRate()==20);
    config("fps = 60\n"); assert(OotPspVideo_GetFrameRate()==60);
    config("fps = 600\n"); assert(OotPspVideo_GetFrameRate()==20);
    OotPspVideo_SetFrameRate(30); assert(OotPspVideo_GetFrameRate()==20);
    for (unsigned enabled=0;enabled<2;enabled++) for (unsigned interlaced=0;interlaced<2;interlaced++) {
        OotPspVideo_SetVSyncEnabled(enabled); sInterlacedOutput=interlaced;
        traceLength=0; gfx_scegu_end_frame();
        const char* expected=interlaced ? (enabled ? "FSWP" : "FSP") : (enabled ? "FSWB" : "FSB");
        assert(strcmp(trace,expected)==0); // Wait follows GPU completion, precedes presentation.
    }
    // Triple buffering: render into the spare while the finished frame is queued.
    sColorBufferCount=3; sInterlacedOutput=false;
    sDrawBuffer=(void*)1; sDisplayBuffer=scanout=(void*)2; sPendingBuffer=queued=NULL;
    OotPspVideo_SetVSyncEnabled(1); waits=0; traceLength=0;
    gfx_scegu_end_frame();
    assert(strcmp(trace,"FSQ")==0 && waits==0);
    assert(sDrawBuffer==(void*)3 && sPendingBuffer==(void*)1 && scanout==(void*)2);
    for (unsigned i=0;i<100;i++) {
        assert(sDrawBuffer!=scanout && sDrawBuffer!=queued);
        traceLength=0; gfx_scegu_end_frame();
        assert(strcmp(trace,"FSWQ")==0);
        assert(sDrawBuffer!=scanout && sDrawBuffer!=queued);
    }
    // An already-latched frame never incurs an extra vblank wait.
    sceDisplayWaitVblankStart(); traceLength=0; gfx_scegu_end_frame();
    assert(strcmp(trace,"FSQ")==0);
    // Turning VSync off safely retires the pending frame, then swaps immediately.
    OotPspVideo_SetVSyncEnabled(0); traceLength=0; gfx_scegu_end_frame();
    assert(strcmp(trace,"FSWI")==0 && sPendingBuffer==NULL);
    traceLength=0; gfx_scegu_end_frame(); assert(strcmp(trace,"FSI")==0);
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_RESET; OotPspHomeMenu_ActivateVideoRow();
    assert(OotPspVideo_IsVSyncEnabled() && applies==1);
    sVideoSelectedIndex=0; OotPspHomeMenu_MoveVideoSelection(-1);
    assert(sVideoSelectedIndex==OOT_PSP_VIDEO_MENU_ROW_BACK);
    OotPspHomeMenu_MoveVideoSelection(1); assert(sVideoSelectedIndex==0);
    assert(OotPspVideo_Save()==0); OotPspVideo_Init("EBOOT.PBP");
    assert(OotPspVideo_IsVSyncEnabled()); // Saved on round-trips.
    // Layouts fit hardware EDRAM and keep the Phat texture base stable.
    sColorPixelFormat=GU_PSM_4444; sBufferWidth=512; sScreenHeight=272; sInterlacedOutput=false;
    gfx_scegu_reset_framebuffers();
    assert(sColorBufferCount==3 && staticOffset<2*1024*1024);
    assert((uintptr_t)sColorBuffers[1]-(uintptr_t)sColorBuffers[0]==512*272*2);
    unsigned phatTextureBase=staticOffset;
    gfx_scegu_reset_framebuffers(); assert(staticOffset==phatTextureBase);
    sColorPixelFormat=3; // Slim 32-bit color, 16-bit depth.
    gfx_scegu_reset_framebuffers(); assert(sColorBufferCount==3 && staticOffset<4*1024*1024);
    sBufferWidth=768; sScreenHeight=480;
    gfx_scegu_reset_framebuffers(); assert(sColorBufferCount==2 && staticOffset<4*1024*1024);
    sInterlacedOutput=true;
    gfx_scegu_reset_framebuffers(); assert(sColorBufferCount==2 && staticOffset<4*1024*1024);
    assert((uintptr_t)sDepthBuffer-(uintptr_t)sSecondaryBuffer==768*503*4);
    sBufferWidth=512; sScreenHeight=240; sInterlacedOutput=false;
    gfx_scegu_reset_framebuffers(); assert(sColorBufferCount==3 && staticOffset<4*1024*1024);
    assert(sPendingBuffer==NULL && sDrawBuffer==sPrimaryBuffer && sDisplayBuffer==sSecondaryBuffer);
    puts("Video: defaults, 20/60 FPS save/load, HOME controls, double/triple buffer ownership and VSync passed");
}
'''
defines = "\n".join(re.findall(r"^#define OOT_PSP_VIDEO_MENU_ROW_.*", menu, re.M)) + "\n"
code = video + stubs + defines
for name in ("OotPspHomeMenu_MoveVideoSelection", "OotPspHomeMenu_AdjustVideoRow", "OotPspHomeMenu_ActivateVideoRow"):
    code += function(menu, name)
code += function(backend, "gfx_scegu_reset_framebuffers") + function(backend, "gfx_scegu_present_triple") + function(backend, "gfx_scegu_complete_frame") + function(backend, "gfx_scegu_end_frame") + tests
with tempfile.TemporaryDirectory(prefix="oot-vsync-") as directory:
    path = Path(directory)
    (path / "pspiofilemgr.h").write_text('''#include <fcntl.h>
#include <unistd.h>
typedef int SceUID;
#define PSP_O_RDONLY O_RDONLY
#define PSP_O_WRONLY O_WRONLY
#define PSP_O_CREAT O_CREAT
#define PSP_O_TRUNC O_TRUNC
#define sceIoOpen open
#define sceIoRead read
#define sceIoWrite write
#define sceIoClose close
''')
    (path / "test.c").write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(path), "-I", str(ROOT / "src/port/psp"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, cwd=path)
