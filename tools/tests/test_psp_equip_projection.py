#!/usr/bin/env python3
"""Check the real HUD projection with the equip icon's z=0 vertices."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/code/z_parameter.c").read_text()
start = source.index("void func_8008A994(")
setup = source[start:source.index("\n}", start) + 2]
ortho = (ROOT / "src/libultra/gu/ortho.c").read_text()
ortho = ortho[ortho.index("void guOrthoF("):ortho.index("\nvoid guOrtho(")]
actor = (ROOT / "src/code/z_actor.c").read_text()
reticle_z = re.search(r"^#define LOCK_ON_RETICLE_Z .*$", actor, re.M)[0]
assert "Matrix_Translate(reticle->pos.x, reticle->pos.y, LOCK_ON_RETICLE_Z," in actor

preamble = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define PLATFORM_PSP 1
typedef float f32;
typedef int s32;
typedef struct { f32 zNear,zFar; int fullscreen; } View;
typedef struct { View view; } InterfaceContext;
static f32 projection[4][4];
static void guMtxIdentF(f32 m[4][4]) {
    memset(m,0,16*sizeof(f32));
    for (int i=0;i<4;i++) m[i][i]=1;
}
#define SET_FULLSCREEN_VIEWPORT(view) ((view)->fullscreen=1)
'''
tests = r'''
static void View_ApplyOrthoToOverlay(View* view) {
    assert(view->fullscreen);
    guOrthoF(projection,-160,160,-120,120,view->zNear,view->zFar,1);
}
''' + setup + "\n" + reticle_z + r'''
int main(void) {
    const float farPlanes[]={60,12800}; // A-button view and the initial HUD view.
    for (unsigned i=0;i<2;i++) {
        InterfaceContext hud={{10,farPlanes[i],0}};
        guOrthoF(projection,-160,160,-120,120,10,farPlanes[i],1);
        assert(projection[3][2] < -1); // The original z=0 icon was fully clipped.
        for (unsigned frame=0;frame<=10;frame++) {
            func_8008A994(&hud);
            assert(hud.view.zNear==10 && hud.view.zFar==farPlanes[i]);
            float reticleZ=LOCK_ON_RETICLE_Z*projection[2][2]+projection[3][2];
            assert(fabsf(reticleZ)<=1); // The old PSP z=-50 workaround clips this entire reticle.
            // Inventory slot (-108,58) to C-right (114,110), shrinking 32->24.
            float t=frame/10.0f,x=-108+(114+108)*t,y=58+(110-58)*t,size=32-8*t;
            for (unsigned vertex=0;vertex<4;vertex++) {
                float px=x+((vertex&1) ? size : 0);
                float py=y-((vertex&2) ? size : 0);
                float cx=px*projection[0][0]+projection[3][0];
                float cy=py*projection[1][1]+projection[3][1];
                float cz=0*projection[2][2]+projection[3][2];
                assert(fabsf(cx)<=1 && fabsf(cy)<=1 && fabsf(cz)<=1);
            }
        }
    }
    puts("HUD projection: equip icons and targeting reticle remain visible; perspective settings preserved");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-equip-projection-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + ortho + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
