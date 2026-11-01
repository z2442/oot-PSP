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
static char trace[32];
static unsigned traceLength;
static void record(char event) { trace[traceLength++]=event; trace[traceLength]=0; }
static void sceGuFinish(void) { record('F'); }
static void sceGuSync(int a,int b) { (void)a;(void)b;record('S'); }
static void sceDisplayWaitVblankStart(void) { record('W'); }
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
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_VSYNC;
    OotPspHomeMenu_AdjustVideoRow(-1); assert(!OotPspVideo_IsVSyncEnabled());
    OotPspHomeMenu_AdjustVideoRow(1); assert(OotPspVideo_IsVSyncEnabled());
    OotPspHomeMenu_ActivateVideoRow(); assert(!OotPspVideo_IsVSyncEnabled());
    assert(applies==0); // VSync changes immediately without applying a display mode.
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_SAVE;
    OotPspHomeMenu_ActivateVideoRow(); assert(strcmp(status,"Saved video.ini")==0);
    OotPspVideo_SetVSyncEnabled(1); OotPspVideo_Init("EBOOT.PBP");
    assert(!OotPspVideo_IsVSyncEnabled()); // Saved off round-trips.
    const char* off[]={"off","FALSE","no","0"};
    for (unsigned i=0;i<4;i++) {
        char text[80]; snprintf(text,sizeof(text),"[video]\r\n VSYNC = %s ; comment\r\n",off[i]);
        config(text); assert(!OotPspVideo_IsVSyncEnabled());
    }
    config("[video]\noutput = lcd\naspect = 4:3\n"); // Older files retain the new default.
    assert(OotPspVideo_IsVSyncEnabled() && strcmp(OotPspVideo_GetAspectName(),"4:3")==0);
    for (unsigned enabled=0;enabled<2;enabled++) for (unsigned interlaced=0;interlaced<2;interlaced++) {
        OotPspVideo_SetVSyncEnabled(enabled); sInterlacedOutput=interlaced;
        traceLength=0; gfx_scegu_end_frame();
        const char* expected=interlaced ? (enabled ? "FSWP" : "FSP") : (enabled ? "FSWB" : "FSB");
        assert(strcmp(trace,expected)==0); // Wait follows GPU completion, precedes presentation.
    }
    OotPspVideo_SetVSyncEnabled(0);
    sVideoSelectedIndex=OOT_PSP_VIDEO_MENU_ROW_RESET; OotPspHomeMenu_ActivateVideoRow();
    assert(OotPspVideo_IsVSyncEnabled() && applies==1);
    sVideoSelectedIndex=0; OotPspHomeMenu_MoveVideoSelection(-1);
    assert(sVideoSelectedIndex==OOT_PSP_VIDEO_MENU_ROW_BACK);
    OotPspHomeMenu_MoveVideoSelection(1); assert(sVideoSelectedIndex==0);
    assert(OotPspVideo_Save()==0); OotPspVideo_Init("EBOOT.PBP");
    assert(OotPspVideo_IsVSyncEnabled()); // Saved on round-trips.
    puts("VSync: enabled defaults, legacy config, save/load, HOME toggle/reset and frame wait ordering passed");
}
'''
defines = "\n".join(re.findall(r"^#define OOT_PSP_VIDEO_MENU_ROW_.*", menu, re.M)) + "\n"
code = video + stubs + defines
for name in ("OotPspHomeMenu_MoveVideoSelection", "OotPspHomeMenu_AdjustVideoRow", "OotPspHomeMenu_ActivateVideoRow"):
    code += function(menu, name)
code += function(backend, "gfx_scegu_end_frame") + tests
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
