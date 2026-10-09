/*
 * File: psp_texture_manager.c
 * Project: gfx
 * File Created: Friday, 7th August 2020 9:11:50 pm
 * Author: HaydenKow
 * -----
 * Copyright (c) 2020 Hayden Kowalchuk, Hayden Kowalchuk
 * License: BSD 3-clause "New" or "Revised" License, http://www.opensource.org/licenses/BSD-3-Clause
 */

#include "psp_texture_manager.h"
#include "oot_psp_memory.h"
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspgu.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static struct PSP_Texture textures[TEXMAN_MAX_TEXTURES];
static void *psp_tex_buffer = NULL;
static void *psp_tex_buffer_start = NULL;
static void *psp_tex_buffer_max = NULL;
static unsigned int psp_tex_number = 0;
unsigned int psp_tex_bound = 0;
static unsigned int sPspTexGuBound = 0;
static unsigned int sIntensityClut[256] __attribute__((aligned(16)));
static int sIntensityClutInited = 0;
static int sIntensityClutApplied = 0;

#define PSP_NATIVE_ADDR_START 0x08800000U
#define PSP_NATIVE_ADDR_END 0x0C000000U
#define PSP_UNCACHED_ADDR_MASK 0x40000000U

static int texman_buffer_is_readable(const void *buffer, unsigned int size) {
    uintptr_t addr = (uintptr_t)buffer;
    uintptr_t end;

    if (size == 0) {
        return 0;
    }

    end = addr + size;
    if (end < addr) {
        return 0;
    }

    return (addr >= PSP_NATIVE_ADDR_START) && (end <= PSP_NATIVE_ADDR_END);
}

static unsigned int texman_active_texture_id(void) {
    /*
     * Texture 0 is deliberately reserved as "invalid".  Never fall back to the
     * most recently created texture when no texture is bound: if texman_create()
     * failed, that old behavior could make the following upload overwrite the
     * previous valid texture.
     */
    if ((psp_tex_bound == 0) || (psp_tex_bound > psp_tex_number) ||
        (psp_tex_bound >= TEXMAN_MAX_TEXTURES)) {
        return 0;
    }

    return psp_tex_bound;
}

static void texman_log_bad_upload_buffer(const char *context, const void *buffer, unsigned int size) {
    static int sBadUploadBufferLogCount = 0;

    if (sBadUploadBufferLogCount < 16) {
        printf("oot-psp texman bad upload buffer context=%s addr=%08lx size=%u\n", context,
               (unsigned long)(uintptr_t)buffer, size);
    } else if (sBadUploadBufferLogCount == 16) {
        printf("oot-psp texman bad upload buffer logs suppressed\n");
    }

    sBadUploadBufferLogCount++;
}

static void texman_log_bad_state(const char *context, unsigned int tex_num, int width, int height,
                                 unsigned int type, unsigned int size) {
    static int sBadStateLogCount = 0;

    if (sBadStateLogCount < 32) {
        printf("oot-psp texman %s tex=%u size=%u dims=%dx%d type=%u cur=%08lx end=%08lx count=%u bound=%u gu=%u\n",
               context, tex_num, size, width, height, type,
               (unsigned long)(uintptr_t)psp_tex_buffer,
               (unsigned long)(uintptr_t)psp_tex_buffer_max,
               psp_tex_number, psp_tex_bound, sPspTexGuBound);
    } else if (sBadStateLogCount == 32) {
        printf("oot-psp texman bad-state logs suppressed\n");
    }

    sBadStateLogCount++;
}

static inline unsigned int getMemorySize(int width, int height, unsigned int psm) {
    uint64_t pixels;
    uint64_t bytes;

    if ((width <= 0) || (height <= 0)) {
        return 0;
    }

    pixels = (uint64_t)(unsigned int)width * (uint64_t)(unsigned int)height;

    switch (psm) {
        case GU_PSM_T4:
            bytes = pixels >> 1;
            break;

        case GU_PSM_T8:
            bytes = pixels;
            break;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            bytes = pixels * 2;
            break;

        case GU_PSM_8888:
        case GU_PSM_T32:
            bytes = pixels * 4;
            break;

        default:
            return 0;
    }

    if ((bytes == 0) || (bytes > 0xFFFFFFFFULL)) {
        return 0;
    }

    return (unsigned int)bytes;
}


