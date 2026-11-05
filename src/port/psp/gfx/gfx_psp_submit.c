/* GE draw/allocation command sequences adapted from PSPSDK.
 * Copyright (c) 2005 Jesper Svennevid. BSD license: docs/PSPSDK-LICENSE.txt.
 */
#include "gfx_psp_submit.h"

#include <stdint.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

/* PSPSDK's GuDisplayList prefix (src/gu/guInternal.h). Only the live command
 * cursor is accessed; the SDK still owns contexts, object stacks and finishes.
 * Keep the command encoding identical to sceGuDrawArray/sceGuGetMemory.
 * https://github.com/pspdev/pspsdk/tree/master/src/gu */
typedef struct GfxPspGuDisplayList {
    unsigned int* start;
    unsigned int* current;
    int parent_context;
} GfxPspGuDisplayList;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(GfxPspGuDisplayList) == 12, "PSPSDK display list ABI changed");
_Static_assert(offsetof(GfxPspGuDisplayList, current) == 4, "PSPSDK cursor ABI changed");
_Static_assert(offsetof(GfxPspGuDisplayList, parent_context) == 8, "PSPSDK parent ABI changed");
#endif
extern GfxPspGuDisplayList* gu_list;
extern int gu_curr_context;
extern int gu_object_stack_depth;
extern int ge_list_executed[2];

static unsigned char sVertexArena[GFX_PSP_VERTEX_ARENA_BYTES] __attribute__((aligned(64)));
static size_t sVertexArenaUsed;
static unsigned int sPendingDraws;
static unsigned int sPendingTriangles;

void gfx_psp_submit_reset(void) {
    sVertexArenaUsed = 0;
    sPendingDraws = sPendingTriangles = 0;
}

void* gfx_psp_submit_alloc_vertices(size_t bytes) {
    size_t aligned;
    void* result;

    if (bytes == 0 || bytes > sizeof(sVertexArena)) {
        return NULL;
    }
    aligned = (bytes + 63U) & ~(size_t)63U;
    if (aligned > sizeof(sVertexArena) - sVertexArenaUsed) {
        return NULL;
    }
    result = sVertexArena + sVertexArenaUsed;
    sVertexArenaUsed += aligned;
    return result;
}

void gfx_psp_submit_writeback_vertices(const void* vertices, size_t bytes) {
    /* Allocations never share a cache line. Publish the entire upload before
     * its PRIM is appended: font/SDK calls may also advance the GE cursor. */
    sceKernelDcacheWritebackRange(vertices, (bytes + 63U) & ~(size_t)63U);
}

void* gfx_psp_submit_get_memory(int bytes) {
    unsigned int* cursor;
    unsigned int* next;
    size_t aligned;

    if (gu_curr_context != GU_DIRECT) {
        return sceGuGetMemory(bytes);
    }
    if (bytes <= 0) {
        return NULL;
    }
    aligned = ((size_t)bytes + 3U) & ~(size_t)3U;
    cursor = gu_list->current;
    next = cursor + 2 + aligned / sizeof(*cursor);
    /* Inline data stays uncached with the command list. The following draw or
     * sceGuFinish publishes this jump; allocation needs no kernel call. */
    cursor[0] = 0x10000000U | (((uintptr_t)next >> 8) & 0x000f0000U);
    cursor[1] = 0x08000000U | ((uintptr_t)next & 0x00ffffffU);
    gu_list->current = next;
    return cursor + 2;
}

static unsigned int TriangleCount(int primitive, unsigned int count) {
    switch (primitive) {
        case GU_TRIANGLES: return count / 3;
        case GU_TRIANGLE_STRIP:
        case GU_TRIANGLE_FAN: return count > 2 ? count - 2 : 0;
        case GU_SPRITES: return (count / 2) * 2;
        default: return 0;
    }
}

void gfx_psp_submit_draw_array(int primitive, int format, int count, const void* indices, const void* vertices) {
    unsigned int* cursor;

    if (gu_curr_context != GU_DIRECT) {
        sceGuDrawArray(primitive, format, count, indices, vertices);
        return;
    }
    if (count <= 0) {
        return;
    }
    cursor = gu_list->current;
    if (format != 0) {
        *cursor++ = 0x12000000U | ((unsigned int)format & 0x00ffffffU);
    }
    if (indices != NULL) {
        *cursor++ = 0x10000000U | (((uintptr_t)indices >> 8) & 0x000f0000U);
        *cursor++ = 0x02000000U | ((uintptr_t)indices & 0x00ffffffU);
    }
    if (vertices != NULL) {
        *cursor++ = 0x10000000U | (((uintptr_t)vertices >> 8) & 0x000f0000U);
        *cursor++ = 0x01000000U | ((uintptr_t)vertices & 0x00ffffffU);
    }
    *cursor++ = 0x04000000U | ((((unsigned int)primitive << 16) | (unsigned int)count) & 0x00ffffffU);
    gu_list->current = cursor;

    sPendingDraws++;
    sPendingTriangles += TriangleCount(primitive, count);
    if (gu_object_stack_depth == 0 &&
        (sPendingDraws >= GFX_PSP_GE_BATCH_DRAWS || sPendingTriangles >= GFX_PSP_GE_BATCH_TRIANGLES)) {
        sceGeListUpdateStallAddr(ge_list_executed[0], cursor);
        sPendingDraws = sPendingTriangles = 0;
    }
    /* SDK sceGuFinish publishes any tail below the thresholds before sync.
     * Object scopes retain the SDK's own publication on sceGuEndObject. */
}
