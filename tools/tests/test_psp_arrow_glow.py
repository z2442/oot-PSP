#!/usr/bin/env python3
"""Exercise arrow edge coverage and Freezard mist combiner selection."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()


def section(start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


constants = "\n".join(line for line in (ROOT / "include/ultra64/gbi.h").read_text().splitlines()
                      if re.match(r"#define G_(?:CCMUX_|ACMUX_|IM_FMT_|IM_SIZ_|TX_(?:WRAP|MIRROR|CLAMP|NOMASK)\s|TEXTURE_IMAGE_FRAC|MDSFT_CYCLETYPE|CYC_)", line))
tile_type = section("typedef struct TextureTileState {", "static struct RSP {")
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include "gfx_cc.h"
#define TARGET_PSP 1
#define GFX_DL_HANDLER
''' + constants + "\n" + tile_type + r'''
static struct {
    TextureTileState texture_tile[2];
    struct { unsigned size_bytes,source_size_bytes,row_stride_bytes,source_nibble_offset; } loaded_texture[2];
    uint32_t other_mode_h,combine_mode;
    bool combine_color_mul_env,combine_color_mul_prim,combine_two_texture_blend,
         combine_two_texture_blend_uses_prim_lod,combine_two_texture_alpha_blend,
         combine_alpha_mul_env,combine_flame_texture_atlas,combine_texture_tint_uses_prim_lod,
         combine_texture_tint_uses_env_alpha,combine_arrow_glow;
    struct { uint8_t a; } env_color;
    uint8_t prim_lod_frac;
} rdp;
struct TextureHashmapNode { unsigned upload_width,upload_height; } textures[2];
struct TriPipelineState {
    float tex_u_scale[2],tex_v_scale[2],tex_u_bias[2],tex_v_bias[2];
    bool arrow_glow,two_texture_blend,two_texture_blend_uses_prim_lod,
         two_texture_alpha_blend,two_texture_uncompensated_alpha;
} coords;
static struct { struct TextureHashmapNode* textures[2]; } rendering_state;
static TextureTileState* gfx_get_texture_tile(int tile) { return &rdp.texture_tile[tile]; }
static unsigned gfx_loaded_texture_source_size(int tile) { return rdp.loaded_texture[tile].source_size_bytes; }
static void gfx_mark_tri_pipeline_dirty(void) {}
'''
combiner_type = section("struct ColorCombiner {", "#define COLOR_COMBINER_CACHE_SET_COUNT")
shader_stub = r'''
struct ShaderProgram { uint32_t id; } shader;
static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint32_t id) {
    shader.id=id;return &shader;
}
'''
generator = section("static uint8_t gfx_cc_pick_vertex_color_source(", "static inline struct RGBA gfx_get_vertex_color(")
dimensions = section("static uint32_t gfx_texture_tile_width(", "static uint32_t gfx_texture_import_width(")
combine = section("static uint8_t color_comb_component(", "static void gfx_dp_set_env_color(")
alpha_start = re.search(r"^static inline .*?void gfx_two_texture_blend_pass_alphas\([^;]*?\)\s*\{",
                        source, re.M).start()
alphas = source[alpha_start:source.index("\n}", alpha_start) + 2]
coordinates = section("static inline float gfx_texture_shift_scale(",
                       "#if defined(TARGET_PSP)\nstatic void gfx_prepare_flame_atlas_coord_state(")
edge_fade = section("static uint8_t gfx_arrow_shimmer_alpha(", "static inline void gfx_color_mul_env(")
alpha_emission_start = source.index("        if (state->two_texture_blend) {",
                                    source.index("        const uint8_t surfaceAlpha = use_alpha ?"))
alpha_emission = source[alpha_emission_start:source.index("        if (state->use_fog) {", alpha_emission_start)]
emission = r'''
static unsigned emit_alpha(uint8_t alpha,float v,bool arrow) {
    struct { struct { uint8_t a; } color; } output={.color={alpha}},*out=&output;
    struct { float v; } input={v},*vertex=&input;
    struct { uint8_t alpha; } buf_vbo_tex1[1];
    unsigned buf_num_vert=0;
    struct TriPipelineState* state=&coords;
    state->two_texture_blend=true;state->two_texture_blend_uses_prim_lod=false;
    state->two_texture_alpha_blend=true;state->two_texture_uncompensated_alpha=true;
    state->arrow_glow=arrow;rdp.env_color.a=128;
''' + alpha_emission + r'''
    return out->color.a | (buf_vbo_tex1[0].alpha<<8);
}
'''
tests = r'''
static void configure(unsigned arrow,unsigned tile,unsigned frame) {
    // Material/TwoTexScroll parameters from the three arrow actors.
    unsigned x,y,w,h,s,t;
    if (arrow==0) {
        x=255-(tile ? frame%256 : (2*frame)%256);
        y=tile ? 511-(10*frame)%512 : 0;
        w=64;h=tile ? 64 : 32;s=1;t=tile ? 0 : 15;
    } else if (arrow==1) {
        x=511-((tile ? 10 : 5)*frame)%512;
        y=tile ? 511-(10*frame)%512 : 0;
        w=tile ? 4 : 128;h=tile ? 16 : 32;s=13;t=tile ? 14 : 15;
    } else {
        x=511-((tile ? 10 : 5)*frame)%512;
        y=tile ? 511-(30*frame)%512 : 0;
        w=tile ? 8 : 4;h=tile ? 16 : 32;s=tile ? 14 : 13;t=tile ? 14 : 15;
    }
    rdp.texture_tile[tile]=(TextureTileState){
        .fmt=G_IM_FMT_I,.siz=G_IM_SIZ_8b,.line_size_bytes=32,
        .cms=G_TX_WRAP,.cmt=G_TX_MIRROR,.masks=5,.maskt=6,
        .uls=x,.ult=y,.lrs=x+(w-1)*4,.lrt=y+(h-1)*4,.shifts=s,.shiftt=t};
    rdp.loaded_texture[tile].size_bytes=rdp.loaded_texture[tile].source_size_bytes=2048;
    unsigned width,height;
    gfx_texture_import_dimensions(tile,&width,&height);
    assert(width==32 && height==64); // The scroll window must never crop/reshape the mask.
    textures[tile]=(struct TextureHashmapNode){width,height*2}; // Mirrored T upload.
    rendering_state.textures[tile]=&textures[tile];
    gfx_prepare_texture_coord_state(&coords,tile,tile,true);
    float uv=4352;
    float expectedU=(uv*gfx_texture_shift_scale(s)+16-x*8.0f)/(32*32);
    float expectedV=(uv*gfx_texture_shift_scale(t)+16-y*8.0f)/(128*32);
    assert(fabsf(uv*coords.tex_u_scale[tile]+coords.tex_u_bias[tile]-expectedU)<0.0001f);
    assert(fabsf(uv*coords.tex_v_scale[tile]+coords.tex_v_bias[tile]-expectedV)<0.0001f);
}
int main(void) {
    rdp.other_mode_h=G_CYC_2CYCLE;
    for (unsigned arrow=0;arrow<3;arrow++) {
        // Original material command words: ice differs in the first alpha subtraction.
        gfx_dp_set_combine(0xfc262a60,arrow==1 ? 0x350cf37f : 0x350ce37f);
        assert(rdp.combine_two_texture_blend && rdp.combine_two_texture_alpha_blend);
        assert(rdp.combine_arrow_glow);
        assert(!rdp.combine_two_texture_blend_uses_prim_lod && !rdp.combine_flame_texture_atlas);
        assert((rdp.combine_mode & SHADER_OPT_TEXTURE_BLEND)!=0);
        assert((rdp.combine_mode & 0xffffff)==
               (color_comb(G_CCMUX_TEXEL0,G_CCMUX_0,G_CCMUX_PRIMITIVE,G_CCMUX_0) |
                (color_comb(G_ACMUX_TEXEL0,G_ACMUX_0,G_ACMUX_PRIMITIVE,G_ACMUX_0)<<12)));
        for (unsigned frame=0;frame<512;frame++) for (unsigned tile=0;tile<2;tile++)
            configure(arrow,tile,frame);
        // Both passes remain visible when charging/flying and fade out with the actor.
        for (unsigned alpha=0;alpha<256;alpha++) {
            uint8_t base,overlay;
            gfx_two_texture_blend_pass_alphas(alpha,128,true,true,&base,&overlay);
            assert(base+overlay>=alpha && base+overlay<=alpha+1);
            if (alpha>=2) assert(base>0 && overlay>0);
            if (!alpha) assert(!base && !overlay);
            // Wide outer ring has V=5 (4 after SPTexture scaling). Its
            // secondary mask must be completely clear, independent of noise.
            assert(emit_alpha(alpha,4,true)==base);
            assert(emit_alpha(alpha,1048,true)==(unsigned)(base | (overlay<<8)));
            assert(emit_alpha(alpha,4,false)==(unsigned)(base | (overlay<<8)));
            unsigned previous=0;
            for (unsigned v=16;v<=1024;v+=16) {
                unsigned emitted=emit_alpha(alpha,v,true);
                assert((emitted&255)==base); // The original main glow is unchanged.
                unsigned shimmer=emitted>>8;
                assert(shimmer>=previous && shimmer<=overlay);previous=shimmer;
            }
        }
    }
    // The actual Freezard steam command. Its stationary TEXEL1 contains
    // cloud coverage; the old fallback exposed the three-vertex billboard.
    gfx_dp_set_combine(0xfc161a60,0xf50d657f);
    assert(!rdp.combine_arrow_glow && !rdp.combine_two_texture_blend);
    struct ColorCombiner mist;
    gfx_generate_cc(&mist,rdp.combine_mode | SHADER_OPT_ALPHA);
    assert(mist.active_texture==1 && !mist.used_textures[0] && mist.used_textures[1]);
    assert(mist.uses_texture_alpha && mist.vertex_color_source[1]==CC_PRIM && mist.texture_blend);
    struct CCFeatures features;gfx_cc_get_features(shader.id,&features);
    assert(features.do_multiply[1] && features.c[1][0]==SHADER_TEXEL1);
    assert(features.c[1][2]==SHADER_INPUT_1); // Soft mask times per-particle primitive alpha.
    // Ordinary single-map tint and unrelated opaque combiners retain one pass.
    gfx_dp_set_combine(0xfc262a60,0x350ceb7f); // Alpha source differs from the arrow material.
    assert(!rdp.combine_two_texture_blend);
    assert(!rdp.combine_arrow_glow);
    rdp.other_mode_h=G_CYC_1CYCLE;
    gfx_dp_set_combine(0xfc262a60,0x350ce37f);
    assert(!rdp.combine_two_texture_blend);
    // Clamp bounds and explicit strided load-tile views must keep their cropped dimensions.
    configure(2,0,0);
    unsigned width,height;
    rdp.texture_tile[0].cms=G_TX_CLAMP;
    gfx_texture_import_dimensions(0,&width,&height);assert(width==4 && height==32);
    rdp.texture_tile[0].cms=G_TX_WRAP;rdp.loaded_texture[0].row_stride_bytes=32;
    gfx_texture_import_dimensions(0,&width,&height);assert(width==4 && height==32);
    rdp.loaded_texture[0].row_stride_bytes=0;rdp.texture_tile[0].fmt=G_IM_FMT_IA;
    gfx_texture_import_dimensions(0,&width,&height);assert(width==4 && height==32);
    puts("Arrow glows: soft outer rim, unchanged core, scrolling UVs and opacity; Freezard mist silhouette/particle fade passed");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-arrow-glow-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + combiner_type + shader_stub + generator + dimensions + combine +
                                  alphas + edge_fade + coordinates + emission + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src/port/psp/gfx"), str(path / "test.c"),
                    str(ROOT / "src/port/psp/gfx/gfx_cc.c"), "-lm",
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
