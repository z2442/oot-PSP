#!/usr/bin/env python3
"""Exercise the actual cache/compiler code on the host with a stub GU/asset loader.

The PSP build checks the real ABI. These tests check cache lifetime and RSP
semantics; they do not substitute for hardware image/performance comparisons.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()
cache = source[source.index("/* Native mesh cache deliberately"):source.index("static void gfx_run_dl(")]

def function(name, following):
    return source[source.index(name):source.index(following, source.index(name))]

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define TARGET_PSP 1
#define F3DEX_GBI_2 1
#define GFX_DL_HANDLER
#define MAX_VERTICES 64
#define G_VTX 1
#define G_TRI1 5
#define G_TRI2 6
#define G_ENDDL 0xdfU
#define G_ZBUFFER 1
#define G_SHADE 4
#define G_SHADING_SMOOTH 0x200000
#define G_CULL_FRONT 0x200
#define G_CULL_BACK 0x400
#define G_CULL_BOTH (G_CULL_FRONT | G_CULL_BACK)
#define G_BL_CLR_FOG 3
#define G_IM_FMT_CI 2
#define CC_0 0
#define CC_SHADE 4
#define PSP_NATIVE_ADDR_START 0x08800000
#define OOT_PSP_EXTERNAL_ASSET_NATIVE 1
#define OOT_PSP_EXTERNAL_ASSET_IMMUTABLE_MESH 4
#define X_POS 1
#define X_NEG 2
#define Y_POS 4
#define Y_NEG 8
#define Z_POS 16
#define Z_NEG 32
struct RGBA { uint8_t r,g,b,a; };
typedef struct { int16_t ob[3]; uint16_t flag; int16_t tc[2]; uint8_t cn[4]; } Vtx_t;
typedef union { Vtx_t v; uint64_t align[2]; } Vtx;
typedef struct { struct { uint32_t w0; uintptr_t w1; } words; } Gfx;
typedef struct { float u,v; struct RGBA color; float x,y,z; } psp_fast_t;
struct LoadedVertex { float x,y,z,w,_x,_y,_z,_w,u,v; struct RGBA color; uint8_t clip_rej,fog_alpha; };
struct GfxVfpuTransformState { const float (*model)[4], (*projection)[4]; int fog_mul, fog_offset; unsigned fog_enabled; };
struct { float modelview_matrix_stack[1][4][4], P_matrix[4][4]; unsigned modelview_matrix_stack_size;
    unsigned geometry_mode; void* segments[16]; struct { uint16_t s,t; } texture_scaling_factor;
    struct LoadedVertex loaded_vertices[64]; } rsp;
struct { bool combine_color_mul_env,combine_color_mul_prim,combine_alpha_mul_env,combine_two_texture_blend,
    combine_flame_texture_atlas; uint32_t other_mode_l;
    struct { const uint8_t* addr; unsigned source_size_bytes; } loaded_texture[2];
    struct { unsigned fmt; } texture_tile[2]; } rdp;
struct ColorCombiner { unsigned vertex_color_source[2], used_textures[2]; } comb;
struct TriPipelineState { bool use_texture,use_fog,use_alpha,texture_tint_colors_corrected;
    bool tex_u_scale_to_primitive[2],tex_v_scale_to_primitive[2];
    float tex_u_scale[2],tex_v_scale[2],tex_u_bias[2],tex_v_bias[2]; struct ColorCombiner* comb; };
struct { struct TriPipelineState tri_pipeline; } rendering_state;
uintptr_t gSegments[16];
float sNdcAspectScale = 1;
unsigned validation_calls, draws, transforms, generation = 1;
bool immutable = true;
static bool gfx_hud_anchor_enabled(void) { return false; }
static void gfx_prepare_tri_pipeline_state(void) {}
static void gfx_flush(void) {}
static void gfx_apply_projection_matrix(void) {}
static unsigned OotPsp_GetExternalAssetGeneration(void) { return generation; }
static bool OotPsp_GetLoadedExternalAssetRangeFlags(const void* p, size_t n, uint32_t* flags) {
    validation_calls++; *flags = immutable ? 5 : 1; return p && n;
}
static void* seg_addr(uintptr_t addr) { return (void*)addr; }
static void sceKernelDcacheWritebackRange(void* p, size_t n) { (void)p; (void)n; }
void gfx_scegu_draw_native_mesh(const void* v, const uint16_t* i, unsigned n, const float m[4][4], uint32_t c) {
    assert(v && i && n % 3 == 0); draws++; (void)m; (void)c;
}
'''
transform = function("static inline void gfx_transform_vec4(", "static inline void gfx_upload_projection_matrix(")
decode = function("static bool gfx_decode_vertex_cmd_f3dex2(", "static GFX_DL_HANDLER bool gfx_sp_vertex_f3dex2(")
stub_transform = r'''
void gfx_transform_vertices_vfpu(struct LoadedVertex* d, const Vtx* v, uint32_t n, const struct GfxVfpuTransformState* s) {
    assert(n == 1); transforms++;
    float input[4] = {v->v.ob[0],v->v.ob[1],v->v.ob[2],1}, m[4], p[4];
    gfx_transform_vec4(m,s->model,input); gfx_transform_vec4(p,s->projection,m);
    d->x=m[0]; d->y=m[1]; d->z=m[2]; d->_x=p[0]; d->_y=p[1]; d->_z=p[2]; d->_w=p[3];
}
'''
tests = r'''
int main(void) {
    Vtx vertices[3] = {0};
    vertices[0].v.ob[0]=-1; vertices[1].v.ob[0]=1; vertices[2].v.ob[1]=1;
    for (unsigned i=0;i<3;i++) { vertices[i].v.cn[0]=42; vertices[i].v.cn[3]=123; }
    Gfx commands[] = {{{(G_VTX<<24)|(3<<12)|(3<<1),(uintptr_t)vertices}},
                      {{(G_TRI1<<24)|(0<<16)|(2<<8)|4,0}},{{G_ENDDL<<24,0}}};
    rsp.modelview_matrix_stack_size=1; rsp.geometry_mode=G_SHADE;
    for (unsigned i=0;i<4;i++) rsp.modelview_matrix_stack[0][i][i]=rsp.P_matrix[i][i]=1;
    rsp.P_matrix[0][0]=rsp.P_matrix[1][1]=rsp.P_matrix[2][2]=0.1f;
    comb.vertex_color_source[0]=comb.vertex_color_source[1]=CC_SHADE;
    rendering_state.tri_pipeline.comb=&comb;
    Gfx* cursor=commands;
    assert(gfx_native_try(&cursor)); assert(cursor==commands+2 && draws==1 && transforms==0);
    unsigned validations=validation_calls;
    commands[1].words.w0=0x02000000; /* Poison original commands: a hit must not decode. */
    rsp.modelview_matrix_stack[0][3][0]=2;
    cursor=commands; assert(gfx_native_try(&cursor));
    assert(validation_calls==validations && transforms==0 && draws==2);
    gfx_native_forget(1); /* A later VTX overwrites slot 0, leaving other slots live. */
    rsp.modelview_matrix_stack[0][3][0]=5;
    gfx_native_materialize(7);
    assert(transforms==2 && rsp.loaded_vertices[1].x==3 && rsp.loaded_vertices[2].x==2);
    assert(rsp.loaded_vertices[1].color.r==42 && rsp.loaded_vertices[1].color.a==123);
    assert(sNativePendingMask==0);
    generation++; cursor=commands; assert(!gfx_native_try(&cursor)); /* MODIFYVTX rejected. */
    commands[1].words.w0=(G_TRI1<<24)|(2<<8)|4;
    generation++; immutable=false; cursor=commands; assert(!gfx_native_try(&cursor));
    immutable=true; generation++;
    rsp.modelview_matrix_stack[0][3][0]=20; cursor=commands; assert(!gfx_native_try(&cursor)); /* bounds */
    rsp.modelview_matrix_stack[0][3][0]=0; cursor=commands; assert(gfx_native_try(&cursor));
    validations=validation_calls;
    rsp.segments[6]=(void*)1; cursor=commands; assert(gfx_native_try(&cursor));
    assert(validation_calls>validations); /* segment remap forces a rebuild */
    struct NativeMesh bad={0};
    commands[1].words.w0=(G_TRI1<<24)|(2<<8)|6;
    assert(!gfx_native_build(&bad,commands)); /* inherited/unloaded slot */
    commands[1].words.w0=(G_TRI1<<24)|(2<<8)|5;
    assert(!gfx_native_build(&bad,commands)); /* odd index */
    commands[1].words.w0=(G_TRI1<<24)|(2<<8)|4;
    rendering_state.tri_pipeline.use_texture=true;
    rendering_state.tri_pipeline.use_alpha=true;
    comb.vertex_color_source[1]=CC_0;
    uint8_t texture[16]={0}; rdp.loaded_texture[0].addr=texture; rdp.loaded_texture[0].source_size_bytes=16;
    rendering_state.tri_pipeline.tex_u_scale[0]=1.0f/32;
    rendering_state.tri_pipeline.tex_v_scale[0]=1.0f/32;
    rsp.texture_scaling_factor.s=rsp.texture_scaling_factor.t=65535;
    vertices[0].v.tc[0]=64; generation++;
    cursor=commands; assert(gfx_native_try(&cursor));
    bool found=false;
    for (unsigned i=0;i<OOT_PSP_NATIVE_MESH_ENTRIES;i++) if (sNativeMeshes[i].generation==generation && sNativeMeshes[i].valid) {
        assert(sNativeMeshes[i].vertices[0].u==63.0f/32);
        assert(sNativeMeshes[i].vertices[0].color.a==255); found=true;
    }
    assert(found);
    rendering_state.tri_pipeline.tex_u_scale_to_primitive[0]=true;
    cursor=commands; assert(!gfx_native_try(&cursor)); /* per-triangle UV mapping */
    rendering_state.tri_pipeline.tex_u_scale_to_primitive[0]=false;
    for (unsigned i=0;i<OOT_PSP_NATIVE_MESH_ENTRIES;i++) sNativeMeshes[i].pinned=true;
    generation++; cursor=commands; assert(!gfx_native_try(&cursor)); /* no overwrite of submitted buffers */
    puts("native mesh cache tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="oot-native-mesh-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + transform + decode + stub_transform + cache + tests)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined,address",
                    str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
