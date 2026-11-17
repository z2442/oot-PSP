#!/usr/bin/env python3
"""Check that memoized SETCOMBINE decoding always matches a fresh decode."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()


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
#include <string.h>
#include "gfx_cc.h"
#define TARGET_PSP 1
#define GFX_DL_HANDLER
''' + constants + r'''
static struct {
    uint32_t other_mode_h,combine_mode;
    bool combine_color_mul_env,combine_color_mul_prim,combine_two_texture_blend,
         combine_two_texture_blend_uses_prim_lod,combine_two_texture_alpha_blend,
         combine_alpha_mul_env,combine_flame_texture_atlas,combine_texture_tint_uses_prim_lod,
         combine_texture_tint_uses_env_alpha,combine_arrow_glow;
} rdp;
static unsigned dirty;
static void gfx_mark_tri_pipeline_dirty(void) { dirty++; }
'''
decode = section(source, "static uint8_t color_comb_component(", "static void gfx_dp_set_env_color(")
tests = r'''
static uint32_t rng=12345;
static uint32_t next(void) { rng=rng*1664525U+1013904223U; return rng; }
int main(void) {
    /* A small pool of real character/effect words plus random ones. Reusing
     * pool entries produces memo hits; 4096 distinct keys force collisions
     * in the 64-entry table, so way replacement is exercised too. */
    uint32_t pool[64][2]={{0xfc177e60,0x45fefd7e},{0xfc30fe04,0x5ffefdfe},{0xfc61ffff,0xfffcf238},
                          {0xfc262a60,0x350cf37f},{0xfc262a60,0x350ce37f},{0xfc161a60,0xf50d657f},
                          {0xfc267e04,0x1ffcfdf8},{0xfc267e03,0x1ffcfdf8},{0xfc127e24,0xfffff3f9},
                          {0xfc121824,0xff33fff8}};
    for (unsigned i=10;i<64;i++) { pool[i][0]=0xfc000000U|(next()&0xffffff); pool[i][1]=next(); }
    const uint32_t cycles[]={G_CYC_1CYCLE,G_CYC_2CYCLE,3U<<G_MDSFT_CYCLETYPE};
    unsigned hits=0;
    for (unsigned iter=0;iter<200000;iter++) {
        uint32_t w0,w1;
        if (iter&1) { unsigned k=next()%64; w0=pool[k][0]; w1=pool[k][1]; }
        else { w0=0xfc000000U|(next()&0xffffff); w1=next(); }
        rdp.other_mode_h=cycles[next()%3] | (next() & ~(3U<<G_MDSFT_CYCLETYPE));
        const uint32_t cycle=rdp.other_mode_h&(3U<<G_MDSFT_CYCLETYPE);
        hits+=gfx_combine_memo_find(w0,w1,cycle)!=NULL;
        gfx_dp_set_combine(w0,w1);
        struct GfxCombineDecode fresh; memset(&fresh,0,sizeof(fresh));
        gfx_dp_decode_combine(w0,w1,&fresh);
        /* What reached rdp must equal a fresh decode of these exact inputs. */
        uint32_t mode=fresh.rgb|(fresh.alpha<<12);
        if (fresh.texture_add) mode|=SHADER_OPT_TEXTURE_ADD;
        else if (fresh.texture_blend)
            mode|=fresh.texture_blend_shade?SHADER_OPT_TEXTURE_BLEND_SHADE:SHADER_OPT_TEXTURE_BLEND;
        assert(rdp.combine_mode==mode);
        assert(rdp.combine_color_mul_env==fresh.color_mul_env);
        assert(rdp.combine_color_mul_prim==fresh.color_mul_prim);
        assert(rdp.combine_two_texture_blend==fresh.two_texture_blend);
        assert(rdp.combine_two_texture_blend_uses_prim_lod==fresh.two_texture_blend_uses_prim_lod);
        assert(rdp.combine_two_texture_alpha_blend==fresh.two_texture_alpha_blend);
        assert(rdp.combine_alpha_mul_env==fresh.alpha_mul_env);
        assert(rdp.combine_flame_texture_atlas==fresh.flame_texture_atlas);
        assert(rdp.combine_texture_tint_uses_prim_lod==fresh.texture_tint_uses_prim_lod);
        assert(rdp.combine_texture_tint_uses_env_alpha==fresh.texture_tint_uses_env_alpha);
        assert(rdp.combine_arrow_glow==fresh.arrow_glow);
        /* The slot now holds these inputs, so an immediate repeat is a hit and
         * leaves the pipeline clean. */
        const struct GfxCombineDecode* stored=gfx_combine_memo_find(w0,w1,cycle);
        assert(stored!=NULL && memcmp(stored,&fresh,sizeof(fresh))==0);
        unsigned before=dirty; gfx_dp_set_combine(w0,w1); assert(dirty==before);
    }
    assert(hits>0);
    /* Character-like stream: a few real combiners reissued per limb. After
     * the first pass these must all be memo hits (no decoder runs). */
    unsigned realHits=0,realCalls=0;
    rdp.other_mode_h=G_CYC_2CYCLE;
    for (unsigned limb=0;limb<1000;limb++) {
        for (unsigned k=0;k<10;k++) {
            const uint32_t cycle=rdp.other_mode_h&(3U<<G_MDSFT_CYCLETYPE);
            realHits+=gfx_combine_memo_find(pool[k][0],pool[k][1],cycle)!=NULL;
            realCalls++;
            gfx_dp_set_combine(pool[k][0],pool[k][1]);
        }
    }
    assert(realHits*100>=realCalls*95);
    printf("Combine memo: 200000 randomized decodes match fresh results; "
           "character stream hit rate %u/%u\n",realHits,realCalls);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="oot-combine-memo-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + decode + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-fsanitize=undefined,address",
                    "-I" + str(ROOT / "src/port/psp/gfx"), "-I" + str(ROOT / "include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
