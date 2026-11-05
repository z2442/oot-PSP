#!/usr/bin/env python3
"""Check road/grass depth separation without covering Link's feet."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()


def function(text, name):
    start = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\s*\{", text, re.M).start()
    return text[start:text.index("\n}", text.index("{", start)) + 2] + "\n"


start = source.index("    if (rendering_state.decal_mode) {", source.index("static void gfx_sp_triangles("))
batch = source[start:source.index("\n    }", start) + len("\n    }")]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#define HALF_SCREEN_WIDTH 160
#define HALF_SCREEN_HEIGHT 120
struct LoadedVertex { float x,y,z,_x,_y,_z,_w,normal[3]; uint16_t decal_lifted; };
static struct { bool decal_mode; struct { unsigned width,height; } viewport; } rendering_state;
static unsigned int sBatchDepthBias,sAppliedDepthBias=~0U,offsetCalls,guOffset;
static void sceGuDepthOffset(unsigned int bias) { guOffset=bias;offsetCalls++; }
static unsigned int flushes;
static void gfx_flush(void) { flushes++;sBatchDepthBias=0; }
static const float projection[4][4]={{1.3f,0,0,0},{0,1.7f,0,0},
    {0,0,-(12800.0f+10)/(12800-10),-1},{0,0,-2*12800.0f*10/(12800-10),0}};
'''
wrapper = r'''
static void queue(struct LoadedVertex vertices[3]) {
    struct LoadedVertex *v1=&vertices[0],*v2=&vertices[1],*v3=&vertices[2];
''' + batch + r'''
}
static struct LoadedVertex project(float x,float y,float z) {
    const float near=10,far=12800;
    return (struct LoadedVertex){.x=x,.y=y,.z=z,.normal={0,120,0},._x=x*1.3f,._y=y*1.7f,
        ._z=-z*(far+near)/(far-near)-2*far*near/(far-near),._w=-z};
}
static float ground_depth(float eyeHeight,float ndcY) {
    float w=-1.7f*eyeHeight/ndcY;
    return ((12800.0f*10/w)-10)*65535/(12800-10);
}
'''
tests = r'''
int main(void) {
    unsigned oldFailures=0,samples=0;
    const unsigned widths[]={362,480,720},heights[]={272,272,480};
    for (unsigned display=0;display<3;display++) for (unsigned camera=0;camera<3;camera++) {
        float eyeHeight=60+camera*30;
        struct LoadedVertex road[]={project(-400,-eyeHeight-0.54f,-100),
            project(400,-eyeHeight-0.54f,-100),project(400,-eyeHeight-0.54f,-1000)};
        for (unsigned i=0;i<3;i++) {
            float before=road[i].y;
            road[i].decal_lifted=gfx_lift_floor_decal(&road[i],projection);
            assert(road[i].decal_lifted && fabsf(road[i].y-before-1)<0.0001f);
            struct LoadedVertex repro=project(road[i].x,road[i].y,road[i].z);
            assert(fabsf(road[i]._y-repro._y)<0.0001f && fabsf(road[i]._z-repro._z)<0.0001f);
        }
        rendering_state.viewport.width=widths[display];rendering_state.viewport.height=heights[display];
        rendering_state.decal_mode=true;sBatchDepthBias=0;queue(road);
        unsigned bias=sBatchDepthBias;assert(bias==2);
        gfx_scegu_set_depth_bias(bias);assert(guOffset==bias);
        unsigned calls=offsetCalls;gfx_scegu_set_depth_bias(bias);assert(offsetCalls==calls);
        float slope=65535.0f*10/(eyeHeight*1.7f*(heights[display]*0.5f));
        // Scene road samples lie as much as ~0.54 units below the grass.
        // Sweep toward the bottom edge and vary raster subpixel rounding.
        for (unsigned row=0;row<100;row++) for (int jitter=-1;jitter<=1;jitter++) {
            float y=-0.15f-row*(0.85f/99);
            float grass=ground_depth(eyeHeight,y);
            float path=ground_depth(eyeHeight+0.54f-1,y);
            unsigned stored=(unsigned)roundf(grass+slope*jitter/16);
            unsigned incoming=(unsigned)roundf(path-slope*jitter/16);
            if ((unsigned)roundf(ground_depth(eyeHeight+0.54f,y)-slope*jitter/16)+32<stored) oldFailures++;
            assert(incoming+guOffset>=stored);
            // Feet just two world units above the ground must win, including
            // far away where the previous screen-depth offset grew too large.
            unsigned foreground=(unsigned)roundf(ground_depth(eyeHeight-2,y));
            assert(incoming+guOffset<foreground);
            samples++;
        }
        unsigned before=flushes;queue(road);assert(flushes==before);
        struct LoadedVertex other[3]={0};queue(other);
        assert(flushes==before+1 && sBatchDepthBias==32);
        queue(road);assert(sBatchDepthBias==2); // Never spread another decal's offset onto paths.
        rendering_state.decal_mode=false;sBatchDepthBias=0;queue(road);assert(sBatchDepthBias==0);
        gfx_scegu_set_depth_bias(0);assert(guOffset==0);
    }
    assert(oldFailures>0);
    struct LoadedVertex wall=project(0,0,-100);wall.normal[0]=120;wall.normal[1]=0;
    assert(!gfx_lift_floor_decal(&wall,projection) && wall.x==0 && wall.y==0);
    struct LoadedVertex tilted=project(1,2,-100);tilted.normal[0]=30;
    struct LoadedVertex before=tilted;
    assert(gfx_lift_floor_decal(&tilted,projection));
    float dx=tilted.x-before.x,dy=tilted.y-before.y,dz=tilted.z-before.z;
    assert(fabsf(dx*dx+dy*dy+dz*dz-1)<0.00001f);
    // Gameplay puts the camera's look-at matrix into the projection. Check
    // a matrix with off-diagonal terms and translation, including changing W.
    const float combined[4][4]={{1.2f,0.3f,0.2f,-0.4f},{0.1f,1.4f,-0.7f,-0.5f},
        {0.6f,0.2f,-1.1f,-0.8f},{12,-40,90,110}};
    struct LoadedVertex moved=project(10,20,-100);moved.normal[0]=30;
    float original[4]={moved.x,moved.y,moved.z,1},clip[4]={0};
    for (unsigned j=0;j<4;j++) for (unsigned k=0;k<4;k++) clip[j]+=original[k]*combined[k][j];
    moved._x=clip[0];moved._y=clip[1];moved._z=clip[2];moved._w=clip[3];
    assert(gfx_lift_floor_decal(&moved,combined));
    float submitted[4]={moved.x,moved.y,moved.z,1},actual[4]={moved._x,moved._y,moved._z,moved._w};
    for (unsigned j=0;j<4;j++) {
        float expected=0;for (unsigned k=0;k<4;k++) expected+=submitted[k]*combined[k][j];
        assert(fabsf(expected-actual[j])<0.0001f);
    }
    printf("Roads: %u grass/feet depth samples passed; %u fixed-bias failures reproduced; bounded lift and bias isolation passed\n",
           samples,oldFailures);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-decal-depth-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + function(source, "gfx_lift_floor_decal") +
                                  function(backend, "gfx_scegu_set_depth_bias") + wrapper + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
