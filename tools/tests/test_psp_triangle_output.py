#!/usr/bin/env python3
"""Compare the new shade output loop byte-for-byte with the general emitter."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'src/port/psp/gfx/gfx_fast3d.c').read_text()

def function(name):
    start = re.search(r'^static [^\n]*\b'+name+r'\([^;]*?\)\s*\{', source, re.M).start()
    return source[start:source.index('\n}', source.index('{',start))+2]+'\n'

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GFX_DL_HANDLER __attribute__((noinline))
#define CC_0 0
#define CC_PRIM 3
#define CC_SHADE 4
#define CC_ENV 5
#define CC_LOD 6
struct RGBA { uint8_t r,g,b,a; };
static const struct RGBA white_color={255,255,255,255};
struct ColorCombiner { unsigned vertex_color_source[2]; };
struct LoadedVertex {
    float x,y,z,w,_x,_y,_z,_w,u,v;
    struct RGBA color;
    uint8_t clip_rej,fog_alpha;
    uint16_t decal_lifted;
    float normal[3];uint32_t lighting;
};
typedef struct { float u,v;struct RGBA color;float normal[3];float x,y,z; } psp_fast_t;
typedef struct { float u,v;uint8_t alpha; } psp_uv_t;
typedef struct { struct RGBA color;float x,y,z; } psp_fog_color_t;
typedef struct { float u,v;struct RGBA color;float x,y,z; } psp_fog_textured_t;
static psp_fast_t buf_vbo[32];
static psp_uv_t buf_vbo_tex1[32];
static uint8_t buf_vbo_fog_tex1_alpha[32];
static union { psp_fog_color_t color[32];psp_fog_textured_t textured[32]; } buf_vbo_fog;
static size_t buf_num_vert,buf_vbo_len;
static bool sBatchHasVisibleFog;
static struct { struct RGBA prim_color,env_color,fog_color;uint8_t prim_lod_frac;
    bool combine_color_mul_env,combine_color_mul_prim,combine_alpha_mul_env,
         combine_texture_tint_uses_prim_lod,combine_texture_tint_uses_env_alpha; } rdp;
static bool pipelineDirty;
static void gfx_mark_tri_pipeline_dirty(void) { pipelineDirty=true; }
struct TriPipelineState {
    struct ColorCombiner* comb;
    float tex_u_scale[2],tex_v_scale[2],tex_u_bias[2],tex_v_bias[2];
    bool use_alpha,use_texture,use_fog,fog_uses_texture_alpha,texture_tint_colors_corrected,
         color_mul_env,color_mul_prim,alpha_mul_env,two_texture_blend,
         two_texture_blend_uses_prim_lod,two_texture_alpha_blend,two_texture_uncompensated_alpha,arrow_glow;
    struct RGBA texture_tint_env_color;
};
'''
common = ''.join(function(name) for name in ('gfx_get_vertex_color','gfx_color_mul_channel',
    'gfx_arrow_shimmer_alpha','gfx_color_mul_env','gfx_color_mul_prim',
    'gfx_two_texture_blend_pass_alphas','gfx_get_vertex_rgba'))
helpers = function('gfx_prepare_color_multipliers')+function('gfx_dp_set_env_color')
helpers += function('gfx_get_constant_vertex_rgba')
helpers += function('gfx_initialize_fog_prefix')
helpers += function('gfx_emit_simple_base_vertex')+function('gfx_emit_simple_vertices')
general = function('gfx_emit_general_vertices')
loop = general[general.index('    size_t i;'):general.rindex('\n}')]
emitter = r'''
static void reference_emit(struct LoadedVertex* const* clipped_vertices,size_t clipped_vertices_num,
                           const struct TriPipelineState* state) {
    struct ColorCombiner* comb=state->comb;
    bool use_alpha=state->use_alpha,use_texture=state->use_texture;
    const float *tex_u_scale=state->tex_u_scale,*tex_v_scale=state->tex_v_scale;
    const float *tex_u_bias=state->tex_u_bias,*tex_v_bias=state->tex_v_bias;
    struct RGBA constantColor={0};
    const bool hasConstantColor=false;
'''
# The reference eagerly writes every fog vertex, as before the optimization.
# Keep it independent of lazy prefix initialization and visibility gating.
reference = emitter + loop.replace('gfx_initialize_fog_prefix(state, buf_num_vert);', '(void)0;').replace(
    'if (sBatchHasVisibleFog) {', 'if (true) {').replace(
    'if (state->use_fog && (sBatchHasVisibleFog || vertex->fog_alpha != 0)) {',
    'if (state->use_fog) {') + '}\n'
reference += general.replace('gfx_emit_general_vertices(', 'general_emit(')
tests = r'''
static uint32_t seed=124893;
static uint32_t next(void) { seed=seed*1664525U+1013904223U;return seed; }
static float value(void) { return ((int)(next()%65536)-32768)*0.03125f; }
static void clear(size_t prefix,bool fog,bool textured) {
    memset(buf_vbo,0xcc,sizeof(buf_vbo));memset(&buf_vbo_fog,0xcc,sizeof(buf_vbo_fog));
    memset(buf_vbo_tex1,0xcc,sizeof(buf_vbo_tex1));
    memset(buf_vbo_fog_tex1_alpha,0xcc,sizeof(buf_vbo_fog_tex1_alpha));
    struct RGBA color=rdp.fog_color;color.a=0;
    for (size_t i=0;i<prefix;i++) {
        if (textured) buf_vbo_fog.textured[i]=(psp_fog_textured_t){buf_vbo[i].u,buf_vbo[i].v,color,
            buf_vbo[i].x,buf_vbo[i].y,buf_vbo[i].z};
        else buf_vbo_fog.color[i]=(psp_fog_color_t){color,buf_vbo[i].x,buf_vbo[i].y,buf_vbo[i].z};
    }
    buf_num_vert=prefix;buf_vbo_len=prefix*sizeof(psp_fast_t);sBatchHasVisibleFog=fog;
}
int main(void) {
    struct LoadedVertex input[21],*vertices[21];
    psp_fast_t expected[32]; unsigned char expectedFog[sizeof(buf_vbo_fog)],initialFog[sizeof(buf_vbo_fog)];
    struct ColorCombiner comb={{CC_SHADE,CC_SHADE}};
    // Environment factors are baked into the prepared flags now. White ->
    // tinted -> white transitions must invalidate state without a new combiner.
    struct TriPipelineState colorState={0};
    rdp.combine_color_mul_env=rdp.combine_alpha_mul_env=true;
    gfx_dp_set_env_color(255,255,255,255);
    assert(pipelineDirty);pipelineDirty=false;
    gfx_prepare_color_multipliers(&colorState);
    assert(!colorState.color_mul_env && !colorState.alpha_mul_env);
    gfx_dp_set_env_color(90,0,140,127);
    assert(pipelineDirty);pipelineDirty=false;
    gfx_prepare_color_multipliers(&colorState);
    assert(colorState.color_mul_env && colorState.alpha_mul_env);
    gfx_dp_set_env_color(255,255,255,255);
    assert(pipelineDirty);pipelineDirty=false;
    gfx_prepare_color_multipliers(&colorState);
    assert(!colorState.color_mul_env && !colorState.alpha_mul_env);
    gfx_dp_set_env_color(255,255,255,255);assert(!pipelineDirty);
    for (unsigned iteration=0;iteration<100000;iteration++) {
        size_t count=3*(1+iteration%7),prefix=3*(iteration%3);
        bool priorFog=(iteration%11)==0;
        struct TriPipelineState state={.comb=&comb,.use_alpha=(iteration&1)!=0,
            .use_texture=(iteration&2)!=0,.use_fog=(iteration&4)!=0,
            .fog_uses_texture_alpha=(iteration&8)!=0,
            .tex_u_scale={value(),value()},.tex_v_scale={value(),value()},
            .tex_u_bias={value(),value()},.tex_v_bias={value(),value()}};
        comb.vertex_color_source[1]=state.use_alpha ? CC_SHADE : iteration%7;
        for (size_t i=0;i<count;i++) {
            input[i]=(struct LoadedVertex){.x=value(),.y=value(),.z=value(),.w=value(),
                .u=value(),.v=value(),.normal={value(),value(),value()},
                .color={next(),next(),next(),next()},.fog_alpha=(iteration%9)==0 ? 0 : next()};
            vertices[i]=&input[i];
        }
        rdp.fog_color=(struct RGBA){next(),next(),next(),next()};
        clear(prefix,priorFog,state.fog_uses_texture_alpha);reference_emit(vertices,count,&state);
        memcpy(expected,buf_vbo,sizeof(expected));memcpy(expectedFog,&buf_vbo_fog,sizeof(expectedFog));
        bool expectedVisible=sBatchHasVisibleFog;
        clear(prefix,priorFog,state.fog_uses_texture_alpha);
        memcpy(initialFog,&buf_vbo_fog,sizeof(initialFog));
        assert(gfx_emit_simple_vertices(vertices,count,&state));
        assert(memcmp(expected,buf_vbo,sizeof(expected))==0);
        if (expectedVisible) assert(memcmp(expectedFog,&buf_vbo_fog,sizeof(expectedFog))==0);
        else assert(memcmp(initialFog,&buf_vbo_fog,sizeof(initialFog))==0);
        assert(buf_num_vert==prefix+count && buf_vbo_len==(prefix+count)*sizeof(psp_fast_t));
        assert(sBatchHasVisibleFog==expectedVisible);
        // All special materials must retain the general emitter with no writes.
        clear(prefix,priorFog,state.fog_uses_texture_alpha);memcpy(expected,buf_vbo,sizeof(expected));
        memcpy(expectedFog,&buf_vbo_fog,sizeof(expectedFog));
        switch (iteration%8) {
            case 0:state.color_mul_env=true;break;
            case 1:state.color_mul_prim=true;break;
            case 2:state.alpha_mul_env=true;break;
            case 3:state.two_texture_blend=true;break;
            case 4:state.texture_tint_colors_corrected=true;break;
            case 5:comb.vertex_color_source[0]=CC_LOD;break;
            case 6:comb.vertex_color_source[0]=CC_ENV;break;
            case 7:state.use_alpha=true;comb.vertex_color_source[1]=CC_PRIM;break;
        }
        assert(!gfx_emit_simple_vertices(vertices,count,&state));
        assert(memcmp(expected,buf_vbo,sizeof(expected))==0);
        assert(memcmp(expectedFog,&buf_vbo_fog,sizeof(expectedFog))==0);
        assert(buf_num_vert==prefix && buf_vbo_len==prefix*sizeof(psp_fast_t));
        assert(sBatchHasVisibleFog==priorFog);
        comb.vertex_color_source[0]=CC_SHADE;
    }
    // Compare the original material flags against the prepared flags through
    // actual vertex output. White factors must unlock the shade loop without
    // changing RGB, alpha, UVs, normals or either fog stream.
    comb.vertex_color_source[0]=comb.vertex_color_source[1]=CC_SHADE;
    for (unsigned iteration=0;iteration<10000;iteration++) {
        size_t count=3*(1+iteration%7);
        struct TriPipelineState state={.comb=&comb,.use_alpha=(iteration&1)!=0,
            .use_texture=(iteration&2)!=0,.use_fog=(iteration&4)!=0,
            .fog_uses_texture_alpha=(iteration&8)!=0,
            .color_mul_env=true,.color_mul_prim=true,.alpha_mul_env=true,
            .tex_u_scale={0.25f},.tex_v_scale={0.5f},.tex_u_bias={0.1f},.tex_v_bias={-0.5f}};
        rdp.combine_color_mul_env=rdp.combine_color_mul_prim=rdp.combine_alpha_mul_env=true;
        rdp.env_color=(iteration&16) ? (struct RGBA){255,255,255,255} :
                                      (struct RGBA){next(),next(),next(),next()};
        rdp.prim_color=(iteration&32) ? (struct RGBA){255,255,255,255} :
                                       (struct RGBA){next(),next(),next(),next()};
        for (size_t i=0;i<count;i++) {
            input[i]=(struct LoadedVertex){.x=value(),.y=value(),.z=value(),.w=value(),
                .u=value(),.v=value(),.normal={value(),value(),value()},
                .color={next(),next(),next(),next()},.fog_alpha=next()};
            vertices[i]=&input[i];
        }
        clear(0,false,state.fog_uses_texture_alpha);reference_emit(vertices,count,&state);
        memcpy(expected,buf_vbo,sizeof(expected));memcpy(expectedFog,&buf_vbo_fog,sizeof(expectedFog));
        bool expectedVisible=sBatchHasVisibleFog;
        gfx_prepare_color_multipliers(&state);
        clear(0,false,state.fog_uses_texture_alpha);
        bool simple=gfx_emit_simple_vertices(vertices,count,&state);
        if (!simple) general_emit(vertices,count,&state);
        if ((iteration&48)==48) assert(simple);
        assert(memcmp(expected,buf_vbo,sizeof(expected))==0);
        if (expectedVisible) assert(memcmp(expectedFog,&buf_vbo_fog,sizeof(expectedFog))==0);
        assert(sBatchHasVisibleFog==expectedVisible && buf_num_vert==count);
    }
    // Mixed batches cross from clear polygons to visible fog and back while
    // alternating the shade and general emitters. The first foggy polygon
    // starts with a fully transparent vertex, so the general path must also
    // backfill earlier vertices from its own partially emitted polygon.
    for (unsigned mode=0;mode<16;mode++) {
        struct TriPipelineState state={.comb=&comb,.use_alpha=true,.use_texture=true,.use_fog=true,
            .fog_uses_texture_alpha=(mode&1)!=0,.tex_u_scale={0.25f},.tex_v_scale={0.5f}};
        rdp.env_color=(struct RGBA){90,60,127,255};
        rdp.fog_color=(struct RGBA){42,85,128,255};
        for (unsigned i=0;i<21;i++) {
            input[i]=(struct LoadedVertex){.x=value(),.y=value(),.z=value(),.u=value(),.v=value(),
                .normal={value(),value(),value()},.color={next(),next(),next(),255},
                .fog_alpha=i>=12 && i<15 ? 128 : 0};
            vertices[i]=&input[i];
        }
        input[12].color.a=0;
        comb.vertex_color_source[1]=(mode&2) ? CC_0 : CC_SHADE;
        clear(0,false,state.fog_uses_texture_alpha);
        for (unsigned part=0;part<7;part++) {
            state.color_mul_env=((part+mode)&1)!=0;
            reference_emit(vertices+part*3,3,&state);
        }
        memcpy(expected,buf_vbo,sizeof(expected));memcpy(expectedFog,&buf_vbo_fog,sizeof(expectedFog));
        assert(sBatchHasVisibleFog);
        clear(0,false,state.fog_uses_texture_alpha);
        for (unsigned part=0;part<7;part++) {
            state.color_mul_env=((part+mode)&1)!=0;
            if (!gfx_emit_simple_vertices(vertices+part*3,3,&state)) general_emit(vertices+part*3,3,&state);
            if (part<4) {
                assert(!sBatchHasVisibleFog);
                unsigned char* fogBytes=(unsigned char*)&buf_vbo_fog;
                for (size_t i=0;i<sizeof(buf_vbo_fog);i++) assert(fogBytes[i]==0xcc);
            }
        }
        assert(buf_num_vert==21 && sBatchHasVisibleFog);
        assert(memcmp(expected,buf_vbo,sizeof(expected))==0);
        assert(memcmp(expectedFog,&buf_vbo_fog,sizeof(expectedFog))==0);
    }
    // Compare fixed material folding with the original per-vertex color
    // calculation, including independent shade/LOD alpha, corrected tints,
    // rounding order, clipped polygons and both two-texture fog passes.
    psp_uv_t expectedTex1[32];
    uint8_t expectedFogAlpha[32];
    unsigned folded=0;
    const unsigned sources[]={CC_0,CC_PRIM,CC_ENV,CC_SHADE,CC_LOD};
    for (unsigned iteration=0;iteration<50000;iteration++) {
        size_t count=3*(1+iteration%7);
        struct TriPipelineState state={.comb=&comb,
            .use_alpha=(iteration&1)!=0,.use_texture=(iteration&2)!=0,
            .use_fog=(iteration&4)!=0,.fog_uses_texture_alpha=(iteration&8)!=0,
            .texture_tint_colors_corrected=(iteration&16)!=0,
            .color_mul_env=(iteration&32)!=0,.color_mul_prim=(iteration&64)!=0,
            .alpha_mul_env=(iteration&128)!=0,.two_texture_blend=(iteration&256)!=0,
            .two_texture_alpha_blend=(iteration&512)!=0,.two_texture_uncompensated_alpha=(iteration&1024)!=0,
            .two_texture_blend_uses_prim_lod=(iteration&2048)!=0,.arrow_glow=(iteration&4096)!=0,
            .texture_tint_env_color={next(),next(),next(),next()},
            .tex_u_scale={value(),value()},.tex_v_scale={value(),value()},
            .tex_u_bias={value(),value()},.tex_v_bias={value(),value()}};
        comb.vertex_color_source[0]=sources[iteration%5];
        comb.vertex_color_source[1]=sources[(iteration/5)%5];
        rdp.prim_color=(struct RGBA){next(),next(),next(),next()};
        rdp.env_color=(struct RGBA){next(),next(),next(),next()};
        rdp.fog_color=(struct RGBA){next(),next(),next(),next()};rdp.prim_lod_frac=next();
        for (size_t i=0;i<count;i++) {
            input[i]=(struct LoadedVertex){.x=value(),.y=value(),.z=value(),.w=next()%12000,
                .u=value(),.v=value(),.normal={value(),value(),value()},
                .color={next(),next(),next(),next()},.fog_alpha=i<count/2 ? 0 : next()};
            vertices[i]=&input[i];
        }
        struct RGBA fixed;
        folded+=gfx_get_constant_vertex_rgba(&state,&fixed);
        clear(0,false,state.fog_uses_texture_alpha);reference_emit(vertices,count,&state);
        memcpy(expected,buf_vbo,sizeof(expected));memcpy(expectedFog,&buf_vbo_fog,sizeof(expectedFog));
        memcpy(expectedTex1,buf_vbo_tex1,sizeof(expectedTex1));
        memcpy(expectedFogAlpha,buf_vbo_fog_tex1_alpha,sizeof(expectedFogAlpha));
        bool visible=sBatchHasVisibleFog;
        clear(0,false,state.fog_uses_texture_alpha);general_emit(vertices,count,&state);
        assert(memcmp(expected,buf_vbo,sizeof(expected))==0);
        assert(memcmp(expectedTex1,buf_vbo_tex1,sizeof(expectedTex1))==0);
        if (visible) {
            assert(memcmp(expectedFog,&buf_vbo_fog,sizeof(expectedFog))==0);
            assert(memcmp(expectedFogAlpha,buf_vbo_fog_tex1_alpha,sizeof(expectedFogAlpha))==0);
        }
        assert(sBatchHasVisibleFog==visible && buf_num_vert==count);
    }
    assert(folded>0 && folded<50000);
    puts("Triangle output: 100000 shade/UV/fog equivalence cases, clipped polygons and material fallback passed");
    puts("Fixed material colors: 50000 per-vertex reference comparisons passed, including shade/LOD alpha and multipass fog");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-triangle-output-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(preamble+common+helpers+reference+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-ffp-contract=off',
                    '-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
