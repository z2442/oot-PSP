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
/* Arena bytes below this offset have been written back to RAM. The arena is a
 * bump allocator, so every unpublished upload lies in [published, used). */
static size_t sVertexArenaPublished;
/* An open reservation is a batch the renderer is still writing in place:
 * [start, end) is held at its maximum size until commit trims it. */
static int sReserveOpen;
static size_t sReserveStart;
static size_t sReserveEnd;
static unsigned int sPendingDraws;
static unsigned int sPendingTriangles;

void gfx_psp_submit_reset(void) {
    if (sReserveOpen) {
        /* The GE is idle, so everything else is dead, but the open batch is
         * still being written. Keep its region; reclaim the rest next time. */
        sVertexArenaUsed = sReserveEnd;
        sVertexArenaPublished = sReserveStart;
    } else {
        sVertexArenaUsed = 0;
        sVertexArenaPublished = 0;
    }
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

void gfx_psp_submit_publish_vertices(void) {
    /* The GE executes only up to the stall address, so uploads need to reach
     * RAM just before a stall update, not when each one is written. One
     * writeback per publication replaces a kernel call per draw. Allocations
     * are 64-byte aligned, so the range covers whole cache lines. */
    if (sReserveOpen) {
        /* No draw can reference an uncommitted batch, and it is still being
         * written. Publish around it and keep `published` at its start, so
         * the batch is written back after commit, in its final state. */
        if (sReserveStart > sVertexArenaPublished) {
            sceKernelDcacheWritebackRange(sVertexArena + sVertexArenaPublished,
                                          sReserveStart - sVertexArenaPublished);
            sVertexArenaPublished = sReserveStart;
        }
        if (sVertexArenaUsed > sReserveEnd) {
            sceKernelDcacheWritebackRange(sVertexArena + sReserveEnd, sVertexArenaUsed - sReserveEnd);
        }
        return;
    }
    if (sVertexArenaUsed > sVertexArenaPublished) {
        sceKernelDcacheWritebackRange(sVertexArena + sVertexArenaPublished,
                                      sVertexArenaUsed - sVertexArenaPublished);
        sVertexArenaPublished = sVertexArenaUsed;
    }
}

void gfx_psp_submit_commit_vertices(size_t bytes) {
    size_t aligned;

    if (!sReserveOpen) {
        return;
    }
    sReserveOpen = 0;
    aligned = (bytes + 63U) & ~(size_t)63U;
    if (aligned > sReserveEnd - sReserveStart) {
        aligned = sReserveEnd - sReserveStart;
    }
    /* Trim only if nothing was allocated after the reservation. Otherwise
     * the unused tail stays allocated until the next reset. */
    if (sVertexArenaUsed == sReserveEnd) {
        sVertexArenaUsed = sReserveStart + aligned;
    }
}

void* gfx_psp_submit_reserve_vertices(size_t maxBytes) {
    unsigned char* result;

    /* An abandoned reservation holds no queued draw; release it. */
    gfx_psp_submit_commit_vertices(0);
    result = gfx_psp_submit_alloc_vertices(maxBytes);
    if (result == NULL) {
        return NULL;
    }
    sReserveOpen = 1;
    sReserveStart = (size_t)(result - sVertexArena);
    sReserveEnd = sVertexArenaUsed;
    return result;
}

int gfx_psp_submit_finish(void) {
    gfx_psp_submit_publish_vertices();
    return sceGuFinish();
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
        gfx_psp_submit_publish_vertices();
        sceGeListUpdateStallAddr(ge_list_executed[0], cursor);
        sPendingDraws = sPendingTriangles = 0;
    }
    /* SDK sceGuFinish publishes any tail below the thresholds before sync.
     * Object scopes retain the SDK's own publication on sceGuEndObject. */
}
