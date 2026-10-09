#!/usr/bin/env python3
"""Exercise texture uploads and GE bindings with the Kokiri's small I8 maps.

The GE reads rows on 16-byte boundaries. Verify every sampled texel, retained
logical dimensions, allocation boundaries, and updates to an existing texture.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/port/psp/gfx/psp_texture_manager.c").read_text()
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))


def replace_section(start, end, replacement):
    global source
    offset = source.index(start)
    source = source[:offset] + replacement + source[source.index(end, offset):]


# Replace hardware address validation and VFPU assembly, retaining the actual
# allocator, small-texture fallback, row copies, and GU binding commands.
replace_section("static int texman_buffer_is_readable(", "static unsigned int texman_active_texture_id(", r'''
static int texman_buffer_is_readable(const void* buffer, unsigned int size) {
    assert(buffer == input && size == input_size);
    return 1;
}
''')
replace_section("static void swizzle_fast(", "int texman_inited(", r'''
static void swizzle_fast(unsigned char* out, const unsigned char* in, unsigned int width, unsigned int height) {
    (void)out; (void)in; (void)width; (void)height;
    assert(0); /* These narrow uploads must use the actual linear fallback. */
}
''')

preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psp_texture_manager.h"
enum { GU_PSM_5650, GU_PSM_5551, GU_PSM_4444, GU_PSM_8888,
       GU_PSM_T4, GU_PSM_T8, GU_PSM_T16, GU_PSM_T32 };
#define GU_FALSE 0
#define GU_TRUE 1
static const void* input;
static unsigned int input_size;
static unsigned int image_width, image_height, image_stride, image_calls;
static const unsigned char* image_buffer;
static void OotPsp_MemcpyVfpu(void* out, const void* in, size_t n) { memcpy(out, in, n); }
static void sceKernelDcacheWritebackRange(void* p, unsigned n) { (void)p; (void)n; }
static void sceGuClutMode(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
static void sceGuClutLoad(int n, const void* p) { (void)n; (void)p; }
static void sceGuTexMode(unsigned a, int b, int c, unsigned d) { (void)a; (void)b; (void)c; (void)d; }
static void sceGuTexImage(int level, int w, int h, int stride, const void* p) {
    assert(level == 0);
    image_width=w; image_height=h; image_stride=stride; image_buffer=p; image_calls++;
}
'''

tests = r'''
static unsigned char vram[4096] __attribute__((aligned(16)));
static void check_upload(unsigned type, unsigned w, unsigned h, unsigned bpp) {
    input_size=w*h*bpp;
    unsigned char* pixels=malloc(input_size);
    assert(pixels);
    for (unsigned i=0; i<input_size; i++) pixels[i]=(i*11+90)&255;
    input=pixels;
    memset(vram, 0xCA, sizeof(vram));
    texman_reset(vram, sizeof(vram));
    unsigned id=texman_create();
    unsigned stride=((w*bpp+15)&~15U);
    texman_upload_swizzle(w,h,type,pixels);
    assert(image_width==w && image_height==h && image_stride==stride/bpp);
    assert(image_buffer==vram && textures[id].swizzled==GU_FALSE);
    for (unsigned y=0; y<h; y++) {
        for (unsigned x=0; x<w*bpp; x++)
            assert(image_buffer[y*image_stride*bpp+x]==pixels[y*w*bpp+x]);
        for (unsigned x=w*bpp; x<stride; x++) assert(image_buffer[y*stride+x]==0);
    }
    assert(vram[stride*h]==0xCA); /* No write beyond the reserved buffer. */
    assert((unsigned char*)psp_tex_buffer==vram+stride*h);
    unsigned calls=image_calls;
    memset(pixels, 123, input_size);
    texman_upload_swizzle(w,h,type,pixels);
    assert((unsigned char*)psp_tex_buffer==vram+stride*h);
    assert(image_calls==calls); /* Same dimensions and stride keep the binding. */
    for (unsigned y=0; y<h; y++)
        for (unsigned x=0; x<w*bpp; x++) assert(image_buffer[y*stride+x]==123);
    unsigned next=texman_create();
    texman_upload_swizzle(w,h,type,pixels);
    assert(textures[next].location==vram+stride*h);
    assert(textures[id].location[0]==123);
    free(pixels);
}
int main(void) {
    check_upload(GU_PSM_T8,8,8,1); /* Kokiri tunic; minimum stride is 16. */
    check_upload(GU_PSM_T8,4,4,1);
    check_upload(GU_PSM_5551,4,4,2);
    check_upload(GU_PSM_4444,2,4,2);
    check_upload(GU_PSM_8888,2,4,4);
    check_upload(GU_PSM_T8,16,4,1); /* Already aligned linear upload. */
    unsigned char pixels[64]={0}; input=pixels; input_size=sizeof(pixels);
    texman_reset(vram,127);
    unsigned id=texman_create();
    texman_upload_swizzle(8,8,GU_PSM_T8,pixels);
    assert(textures[id].width==0 && psp_tex_buffer==vram);
    assert(texman_storage_size(8,8,GU_PSM_T8)==128);
    puts("texture stride tests passed");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-texture-stride-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + source + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined,address", "-I", str(ROOT / "src/port/psp/gfx"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
