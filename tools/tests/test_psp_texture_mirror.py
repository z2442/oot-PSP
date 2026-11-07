#!/usr/bin/env python3
"""Exercise PSP uploads and UVs for Epona's clamped, mirrored materials."""
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
#include <string.h>
#include <math.h>
#define TARGET_PSP 1
#define G_TX_NOMASK 0
#define G_TX_MIRROR 1
#define G_TX_CLAMP 2
#define G_TEXTURE_IMAGE_FRAC 2
#define GU_PSM_T8 5
#define GU_PSM_5551 1
#define GU_PSM_4444 2
#define OotPsp_MemcpyVfpu memcpy
struct RGBA { uint8_t r, g, b, a; };
'''
declarations = section("struct TriPipelineState {", "static struct RSP {")
stubs = r'''
struct TextureHashmapNode {
    uint16_t upload_width, upload_height;
    bool mirror_s, mirror_t;
};
static struct { TextureTileState texture_tile[2]; } rdp;
static struct { struct TextureHashmapNode* textures[2]; } rendering_state;
static uint8_t psp_texture_stage_buf[256 * 256 * 4];
static uint16_t uploaded[4096];
static unsigned int uploaded_width, uploaded_height;
static void upload(const uint8_t* data, int width, int height, unsigned int type) {
    assert(type == GU_PSM_5551);
    assert(width * height <= 4096);
    uploaded_width = width;
    uploaded_height = height;
    memcpy(uploaded, data, width * height * 2);
}
static struct { void (*upload_texture)(const uint8_t*, int, int, unsigned int); } api = {upload};
static typeof(api)* gfx_rapi = &api;
'''
helpers = section("static inline TextureTileState* gfx_get_texture_tile(",
                  "static inline bool gfx_get_render_tile_slot(")
upload = section("static inline uint32_t gfx_next_power_of_two(",
                 "#else\nstatic inline void gfx_upload_texture(")
coords = section("static inline float gfx_texture_shift_scale(",
                 "#if defined(TARGET_PSP)\nstatic void gfx_prepare_flame_atlas_coord_state(")
tests = r'''
static void check_clamped(unsigned int width, unsigned int height, unsigned int masks, unsigned int maskt) {
    uint16_t pixels[1024];
    struct TextureHashmapNode node = {0};
    struct TriPipelineState coords = {0};
    assert(width * height <= 1024);
    for (unsigned int i = 0; i < width * height; i++) pixels[i] = i;
    rendering_state.textures[0] = &node;
    rdp.texture_tile[0] = (TextureTileState){
        .cms=G_TX_MIRROR | G_TX_CLAMP, .cmt=G_TX_MIRROR | G_TX_CLAMP,
        .masks=masks, .maskt=maskt, .lrs=(width-1)*4, .lrt=(height-1)*4,
    };
    gfx_upload_texture(0, (const uint8_t*)pixels, width, height, GU_PSM_5551);
    assert(uploaded_width == width && uploaded_height == height);
    assert(!node.mirror_s && !node.mirror_t);
    assert(memcmp(uploaded, pixels, width * height * 2) == 0);
    gfx_prepare_texture_coord_state(&coords, 0, 0, false);
    /* GE must reach its clamp edge at the original image boundary. The eye
     * mesh's 0x698 S and 0x3C1 T coordinates extend past that boundary;
     * doubling the upload used to expose reflected copies of the eye. */
    assert(fabsf(coords.tex_u_scale[0] * width * 32 - 1) < 0.00001f);
    assert(fabsf(coords.tex_v_scale[0] * height * 32 - 1) < 0.00001f);
    if (width == 32 && height == 16) {
        assert(0x698 * coords.tex_u_scale[0] > 1);
        assert(0x3C1 * coords.tex_v_scale[0] > 1);
    }
    /* Removing clamp must still produce the original mirrored repeat. */
    rdp.texture_tile[0].cms = rdp.texture_tile[0].cmt = G_TX_MIRROR;
    gfx_upload_texture(0, (const uint8_t*)pixels, width, height, GU_PSM_5551);
    assert(uploaded_width == 2*width && uploaded_height == 2*height);
    assert(node.mirror_s && node.mirror_t);
    for (unsigned int y = 0; y < 2*height; y++) {
        for (unsigned int x = 0; x < 2*width; x++) {
            unsigned int sx = x < width ? x : 2*width - 1 - x;
            unsigned int sy = y < height ? y : 2*height - 1 - y;
            assert(uploaded[y*2*width+x] == pixels[sy*width+sx]);
        }
    }
    /* Each axis has independent clamp and mirror flags. */
    rdp.texture_tile[0].cms |= G_TX_CLAMP;
    gfx_upload_texture(0, (const uint8_t*)pixels, width, height, GU_PSM_5551);
    assert(uploaded_width == width && uploaded_height == 2*height);
    assert(!node.mirror_s && node.mirror_t);
}
int main(void) {
    check_clamped(32,16,5,4); /* Adult and child Epona eyes. */
    check_clamped(8,8,3,3);   /* Hair and horseshoes. */
    check_clamped(16,8,4,3);  /* Crest. */
    check_clamped(16,32,4,5); /* Nostrils. */
    assert(!gfx_texture_axis_needs_mirror(G_TX_MIRROR,0,0,124));
    assert(!gfx_texture_axis_needs_mirror(G_TX_CLAMP,5,0,124));
    assert(!gfx_texture_axis_needs_mirror(G_TX_MIRROR|G_TX_CLAMP,5,16,140));
    /* Clamp beyond a mask period still allows mirroring within that tile. */
    assert(gfx_texture_axis_needs_mirror(G_TX_MIRROR|G_TX_CLAMP,2,0,44));
    puts("Epona texture clamp, mirrored repeat, and UV checks passed");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-texture-mirror-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + declarations + stubs + helpers + upload + coords + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
