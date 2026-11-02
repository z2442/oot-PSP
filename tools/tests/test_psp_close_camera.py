#!/usr/bin/env python3
"""Check foreground clipping and fog during doors, ocarina use and warps."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/code/z_play.c").read_text()
start = source.index("static f32 Play_GetPspPerspectiveNear(")
policy = source[start:source.index("\n}", start) + 2]
start = source.index("static void Play_GetPspFogProjection(")
fog_projection = source[start:source.index("\n}", start) + 2]
start = source.index("            f32 savedNear = this->view.zNear;", source.index("void Play_Draw("))
end = source.index("            this->view.zNear = savedNear;", start)
apply_view = source[start:end] + "            this->view.zNear = savedNear;\n"
perspective = (ROOT / "src/libultra/gu/perspective.c").read_text()
perspective = perspective[perspective.index("void guPerspectiveF("):perspective.index("\nvoid guPerspective(")]
camera_header = (ROOT / "include/camera.h").read_text()
modes = re.search(r"typedef enum CameraModeType \{.*?\} CameraModeType;", camera_header, re.S)[0]
extensions = (ROOT / "src/port/psp/oot_psp_gfx_ext.h").read_text()
fog_type = extensions[extensions.index("typedef struct OotPspFogProjection"):extensions.index("typedef enum")]
fog_macro = extensions[extensions.index("#define gOotPspSetFogProjection("):extensions.index("/* A pointer-bearing")]
renderer = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
start = renderer.index("static void gfx_set_fog_projection(")
fog_setter = renderer[start:renderer.index("\n}", start) + 2]
dispatch = renderer.split("            case G_NOOP:\n", 1)[1].split("                break;", 1)[0]
start = renderer.index("        const struct GfxVfpuTransformState transformState = {", renderer.index("static bool gfx_sp_vertex("))
factors = renderer[start:renderer.index("        };", start) + len("        };")]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define PLATFORM_PSP 1
#define GU_PI 3.14159265358979323846f
#define VIEW_ALL 15
#define CS_STATE_IDLE 0
#define G_NOOP 0xc0
#define G_FOG 1
#define G_LIGHTING 2
#define OOT_PSP_FOG_PROJECTION_CMD 0x464F47U
#define OOT_PSP_SHADOW_CMD 0x534844U
#define _SHIFTL(v,s,w) ((uint32_t)(v)<<(s))
typedef float f32;
typedef double f64;
typedef int s32;
typedef uint16_t u16;
typedef struct { float mf[4][4]; } Mtx;
typedef Mtx MtxF;
typedef struct { int mode; } Camera;
typedef struct { f32 zNear,zFar,fovy,scale; Mtx projection; } View;
typedef struct { View view; struct { f32 zFar; } lightCtx; Camera* camera;
                 struct { int state; } csCtx; bool blocking; } PlayState;
#define GET_ACTIVE_CAM(play) ((play)->camera)
static bool Play_InCsMode(PlayState* play) { return play->csCtx.state!=CS_STATE_IDLE || play->blocking; }
typedef struct { struct { uint32_t w0; uintptr_t w1; } words; } Gfx;
static Gfx opaque[16],translucent[16],overlay[16],*opa,*xlu,*ovl;
#define POLY_OPA_DISP opa
#define POLY_XLU_DISP xlu
#define OVERLAY_DISP ovl
static unsigned char allocation[16];
#define GRAPH_ALLOC(ctx,size) ((void)(ctx),assert((size)<=sizeof(allocation)),(void*)allocation)
static void Matrix_MtxToMtxF(const Mtx* m,MtxF* f) { *f=*m; }
struct GfxVfpuTransformState {
    const float (*model)[4],(*projection)[4]; float fog_mul,fog_offset;
    unsigned fog_enabled,lighting_enabled;
};
static struct { float modelview_matrix_stack[1][4][4],P_matrix[4][4];
                unsigned modelview_matrix_stack_size; int16_t fog_mul,fog_offset; } rsp={.modelview_matrix_stack_size=1};
typedef void OotPspShadowMask;
static const OotPspShadowMask* sPendingShadowMask;
static void gfx_flush(void) { assert(0); }
static void gfx_dp_noop(unsigned tag) { (void)tag;assert(0); }
static void* seg_addr(uintptr_t p) { return (void*)p; }
static float matrix[4][4],appliedNear;
static void guMtxIdentF(float m[4][4]) {
    memset(m,0,16*sizeof(float));for (unsigned i=0;i<4;i++) m[i][i]=1;
}
static void OotPspVfpu_SinCos(float a,float* s,float* c) { *s=sinf(a);*c=cosf(a); }
'''
wrapper = r'''
static void guPerspective(Mtx* m,u16* norm,float fov,float aspect,float near,float far,float scale) {
    guPerspectiveF(m->mf,norm,fov,aspect,near,far,scale);
    for (unsigned i=0;i<4;i++) for (unsigned j=0;j<4;j++)
        m->mf[i][j]=(int32_t)(m->mf[i][j]*65536)/65536.0f;
}
''' + fog_projection + r'''
static OotPspFogProjection sFogProjection={1,0};
''' + fog_setter + r'''
static void command(Gfx* cmd) {
''' + dispatch + r'''
}
static struct GfxVfpuTransformState fog_factors(int16_t mul,int16_t off) {
    unsigned geometryMode=G_FOG;rsp.fog_mul=mul;rsp.fog_offset=off;
''' + factors + r'''
    return transformState;
}
static void View_SetPerspective(View* view,float fov,float near,float far) {
    view->fovy=fov;view->zNear=near;view->zFar=far;
}
static void View_Apply(View* view,int mask) {
    assert(mask==VIEW_ALL);appliedNear=view->zNear;
    guPerspective(&view->projection,NULL,view->fovy,4.0f/3,view->zNear,view->zFar,view->scale);
    memcpy(matrix,view->projection.mf,sizeof(matrix));
}
static void apply(PlayState* this) {
    void* gfxCtx=NULL;opa=opaque;xlu=translucent;ovl=overlay;
''' + apply_view + r'''
    assert(opa==opaque+1 && xlu==translucent+1 && ovl==overlay+1);
    assert(opaque[0].words.w1==translucent[0].words.w1);
    assert(overlay[0].words.w1==0);
}
static bool visible(float distance) {
    float z=-distance*matrix[2][2]+matrix[3][2],w=distance;
    return w>0 && z>=-w && z<=w;
}
int main(void) {
    const int closeModes[]={CAM_MODE_FIRST_PERSON,CAM_MODE_AIM_CHILD,CAM_MODE_AIM_ADULT,
                            CAM_MODE_Z_AIM,CAM_MODE_AIM_BOOMERANG};
    Camera camera={CAM_MODE_NORMAL};PlayState play={.view={.zNear=10,.zFar=12800,.fovy=60,.scale=1},
                                                 .lightCtx={12800},.camera=&camera};
    apply(&play);assert(appliedNear==10 && !visible(2) && !visible(5) && visible(12));
    float normal[4][4];memcpy(normal,matrix,sizeof(normal));
    for (unsigned i=0;i<sizeof(closeModes)/sizeof(closeModes[0]);i++) {
        camera.mode=closeModes[i];apply(&play);
        assert(appliedNear==1 && play.view.zNear==10);
        assert(visible(1.5f) && visible(2) && visible(5) && visible(9) && visible(1000));
        assert(!visible(-1) && !visible(0.1f)); // Eye-plane/behind-camera rejection remains enabled.
    }
    // Doors and ocarina use block player input but do not need a shorter plane.
    camera.mode=CAM_MODE_NORMAL;play.blocking=true;assert(Play_InCsMode(&play));apply(&play);
    assert(appliedNear==10 && memcmp(matrix,normal,sizeof(normal))==0);
    play.blocking=false;play.csCtx.state=1;apply(&play);
    assert(appliedNear==1 && visible(2) && visible(5) && play.view.zNear==10);
    play.csCtx.state=CS_STATE_IDLE;apply(&play);
    assert(appliedNear==10 && memcmp(matrix,normal,sizeof(normal))==0);
    play.camera=NULL;apply(&play);assert(appliedNear==10);
    play.csCtx.state=1;apply(&play);assert(appliedNear==1);
    play.view.zNear=0.5f;apply(&play);assert(appliedNear==0.5f && play.view.zNear==0.5f);
    // Scripted warps and aiming still get the closer plane. Fog must retain
    // its original density for every room/actor factor, including zero-mul
    // full-screen fog fades, on both world streams and then reset for HUD.
    unsigned comparisons=0;
    const float fars[]={1000,12800,32700},scales[]={0.5f,1,2};
    const int16_t fog[][2]={{0,0},{0,255},{32767,-32512},{256,0},{12800,-12800},{5000,-4850}};
    for (unsigned f=0;f<3;f++) for (unsigned s=0;s<3;s++) {
        play.view.zNear=10;play.lightCtx.zFar=fars[f];play.view.scale=scales[s];
        play.csCtx.state=CS_STATE_IDLE;apply(&play);memcpy(normal,matrix,sizeof(normal));
        play.csCtx.state=1;apply(&play);
        for (unsigned stream=0;stream<2;stream++) {
            command(stream ? translucent : opaque);
            for (unsigned factor=0;factor<sizeof(fog)/sizeof(fog[0]);factor++) {
                struct GfxVfpuTransformState corrected=fog_factors(fog[factor][0],fog[factor][1]);
                for (float distance=2;distance<fars[f];distance*=1.17f) {
                    float oldZ=-distance*normal[2][2]+normal[3][2],oldW=-distance*normal[2][3];
                    float z=-distance*matrix[2][2]+matrix[3][2],w=-distance*matrix[2][3];
                    float expected=fmaxf(0,fminf(255,oldZ/oldW*fog[factor][0]+fog[factor][1]));
                    float actual=fmaxf(0,fminf(255,z/w*corrected.fog_mul+corrected.fog_offset));
                    assert(fabsf(actual-expected)<0.16f);comparisons++;
                    if (stream==0 && f==1 && s==1)
                        printf("VFPU %.9g %.9g %.9g %.9g %.9g\n",z,w,corrected.fog_mul,corrected.fog_offset,expected);
                }
            }
        }
        command(overlay);assert(sFogProjection.scale==1 && sFogProjection.offset==0);
        play.csCtx.state=CS_STATE_IDLE;apply(&play);command(opaque);
        assert(sFogProjection.scale==1 && sFogProjection.offset==0);
        assert(sPendingShadowMask==NULL);
    }
    printf("Close camera/fog: %u comparisons passed; doors/ocarina keep normal depth; warps/aiming preserve fog; overlay/normal resets passed\n",comparisons);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-close-camera-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + fog_type + fog_macro + modes + perspective + policy + wrapper)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    result = subprocess.run([str(path / "test")], check=True, text=True, capture_output=True)
    # Execute the actual transform assembly with the generated corrected fog
    # factors; this catches state ABI/conversion regressions as well as math.
    from test_psp_vertex_pipeline import Machine
    for line in result.stdout.splitlines():
        if not line.startswith("VFPU "):
            print(line)
            continue
        z,w,mul,off,expected=map(float,line.split()[1:])
        m=Machine()
        identity=[1 if i//4==i%4 else 0 for i in range(16)]
        projection=[0.0]*16;projection[14]=z;projection[15]=w
        m.store(0x4000,"16f",*identity);m.store(0x4100,"16f",*projection)
        m.store(0x3000,"IIffII",0x4000,0x4100,mul,off,1,0)
        m.store(0x2000,"hhhHhhbbbB",0,0,0,0,0,0,0,0,0,137)
        m.execute("gfx_transform_vfpu.s",(0x1000,0x2000,1,0x3000))
        assert abs(m.load(0x1000+45,"B")[0]-expected)<1.16
        assert m.load(0x1000+43,"B")[0]==137
