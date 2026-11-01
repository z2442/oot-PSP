#!/usr/bin/env python3
"""Exercise renderer light selection, snapshot lifetime, batching and GU state.

Uses actual renderer functions with a recording GU. Image quality and GE timing
still require PSP testing; this does not emulate rasterization.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()


def function(text, name):
    start = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^;]*?\)\s*\{", text, re.M).start()
    return text[start:text.index("\n}", text.index("{", start)) + 2] + "\n"


batch = source[source.index("    /* Lighting is a load-time property"):
               source.index("    const size_t new_tri_count", source.index("    /* Lighting is a load-time property"))]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gfx_psp_lighting.h"
#define MAX_LIGHTS 7
#define MAX_VERTICES 64
#define CC_SHADE 4
typedef struct { uint8_t col[3],pad,colc[3],pad2; int8_t dir[3]; uint8_t pad3; } Light_t;
struct RGBA { uint8_t r,g,b,a; };
struct LoadedVertex { float x,y,z,w,_x,_y,_z,_w,u,v; struct RGBA color;
    uint8_t clip_rej,fog_alpha; uint16_t padding; float normal[3]; uint32_t lighting; };
static struct { unsigned current_num_lights; Light_t current_lights[8];
    struct LoadedVertex loaded_vertices[64]; } rsp;
static struct GfxPspLighting sLightingStates[MAX_VERTICES+2],sBatchLighting;
static bool sLightingStateDirty=true,sBatchUsesLighting;
static uint32_t sCurrentLighting,sBatchLightingId;
static unsigned flushes;
static struct GfxPspLighting flushed;
static void gfx_flush(void) { ++flushes; flushed=sBatchLighting; }
struct ColorCombiner { unsigned vertex_color_source[2]; };
struct TriPipelineState { bool texture_tint_colors_corrected; };
enum { GU_LIGHTING,GU_LIGHT0=11,GU_DIRECTIONAL=20,GU_AMBIENT_AND_DIFFUSE,
       GU_SINGLE_COLOR,GU_AMBIENT=1,GU_DIFFUSE=2,GU_SPECULAR=4 };
typedef struct { float x,y,z; } ScePspFVector3;
static bool enabled[32];
static unsigned colors[4][8],ambient,material,mode;
static ScePspFVector3 directions[4];
static void sceGuEnable(unsigned state) { enabled[state]=true; }
static void sceGuDisable(unsigned state) { enabled[state]=false; }
static void sceGuLightMode(unsigned value) { mode=value; }
static void sceGuColorMaterial(unsigned value) { material=value; }
static void sceGuModelColor(unsigned e,unsigned a,unsigned d,unsigned s) {
    assert(e==0 && a==0xffffffffU && d==0xffffffffU && s==0);
}
static void sceGuAmbient(unsigned value) { ambient=value; }
static void sceGuLight(unsigned i,unsigned type,unsigned components,const ScePspFVector3* direction) {
    assert(i<4 && type==GU_DIRECTIONAL && components==GU_AMBIENT_AND_DIFFUSE);
    directions[i]=*direction;
}
static void sceGuLightColor(unsigned i,unsigned component,unsigned color) { colors[i][component]=color; }
'''
tests = r'''
static void prepare_batch(struct LoadedVertex* v1,struct LoadedVertex* v2,struct LoadedVertex* v3,
                          struct ColorCombiner* comb,struct TriPipelineState* state) {
''' + batch + r'''
}
int main(void) {
    assert(sizeof(struct LoadedVertex)==64);
    assert(gfx_clamp_num_lights(0)==1 && gfx_clamp_num_lights(100)==8);
    // Seven directional lights, with ambient stored AFTER all seven, not after four.
    rsp.current_num_lights=8;
    for (unsigned i=0;i<7;i++) {
        rsp.current_lights[i].col[0]=10*(i+1);
        rsp.current_lights[i].dir[i%3]=120;
    }
    rsp.current_lights[7].col[0]=17;
    rsp.current_lights[7].col[1]=23;
    rsp.current_lights[7].col[2]=42;
    unsigned id=gfx_capture_lighting();
    struct GfxPspLighting state=sLightingStates[id];
    assert(id && state.count==4 && state.ambient==0xff2a1721U);
    for (unsigned i=0;i<4;i++) assert(state.lights[i].color==70-10*i);
    assert(gfx_capture_lighting()==id);
    // Same light state reuses its snapshot after a redundant light command.
    sLightingStateDirty=true;
    assert(gfx_capture_lighting()==id);
    rsp.loaded_vertices[0].lighting=id;
    // Zero directions and black lights never consume hardware slots; ties are stable.
    rsp.current_lights[6].dir[0]=rsp.current_lights[6].dir[1]=rsp.current_lights[6].dir[2]=0;
    rsp.current_lights[5].col[0]=0;
    rsp.current_lights[3].col[0]=50;
    sLightingStateDirty=true;
    unsigned second=gfx_capture_lighting();
    assert(second!=id);
    assert(sLightingStates[second].lights[0].direction[0]==120); // index 3 wins tie with 4
    assert(sLightingStates[second].lights[1].direction[1]==120);
    // Keep every RSP slot's light state alive while loading 64 distinct sets.
    rsp.current_num_lights=2;
    memset(rsp.loaded_vertices,0,sizeof(rsp.loaded_vertices));
    for (unsigned i=0;i<64;i++) {
        rsp.current_lights[1].col[0]=i;
        sLightingStateDirty=true;
        rsp.loaded_vertices[i].lighting=gfx_capture_lighting();
    }
    for (unsigned i=0;i<64;i++) assert((sLightingStates[rsp.loaded_vertices[i].lighting].ambient&255)==i);
    // An additional capture has a spare; a submitted batch owns an independent copy.
    sBatchLighting=sLightingStates[rsp.loaded_vertices[0].lighting];
    rsp.current_lights[1].col[0]=200; sLightingStateDirty=true;
    assert((sLightingStates[gfx_capture_lighting()].ambient&255)==200);
    assert((sBatchLighting.ambient&255)==0);
    rsp.current_num_lights=1; sLightingStateDirty=true;
    struct GfxPspLighting ambientOnly=sLightingStates[gfx_capture_lighting()];
    assert(ambientOnly.count==0);
    // Batch transitions flush the PREVIOUS light set, and do not flush identical sets.
    struct ColorCombiner comb={{CC_SHADE,CC_SHADE}};
    struct TriPipelineState pipeline={0};
    sBatchUsesLighting=false; flushes=0;
    prepare_batch(&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&comb,&pipeline);
    assert(sBatchUsesLighting && flushes==1 && (sBatchLighting.ambient&255)==0);
    prepare_batch(&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&comb,&pipeline);
    assert(flushes==1);
    prepare_batch(&rsp.loaded_vertices[0],&rsp.loaded_vertices[1],&rsp.loaded_vertices[1],&comb,&pipeline);
    assert(flushes==2 && (flushed.ambient&255)==0 && (sBatchLighting.ambient&255)==1);
    pipeline.texture_tint_colors_corrected=true;
    prepare_batch(&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&rsp.loaded_vertices[0],&comb,&pipeline);
    assert(!sBatchUsesLighting && flushes==3);
    // Recycling an unreferenced snapshot invalidates batch identity without
    // changing the queued batch's owned light values.
    unsigned released=rsp.loaded_vertices[2].lighting;
    sBatchLightingId=released; sBatchLighting=sLightingStates[released];
    rsp.loaded_vertices[2].lighting=0;
    rsp.current_num_lights=2; rsp.current_lights[1].col[0]=201;
    sLightingStateDirty=true;
    assert(gfx_capture_lighting()==released);
    assert(sBatchLightingId==0 && (sBatchLighting.ambient&255)==2);
    // GE material tracks vertex RGBA; global ambient alpha stays opaque.
    gfx_scegu_set_lighting(&state);
    assert(enabled[GU_LIGHTING] && material==(GU_AMBIENT|GU_DIFFUSE) && mode==GU_SINGLE_COLOR);
    assert(ambient==state.ambient);
    for (unsigned i=0;i<4;i++) {
        assert(enabled[GU_LIGHT0+i] && colors[i][GU_DIFFUSE]==state.lights[i].color);
        assert(colors[i][GU_AMBIENT]==0 && colors[i][GU_SPECULAR]==0);
    }
    gfx_scegu_set_lighting(&ambientOnly);
    assert(enabled[GU_LIGHTING]);
    for (unsigned i=0;i<4;i++) assert(!enabled[GU_LIGHT0+i]);
    gfx_scegu_set_lighting(NULL);
    assert(!enabled[GU_LIGHTING]);
    // Clipped normals interpolate independently from alpha; snapshot stays an integer ID.
    struct LoadedVertex a={.color={10,20,30,80},.fog_alpha=0,.normal={1,0,0},.lighting=17};
    struct LoadedVertex b={.color={30,40,50,160},.fog_alpha=200,.normal={0,1,0},.lighting=17},out;
    gfx_clip_interpolate_vert(&out,&a,&b,0.25f);
    assert(out.normal[0]==0.75f && out.normal[1]==0.25f && out.lighting==17);
    assert(out.color.a==100 && out.fog_alpha==50);
    puts("GPU lighting: strongest-light selection, snapshot lifetime, batching, alpha and GU state passed");
}
'''
code = preamble + function(source, "gfx_clamp_num_lights") + function(source, "gfx_capture_lighting")
code += function(source, "gfx_clip_interpolate_vert") + function(backend, "gfx_scegu_set_lighting") + tests
with tempfile.TemporaryDirectory(prefix="oot-gpu-lighting-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "src/port/psp/gfx"),
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