static inline unsigned int getTexWidthBytes(int width, unsigned int psm) {
    uint64_t bytes;

    if (width <= 0) {
        return 0;
    }

    switch (psm) {
        case GU_PSM_T4:
            bytes = (unsigned int)width >> 1;
            break;

        case GU_PSM_T8:
            bytes = (unsigned int)width;
            break;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            bytes = (uint64_t)(unsigned int)width * 2;
            break;

        case GU_PSM_8888:
        case GU_PSM_T32:
            bytes = (uint64_t)(unsigned int)width * 4;
            break;

        default:
            return 0;
    }

    if ((bytes == 0) || (bytes > 0xFFFFFFFFULL)) {
        return 0;
    }

    return (unsigned int)bytes;
}

/* GE texture rows have a 16-byte stride even for small, linear textures.
 * Keep the sampling dimensions unchanged; only the backing buffer is padded. */
static unsigned int texman_buffer_width(int width, unsigned int type) {
    unsigned int alignment;

    switch (type) {
        case GU_PSM_T4:
            alignment = 32;
            break;
        case GU_PSM_T8:
            alignment = 16;
            break;
        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            alignment = 8;
            break;
        case GU_PSM_8888:
        case GU_PSM_T32:
            alignment = 4;
            break;
        default:
            return 0;
    }

    if ((width <= 0) || ((unsigned int)width > 0x7FFFFFFFU - (alignment - 1))) {
        return 0;
    }
    return ((unsigned int)width + alignment - 1) & ~(alignment - 1);
}

static unsigned int texman_storage_size(int width, int height, unsigned int type) {
    return getMemorySize(texman_buffer_width(width, type), height, type);
}


static inline uintptr_t texman_align_up(uintptr_t value) {
    return (value + (TEX_ALIGNMENT - 1)) & ~(uintptr_t)(TEX_ALIGNMENT - 1);
}

static void swizzle_fast(unsigned char *out, const unsigned char *in, unsigned int width,
                         unsigned int height) {
    unsigned int blockx, blocky;
    unsigned int j;

    unsigned int width_blocks = (width / 16);
    unsigned int height_blocks = (height / 8);

    unsigned int src_pitch = (width - 16) / 4;
    unsigned int src_row = width * 8;

    const unsigned char *ysrc = in;
    unsigned int *dst = (unsigned int *) out;

    for (blocky = 0; blocky < height_blocks; ++blocky) {
        const unsigned char *xsrc = ysrc;
        for (blockx = 0; blockx < width_blocks; ++blockx) {
            const unsigned int *src = (unsigned int *) xsrc;
            for (j = 0; j < 8; ++j) {
                __asm__ volatile(
                    ".set push\n"
                    ".set noreorder\n"
                    "ulv.q C000, 0(%[src])\n"
                    "sv.q C000, 0(%[dst])\n"
                    ".set pop\n"
                    :
                    : [src] "r"(src), [dst] "r"(dst)
                    : "memory");
                dst += 4;
                src += 4;
                src += src_pitch;
            }
            xsrc += 16;
        }
        ysrc += src_row;
    }
}

int texman_inited(void) {
    return psp_tex_buffer != 0;
}

void texman_reset(void *buf, unsigned int size) {
    uintptr_t start = (uintptr_t)buf;

    memset(textures, 0, sizeof(textures));
    psp_tex_number = 0;
    psp_tex_bound = 0;
    sPspTexGuBound = 0;
    sIntensityClutApplied = 0;

    psp_tex_buffer = psp_tex_buffer_start = buf;
    if ((buf == NULL) || (size == 0) || (start > (UINTPTR_MAX - size))) {
        psp_tex_buffer_max = buf;
        texman_log_bad_state("bad reset buffer", 0, 0, 0, 0, size);
    } else {
        psp_tex_buffer_max = (void *)(start + size);
    }

#ifdef DEBUG
    {
        char msg[64];
        sprintf(msg, "TEXMAN reset @ %p size %d bytes\n", buf, size);
        sceIoWrite(1, msg, strlen(msg));
    }
#endif
}

void texman_clear(void) {
    /*
     * IMPORTANT: this only clears texman.  The higher-level renderer texture
     * map must be cleared at the same time.  texman_create() therefore never
     * calls this implicitly.
     */
    memset(textures, 0, sizeof(textures));
    psp_tex_number = 0;
    psp_tex_bound = 0;
    sPspTexGuBound = 0;
    sIntensityClutApplied = 0;
    psp_tex_buffer = psp_tex_buffer_start;

#ifdef DEBUG
    {
        char msg[64];
        sprintf(msg, "TEXMAN clear %p size %d bytes!\n", psp_tex_buffer, TEXMAN_BUFFER_SIZE);
        sceIoWrite(1, msg, strlen(msg));
    }
#endif
}

