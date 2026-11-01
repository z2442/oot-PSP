#!/usr/bin/env python3
"""Compare actual GU mask commands with the game's original CPU stamp loops.

Host sprite rasterization checks masks, alpha/stencil, copies and state restore.
It does not substitute for testing PSP GE execution, synchronization or timing.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
link = (ROOT / "src/overlays/actors/ovl_En_Sda/z_en_sda.c").read_text()
ganon = (ROOT / "src/overlays/actors/ovl_Boss_Ganon2/z_boss_ganon2.c").read_text()


def array(source, name):
    return re.search(r"static s16 " + name + r"\[\] = \{.*?\};", source, re.S)[0] + "\n"


def stamp_loop(source, start):
    body = source[source.index(start):]
    return body[:body.index("\n#endif")]


reference = "".join(array(link, name) for name in
                    ("D_80AFA0DC", "D_80AFA0E8", "D_80AFA0F8", "D_80AFA108", "D_80AFA120"))
reference += "".join(array(ganon, name) for name in
                     ("D_80907188", "D_80907194", "D_809071A4", "D_809071B4"))
reference += r'''
static void reference_stamp(uint8_t* shadowTexture,int x,int y,unsigned type,bool wrap) {
    int i=0,j,phi_a3,phi_a0;
    int temp_t0=x,temp_t1=y*64,temp_v0,temp_v1;
    if (type>=5) {
        for (int row=-1;row<=1;row++) for (int col=(type==5 ? -3 : -1);col<=(type==5 ? 3 : 1);col++) {
            int index=(y+row)*64+x+col;
            if (index>=0 && index<4096) shadowTexture[index]=255;
        }
    } else if (!wrap) {
        unsigned D_80AFA15C[1]={type};
''' + stamp_loop(link, "            if (D_80AFA15C[i] == 2) {") + r'''
    } else {
        int temp_a3=x,phi_v1,phi_a1;
        temp_t0=y*64;
        unsigned D_809071EC[1]={type};
''' + stamp_loop(ganon, "            if (D_809071EC[i] == 2) {") + r'''
    }
}
'''

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oot_psp_shadow.h"
#include "psp_texture_manager.h"
typedef int16_t s16;
enum { GU_SCISSOR_TEST,GU_DEPTH_TEST,GU_LIGHTING,GU_FOG,GU_CULL_FACE,GU_DITHER,
       GU_COLOR_TEST,GU_COLOR_LOGIC_OP,GU_BLEND,GU_ALPHA_TEST,GU_TEXTURE_2D,GU_STENCIL_TEST };
enum { GU_PSM_8888=20,GU_KEEP,GU_REPLACE,GU_ALWAYS,GU_GREATER,GU_NEAREST,GU_CLAMP,
       GU_TFX_REPLACE,GU_TFX_MODULATE,GU_TCC_RGBA,GU_SPRITES };
#define GU_TRUE 1
#define GU_FALSE 0
#define GU_TEXTURE_16BIT 1
#define GU_COLOR_8888 2
#define GU_VERTEX_16BIT 4
#define GU_TRANSFORM_2D 8
#define BUF_WIDTH 512
struct ShaderProgram { int dummy; } shader;
static struct ShaderProgram* sAppliedShader=&shader;
static bool sAppliedSamplerStateValid,sDepthWriteEnabled=true;
static uint32_t sTextureEnvColor=0xffffffffU;
static int sScissorBounds[4]={12,20,460,250},scissor[4];
static unsigned statuses,stencilRef,alphaRef,depthMask,copyCount,binds,texFunction;
static uint32_t target[4096],pixels[4096],mainBuffer[512*272];
static void* sShadowMaskBuffer=target;
static void* sDrawBuffer=mainBuffer;
static int sColorPixelFormat=GU_PSM_8888;
static uint32_t* drawTarget=mainBuffer;
static const uint32_t* textureSource;
static unsigned textureStride;
static struct PSP_Texture texture;
static unsigned char listMemory[65536] __attribute__((aligned(16)));
static size_t reserved;
static void* sceGuGetMemory(size_t bytes) { assert(bytes<=reserved);return listMemory; }
static void gfx_scegu_reserve_list_memory(size_t bytes) { reserved=bytes;assert(bytes<=sizeof(listMemory)); }
struct PSP_Texture* texman_prepare_gpu_texture(unsigned id,int w,int h,unsigned type) {
    assert(id==7 && w==64 && h==64 && type==GU_PSM_8888);
    texture=(struct PSP_Texture){(unsigned char*)pixels,w,h,type,0};return &texture;
}
static void sceKernelDcacheWritebackRange(const void* p,size_t bytes) { assert(p && bytes==8192); }
static void* gfx_scegu_vram_cpu_addr(void* p) { return p; }
static unsigned sceGuGetAllStatus(void) { return statuses; }
static void sceGuSetAllStatus(unsigned value) { statuses=value; }
static void sceGuEnable(unsigned state) { statuses|=1U<<state; }
static void sceGuDisable(unsigned state) { statuses&=~(1U<<state); }
static void sceGuDrawBufferList(unsigned type,void* buffer,unsigned width) {
    assert(type==GU_PSM_8888 && width==(buffer==target ? 64 : 512)); drawTarget=buffer;
}
static void sceGuScissor(int l,int t,int r,int b) { scissor[0]=l;scissor[1]=t;scissor[2]=r;scissor[3]=b; }
static void sceGuDepthMask(unsigned value) { depthMask=value; }
static void sceGuPixelMask(unsigned value) { (void)value; }
static void sceGuStencilOp(unsigned fail,unsigned zfail,unsigned pass) {
    assert(fail==GU_KEEP && zfail==GU_KEEP && pass==GU_REPLACE);
}
static void sceGuStencilFunc(unsigned func,unsigned value,unsigned mask) {
    assert(func==GU_ALWAYS && mask==255);stencilRef=value;
}
static void sceGuTexMode(unsigned a,unsigned b,unsigned c,unsigned d) { (void)a;(void)b;(void)c;(void)d; }
static void sceGuTexImage(unsigned level,unsigned w,unsigned h,unsigned stride,const void* p) {
    (void)w;(void)h;assert(level==0);textureSource=p;textureStride=stride;
}
static void sceGuTexFilter(unsigned a,unsigned b) { (void)a;(void)b; }
static void sceGuTexWrap(unsigned a,unsigned b) { (void)a;(void)b; }
static void sceGuTexFunc(unsigned func,unsigned components) { assert(components==GU_TCC_RGBA);texFunction=func; }
static void sceGuAlphaFunc(unsigned func,unsigned value,unsigned mask) { assert(func==GU_GREATER && mask==255);alphaRef=value; }
static void sceGuTexSync(void) {}
static void sceGuTexFlush(void) {}
static void sceGuTexEnvColor(unsigned color) { assert(color==sTextureEnvColor); }
static void sceGuCopyImage(unsigned type,int sx,int sy,int w,int h,int stride,const void* src,
                           int dx,int dy,int dstStride,void* dst) {
    assert(type==GU_PSM_8888 && sx==0 && sy==0 && w==64 && h==64 && stride==64);
    assert(dx==0 && dy==0 && dstStride==64 && src==target && dst==pixels);
    memcpy(dst,src,16384);copyCount++;
}
static void gfx_scegu_apply_shader(struct ShaderProgram* prg) { assert(prg==&shader);sAppliedShader=prg; }
void texman_invalidate_binding(void) {}
static void gfx_scegu_select_texture(int tile,unsigned id) { assert(tile==0 && id==7);binds++; }
static void sceGuDrawArray(unsigned primitive,unsigned format,unsigned count,const void* indices,const void* vertices);
'''
raster = r'''
static void sceGuDrawArray(unsigned primitive,unsigned format,unsigned count,const void* indices,const void* vertices) {
    assert(primitive==GU_SPRITES && format==15 && indices==NULL && count%2==0 && drawTarget==target);
    assert(!(statuses&(1U<<GU_DEPTH_TEST)) && depthMask==GU_TRUE);
    assert(!(statuses&(1U<<GU_BLEND)) && (statuses&(1U<<GU_STENCIL_TEST)));
    const ShadowVertex* v=vertices;
    for (unsigned i=0;i<count;i+=2) {
        const ShadowVertex *a=&v[i],*b=&v[i+1];
        for (unsigned y=a->y;y<b->y;y++) for (unsigned x=a->x;x<b->x;x++) {
            assert(x<64 && y<64);
            unsigned color=a->color;
            if (statuses&(1U<<GU_TEXTURE_2D)) {
                unsigned texel=textureSource[(a->v+y-a->y)*textureStride+a->u+x-a->x];
                if (texFunction==GU_TFX_REPLACE) color=texel;
                else {
                    unsigned value=(color&255)*(texel&255)/255;
                    color=value*0x01010101U;
                }
            }
            if ((statuses&(1U<<GU_ALPHA_TEST)) && (color>>24)<=alphaRef) continue;
            drawTarget[y*64+x]=(color&0xffffffU)|(stencilRef<<24);
        }
    }
}
'''
tests = r'''
static unsigned randomState=714;
static unsigned next(void) { return randomState=randomState*1664525U+1013904223U; }
static void compare(OotPspShadowMask* mask) {
    uint8_t expected[4096]={0};
    for (unsigned i=0;i<mask->count;i++) {
        OotPspShadowStamp* s=&mask->stamps[i];
        reference_stamp(expected,s->x,s->y,s->type,(mask->flags&OOT_PSP_SHADOW_WRAP_ROWS)!=0);
    }
    if (mask->flags&OOT_PSP_SHADOW_BORDER) {
        for (unsigned i=0;i<64;i++) expected[i]=expected[4032+i]=expected[i*64]=expected[i*64+63]=255;
    }
    if (mask->flags&OOT_PSP_SHADOW_GRADIENT) {
        for (unsigned i=0;i<4096;i++) if (expected[i]) expected[i]=mask->gradientBase-(i/64)*mask->gradientStep;
    }
    const unsigned saved=(1U<<GU_SCISSOR_TEST)|(1U<<GU_DEPTH_TEST)|(1U<<GU_BLEND)|(1U<<GU_LIGHTING);
    statuses=saved;copyCount=binds=0;
    memset(target,0xCA,sizeof(target));memset(pixels,0xAA,sizeof(pixels));
    gfx_scegu_upload_shadow_mask(0,7,mask);
    for (unsigned i=0;i<4096;i++) {
        if (pixels[i]!=(unsigned)expected[i]*0x01010101U) {
            fprintf(stderr,"mask mismatch pixel=%u actual=%08x expected=%u flags=%u\n",i,pixels[i],expected[i],mask->flags);
            for (unsigned j=0;j<mask->count;j++) fprintf(stderr,"stamp %d,%d type=%u\n",mask->stamps[j].x,mask->stamps[j].y,mask->stamps[j].type);
            abort();
        }
    }
    assert(statuses==saved && drawTarget==mainBuffer && memcmp(scissor,sScissorBounds,sizeof(scissor))==0);
    assert(depthMask==(sDepthWriteEnabled ? GU_FALSE : GU_TRUE));
    assert(copyCount==((mask->flags&OOT_PSP_SHADOW_GRADIENT) ? 2 : 1) && binds==1);
    assert(!sAppliedSamplerStateValid && sAppliedShader==&shader);
}
int main(void) {
    OotPspShadowMask mask;
    unsigned tests=0;
    // All body profiles at boundaries, including the boss's linear row wrapping.
    for (unsigned flags=0;flags<2;flags++) for (unsigned type=0;type<5;type++) {
        if (flags && type==4) continue; // Boss stamp tables only use types 0..3.
        for (int x=-8;x<=72;x+=4) for (int y=-8;y<=72;y+=4) {
            OotPspShadow_Begin(&mask,flags);OotPspShadow_Add(&mask,x,y,type);compare(&mask);tests++;
        }
    }
    // Random overlapping body/shield/cape stamps, debug borders and row fades.
    for (unsigned trial=0;trial<160;trial++) {
        unsigned flags=(trial&1 ? OOT_PSP_SHADOW_WRAP_ROWS : 0) | (trial&2 ? OOT_PSP_SHADOW_BORDER : 0) |
                       (trial&4 ? OOT_PSP_SHADOW_GRADIENT : 0);
        OotPspShadow_Begin(&mask,flags);mask.gradientBase=254;mask.gradientStep=4;
        unsigned stamps=trial==159 ? 256 : next()%120;
        for (unsigned i=0;i<stamps;i++) {
            unsigned type=next()%((flags&OOT_PSP_SHADOW_WRAP_ROWS) ? 7 : 5);
            // Boss profile 4 is unused; its original CPU default branch is profile 3.
            if ((flags&OOT_PSP_SHADOW_WRAP_ROWS) && type==4) type=3;
            OotPspShadow_Add(&mask,(int)(next()%96)-16,(int)(next()%96)-16,type);
        }
        sDepthWriteEnabled=(trial&8)!=0;compare(&mask);tests++;
    }
    printf("GU shadows: %u masks match original pixel loops; alpha, overlap, GPU copies and state restoration passed\n",tests);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-gpu-shadows-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + '#include "gfx_psp_shadow.inc.c"\n' + raster + reference + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "src/port/psp"),
                    "-I", str(ROOT / "src/port/psp/gfx"), str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
