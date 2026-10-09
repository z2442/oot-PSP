#!/usr/bin/env python3
"""Check that fog rectangles keep letterbox clipping and overlays can fill bars."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
interpreter = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()


def section(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define G_MDSFT_CYCLETYPE 20
#define G_MDSFT_TEXTFILT 12
#define G_CYC_COPY (2U<<20)
#define G_TF_POINT 0
#define Z_CMP 16
#define Z_UPD 32
#define G_ZBUFFER 1
#define HALF_SCREEN_WIDTH 160
#define HALF_SCREEN_HEIGHT 120
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
struct XYWidthHeight { uint16_t x,y,width,height; };
struct VertexColor { uint16_t x,y; };
static struct { unsigned width,height; } gfx_current_dimensions={480,272};
static struct {
    uint32_t other_mode_h,other_mode_l;
    struct XYWidthHeight viewport,scissor;
    bool viewport_or_scissor_changed;
} rdp;
static struct { uint32_t geometry_mode; struct VertexColor loaded_vertices_2D[2]; } rsp;
static struct XYWidthHeight drawn_scissor,drawn_viewport;
static int scissor_calls;
static void sceGuViewport(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
static void sceGuScissor(int x,int y,int r,int b) { (void)x;(void)y;(void)r;(void)b;scissor_calls++; }
static void gfx_mark_tri_pipeline_dirty(void) {}
static float gfx_hud_anchor_offset_pixels(void) { return 0; }
static float gfx_adjust_x_for_aspect_ratio(float x) { return x; }
static void gfx_sp_tri1_2d(int a,int b,int c) {
    (void)a;(void)b;(void)c;
    drawn_scissor=rdp.scissor;
    drawn_viewport=rdp.viewport;
}
'''
tests = r'''
int main(void) {
    struct XYWidthHeight viewport={10,20,300,200};
    struct XYWidthHeight full={0,0,480,272};
    rdp.viewport=viewport;
    rsp.geometry_mode=0x1234;
    for (unsigned bar=1;bar<=32;bar++) {
        unsigned pixels=(bar*272+239)/240;
        struct XYWidthHeight letterbox={0,pixels,480,272-2*pixels};
        rdp.scissor=letterbox;
        /* Environment_DrawSkyboxFilters fills the screen in one-cycle mode,
         * with the scene's letterbox scissor still active. */
        gfx_draw_rectangle(0,0,319*4,239*4,true,true);
        assert(memcmp(&drawn_scissor,&letterbox,sizeof(letterbox))==0);
        assert(memcmp(&drawn_viewport,&full,sizeof(full))==0);
        assert(memcmp(&rdp.viewport,&viewport,sizeof(viewport))==0);
        assert(memcmp(&rdp.scissor,&letterbox,sizeof(letterbox))==0);
        assert(rsp.geometry_mode==0x1234);
        gfx_scegu_set_scissor(letterbox.x,letterbox.y,letterbox.width,letterbox.height);
        int calls=scissor_calls;
        gfx_scegu_set_viewport(0,0,480,272);
        assert(scissor_calls==calls);
        /* Gfx_SetupFrame explicitly selects a full scissor for OVERLAY_DISP,
         * so the black fill rectangles must be able to cover both bars. */
        rdp.scissor=full;
        gfx_draw_rectangle(0,0,320*4,bar*4,false,true);
        assert(memcmp(&drawn_scissor,&full,sizeof(full))==0);
        gfx_draw_rectangle(0,(240-bar)*4,320*4,240*4,false,true);
        assert(memcmp(&drawn_scissor,&full,sizeof(full))==0);
    }
    puts("Fog rectangles retain letterbox clipping; overlays cover bars; viewport preserves scissor");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-rectangle-scissor-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(
        preamble
        + section(interpreter, "static void gfx_draw_rectangle(", "#if defined(TARGET_PSP)\ntypedef struct GfxRectAxisSegment")
        + section(backend, "static void gfx_scegu_set_viewport(", "static void gfx_scegu_set_use_alpha(")
        + tests
    )
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
