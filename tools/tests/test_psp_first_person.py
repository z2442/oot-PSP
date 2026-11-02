#!/usr/bin/env python3
"""Check Link's first-person model selection independently of PSP clipping."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
player = (ROOT / "src/overlays/actors/ovl_player_actor/z_player.c").read_text()
library = (ROOT / "src/code/z_player_lib.c").read_text()


def function(source, name):
    start = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", start) + 2] + "\n"


helper = function(player, "Player_GetPspHeadClipZ")
callback = function(library, "Player_OverrideLimbDrawGameplayFirstPerson")
start = player.index("        if (this->unk_6AD != 0) {", player.index("void Player_Draw("))
selection = player[start:player.index("        } else if (this->stateFlags2 & PLAYER_STATE2_CRAWLING)", start)] + "        }\n"
header = (ROOT / "include/player.h").read_text()
limbs = re.search(r"typedef enum PlayerLimb \{.*?\} PlayerLimb;", header, re.S)[0]
perspective = function((ROOT / "src/libultra/gu/perspective.c").read_text(), "guPerspectiveF")
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define PLATFORM_PSP 1
#define GU_PI 3.14159265358979323846f
typedef float f32;
typedef double f64;
typedef int s32;
typedef uint16_t u16;
typedef struct { float x,y,z; } Vec3f;
typedef struct { int16_t x,y,z; } Vec3s;
typedef struct { float mf[4][4]; } MtxF;
typedef MtxF Mtx;
typedef struct { int id; } Gfx;
typedef struct { struct { Vec3f pos; } focus; } Actor;
typedef struct { Actor actor; uint8_t unk_6AD; bool hookshot; } Player;
typedef struct { struct { float zNear,zFar,scale; Mtx viewing,projection; } view; } PlayState;
typedef s32 (*OverrideLimbDrawOpa)(PlayState*,s32,Gfx**,Vec3f*,Vec3s*,void*);
static struct { struct { int linkAge; } save; } gSaveContext;
static Gfx meshes[11];
static Gfx* sFirstPersonLeftForearmDLs[]={meshes,meshes+1};
static Gfx* sFirstPersonLeftHandDLs[]={meshes+2,meshes+3};
static Gfx* sFirstPersonRightShoulderDLs[]={meshes+4,meshes+5};
static Gfx* sFirstPersonForearmDLs[]={meshes+6,meshes+7};
static Gfx* sFirstPersonRightHandHoldingWeaponDLs[]={meshes+8,meshes+9};
static Gfx* gLinkAdultRightHandHoldingHookshotFarDL=meshes+10;
static bool Player_HoldsHookshot(Player* p) { return p->hookshot; }
static s32 Player_OverrideLimbDrawGameplayCommon(PlayState* p,s32 i,Gfx** d,Vec3f* v,Vec3s* r,void* t) {
    (void)p;(void)i;(void)d;(void)v;(void)r;(void)t;return false;
}
#define Player_OverrideLimbDrawGameplayDefault Player_OverrideLimbDrawGameplayCommon
static void Matrix_MtxToMtxF(Mtx* m,MtxF* f) { *f=*m; }
static void SkinMatrix_Vec3fMtxFMultXYZ(MtxF* m,Vec3f* p,Vec3f* out) {
    out->x=p->x*m->mf[0][0]+p->y*m->mf[1][0]+p->z*m->mf[2][0]+m->mf[3][0];
    out->y=p->x*m->mf[0][1]+p->y*m->mf[1][1]+p->z*m->mf[2][1]+m->mf[3][1];
    out->z=p->x*m->mf[0][2]+p->y*m->mf[1][2]+p->z*m->mf[2][2]+m->mf[3][2];
}
static void guMtxIdentF(float m[4][4]) {
    memset(m,0,16*sizeof(float));for(unsigned i=0;i<4;i++) m[i][i]=1;
}
static void OotPspVfpu_SinCos(float a,float* s,float* c) { *s=sinf(a);*c=cosf(a); }
static void quantize(float m[4][4]) {
    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<4;j++) m[i][j]=(int32_t)(m[i][j]*65536)/65536.0f;
}
'''
tests = r'''
static OverrideLimbDrawOpa select_model(PlayState* play,Player* this) {
    OverrideLimbDrawOpa overrideLimbDraw=Player_OverrideLimbDrawGameplayDefault;
''' + selection + r'''
    return overrideLimbDraw;
}
int main(void) {
    PlayState play={0};Player link={0};unsigned comparisons=0,regressions=0;
    const float nears[]={5,10,20},fars[]={1000,12800,32700},scales[]={0.5f,1,2};
    for(unsigned n=0;n<3;n++) for(unsigned f=0;f<3;f++) for(unsigned s=0;s<3;s++) {
        play.view.zNear=nears[n];play.view.zFar=fars[f];play.view.scale=scales[s];
        MtxF reference;guPerspectiveF(reference.mf,NULL,60,4.0f/3,nears[n],fars[f],scales[s]);
        quantize(reference.mf);
        // Sweep camera rotation and the transition from outside Link's head
        // to inside it, while the actual render projection has near=1.
        guPerspectiveF(play.view.projection.mf,NULL,60,4.0f/3,1,fars[f],scales[s]);
        quantize(play.view.projection.mf);
        for(unsigned angle=0;angle<7;angle++) {
            float a=angle*0.73f;
            guMtxIdentF(play.view.viewing.mf);
            play.view.viewing.mf[0][2]=sinf(a);play.view.viewing.mf[2][2]=cosf(a);
            play.view.viewing.mf[3][2]=17;
            quantize(play.view.viewing.mf);
            for(int step=-200;step<=400;step++) {
                link.actor.focus.pos=(Vec3f){step*0.17f,52,step*0.23f};
                Vec3f viewHead;SkinMatrix_Vec3fMtxFMultXYZ(&play.view.viewing,&link.actor.focus.pos,&viewHead);
                float expected=viewHead.z*reference.mf[2][2]+reference.mf[3][2];
                float broken=viewHead.z*play.view.projection.mf[2][2]+play.view.projection.mf[3][2];
                assert(fabsf(Player_GetPspHeadClipZ(&play,&link)-expected)<0.0001f);
                for(unsigned mode=0;mode<=2;mode++) {
                    link.unk_6AD=mode;
                    bool hidden=select_model(&play,&link)==Player_OverrideLimbDrawGameplayFirstPerson;
                    assert(hidden==(mode!=0 && expected < -4));comparisons++;
                    if(mode && expected < -4 && broken >= -4) regressions++;
                }
            }
        }
    }
    assert(regressions>0);
    // The selected real callback hides head/hair/hat and keeps the aiming
    // arms/weapon, for both ages. Looking hides the full model as before.
    guMtxIdentF(play.view.viewing.mf);link.actor.focus.pos=(Vec3f){0,0,-2};
    play.view.zNear=10;play.view.zFar=12800;play.view.scale=1;
    for(int age=0;age<2;age++) for(int mode=1;mode<=2;mode++) for(int hook=0;hook<=1;hook++) {
        gSaveContext.save.linkAge=age;link.unk_6AD=mode;link.hookshot=hook;
        OverrideLimbDrawOpa draw=select_model(&play,&link);
        for(int limb=PLAYER_LIMB_ROOT;limb<PLAYER_LIMB_MAX;limb++) {
            Gfx* mesh=meshes;Vec3f pos={0};Vec3s rot={0};
            draw(&play,limb,&mesh,&pos,&rot,&link);
            bool arm=limb==PLAYER_LIMB_L_FOREARM || limb==PLAYER_LIMB_L_HAND ||
                     limb==PLAYER_LIMB_R_SHOULDER || limb==PLAYER_LIMB_R_FOREARM || limb==PLAYER_LIMB_R_HAND;
            assert((mesh!=NULL)==(mode==2 && arm));
        }
    }
    printf("First-person model: %u selections passed; %u head/hair visibility regressions reproduced; looking/aiming limbs passed for both ages\n",comparisons,regressions);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-first-person-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + limbs + perspective + helper + callback + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