void texman_set_buffer(void *buf, unsigned int size) {
    uintptr_t start = (uintptr_t)buf;

    psp_tex_buffer = buf;
    if ((buf == NULL) || (size == 0) || (start > (UINTPTR_MAX - size))) {
        psp_tex_buffer_max = buf;
        texman_log_bad_state("bad set buffer", 0, 0, 0, 0, size);
        return;
    }

    psp_tex_buffer_max = (void *)(start + size);
}

int gfx_vram_space_available(void) {
    uintptr_t current = (uintptr_t)psp_tex_buffer;
    uintptr_t end = (uintptr_t)psp_tex_buffer_max;

    if ((psp_tex_buffer == NULL) || (psp_tex_buffer_max == NULL) || (current > end)) {
        return 0;
    }

    return (end - current) > (32 * 1024);
}

int texman_vram_space_available(unsigned int size) {
    uintptr_t start = (uintptr_t)psp_tex_buffer;
    uintptr_t end = (uintptr_t)psp_tex_buffer_max;
    uintptr_t requestedEnd;
    uintptr_t alignedEnd;

    if ((size == 0) || (psp_tex_buffer == NULL) || (psp_tex_buffer_max == NULL) || (start > end)) {
        return 0;
    }

    if (start > (UINTPTR_MAX - size)) {
        return 0;
    }

    requestedEnd = start + size;
    if (requestedEnd > (UINTPTR_MAX - (TEX_ALIGNMENT - 1))) {
        return 0;
    }

    alignedEnd = texman_align_up(requestedEnd);
    return alignedEnd <= end;
}

int texman_texture_slot_available(void) {
    return psp_tex_number < (TEXMAN_MAX_TEXTURES - 1);
}

unsigned char *texman_get_tex_data(unsigned int num) {
    if ((num == 0) || (num > psp_tex_number) || (num >= TEXMAN_MAX_TEXTURES)) {
        return NULL;
    }

    return textures[num].location;
}

unsigned char texman_get_tex_type(unsigned int num) {
    if ((num == 0) || (num > psp_tex_number) || (num >= TEXMAN_MAX_TEXTURES)) {
        return 0;
    }

    return textures[num].type;
}

struct PSP_Texture *texman_reserve_memory(int width, int height, unsigned int type) {
    unsigned int tex_size = texman_storage_size(width, height, type);
    unsigned int tex_num = texman_active_texture_id();
    struct PSP_Texture *current;
    uintptr_t allocation;
    uintptr_t newEnd;

    if ((tex_num == 0) || (tex_size == 0)) {
        texman_log_bad_state("invalid reserve", tex_num, width, height, type, tex_size);
        return NULL;
    }

    current = &textures[tex_num];

    /*
     * Fast path for normal dynamic texture updates: same texture ID, same
     * dimensions/format, same storage.  No allocator movement at all.
     */
    if ((current->location != NULL) && (current->width == width) &&
        (current->height == height) && (current->type == type)) {
        return current;
    }

    /*
     * If this ID already owns storage and the replacement fits in that storage,
     * keep its address.  This prevents repeated same-ID updates with a smaller
     * or differently formatted texture from consuming the cache forever.
     */
    if (current->location != NULL) {
        unsigned int old_size = texman_storage_size(current->width, current->height, current->type);

        if ((old_size != 0) && (tex_size <= old_size)) {
            if (sPspTexGuBound == tex_num) {
                sPspTexGuBound = 0;
            }
            return current;
        }

        /*
         * If this texture is the most recent allocation, it can safely grow in
         * place.  Do not move the global bump pointer until the larger size has
         * been proven to fit; otherwise a failed growth could make the next
         * texture overlap this still-valid texture.
         */
        if (old_size != 0) {
            uintptr_t old_start = (uintptr_t)current->location;
            uintptr_t old_end;
            uintptr_t candidate_end;

            if ((old_start <= (UINTPTR_MAX - old_size)) &&
                ((old_start + old_size) <= (UINTPTR_MAX - (TEX_ALIGNMENT - 1)))) {
                old_end = texman_align_up(old_start + old_size);

                if ((old_end == (uintptr_t)psp_tex_buffer) &&
                    (old_start <= (UINTPTR_MAX - tex_size)) &&
                    ((old_start + tex_size) <= (UINTPTR_MAX - (TEX_ALIGNMENT - 1)))) {
                    candidate_end = texman_align_up(old_start + tex_size);

                    if (candidate_end <= (uintptr_t)psp_tex_buffer_max) {
                        allocation = old_start;
                        newEnd = candidate_end;
                        current->location = (unsigned char *)allocation;
                        psp_tex_buffer = (void *)newEnd;

                        if (sPspTexGuBound == tex_num) {
                            sPspTexGuBound = 0;
                        }

#ifdef DEBUG
                        printf("TEX_MAN tex [%u] grew tail allocation to %u bytes @ %08lx left: %lu kb\\n",
                               tex_num, tex_size, (unsigned long)allocation,
                               (unsigned long)(((uintptr_t)psp_tex_buffer_max - newEnd) / 1024));
#endif
                        return current;
                    }
                }
            }
        }
    }

