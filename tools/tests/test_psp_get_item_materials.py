#!/usr/bin/env python3
"""Check Odd Mushroom/Potion reflection maps and bomb flashes through GU shaders."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()


def section(text, start, end):
    offset = text.index(start)
    return text[offset:text.index(end, offset)]


constants = "\n".join(line for line in (ROOT / "include/ultra64/gbi.h").read_text().splitlines()
                      if re.match(r"#define G_(?:CCMUX_|ACMUX_|MDSFT_CYCLETYPE|CYC_)", line))
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "gfx_cc.h"
#define TARGET_PSP 1
#define GFX_DL_HANDLER
''' + constants + r'''
struct RGBA { uint8_t r,g,b,a; };
static const struct RGBA white_color={255,255,255,255};
static struct {
    uint32_t other_mode_h,combine_mode;
    bool combine_color_mul_env,combine_color_mul_prim,combine_two_texture_blend,
         combine_two_texture_blend_uses_prim_lod,combine_two_texture_alpha_blend,
         combine_alpha_mul_env,combine_flame_texture_atlas,combine_texture_tint_uses_prim_lod,
         combine_texture_tint_uses_env_alpha,combine_arrow_glow;
    struct RGBA prim_color,env_color;
    uint8_t prim_lod_frac;
} rdp;
static unsigned dirty;
static void gfx_mark_tri_pipeline_dirty(void) { dirty++; }
struct ShaderProgram { uint32_t shader_id; struct CCFeatures cc; } shader;
static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint32_t id) {
    shader.shader_id=id;gfx_cc_get_features(id,&shader.cc);return &shader;
}
enum { GU_TFX_MODULATE,GU_TFX_DECAL,GU_TFX_BLEND,GU_TFX_ADD };
'''
combiner = section(source, "struct ColorCombiner {", "#define COLOR_COMBINER_CACHE_SET_COUNT")
generate = section(source, "static uint8_t gfx_cc_pick_vertex_color_source(",
                   "static inline struct RGBA gfx_get_vertex_color(")
decode = section(source, "static uint8_t color_comb_component(", "static void gfx_dp_set_env_color(")
texture_mode = section(backend, "static inline int texenv_set_texture_color(",
                       "static inline int texenv_set_texture_texture(")
texture_alpha = section(backend, "static bool gfx_scegu_shader_uses_texture_alpha(",
                        "static void gfx_scegu_apply_shader(")
vertex_color = section(source, "static inline struct RGBA gfx_get_vertex_color(",
                       "static inline uint8_t gfx_color_mul_channel(")
prim_color = section(source, "static void gfx_dp_set_prim_color(", "static void gfx_dp_set_fog_color(")
alpha_start = re.search(r"^static inline .*?void gfx_two_texture_blend_pass_alphas\([^;]*?\)\s*\{",
                        source, re.M).start()
pass_alphas = source[alpha_start:source.index("\n}", alpha_start) + 2]
tests = r'''
int main(void) {
    rdp.other_mode_h=G_CYC_2CYCLE;
    // Actual SETCOMBINE words shared by the two get-item outer surfaces.
    const uint32_t outer0=0xfc177e60,outer1=0x45fefd7e;
    const uint32_t inner0=0xfc30fe04,inner1=0x5ffefdfe;
    for (unsigned item=0;item<2;item++) {
        // Each model switches from its outer shell to a different inner/stem material.
        gfx_dp_set_combine(outer0,outer1);
        struct ColorCombiner shell;
        // The real display lists enable texture-edge alpha testing. Dark
        // reflection texels must still keep the shell completely solid.
        gfx_generate_cc(&shell,rdp.combine_mode | SHADER_OPT_ALPHA | SHADER_OPT_TEXTURE_EDGE);
        assert(shell.used_textures[0] && !shell.used_textures[1] && shell.active_texture==0);
        assert(shell.texture_blend && !shell.uses_texture_alpha && !shader.cc.opt_texture_add);
        assert(!gfx_scegu_shader_uses_texture_alpha(shell.prg));
        assert(shell.vertex_color_source[1]==CC_PRIM);
        assert(shell.vertex_color_source[0]==CC_ENV && shell.texture_blend_color_source==CC_PRIM);
        assert(texenv_set_texture_color(shell.prg)==GU_TFX_BLEND);
        assert(!rdp.combine_two_texture_blend && !rdp.combine_arrow_glow && !rdp.combine_flame_texture_atlas);
        // The old shader discarded TEXEL0 and collapsed to ENV. The actual
        // decoded GU shader must now consume the reflection map's RGB.
        bool samples=false;
        for (unsigned c=0;c<4;c++) samples |= shader.cc.c[0][c]==SHADER_TEXEL0;
        assert(samples);
        unsigned before=dirty;
        gfx_dp_set_combine(outer0,outer1);assert(dirty==before);
        gfx_dp_set_combine(inner0,inner1);
        assert((rdp.combine_mode&0xfff)==
               color_comb(G_CCMUX_PRIMITIVE,G_CCMUX_ENVIRONMENT,G_CCMUX_TEXEL0,G_CCMUX_ENVIRONMENT));
    }
    // Similar formulas with different alpha/cycle state must not acquire the shell approximation.
    gfx_dp_set_combine(outer0,outer1^1);
    assert((rdp.combine_mode&0xfff)==
           color_comb(G_CCMUX_TEXEL0,G_CCMUX_SHADE,G_CCMUX_PRIM_LOD_FRAC,G_CCMUX_ENVIRONMENT));
    rdp.other_mode_h=G_CYC_1CYCLE;
    gfx_dp_set_combine(outer0,outer1);
    assert((rdp.combine_mode&SHADER_OPT_TEXTURE_BLEND)==0);
    // Actual gBombBodyDL equation: TEXEL0 + PRIMITIVE, alpha TEXEL0.
    // Exercise both one-cycle and the normal two-cycle passthrough setup.
    for (unsigned cycle=0;cycle<2;cycle++) {
        rdp.other_mode_h=cycle ? G_CYC_2CYCLE : G_CYC_1CYCLE;
        gfx_dp_set_combine(0xfc61ffff,0xfffcf238);
        struct ColorCombiner bomb;
        gfx_generate_cc(&bomb,rdp.combine_mode | SHADER_OPT_ALPHA | SHADER_OPT_TEXTURE_EDGE);
        assert(shader.cc.opt_texture_add && !shader.cc.opt_texture_blend && !shader.cc.opt_texture_blend_shade);
        assert(!bomb.texture_blend && bomb.vertex_color_source[0]==CC_PRIM);
        assert(bomb.active_texture==0 && bomb.uses_texture_alpha);
        assert(texenv_set_texture_color(bomb.prg)==GU_TFX_ADD);
        assert(gfx_scegu_shader_uses_texture_alpha(bomb.prg));
        const unsigned flash[]={0,20,70,140,70,20,0};
        for (unsigned frame=0;frame<sizeof(flash)/sizeof(flash[0]);frame++) {
            gfx_dp_set_prim_color(0,flash[frame],0,40,255);
            struct RGBA rgb=gfx_get_vertex_color(&bomb,false,&white_color,0,true);
            struct RGBA alpha=gfx_get_vertex_color(&bomb,true,&white_color,0,true);
            assert(rgb.r==flash[frame] && rgb.g==0 && rgb.b==40 && alpha.a==255);
        }
    }
    // Switching materials must remove the additive mode from subsequent tint shaders.
    gfx_dp_set_combine(outer0,outer1);
    struct ColorCombiner shell;gfx_generate_cc(&shell,rdp.combine_mode | SHADER_OPT_ALPHA);
    assert(!shader.cc.opt_texture_add && texenv_set_texture_color(shell.prg)==GU_TFX_BLEND);
    struct CCFeatures shadeTint;gfx_cc_get_features(SHADER_OPT_TEXTURE_BLEND_SHADE,&shadeTint);
    assert(shadeTint.opt_texture_blend && shadeTint.opt_texture_blend_shade && !shadeTint.opt_texture_add);
    gfx_dp_set_combine(0xfc61ffff,0xfffcf239); // Different second-cycle alpha cannot use the bomb fallback.
    assert((rdp.combine_mode&SHADER_OPT_TEXTURE_ADD)!=SHADER_OPT_TEXTURE_ADD);
    // Temple of Time floor lists use identical texture crossfades with
    // either PRIMITIVE or SHADE as their final color multiplier. The former
    // was dropped, leaving sections of the blue floor transition unlit.
    for (unsigned shade=0;shade<2;shade++) {
        gfx_dp_set_combine(shade ? 0xfc267e04 : 0xfc267e03,0x1ffcfdf8);
        assert(rdp.combine_two_texture_blend && !rdp.combine_two_texture_blend_uses_prim_lod);
        assert(!rdp.combine_two_texture_alpha_blend && !rdp.combine_arrow_glow);
        struct ColorCombiner floor;gfx_generate_cc(&floor,rdp.combine_mode & ~0xfff000U);
        assert(floor.vertex_color_source[0]==(shade ? CC_SHADE : CC_PRIM));
        assert(!shader.cc.opt_texture_add && !floor.texture_blend);
        rdp.prim_color=(struct RGBA){82,110,150,255};
        for (unsigned phase=0;phase<256;phase++) {
            uint8_t base,overlay;
            gfx_two_texture_blend_pass_alphas(255,phase,false,false,&base,&overlay);
            assert(base==(phase==255 ? 0 : 255) && overlay==phase);
            const unsigned day[3]={200,190,175},blue[3]={60,100,180};
            struct RGBA color=gfx_get_vertex_color(&floor,false,&white_color,0,true);
            for (unsigned c=0;c<3;c++) {
                unsigned tint=((uint8_t*)&color)[c];
                unsigned t0=(day[c]*tint+127)/255,t1=(blue[c]*tint+127)/255;
                unsigned rendered=(t0*(255-overlay)+t1*overlay+127)/255;
                if (!phase) assert(rendered==t0);
                if (phase==255) assert(rendered==t1); // All sections reach their blue texture.
            }
        }
    }
    puts("Item materials, bomb red pulses and both Temple of Time floor crossfade variants passed");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-get-item-materials-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + combiner + generate + decode + texture_mode +
                                  texture_alpha + vertex_color + prim_color + pass_alphas + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src/port/psp/gfx"), str(path / "test.c"),
                    str(ROOT / "src/port/psp/gfx/gfx_cc.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
