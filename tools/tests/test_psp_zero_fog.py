#!/usr/bin/env python3
"""Check actual fog emission/flush code, including two-texture alpha passes."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()


def function(name):
    start = re.search(r"^static .*?\b" + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


flush = function("gfx_flush")
reference = flush.replace("gfx_flush(", "reference_flush(").replace(
    "rendering_state.tri_pipeline.use_fog && sBatchHasVisibleFog", "rendering_state.tri_pipeline.use_fog")
start = source.index("        if (state->use_fog", source.index("static GFX_DL_HANDLER void gfx_emit_general_vertices("))
end = source.index("        buf_num_vert++;", start)
emit = source[start:end]

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TARGET_PSP 1
#include "gfx_psp_lighting.h"
static struct GfxPspLighting sBatchLighting;
static bool sBatchUsesLighting;
void gfx_scegu_set_lighting(const struct GfxPspLighting* state) { (void)state; }
struct RGBA { uint8_t r,g,b,a; };
typedef struct { float u,v; struct RGBA color; float normal[3]; float x,y,z; } psp_fast_t;
typedef struct { float u,v; uint8_t alpha; } psp_uv_t;
typedef struct { struct RGBA color; float x,y,z; } psp_fog_color_t;
typedef struct { float u,v; struct RGBA color; float x,y,z; } psp_fog_textured_t;
union { psp_fog_color_t color[12]; psp_fog_textured_t textured[12]; } buf_vbo_fog;
static psp_fast_t buf_vbo[12];
static psp_uv_t buf_vbo_tex1[12];
static uint8_t buf_vbo_fog_tex1_alpha[12];
static size_t buf_vbo_len,buf_num_vert,buf_vbo_num_tris;
static bool sBatchHasVisibleFog;
static unsigned int sBatchDepthBias;
struct ColorCombiner { int active_texture; } comb;
struct TriPipelineState {
    bool used_textures[2];
    bool use_fog,use_texture,flame_texture_atlas,two_texture_blend,fog_uses_texture_alpha,
         two_texture_alpha_blend,two_texture_uncompensated_alpha,two_texture_blend_uses_prim_lod;
    struct ColorCombiner* comb;
};
struct Texture { unsigned texture_id,last_used_frame; } textures[2]={{1,0},{2,0}};
static struct { struct TriPipelineState tri_pipeline; struct Texture* textures[2];
    bool alpha_blend; unsigned bound_texture_id; int bound_texture_tile; } rendering_state;
static struct { struct RGBA fog_color,env_color; uint8_t prim_lod_frac; } rdp;
static unsigned sTextureCacheFrameSerial=1;
struct LoadedVertex { float x,y,z; uint8_t fog_alpha; };
typedef struct { unsigned baseCalls,fogCalls,baseHash,fogHash,visibleFogCalls,baseBias,fogBias; } Trace;
static Trace trace;
static unsigned appliedDepthBias;
static unsigned hash(const void* buffer,size_t size) {
    unsigned h=2166136261U;
    const uint8_t* bytes=buffer;
    for (size_t i=0;i<size;i++) h=(h^bytes[i])*16777619U;
    return h;
}
static void base(float* buffer,size_t bytes,size_t tris) {
    assert(tris==3); trace.baseCalls++; trace.baseHash=trace.baseHash*31+hash(buffer,bytes);
    trace.baseBias=appliedDepthBias;
}
static void fog(float* buffer,size_t bytes,size_t tris,bool textured,bool restore) {
    (void)restore; assert(tris==3);
    unsigned alpha=0;
    for (unsigned i=0;i<9;i++) alpha+=textured ? ((psp_fog_textured_t*)buffer)[i].color.a :
                                               ((psp_fog_color_t*)buffer)[i].color.a;
    trace.fogCalls++;
    trace.fogBias=appliedDepthBias;
    if (alpha) { trace.visibleFogCalls++; trace.fogHash=trace.fogHash*31+hash(buffer,bytes); }
}
static void set_alpha(bool b) { (void)b; }
static void select_texture(int slot,unsigned id) { (void)slot;(void)id; }
static void set_depth_bias(unsigned int bias) { appliedDepthBias=bias; }
static struct { void (*draw_triangles)(float*,size_t,size_t); void (*set_use_alpha)(bool);
    void (*select_texture)(int,unsigned); void (*draw_fog_triangles)(float*,size_t,size_t,bool,bool);
    void (*set_depth_bias)(unsigned int); }
    api={base,set_alpha,select_texture,fog,set_depth_bias},*gfx_rapi=&api;
'''
common = function("gfx_color_mul_channel") + function("gfx_two_texture_blend_pass_alphas")
common += function("gfx_initialize_fog_prefix")
common += "static void emit(struct LoadedVertex* vertex,uint8_t surfaceAlpha) {\n"
common += "struct TriPipelineState* state=&rendering_state.tri_pipeline;\npsp_fast_t* out=&buf_vbo[buf_num_vert];\n"
common += emit + "}\n"
common += "static void reference_emit(struct LoadedVertex* vertex,uint8_t surfaceAlpha) {\n"
common += "struct TriPipelineState* state=&rendering_state.tri_pipeline;\npsp_fast_t* out=&buf_vbo[buf_num_vert];\n"
common += emit.replace("gfx_initialize_fog_prefix(state, buf_num_vert);", "(void)0;").replace(
    "if (sBatchHasVisibleFog) {", "if (true) {").replace(
    "if (state->use_fog && (sBatchHasVisibleFog || vertex->fog_alpha != 0)) {",
    "if (state->use_fog) {") + "}\n"

tests = r'''
static bool referenceEmission;
static void prepare(unsigned mode,uint8_t alpha,uint8_t mix,bool mixed,bool transparent) {
    memset(&rendering_state,0,sizeof(rendering_state));
    rendering_state.tri_pipeline=(struct TriPipelineState){
        .use_fog=(mode&1)!=0,.use_texture=true,.two_texture_blend=(mode&2)!=0,
        .fog_uses_texture_alpha=(mode&4)!=0,.two_texture_alpha_blend=(mode&8)!=0,
        .two_texture_uncompensated_alpha=(mode&16)!=0,.two_texture_blend_uses_prim_lod=(mode&32)!=0,.comb=&comb};
    rendering_state.textures[0]=&textures[0]; rendering_state.textures[1]=&textures[1];
    rdp.fog_color=(struct RGBA){42,85,128,255}; rdp.env_color.a=mix; rdp.prim_lod_frac=mix;
    memset(buf_vbo,0,sizeof(buf_vbo)); memset(&buf_vbo_fog,0xcc,sizeof(buf_vbo_fog));
    memset(buf_vbo_fog_tex1_alpha,0xdd,sizeof(buf_vbo_fog_tex1_alpha));
    sBatchHasVisibleFog=false;
    sBatchDepthBias=0;
    for (unsigned i=0;i<9;i++) {
        buf_num_vert=i;
        buf_vbo[i]=(psp_fast_t){i,i,{200,150,100,255},{0,0,0},i,i,i};
        buf_vbo_tex1[i]=(psp_uv_t){i+10,i+20,128};
        struct LoadedVertex vertex={i,i,i,mixed && i!=8 ? 0 : alpha};
        if (referenceEmission) reference_emit(&vertex,transparent ? 0 : 255);
        else emit(&vertex,transparent ? 0 : 255);
    }
    buf_num_vert=9; buf_vbo_num_tris=3; buf_vbo_len=9*sizeof(psp_fast_t);
    trace=(Trace){0};
}
int main(void) {
    unsigned tests=0;
    const uint8_t values[]={0,1,2,127,254,255};
    for (unsigned mode=0;mode<64;mode++) for (unsigned a=0;a<6;a++) for (unsigned mix=0;mix<256;mix++) {
        for (unsigned variant=0;variant<3;variant++) {
            referenceEmission=true;
            prepare(mode,values[a],mix,variant==1,variant==2);
            reference_flush(); Trace expected=trace;
            referenceEmission=false;
            prepare(mode,values[a],mix,variant==1,variant==2);
            bool visible=sBatchHasVisibleFog;
            gfx_flush();
            assert(trace.baseCalls==expected.baseCalls && trace.baseHash==expected.baseHash);
            assert(trace.visibleFogCalls==expected.visibleFogCalls && trace.fogHash==expected.fogHash);
            assert(trace.fogCalls==(visible ? expected.fogCalls : 0));
            assert(!sBatchHasVisibleFog && buf_vbo_len==0 && buf_num_vert==0 && buf_vbo_num_tris==0);
            tests++;
        }
    }
    // One regular clear batch produces only the base draw; visible fog retains both.
    prepare(1,0,0,false,false); gfx_flush(); assert(trace.baseCalls==1 && trace.fogCalls==0);
    prepare(1,1,0,false,false); gfx_flush(); assert(trace.baseCalls==1 && trace.fogCalls==1);
    // A decal's base and fog must compare at exactly the same biased depth.
    prepare(3,100,128,false,false);sBatchDepthBias=120;gfx_flush();
    assert(trace.baseBias==120 && trace.fogBias==120 && sBatchDepthBias==0);
    prepare(1,100,0,false,false);gfx_flush();
    assert(trace.baseBias==0 && trace.fogBias==0); // No bias leaks into normal geometry.
    prepare(1,0,0,false,false); gfx_flush(); assert(trace.fogCalls==0);
    printf("Fog batches: %u comparisons passed; visible fog passes and base geometry unchanged\n",tests);
    puts("Zero-opacity fog batch: GPU draw submissions 2 -> 1");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-zero-fog-test-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(preamble + common + reference + flush + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-I", str(ROOT / "src/port/psp/gfx"), "-std=c11", "-O2", "-Wall", "-Wextra",
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
