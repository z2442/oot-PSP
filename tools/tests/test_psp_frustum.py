#!/usr/bin/env python3
"""Check CPU/GU clip-volume agreement and roads crossing screen/near edges."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()


def section(start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
struct RGBA { uint8_t r,g,b,a; };
struct LoadedVertex {
    float x,y,z,w,_x,_y,_z,_w,u,v; struct RGBA color;
    uint8_t clip_rej,fog_alpha; uint16_t padding; float normal[3]; uint32_t lighting;
};
static bool anchored,sHudViewportFullscreen;
static float sNdcAspectScale,sHudAnchorOffsetNdc,uploaded[4][4];
static struct { float P_matrix[4][4]; } rsp;
static bool gfx_hud_anchor_enabled(void) { return anchored; }
static void gfx_upload_projection_matrix(const float m[4][4]) { memcpy(uploaded,m,sizeof(uploaded)); }
'''
projection = section("static inline __attribute__((always_inline)) void gfx_apply_projection_matrix(",
                     "static void gfx_apply_modelview_matrix(")
clip_x = section("static inline float gfx_transform_clip_x(", "static void gfx_update_screen_metrics(")
load_x = section("        float w = d->_w;", "        const float y = d->_y;")
classify = section("        uint32_t clipRej = 0;", "\n#if !defined(TARGET_PSP)\n        d->x = model_vec[0];")
clipping = section("// Bits for clipping", "static inline float vec3_dot(")
clipping += section("static inline float vec4_dot(", "static struct LoadedVertex temp_a[12];")
clipping += section("static struct LoadedVertex temp_a[12];", "static void gfx_flush(void) {")
wrappers = r'''
static float loaded_x(struct LoadedVertex* d) {
''' + load_x + r'''
    return x;
}
static void classify(struct LoadedVertex* d) {
    float x=d->_x,y=d->_y,z=d->_z,w=d->_w;
''' + classify + r'''
}
static void project(struct LoadedVertex* d) {
    d->_x=d->x*uploaded[0][0]+d->z*uploaded[2][0]+uploaded[3][0];
    d->_y=d->y*uploaded[1][1];
    d->_z=d->z*uploaded[2][2]+uploaded[3][2];
    d->_w=d->z*uploaded[2][3]+uploaded[3][3];
    d->w=d->_w;classify(d);
}
static float area(const struct LoadedVertex* a,const struct LoadedVertex* b,const struct LoadedVertex* c) {
    float ax=a->_x/a->_w,ay=a->_y/a->_w,bx=b->_x/b->_w,by=b->_y/b->_w,cx=c->_x/c->_w,cy=c->_y/c->_w;
    return fabsf((bx-ax)*(cy-ay)-(by-ay)*(cx-ax))*0.5f;
}
static void inside(const struct LoadedVertex* v) {
    assert(v->_w>0);
    float epsilon=v->_w*0.00002f;
    assert(fabsf(v->_x)<=v->_w+epsilon && fabsf(v->_y)<=v->_w+epsilon && fabsf(v->_z)<=v->_w+epsilon);
}
'''
tests = r'''
int main(void) {
    // The actual load-time clip X must match the projection uploaded to GU
    // for world geometry, fullscreen anchored HUD and sub-viewport HUD.
    const float aspects[]={1,272.0f*(4.0f/3)/480,480.0f*(4.0f/3)/720};
    for (unsigned mode=0;mode<4;mode++) for (unsigned ratio=0;ratio<3;ratio++) {
        anchored=mode&1;sHudViewportFullscreen=mode&2;sNdcAspectScale=aspects[ratio];sHudAnchorOffsetNdc=0.2f;
        memset(rsp.P_matrix,0,sizeof(rsp.P_matrix));
        for (unsigned i=0;i<4;i++) rsp.P_matrix[i][i]=1;
        gfx_apply_projection_matrix();
        for (int step=-24;step<=24;step++) {
            struct LoadedVertex d={._x=step/20.0f,._w=1};
            float gpuX=d._x*uploaded[0][0]+uploaded[3][0];
            assert(fabsf(loaded_x(&d)-gpuX)<0.000001f);
        }
    }
    // Each individual rejection bit must select the plane which rejected it.
    const uint32_t bits[]={X_POS,X_NEG,Y_POS,Y_NEG,Z_POS,Z_NEG};
    for (unsigned plane=0;plane<6;plane++) {
        struct LoadedVertex input[3]={0},scratch[12],other[12],*out;
        for (unsigned i=0;i<3;i++) input[i]._w=1;
        float* coord=plane<2 ? &input[0]._x : (plane<4 ? &input[0]._y : &input[0]._z);
        *coord=(plane&1) ? 2 : -2;
        classify(&input[0]);assert(input[0].clip_rej==bits[plane]);
        memcpy(scratch,input,sizeof(input));
        unsigned count=clip_to_frustum(scratch,other,3,bits[plane],&out);
        assert(count==4);
        for (unsigned i=0;i<count;i++) inside(&out[i]);
    }
    anchored=false;sHudViewportFullscreen=true;sNdcAspectScale=aspects[1];
    memset(rsp.P_matrix,0,sizeof(rsp.P_matrix));
    rsp.P_matrix[0][0]=1.3f;rsp.P_matrix[1][1]=1.7f;
    rsp.P_matrix[2][2]=-(4000.0f+10)/(4000-10);
    rsp.P_matrix[3][2]=-2*4000.0f*10/(4000-10);rsp.P_matrix[2][3]=-1;
    gfx_apply_projection_matrix();
    unsigned sweeps=0;
    for (unsigned pose=0;pose<3;pose++) for (int pan=-1200;pan<=1200;pan+=10) {
        struct LoadedVertex quad[4]={
            {.x=-400+pan,.y=-80,.z=pose==0 ? -50 : (pose==1 ? -5 : 20)},
            {.x=400+pan,.y=-80,.z=pose==0 ? -50 : (pose==1 ? -5 : 20)},
            {.x=400+pan,.y=-80,.z=-1000}, {.x=-400+pan,.y=-80,.z=-1000}};
        for (unsigned i=0;i<4;i++) project(&quad[i]);
        // Clip the complete coplanar quad as a reference. The two original
        // path triangles must cover the same projected area without holes.
        struct LoadedVertex a[12],b[12],*polygon;memcpy(a,quad,sizeof(quad));
        unsigned n=clip_to_frustum(a,b,4,CLIP_TEST_FLAGS,&polygon);
        float expected=0,actual=0;
        for (unsigned i=0;i<n;i++) inside(&polygon[i]);
        for (unsigned i=1;i+1<n;i++) expected+=area(&polygon[0],&polygon[i],&polygon[i+1]);
        for (unsigned triangle=0;triangle<2;triangle++) {
            struct LoadedVertex* input[]={&quad[0],&quad[triangle+1],&quad[triangle+2]};
            unsigned flags=input[0]->clip_rej | input[1]->clip_rej | input[2]->clip_rej;
            struct LoadedVertex result[21];size_t count;
            gfx_clip_single_vert(result,&count,input,flags);
            for (unsigned i=0;i<count;i++) {
                inside(&result[i]);
                // Reproject the modelview positions actually submitted to GE.
                struct LoadedVertex repro=result[i];project(&repro);inside(&repro);
                assert(fabsf(repro._x-result[i]._x)<0.002f);
            }
            for (unsigned i=0;i<count;i+=3) actual+=area(&result[i],&result[i+1],&result[i+2]);
        }
        assert(fabsf(actual-expected)<0.0001f);sweeps++;
    }
    printf("Frustum: CPU/GU agreement, all six planes and %u road edge/near-plane sweeps passed\n",sweeps);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-frustum-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + projection + clip_x + clipping + wrappers + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable", str(path / "test.c"), "-lm",
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
