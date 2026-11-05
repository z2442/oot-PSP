#!/usr/bin/env python3
"""Exercise actual command submission, cached uploads and fence/restart helpers."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GFX = ROOT/'src/port/psp/gfx'
backend = (GFX/'gfx_scegu.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?[^\n]*\b'+name+r'\([^;]*?\)\s*\{', backend, re.M)
    start = match.start()
    return backend[start:backend.index('\n}', backend.index('{', start))+2]+'\n'

stubs = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_psp_submit.c"
static unsigned int list[GFX_PSP_COMMAND_LIST_BYTES/sizeof(unsigned int)] __attribute__((aligned(64)));
static GfxPspGuDisplayList sdkList;
GfxPspGuDisplayList* gu_list=&sdkList;
int gu_curr_context=GU_DIRECT,gu_object_stack_depth,ge_list_executed[2]={7,8};
static unsigned updates,finishes,syncs,starts,fallbackDraws,fallbackAllocations,writebacks;
static void* published;
static unsigned publishedWords;
static int gpuNeedsSync;
static void* uploads[32];
static size_t uploadSizes[32];
static unsigned uploadHashes[32],uploadCount;
static unsigned hash(const void* data,size_t bytes) {
    unsigned h=2166136261U; const unsigned char* p=data;
    while (bytes--) h=(h^*p++)*16777619U;
    return h;
}
void sceKernelDcacheWritebackRange(const void* data,unsigned int bytes) {
    assert(((uintptr_t)data&63)==0 && (bytes&63)==0 && bytes>0);
    assert(uploadCount<32);
    uploads[uploadCount]=(void*)data;uploadSizes[uploadCount]=bytes;
    uploadHashes[uploadCount++]=hash(data,bytes);
    writebacks++; gpuNeedsSync=1;
}
int sceGeListUpdateStallAddr(int id,void* end) {
    assert(id==7 && end>=((void*)list) && end<=(void*)(list+sizeof(list)/sizeof(list[0])));
    published=end; publishedWords=(unsigned int*)end-list; updates++; return 0;
}
void sceGuDrawArray(int prim,int format,int count,const void* indices,const void* vertices) {
    (void)prim;(void)format;(void)count;(void)indices;(void)vertices;fallbackDraws++;
}
void* sceGuGetMemory(int bytes) { assert(bytes>0);fallbackAllocations++;return list; }
static void* sDrawBuffer=(void*)0x40044000;
static void* boundDrawBuffer;
static int sColorPixelFormat=3;
#define BUF_WIDTH 512
#define GU_LIST_COMMAND_RESERVE 4096
static int sceGuCheckList(void) { return (gu_list->current-gu_list->start)*sizeof(unsigned); }
static void sceGuFinish(void) {
    *gu_list->current++=0x0f000000;*gu_list->current++=0x0c000000;
    sceGeListUpdateStallAddr(7,gu_list->current); finishes++;
}
static void sceGuSync(int mode,int what) {
    (void)mode;(void)what;
    // Delayed GPU reads must see the original immutable uploads at every fence.
    for (unsigned i=0;i<uploadCount;i++) assert(hash(uploads[i],uploadSizes[i])==uploadHashes[i]);
    uploadCount=0;gpuNeedsSync=0;syncs++;
}
static void sceGuStart(int mode,void* commands) {
    assert(!gpuNeedsSync && mode==GU_DIRECT && commands==list);
    gu_list->start=gu_list->current=list;
    boundDrawBuffer=(void*)0x40000000; // SDK restores its saved primary target.
    starts++;
}
static void sceGuDrawBufferList(int format,void* target,int width) {
    assert(format==sColorPixelFormat && width==512);boundDrawBuffer=target;
}
static void OotPsp_MemcpyVfpu(void* dst,const void* src,size_t bytes) { memcpy(dst,src,bytes); }
static void gfx_scegu_start_list(void);
'''
helpers = ''.join(function(name) for name in ('gfx_scegu_reserve_list_memory',
    'gfx_scegu_sync_texture_cache', 'gfx_scegu_start_list', 'gfx_scegu_upload_vertices'))
tests = r'''
static void reset(void) {
    sceGuSync(0,0);gfx_scegu_start_list();updates=0;published=NULL;publishedWords=0;
}
static void draw(unsigned vertices) {
    gfx_psp_submit_draw_array(GU_TRIANGLES,0x123,vertices,NULL,(void*)0x08812000);
}
int main(void) {
    reset();
    for (int i=0;i<11;i++) draw(3);
    assert(updates==0); draw(3); assert(updates==1 && published==gu_list->current);
    assert(publishedWords==12*4); // VTYPE, BASE, VADDR, PRIM
    for (int i=0;i<12;i++) {
        unsigned* p=list+4*i;
        assert(p[0]==0x12000123 && p[1]==0x10080000 && p[2]==0x01812000 && p[3]==0x04030003);
    }
    reset();
    for (int i=0;i<9;i++) draw(66); // 9*22 triangles is below the triangle threshold.
    assert(updates==0); draw(66); assert(updates==1);
    reset();draw(663);assert(updates==1); // One large draw publishes immediately.
    reset();
    for (int i=0;i<11;i++) gfx_psp_submit_draw_array(GU_SPRITES,0,20,NULL,NULL);
    assert(updates==1 && publishedWords==11); // Sprite quads count as two triangles.
    reset();
    gfx_psp_submit_draw_array(GU_TRIANGLES,0x987654,6,(void*)0x09876540,(void*)0x089abc00);
    const unsigned expected[]={0x12987654,0x10090000,0x02876540,0x10080000,0x019abc00,0x04030006};
    assert(memcmp(list,expected,sizeof(expected))==0);
    unsigned* cursor=gu_list->current;
    gfx_psp_submit_draw_array(GU_TRIANGLES,0,0,NULL,NULL);assert(cursor==gu_list->current);
    sceGuFinish();assert(updates==1 && publishedWords==8); // Below-threshold tail is not lost.
    reset();
    for (int mode=GU_TRIANGLE_STRIP;mode<=GU_TRIANGLE_FAN;mode++) {
        reset();gfx_psp_submit_draw_array(mode,0,222,NULL,NULL);assert(updates==1);
    }
    reset();gu_object_stack_depth=1;
    for (int i=0;i<20;i++) draw(3);
    assert(updates==0);gu_object_stack_depth=0;draw(3);assert(updates==1);
    reset();gu_curr_context=GU_CALL;draw(3);
    assert(fallbackDraws==1 && updates==0 && gu_list->current==list);
    assert(gfx_psp_submit_get_memory(4)==list && fallbackAllocations==1);gu_curr_context=GU_DIRECT;
    reset();
    unsigned* memory=gfx_psp_submit_get_memory(5);
    assert(memory==list+2 && gu_list->current==list+4 && updates==0);
    assert(list[0]==(0x10000000U|(((uintptr_t)(list+4)>>8)&0xf0000U)));
    assert(list[1]==(0x08000000U|((uintptr_t)(list+4)&0xffffffU)));
    assert(gfx_psp_submit_get_memory(0)==NULL && gfx_psp_submit_get_memory(-1)==NULL);
    draw(3);sceGuFinish();assert(updates==1);
    // Uploads are line aligned, immutable and distinct across material passes.
    reset();unsigned char source[128];memset(source,0x5a,sizeof(source));
    void* base=gfx_scegu_upload_vertices(source,65);
    assert(((uintptr_t)base&63)==0 && uploadSizes[0]==128);
    memset(source,0xa5,sizeof(source));void* overlay=gfx_scegu_upload_vertices(source,65);
    assert((unsigned char*)overlay-(unsigned char*)base==128);
    assert(*(unsigned char*)base==0x5a && *(unsigned char*)overlay==0xa5 && writebacks==2);
    assert(gfx_psp_submit_alloc_vertices(0)==NULL);
    assert(gfx_psp_submit_alloc_vertices(GFX_PSP_VERTEX_ARENA_BYTES+1)==NULL);
    // Exhaustion cannot recycle an in-flight upload. The backend fences first.
    assert(gfx_psp_submit_alloc_vertices(GFX_PSP_VERTEX_ARENA_BYTES-256)!=NULL);
    assert(gfx_psp_submit_alloc_vertices(1)==NULL);
    unsigned previousSyncs=syncs;
    void* next=gfx_scegu_upload_vertices(source,sizeof(source));
    assert(syncs==previousSyncs+1 && next==base && boundDrawBuffer==sDrawBuffer);
    // Command-list rollover also fences the arena and restores the active target.
    gu_list->current=list+sizeof(list)/sizeof(list[0])-16;
    previousSyncs=syncs;sDrawBuffer=(void*)0x40088000;
    next=gfx_scegu_upload_vertices(source,64);
    assert(syncs==previousSyncs+1 && next==base && boundDrawBuffer==sDrawBuffer);
    gfx_scegu_sync_texture_cache();assert(boundDrawBuffer==sDrawBuffer);
    // A synthetic 120-draw, one-triangle workload: 240 old allocation/draw
    // updates become 120 writebacks + 10 draw publications (+ frame finish).
    reset();for (int i=0;i<120;i++) draw(3);
    assert(updates==10);sceGuFinish();assert(updates==11);
    sceGuSync(0,0);
    printf("GE submit: command parity, 12 draws/220 triangles, arena lifetime, multipass uploads and rollover passed\n");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-ge-submit-') as directory:
    path=Path(directory)
    (path/'pspgu.h').write_text('''#pragma once
#define GU_DIRECT 0
#define GU_CALL 1
#define GU_POINTS 0
#define GU_TRIANGLES 3
#define GU_TRIANGLE_STRIP 4
#define GU_TRIANGLE_FAN 5
#define GU_SPRITES 6
#define GU_SYNC_FINISH 0
#define GU_SYNC_WHAT_DONE 0
void sceGuDrawArray(int,int,int,const void*,const void*);
void* sceGuGetMemory(int);
''')
    (path/'pspge.h').write_text('int sceGeListUpdateStallAddr(int,void*);\n')
    (path/'pspkernel.h').write_text('void sceKernelDcacheWritebackRange(const void*,unsigned int);\n')
    (path/'test.c').write_text(stubs+helpers+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(path),'-I'+str(GFX),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
