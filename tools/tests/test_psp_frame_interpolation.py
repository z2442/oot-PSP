#!/usr/bin/env python3
"""Exercise owned render history independently of the game and the PSP SDK."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / 'src/port/psp'
TEST = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "oot_psp_frame_interpolation.h"
static float arena[2100][4][4];
static int state, actorA, actorB, siteA, siteB;
static void matrix(float value[4][4], float x) {
    memset(value, 0, 16*sizeof(float));
    for (int i=0;i<4;i++) value[i][i]=1;
    value[3][0]=x;
}
static void begin(int scene, int room, int camera, int enabled) {
    OotPspFrameInterpolation_Begin(&state,scene,room,camera,enabled);
    OotPspFrameInterpolation_SetWorldScope(1);
}
static void record(int slot, void* owner, void* site, float x) {
    matrix(arena[slot],x);
    OotPspFrameInterpolation_SetIdentity(owner);
    OotPspFrameInterpolation_Record(arena[slot],arena[slot],site);
}
static void expect(int slot, float x) {
    float value[4][4];
    memcpy(value,arena[slot],sizeof(value));
    OotPspFrameInterpolation_Resolve(arena[slot],value);
    assert(fabsf(value[3][0]-x)<0.001f);
}
int main(void) {
    OotPspFrameInterpolation_Reset();
    assert(!OotPspFrameInterpolation_HasSample());
    begin(1,2,0,1);
    record(0,&actorA,&siteA,0); record(1,&actorA,&siteA,10);
    record(2,&actorB,&siteA,100);
    OotPspFrameInterpolation_End();
    OotPspFrameInterpolation_SetFraction(0.5f); expect(0,0); // first sample snaps
    // Reverse actor draw order and use a different graphics arena.
    begin(1,2,0,1);
    record(3,&actorB,&siteA,160);
    record(4,&actorA,&siteA,60); record(5,&actorA,&siteA,70);
    OotPspFrameInterpolation_End();
    memset(arena[0],0xff,3*sizeof(arena[0])); // old arena may already be reused
    for (int pass=1;pass<=3;pass++) {
        OotPspFrameInterpolation_SetFraction(pass/3.0f);
        expect(3,100+20*pass); expect(4,20*pass); expect(5,10+20*pass);
    }
    assert(arena[4][3][0]==60); // Resolve never patches the display-list matrix
    // An allocation reused by another actor must not inherit its pose.
    OotPspFrameInterpolation_ForgetIdentity(&actorB);
    begin(1,2,0,1);
    record(6,&actorA,&siteA,120); // topology changed: 2 limbs -> 1
    record(7,&actorB,&siteA,220);
    record(8,&actorA,&siteB,999); // new draw site
    OotPspFrameInterpolation_End();
    OotPspFrameInterpolation_SetFraction(0.5f);
    expect(6,120); expect(7,220); expect(8,999);
    // Context changes, including changes during Update, invalidate matching.
    begin(1,2,0,1);
    record(9,&actorA,&siteA,180);
    OotPspFrameInterpolation_SetContext(1,2,1);
    OotPspFrameInterpolation_End();
    OotPspFrameInterpolation_SetFraction(0.5f); expect(9,180);
    begin(1,3,1,1); record(10,&actorA,&siteA,240);
    OotPspFrameInterpolation_End(); OotPspFrameInterpolation_SetFraction(0.5f); expect(10,240);
    begin(2,3,1,1); record(11,&actorA,&siteA,300);
    OotPspFrameInterpolation_End(); OotPspFrameInterpolation_SetFraction(0.5f); expect(11,300);
    // Teleports and 180-degree flips snap rather than collapse the skeleton.
    begin(2,3,1,1); record(12,&actorA,&siteA,5000);
    OotPspFrameInterpolation_End(); OotPspFrameInterpolation_SetFraction(0.5f); expect(12,5000);
    begin(2,3,1,1); record(13,&actorA,&siteA,5060);
    arena[13][0][0]=-1; arena[13][2][2]=-1;
    OotPspFrameInterpolation_Record(arena[13],arena[13],&siteA);
    OotPspFrameInterpolation_End(); OotPspFrameInterpolation_SetFraction(0.5f); expect(13,5060);
    // Disabled scope excludes HUD and UI matrices; disabled capture severs history.
    begin(2,3,1,1); OotPspFrameInterpolation_SetWorldScope(0);
    record(14,&actorA,&siteA,5120); OotPspFrameInterpolation_End();
    assert(!OotPspFrameInterpolation_HasSample());
    begin(2,3,1,0); record(15,&actorA,&siteA,5180); OotPspFrameInterpolation_End();
    assert(!OotPspFrameInterpolation_HasSample());
    begin(2,3,1,1); record(16,&actorA,&siteA,5240); OotPspFrameInterpolation_End();
    OotPspFrameInterpolation_SetFraction(0.5f); expect(16,5240);
    // A later conversion at the same address updates one entry, not limb count.
    begin(2,3,1,1); record(17,&actorA,&siteA,5250); record(17,&actorA,&siteA,5300);
    OotPspFrameInterpolation_End(); OotPspFrameInterpolation_SetFraction(0.5f); expect(17,5270);
    OotPspFrameInterpolation_SetFraction(NAN); expect(17,5300);
    // Overflow is bounded and invalidates the sample instead of blending partial poses.
    begin(2,3,1,1);
    for (int i=0;i<2100;i++) record(i,&actorA,&siteA,i);
    OotPspFrameInterpolation_End(); assert(!OotPspFrameInterpolation_HasSample());
    begin(2,3,1,1); record(18,&actorA,&siteA,5360); OotPspFrameInterpolation_End();
    OotPspFrameInterpolation_SetFraction(0.5f); expect(18,5360);
    OotPspFrameInterpolation_Reset(); expect(18,5360);
    puts("Frame interpolation: identity, arena reuse, poses, context, discontinuities, UI, overflow passed");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-interpolation-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(TEST)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(PORT), str(path/'test.c'), str(PORT/'oot_psp_frame_interpolation.c'),
                    '-lm', '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
