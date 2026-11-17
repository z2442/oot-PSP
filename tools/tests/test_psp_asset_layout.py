#!/usr/bin/env python3
"""Exercise canonical texture reads and linked refill recipes with sanitizers."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "src/port/psp"


def function(source, name):
    start = source.index("static s32 " + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


header = (PORT / "oot_psp_asset_loader.h").read_text()
types = "\n".join(re.findall(r"typedef struct OotPspExternalAsset(?:TextureRange)? \{.*?\} \w+;", header, re.S))
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "oot_psp_texture.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
#define OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS 2
#define OOT_PSP_ASSET_READ_OK 0
''' + types

read_stubs = r'''
#define BASE 0x20000000U
static const OotPspExternalAsset asset = { BASE, BASE+64, 0, 0, 3, 0, "mesh" };
const OotPspExternalAssetTextureRange gOotPspExternalAssetTextureRanges[] = {
    {BASE-32, BASE-16}, {BASE+3, BASE+30}, {BASE+40, BASE+56}, {BASE+80, BASE+88}
};
const size_t gOotPspExternalAssetTextureRangeCount = 4;
static u8 packed[64], canonical[64];
static unsigned reads, fail_read;
static s32 OotPsp_ReadPackedAssetFileRangeRaw(const OotPspExternalAsset* a, size_t offset,
                                             u8* out, size_t size, s32 cache, s32 vfpu, s32 audio) {
    assert(a && offset+size<=64);
    (void)cache; (void)vfpu; (void)audio;
    if (++reads==fail_read) return false;
    memcpy(out,packed+offset,size);
    return true;
}
'''
read_tests = r'''
int main(void) {
    for (unsigned i=0;i<64;i++) canonical[i]=(u8)(i*37+11);
    memcpy(packed,canonical,sizeof(packed));
    // Encode independently of the production conversion, including an
    // unaligned texture start and its final three-byte word.
    for (unsigned i=1;i<=2;i++) {
        unsigned first=gOotPspExternalAssetTextureRanges[i].vromStart-BASE;
        unsigned end=gOotPspExternalAssetTextureRanges[i].vromEnd-BASE;
        for (unsigned word=first;word<end;word+=8) {
            unsigned count=end-word<8 ? end-word : 8;
            for(unsigned j=0;j<count;j++) packed[word+j]=canonical[word+count-1-j];
        }
    }
    for (unsigned cache=0;cache<2;cache++) {
        for (unsigned offset=0;offset<=64;offset++) {
            for (unsigned size=0;size<=64-offset;size++) {
                for (unsigned phase=0;phase<8;phase++) {
                    u8 output[80]; memset(output,0xca,sizeof(output));
                    assert(OotPsp_ReadPackedAssetFileRange(&asset,offset,output+phase,size,cache,true,true));
                    assert(memcmp(output+phase,canonical+offset,size)==0);
                    for (unsigned i=0;i<phase;i++) assert(output[i]==0xca);
                    for (unsigned i=phase+size;i<80;i++) assert(output[i]==0xca);
                }
            }
        }
    }
    u8 output[64], window[64];
    assert(OotPsp_ReadPackedAssetFileRange(&asset,0,window,64,true,false,false));
    memcpy(output,window+4,17); // Logical cache hits copy already canonical bytes.
    assert(memcmp(output,canonical+4,17)==0);
    OotPspExternalAsset raw=asset; raw.flags=0;
    assert(OotPsp_ReadPackedAssetFileRange(&raw,0,output,64,false,false,false));
    assert(memcmp(output,packed,64)==0); // Structured/raw spans keep their bytes.
    assert(!OotPsp_ReadPackedAssetFileRange(&asset,63,output,2,false,false,false));
    assert(!OotPsp_ReadPackedAssetFileRange(&asset,SIZE_MAX,output,2,false,false,false));
    assert(!OotPsp_ReadPackedAssetFileRange(NULL,0,output,1,false,false,false));
    reads=0; fail_read=1;
    assert(!OotPsp_ReadPackedAssetFileRange(&asset,4,output,1,true,false,false));
    reads=0; fail_read=2; // An extra boundary-word read must propagate failure.
    assert(!OotPsp_ReadPackedAssetFileRange(&asset,4,output,1,true,false,false));
    puts("asset texture layout tests passed");
}
'''

# The legacy packed format and refill recipes remain unchanged. A selector
# reverses a source u64 to compiler layout; only texture records are then
# normalized for publication. Nontexture fields retain the compiler layout.
permutations = bytes(range(7, -1, -1)) * 56
blob = bytearray(b"OPB2" + struct.pack("<II", 2, 56) + permutations)
for destination, flags in ((0, 1), (16, 0)):
    payload = zlib.compress(bytes([0, 0]))
    blob.extend(struct.pack("<7I", destination, 0, 16, 2, len(payload), 0, flags))
    blob.extend(payload)
patch_stubs = r'''
u8 _ftext[64];
static const OotPspExternalAsset gOotPspExternalAssets[] = { { .name="code" } };
static const size_t gOotPspExternalAssetCount=1;
static u8 njpgdspMainTextStart[0xaf0], njpgdspMainDataStart[0x60];
static s32 OotPsp_AssetRead(void* output, uintptr_t vrom, size_t size) {
    (void)vrom;
    for(size_t i=0;i<size;i++) ((u8*)output)[i]=(u8)i;
    return OOT_PSP_ASSET_READ_OK;
}
static void sceKernelDcacheWritebackInvalidateAll(void) {}
''' + "\nconst u8 gOotPspRuntimePatchBlob[] = {" + ",".join(map(str, blob)) + "};\n" + r'''
const u8* const gOotPspRuntimePatchBlobEnd=gOotPspRuntimePatchBlob+sizeof(gOotPspRuntimePatchBlob);
'''
patch_source = (PORT / "oot_psp_runtime_patch.c").read_text()
patch_source = "\n".join(line for line in patch_source.splitlines()
                         if not line.startswith(("#include", "extern ")))
patch_tests = r'''
int main(void) {
    assert(OotPspRuntimePatch_Apply());
    for(unsigned i=0;i<16;i++) {
        assert(_ftext[i]==i);
        assert(_ftext[16+i]==(i^7));
    }
    puts("runtime texture publication tests passed");
}
'''

jpeg = (ROOT / "src/code/z_jpeg.c").read_text()
jpeg_source = "\n".join(line for line in jpeg.splitlines() if line.startswith("#define MARKER_")) + "\n"
start = jpeg.index("s32 Jpeg_IsJpeg(")
jpeg_source += jpeg[start:jpeg.index("\n}", start) + 2] + "\n"
start = jpeg.index("#define JPEG_PSP_MAX_WIDTH")
jpeg_source += jpeg[start:jpeg.index("\n#else\ns32 JpegPsp_WasDecoded", start)]
jpeg_stubs = r'''
#include <jpeglib.h>
#include <setjmp.h>
typedef uint16_t u16;
#define SCREEN_WIDTH 32
#define SCREEN_HEIGHT 32
#define osSyncPrintf printf
static void sceKernelDcacheWritebackRange(void* p, size_t size) { assert(p && size==2048); }
'''
jpeg_tests = r'''
int main(void) {
    struct jpeg_compress_struct encoder;
    struct jpeg_error_mgr error;
    unsigned char* compressed=NULL;
    unsigned long size=0;
    u8 row[4*3];
    for(unsigned i=0;i<4;i++) { row[3*i]=255; row[3*i+1]=row[3*i+2]=0; }
    encoder.err=jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    jpeg_mem_dest(&encoder,&compressed,&size);
    encoder.image_width=encoder.image_height=4;
    encoder.input_components=3; encoder.in_color_space=JCS_RGB;
    jpeg_set_defaults(&encoder); jpeg_set_quality(&encoder,100,TRUE);
    jpeg_start_compress(&encoder,TRUE);
    while (encoder.next_scanline<encoder.image_height) {
        JSAMPROW rows[]={row}; jpeg_write_scanlines(&encoder,rows,1);
    }
    jpeg_finish_compress(&encoder); jpeg_destroy_compress(&encoder);
    u8 output[2048+8], inplace[2048];
    assert(size<sizeof(inplace));
    memset(output,0xca,sizeof(output));
    assert(JpegPsp_Decode(compressed,output+3)==0);
    memcpy(inplace,compressed,size); free(compressed);
    assert(JpegPsp_Decode(inplace,inplace)==0);
    assert(JpegPsp_WasDecoded(inplace) && JpegPsp_WasDecoded(output+3));
    for(unsigned y=0;y<32;y++) for(unsigned x=0;x<32;x++) {
        unsigned offset=(y*32+x)*2;
        assert(output[3+offset]==(y<4 && x<4 ? 0xf8 : 0));
        assert(output[4+offset]==(y<4 && x<4 ? 0x01 : 0));
    }
    assert(memcmp(output+3,inplace,2048)==0);
    assert(output[2]==0xca && output[2051]==0xca);
    puts("JPEG canonical and in-place output tests passed");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-asset-layout-") as directory:
    path = Path(directory)
    loader = (PORT / "oot_psp_asset_loader.c").read_text()
    cases = {
        "reads": read_stubs + function(loader, "OotPsp_ReadPackedAssetFileRange") + read_tests,
        "patches": patch_stubs + patch_source + patch_tests,
        "jpeg": jpeg_stubs + jpeg_source + jpeg_tests,
    }
    for name, source in cases.items():
        c_file = path / f"{name}.c"
        c_file.write_text(preamble + source)
        binary = path / name
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined,address", "-I", str(PORT), str(c_file), "-lz", "-ljpeg", "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