    if (!texman_vram_space_available(tex_size)) {
        texman_log_bad_state("out of texture memory", tex_num, width, height, type, tex_size);
        return NULL;
    }

    allocation = (uintptr_t)psp_tex_buffer;
    newEnd = texman_align_up(allocation + tex_size);

    current->location = (unsigned char *)allocation;
    psp_tex_buffer = (void *)newEnd;

    /*
     * A texture ID can be re-uploaded with a different size/format.  If that
     * changed its storage, force the next bind to re-emit GU_TEXMODE/TEXIMAGE
     * instead of incorrectly accepting the old cached GU binding.
     */
    if (sPspTexGuBound == tex_num) {
        sPspTexGuBound = 0;
    }

#ifdef DEBUG
    printf("TEX_MAN tex [%u] reserved %u bytes @ %08lx left: %lu kb\n",
           tex_num, tex_size, (unsigned long)allocation,
           (unsigned long)(((uintptr_t)psp_tex_buffer_max - newEnd) / 1024));
#endif

    return current;
}

static void texman_ensure_intensity_clut(void) {
    unsigned int i;

    if (sIntensityClutInited) {
        return;
    }

    for (i = 0; i < 256; i++) {
        sIntensityClut[i] = i | (i << 8) | (i << 16) | (i << 24);
    }

    sceKernelDcacheWritebackRange(sIntensityClut, sizeof(sIntensityClut));
    sIntensityClutInited = 1;
}

static inline void texman_writeback_if_cached(void *buffer, unsigned int size) {
    if (((uintptr_t)buffer & PSP_UNCACHED_ADDR_MASK) == 0) {
        sceKernelDcacheWritebackRange(buffer, size);
    }
}

unsigned int texman_create(void) {
    if (!texman_texture_slot_available()) {
        /*
         * Never clear here.  Doing so leaves the renderer's higher-level cache
         * holding IDs whose texman entries have just been erased.
         *
         * The renderer already knows how to clear both cache layers together;
         * returning 0 lets that path detect/recover from exhaustion safely.
         */
        psp_tex_bound = 0;
        texman_log_bad_state("out of texture slots", 0, 0, 0, 0, 0);
        return 0;
    }

    psp_tex_number++;
    textures[psp_tex_number] = (struct PSP_Texture){
        location : psp_tex_buffer,
        width : 0,
        height : 0,
        type : 0,
        swizzled : 0
    };
    psp_tex_bound = psp_tex_number;

#ifdef DEBUG
    printf("TEX_MAN new tex [%u] @ %08lx\n", psp_tex_number,
           (unsigned long)(uintptr_t)psp_tex_buffer);
#endif
    return psp_tex_number;
}

void texman_upload_swizzle(int width, int height, unsigned int type, const void *buffer) {
    unsigned int size = getMemorySize(width, height, type);
    unsigned int widthBytes = getTexWidthBytes(width, type);
    struct PSP_Texture *current;
    unsigned int tex_num;

    if ((size == 0) || (widthBytes == 0)) {
        texman_log_bad_state("bad swizzle dimensions", texman_active_texture_id(),
                             width, height, type, size);
        return;
    }

    if (!texman_buffer_is_readable(buffer, size)) {
        texman_log_bad_upload_buffer("swizzle", buffer, size);
        return;
    }

    /*
     * swizzle_fast() operates on complete 16-byte x 8-row blocks.  Falling
     * back to linear upload for partial blocks avoids silently dropping the
     * right/bottom edge of unusual textures.
     */
    if ((widthBytes < 16) || (height < 8) ||
        ((widthBytes & 15U) != 0) || (((unsigned int)height & 7U) != 0)) {
        texman_upload(width, height, type, buffer);
        return;
    }

    tex_num = texman_active_texture_id();
    if (tex_num == 0) {
        texman_log_bad_state("swizzle with no active texture", 0, width, height, type, size);
        return;
    }

    current = texman_reserve_memory(width, height, type);
    if (current == NULL) {
        return;
    }

    if ((current->type != type) || (current->swizzled != GU_TRUE)) {
        if (sPspTexGuBound == tex_num) {
            sPspTexGuBound = 0;
        }
    }

    current->width = width;
    current->height = height;
    current->type = type;

    /* Width passed to swizzle_fast is in bytes, not pixels. */
    swizzle_fast(current->location, buffer, widthBytes, height);
    current->swizzled = GU_TRUE;

#ifdef DEBUG
    printf("TEX_MAN upload swizzled [%u]\n", tex_num);
#endif

    texman_writeback_if_cached(current->location, size);
    texman_bind_tex(tex_num);
}

