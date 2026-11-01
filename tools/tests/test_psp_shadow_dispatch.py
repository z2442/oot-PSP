#!/usr/bin/env python3
"""Exercise the real shadow command, N64 texture load and GPU import routing."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
extensions = (ROOT / "src/port/psp/oot_psp_gfx_ext.h").read_text()


def function(name):
    start = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


dispatch = source.split("            case G_NOOP:\n", 1)[1].split("                break;", 1)[0]
macro = extensions[extensions.index("#define gOotPspShadowMask("):extensions.rindex("#endif")]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "oot_psp_shadow.h"
#define TARGET_PSP 1
#define GFX_CAPTURE_CMD(a,b) ((void)0)
#define G_TX_LOADTILE 7
#define OOT_PSP_SHADOW_CMD 0x534844U
#define G_NOOP 0xc0
#define _SHIFTL(v,s,w) ((uint32_t)(v)<<(s))
#define _UNUSED(v) ((void)(v))
enum { G_IM_FMT_RGBA,G_IM_FMT_IA,G_IM_FMT_CI,G_IM_FMT_I };
enum { G_IM_SIZ_4b,G_IM_SIZ_8b,G_IM_SIZ_16b,G_IM_SIZ_32b };
typedef struct { struct { uint32_t w0; uintptr_t w1; } words; } Gfx;
typedef struct { uint8_t fmt,siz; } TextureTileState;
static struct {
    struct { const uint8_t* addr; const OotPspShadowMask* shadow; uint8_t fmt,siz; unsigned width; } texture_to_load;
    struct { const uint8_t* addr; const OotPspShadowMask* shadow;
        unsigned size_bytes,source_size_bytes,row_stride_bytes,source_nibble_offset; } loaded_texture[2];
    bool textures_changed[2];
} rdp;
static TextureTileState tiles[2];
struct TextureHashmapNode { unsigned texture_id; } node={7};
static struct { struct TextureHashmapNode* textures[2]; } rendering_state;
static const OotPspShadowMask* sPendingShadowMask;
static unsigned flushes,noops,allocations,gpuUploads,cpuUploads,validations,dirty;
static bool reuse;
static void* seg_addr(uintptr_t p) { return (void*)p; }
static void gfx_flush(void) { ++flushes; }
static void gfx_dp_noop(unsigned tag) { assert(tag==123); ++noops; }
static bool gfx_texture_load_slot(const char* context,int* slot) { (void)context;*slot=0;return true; }
static void gfx_log_bad_texture_source(int slot,const char* c,const void* p,unsigned n) {
    (void)slot;(void)c;(void)p;(void)n;abort();
}
static void gfx_set_invalid_loaded_texture(int tile) { (void)tile;abort(); }
static void gfx_set_invalid_texture_tile(int tile) { (void)tile;abort(); }
static bool gfx_loaded_texture_matches_static_cache(int slot,const void* p,unsigned a,unsigned b,unsigned c,unsigned d) {
    (void)slot;(void)p;(void)a;(void)b;(void)c;(void)d;return false;
}
static void gfx_mark_tri_pipeline_dirty(void) { dirty++; }
static TextureTileState* gfx_get_texture_tile(int tile) { return &tiles[tile]; }
static bool gfx_texture_cache_lookup(int tile,struct TextureHashmapNode** entry,const uint8_t* addr,
                                     unsigned fmt,unsigned siz,bool allocate,bool* inPlace) {
    assert(tile==0 && addr==rdp.loaded_texture[0].addr && fmt==G_IM_FMT_I && siz==G_IM_SIZ_8b);
    *inPlace=!allocate && reuse;
    if (*inPlace || allocate) *entry=&node;
    if (allocate) allocations++;
    return false;
}
static void gfx_validate_texture_source(int tile,const char* c) {
    (void)c;assert(rdp.loaded_texture[tile].shadow==NULL);validations++;
}
static void gfx_validate_texture_tile_dimensions(int tile,const char* c) { (void)tile;(void)c; }
void gfx_scegu_upload_shadow_mask(int tile,unsigned textureId,const OotPspShadowMask* mask) {
    assert(tile==0 && textureId==7 && mask==rdp.loaded_texture[tile].shadow);gpuUploads++;
}
static void import_texture_i8(int tile) { assert(tile==0 && rdp.loaded_texture[tile].shadow==NULL);cpuUploads++; }
static void unused_import(int tile) { (void)tile;abort(); }
'''
for name in re.findall(r"\b(import_texture_\w+)\(tile\)", function("import_texture")):
    if name != "import_texture_i8":
        preamble += "#define " + name + " unused_import\n"
tests = r'''
static void command(Gfx* cmd) {
''' + dispatch + r'''
}
int main(void) {
    OotPspShadowMask mask;uint8_t ordinary[4096];
    OotPspShadow_Begin(&mask,0);OotPspShadow_Add(&mask,32,32,2);
    Gfx packets[2],*cursor=packets;
    gOotPspShadowMask(cursor++,&mask);
    assert(cursor==packets+1 && packets[0].words.w1==(uintptr_t)&mask);
    command(packets);assert(sPendingShadowMask==&mask && flushes==1);
    gfx_dp_set_texture_image(G_IM_FMT_I,G_IM_SIZ_16b,1,&mask);
    assert(rdp.texture_to_load.shadow==&mask && sPendingShadowMask==NULL);
    gfx_dp_load_block(7,0,0,2047,0);
    assert(rdp.loaded_texture[0].shadow==&mask && rdp.loaded_texture[0].size_bytes==4096 && dirty==1);
    tiles[0]=(TextureTileState){G_IM_FMT_I,G_IM_SIZ_8b};
    import_texture(0);assert(gpuUploads==1 && cpuUploads==0 && validations==0 && allocations==1);
    reuse=true;import_texture(0);assert(gpuUploads==2 && allocations==1);
    gfx_dp_set_texture_image(G_IM_FMT_I,G_IM_SIZ_16b,1,ordinary);
    gfx_dp_load_block(7,0,0,2047,0);assert(rdp.loaded_texture[0].shadow==NULL);
    import_texture(0);assert(cpuUploads==1 && validations==1 && gpuUploads==2);
    // A marker only applies to its matching next image. Unrelated texture
    // loads and ordinary NOOP tags must retain their existing behavior.
    command(packets);gfx_dp_set_texture_image(G_IM_FMT_I,G_IM_SIZ_16b,1,ordinary);
    gfx_dp_set_texture_image(G_IM_FMT_I,G_IM_SIZ_16b,1,&mask);
    assert(rdp.texture_to_load.shadow==NULL);
    packets[0]=(Gfx){{G_NOOP<<24,123}};command(packets);assert(noops==1);
    puts("Shadow dispatch: pointer command, load-block metadata, GPU-only import and ordinary texture routing passed");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-shadow-dispatch-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + macro + function("gfx_dp_set_texture_image") +
                                 function("gfx_dp_load_block") + function("import_texture") + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src/port/psp"), str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
