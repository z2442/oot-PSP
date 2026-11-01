#!/usr/bin/env python3
"""Check actual fade setup, animation and emitted colors on a little-endian host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/code/z_fbdemo_fade.c").read_text()
source = source[source.index("typedef enum TransitionFadeDirection"):]
header = (ROOT / "include/transition_fade.h").read_text()
fade_type = re.search(r"typedef struct TransitionFade \{.*?\} TransitionFade;", header, re.S)[0]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PLATFORM_PSP 1
#define PLATFORM_N64 0
#define ULTRA64_ULTRATYPES_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef float f32;
#include "color.h"
#include "transition_instances.h"
typedef uint32_t Gfx;
#define gsDPPipeSync() 0
#define gsSPClearGeometryMode(...) 0
#define gsDPSetOtherMode(...) 0
#define gsDPSetCombineLERP(...) 0
#define gsSPEndDisplayList() 0
#define PRINTF(...) ((void)0)
#define bzero(p,n) memset(p,0,n)
static struct { int transFadeDuration; } gSaveContext;
static s16 R_TRANS_FADE_FLASH_ALPHA_STEP;
static int gScreenWidth=320,gScreenHeight=240;
static unsigned draws;
static Color_RGBA8_u32 emitted;
static void command(Gfx* p) { (void)p; }
static void primitive(Gfx* p,int r,int g,int b,int a) {
    (void)p;emitted.r=r;emitted.g=g;emitted.b=b;emitted.a=a;draws++;
}
#define gSPDisplayList(p,list) ((void)(list),command(p))
#define gDPSetPrimColor(p,m,l,r,g,b,a) primitive(p,r,g,b,a)
#define gDPFillRectangle(p,l,t,r,b) ((void)(r),(void)(b),command(p))
#define gDPPipeSync(p) command(p)
static bool Math_StepToS(s16* value,s16 target,s16 step) {
    int difference=target-*value;
    if (difference>step) *value+=step;
    else if (difference < -step) *value-=step;
    else { *value=target;return true; }
    return false;
}
'''
tests = r'''
int main(void) {
    const u32 colors[]={RGBA8(160,160,160,255),RGBA8(0,0,0,255),RGBA8(255,255,255,255),
                        RGBA8(140,140,100,255),RGBA8(70,100,110,255),RGBA8(18,52,86,120)};
    const int durations[]={1,20,60};
    for (unsigned c=0;c<sizeof(colors)/sizeof(colors[0]);c++) {
        TransitionFade fade;TransitionFade_Init(&fade);TransitionFade_SetColor(&fade,colors[c]);
        assert(fade.color.r==(colors[c]>>24) && fade.color.g==((colors[c]>>16)&255));
        assert(fade.color.b==((colors[c]>>8)&255) && fade.color.a==(colors[c]&255));
        for (unsigned d=0;d<3;d++) for (int direction=0;direction<2;direction++) {
            TransitionFade_SetType(&fade,direction ? TRANS_INSTANCE_TYPE_FILL_OUT : TRANS_INSTANCE_TYPE_FILL_IN);
            gSaveContext.transFadeDuration=durations[d];TransitionFade_Start(&fade);
            for (int tick=0;tick<=durations[d];tick++) {
                Gfx buffer[4],*cursor=buffer;draws=0;
                TransitionFade_Draw(&fade,&cursor);
                unsigned alpha=(255.0f*tick)/durations[d];
                if (direction) alpha=255-alpha;
                assert(fade.color.a==alpha && draws==(alpha!=0));
                assert(cursor==buffer+(alpha ? 4 : 0));
                if (alpha) {
                    assert(emitted.r==(colors[c]>>24) && emitted.g==((colors[c]>>16)&255));
                    assert(emitted.b==((colors[c]>>8)&255) && emitted.a==alpha);
                }
                if (tick<durations[d]) TransitionFade_Update(&fade,1);
            }
            assert(TransitionFade_IsDone(&fade));
        }
    }
    puts("PSP fade colors: neutral white/black, colored fades, alpha progression and emitted rectangles passed");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-fade-color-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + fade_type + source + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", str(ROOT / "include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