void texman_upload(int width, int height, unsigned int type, const void *buffer) {
    unsigned int size = getMemorySize(width, height, type);
    const unsigned int rowBytes = getTexWidthBytes(width, type);
    const unsigned int stride = getTexWidthBytes(texman_buffer_width(width, type), type);
    struct PSP_Texture *current;
    unsigned int tex_num;

    if (size == 0) {
        texman_log_bad_state("bad plain dimensions", texman_active_texture_id(),
                             width, height, type, size);
        return;
    }

    if (!texman_buffer_is_readable(buffer, size)) {
        texman_log_bad_upload_buffer("plain", buffer, size);
        return;
    }

    tex_num = texman_active_texture_id();
    if (tex_num == 0) {
        texman_log_bad_state("plain upload with no active texture", 0, width, height, type, size);
        return;
    }

    current = texman_reserve_memory(width, height, type);
    if (current == NULL) {
        return;
    }

    if ((current->type != type) || (current->swizzled != GU_FALSE)) {
        if (sPspTexGuBound == tex_num) {
            sPspTexGuBound = 0;
        }
    }

    current->width = width;
    current->height = height;
    current->type = type;
    current->swizzled = GU_FALSE;

    if (rowBytes == stride) {
        OotPsp_MemcpyVfpu(current->location, buffer, size);
    } else {
        for (int y = 0; y < height; y++) {
            unsigned char* row = current->location + (size_t)y * stride;
            memcpy(row, (const unsigned char*)buffer + (size_t)y * rowBytes, rowBytes);
            memset(row + rowBytes, 0, stride - rowBytes);
        }
    }

#ifdef DEBUG
    // printf("TEX_MAN upload plain [%u]\n", tex_num);
#endif

    texman_writeback_if_cached(current->location, texman_storage_size(width, height, type));
    texman_bind_tex(tex_num);
}

void texman_bind_tex(unsigned int num) {
    const struct PSP_Texture *current;
    unsigned int size;
    uintptr_t location;
    uintptr_t end;

    if ((num == 0) || (num > psp_tex_number) || (num >= TEXMAN_MAX_TEXTURES)) {
        texman_log_bad_state("invalid bind id", num, 0, 0, 0, 0);
        return;
    }

    current = &textures[num];
    size = texman_storage_size(current->width, current->height, current->type);

    if ((current->location == NULL) || (size == 0)) {
        texman_log_bad_state("bind empty texture", num, current->width, current->height,
                             current->type, size);
        return;
    }

    location = (uintptr_t)current->location;
    end = (uintptr_t)psp_tex_buffer_max;

    if ((location > end) || (size > (end - location))) {
        texman_log_bad_state("bind texture outside cache", num, current->width, current->height,
                             current->type, size);
        return;
    }

    if (sPspTexGuBound == num) {
        psp_tex_bound = num;
        return;
    }

#ifdef DEBUG
    /* Note this will SPAM if you enable */
    // if (psp_tex_bound != num)
    //    printf("TEX_MAN bind tex [%u]\n", num);
#endif

    if (current->type == GU_PSM_T8) {
        texman_ensure_intensity_clut();
        if (!sIntensityClutApplied) {
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(32, sIntensityClut);
            sIntensityClutApplied = 1;
        }
    }

    sceGuTexMode(current->type, 0, 0, current->swizzled);
    sceGuTexImage(0, current->width, current->height,
                  texman_buffer_width(current->width, current->type), current->location);
    psp_tex_bound = num;
    sPspTexGuBound = num;
}

void texman_invalidate_binding(void) {
    sPspTexGuBound = 0;
    sIntensityClutApplied = 0;
}

unsigned int texman_get_bound(void) {
    return psp_tex_bound;
}
