#!/usr/bin/env python3
"""Check actual texture preparation and triangle UV emission on the host.

The moon's 128-texel coordinates on a 64-texel clamped tile must preserve
its original footprint rather than stretching the disk across the quad.
"""
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
#define GFX_DL_HANDLER
#define G_TX_NOMASK 0
#define MAX_BUFFERED 16
struct RGBA { uint8_t r, g, b, a; };
struct ColorCombiner { int unused; };
struct LoadedVertex { float x, y, z, u, v; };
struct TextureHashmapNode { uint16_t upload_width, upload_height; };
struct ShaderProgram { uint32_t shader_id; };
typedef struct { float u, v; struct RGBA color; float x, y, z; } psp_fast_t;
static psp_fast_t buf_vbo[48], buf_vbo_tex1[48];
static size_t buf_num_vert, buf_vbo_len, buf_vbo_num_tris;
static void gfx_flush(void) { assert(false); }
'''
declarations = section("struct TriPipelineState {", "static struct RSP {")
globals_and_stubs = r'''
static struct { TextureTileState texture_tile[2]; } rdp;
static struct {
    struct TriPipelineState tri_pipeline;
    struct TextureHashmapNode* textures[2];
    struct ShaderProgram* shader_program;
} rendering_state;
static TextureTileState* gfx_get_texture_tile(int slot) { return &rdp.texture_tile[slot]; }
static void gfx_prepare_tri_pipeline_state(void) {}
'''
preparation = section("static inline float gfx_texture_shift_scale(",
                      "#if defined(TARGET_PSP)\nstatic void gfx_prepare_flame_atlas_coord_state(")
# Include the former fitting helper when checking this test against old code.
helper = ""
if "static GFX_DL_HANDLER void gfx_apply_unmasked_texture_axis(" in source:
    helper = section("static GFX_DL_HANDLER void gfx_apply_unmasked_texture_axis(", "struct ShaderProgram {")
triangles = section("static void gfx_sp_triangles(", "#undef GFX_TRI_INDEX_DIVISOR")
emission = triangles[triangles.index("    gfx_prepare_tri_pipeline_state();"):
                     triangles.index("        out->color = gfx_get_vertex_rgba(")]
wrapper = r'''
static void emit(struct LoadedVertex vertices[3]) {
    struct LoadedVertex *v1 = &vertices[0], *v2 = &vertices[1], *v3 = &vertices[2];
    struct LoadedVertex* clipped_vertices[] = {v1, v2, v3};
    size_t clipped_vertices_num = 3;
    buf_num_vert = buf_vbo_len = buf_vbo_num_tris = 0;
    for (int once = 0; once < 1; once++) {
''' + emission + r'''
        ++buf_num_vert;
    }
    }
    assert(buf_num_vert == 3);
}
'''
tests = r'''
static void close_to(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.00001f);
}
int main(void) {
    struct ShaderProgram shader = {0};
    rendering_state.shader_program = &shader;
    rendering_state.tri_pipeline.use_texture = true;
    /* gMoonVtx uses 0 and 0x1000, scaled by SPTexture(0xffff). */
    struct LoadedVertex moon[3] = {{.u=0,.v=0}, {.u=4095,.v=0}, {.u=0,.v=4095}};
    rdp.texture_tile[0] = (TextureTileState){.lrs=252,.lrt=252};
    gfx_prepare_texture_coord_state(&rendering_state.tri_pipeline, 0, 0, false);
    emit(moon);
    close_to(buf_vbo[0].u, 0);
    close_to(buf_vbo[1].u, 4095.0f / 2048);
    close_to(buf_vbo[2].v, 4095.0f / 2048);
    /* The tile edge falls halfway across the quad on both axes. */
    close_to(1 / buf_vbo[1].u, 2048.0f / 4095);
    close_to(1 / buf_vbo[2].v, 2048.0f / 4095);
    /* Masked coordinates follow the same mapping; the sampler controls wrap. */
    rdp.texture_tile[0].masks = rdp.texture_tile[0].maskt = 6;
    gfx_prepare_texture_coord_state(&rendering_state.tri_pipeline, 0, 0, false);
    emit(moon);
    close_to(buf_vbo[1].u, 4095.0f / 2048);
    /* Tile shifts, tile origins, filtering and padded uploads still apply. */
    rdp.texture_tile[0] = (TextureTileState){.uls=8,.ult=16,.lrs=260,.lrt=268,.shifts=1,.shiftt=15};
    struct TextureHashmapNode upload = {.upload_width=128,.upload_height=64};
    rendering_state.textures[0] = &upload;
    gfx_prepare_texture_coord_state(&rendering_state.tri_pipeline, 0, 0, true);
    emit(moon);
    close_to(buf_vbo[1].u, (4095 * 0.5f + 16 - 8 * 8) / 4096);
    close_to(buf_vbo[2].v, (4095 * 2.0f + 16 - 16 * 8) / 2048);
    /* The second texture keeps its own tile transform. */
    rendering_state.tri_pipeline.two_texture_blend = true;
    rdp.texture_tile[1] = (TextureTileState){.lrs=124,.lrt=124};
    gfx_prepare_texture_coord_state(&rendering_state.tri_pipeline, 1, 1, false);
    emit(moon);
    close_to(buf_vbo_tex1[1].u, 4095.0f / 1024);
    puts("Moon footprint and texture coordinate emission checks passed");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-texture-coords-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + declarations + globals_and_stubs +
                                  preparation + helper + wrapper + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Wno-unused-variable", str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
