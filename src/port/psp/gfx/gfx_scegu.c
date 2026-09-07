#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "ultra64.h"

#ifdef GU_PI
#undef GU_PI
#endif
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>

#ifndef OOT_PSP_USE_INTRAFONT
#define OOT_PSP_USE_INTRAFONT 0
#endif

#if OOT_PSP_USE_INTRAFONT
#include <intraFont.h>
#else
#define INTRAFONT_ALIGN_LEFT   0
#define INTRAFONT_ALIGN_CENTER 0
#endif

#include "psp_texture_manager.h"
#include "gfx_fast3d.h"
#include "oot_psp_controls.h"
#include "oot_psp_dve.h"
#include "oot_psp_memory.h"
#include "oot_psp_video.h"

#ifndef OOT_PSP_WAIT_VBLANK
#define OOT_PSP_WAIT_VBLANK 1
#endif
#include "oot_port_macros.h"

#define LCD_BUFFER_WIDTH 512
#define LCD_SCREEN_WIDTH 480
#define LCD_SCREEN_HEIGHT 272
#define TV_BUFFER_WIDTH 768
#define TV_SCREEN_WIDTH 720
#define TV_SCREEN_HEIGHT 480
#define TV_INTERLACED_BUFFER_HEIGHT 503
#define TV_INTERLACED_FIELD_OFFSET 262
#define HOME_MENU_WIDTH 480
#define HOME_MENU_HEIGHT 272
#define PSP_PHAT_VRAM_SIZE (2 * 1024 * 1024)

static int sBufferWidth = LCD_BUFFER_WIDTH;
static int sScreenWidth = LCD_SCREEN_WIDTH;
static int sScreenHeight = LCD_SCREEN_HEIGHT;
static int sMaxBufferWidth = LCD_BUFFER_WIDTH;
static int sMaxScreenHeight = LCD_SCREEN_HEIGHT;
static int sColorPixelFormat = GU_PSM_5650;
static int sColorPixelSize = sizeof(uint16_t);
static size_t sFramebufferCapacity;
static size_t sVramSize = PSP_PHAT_VRAM_SIZE;

#define BUF_WIDTH (sBufferWidth)
#define SCR_WIDTH (sScreenWidth)
#define SCR_HEIGHT (sScreenHeight)
#define FRAMEBUFFER_SIZE ((size_t)BUF_WIDTH * (size_t)SCR_HEIGHT * (size_t)sColorPixelSize)
#define VRAM_SIZE (sVramSize)

float identity_matrix[4][4] __attribute__((aligned(16))) = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

/* Shader IDs
id        alp fog edg nse ut0 ut1 num sin0 sin1 mul0 mul1 mix0 mix1 cas
-----------------------------------------------------------------------
69        0   0   0   0   1   0   1   0    1    1    1    1    1    0
512       0   0   0   0   0   0   1   1    1    0    1    0    1    0
909       0   0   0   0   1   0   1   0    1    0    1    1    1    0
1361      0   0   0   0   1   0   2   0    1    0    1    1    1    0
2560      0   0   0   0   1   0   0   1    1    0    1    0    1    0
17059909  1   0   0   0   1   0   1   0    0    1    1    1    1    1
17062400  1   0   0   0   1   0   1   1    0    0    1    0    1    0
17305729  1   0   0   0   0   0   2   0    0    1    1    1    1    1
18092101  1   0   0   0   1   0   1   0    0    1    1    1    1    0
18874437  1   0   0   0   1   0   1   0    1    1    0    1    0    0
18874880  1   0   0   0   0   0   1   1    1    0    0    0    0    1
18875277  1   0   0   0   1   0   1   0    1    0    0    1    0    0
18876928  1   0   0   0   1   0   1   1    1    0    0    0    0    0
27263045  1   0   0   0   1   0   1   0    1    1    0    1    0    0
27265536  1   0   0   0   1   0   0   1    1    0    0    0    0    1
27265647  1   0   0   0   1   1   1   0    1    0    0    1    0    0
52428869  1   1   0   0   1   0   1   0    1    1    0    1    0    0
52429312  1   1   0   0   0   0   1   1    1    0    0    0    0    1
52431360  1   1   0   0   1   0   1   1    1    0    0    0    0    0
84168773  1   0   1   0   1   0   1   0    0    1    1    1    1    1
85983744  1   0   1   0   0   0   1   1    1    0    0    0    0    1
94374400  1   0   1   0   1   0   0   1    1    0    0    0    0    1
127928832 1   1   1   0   1   0   0   1    1    0    0    0    0    1
153092165 1   0   0   1   1   0   1   0    1    1    0    1    0    0
153092608 1   0   0   1   0   0   1   1    1    0    0    0    0    1
153093005 1   0   0   1   1   0   1   0    1    0    0    1    0    0
153094656 1   0   0   1   1   0   1   1    1    0    0    0    0    0

printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", shader_id,
    cc_features.opt_alpha,
    cc_features.opt_fog,
    cc_features.opt_texture_edge,
    cc_features.opt_noise,
    cc_features.used_textures[0],
    cc_features.used_textures[1],
    cc_features.num_inputs,
    cc_features.do_single[0],
    cc_features.do_single[1],
    cc_features.do_multiply[0],
    cc_features.do_multiply[1],
    cc_features.do_mix[0],
    cc_features.do_mix[1],
    cc_features.color_alpha_same
);
*/

/* Shader Working List:
84168773    - Menu Overlays
*/

/* Shader Broken List:
153092165   - Noise
153092608   - Noise
153093005   - Noise
153094656   - Noise
*/

// clang-format off
// clang-format on

unsigned int __attribute__((aligned(64))) list[262144 * 2];

/* sceGuGetMemory stores transient vertices inside the display list. Fog adds a
 * second vertex stream, so dense scenes can otherwise run past the fixed list
 * buffer and corrupt later texture commands on real hardware. Leave room for
 * the draw/state commands emitted after each allocation and safely continue in
 * a fresh list when necessary. GE state and the frame/depth buffers persist
 * across direct lists. */
#define GU_LIST_COMMAND_RESERVE 4096

static void gfx_scegu_reserve_list_memory(size_t dataSize) {
    const size_t allocationSize = (dataSize + 3) & ~(size_t)3;
    const size_t requiredSize = allocationSize + 8 + GU_LIST_COMMAND_RESERVE;

    if (((size_t)sceGuCheckList() + requiredSize) <= sizeof(list)) {
        return;
    }

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuStart(GU_DIRECT, list);
}

void gfx_scegu_sync_texture_cache(void) {
    /* Texture-cache eviction reuses the same VRAM addresses immediately. Make
     * sure every draw that references the old contents has completed before
     * the allocator overwrites them, then continue the frame in a fresh list. */
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuStart(GU_DIRECT, list);
}

static unsigned int staticOffset = 0;
unsigned int scegu_fog_color = 0;

static unsigned int getMemorySize(unsigned int width, unsigned int height, unsigned int psm) {
    switch (psm) {
        case GU_PSM_T4:
            return (width * height) >> 1;

        case GU_PSM_T8:
            return width * height;

        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
        case GU_PSM_T16:
            return 2 * width * height;

        case GU_PSM_8888:
        case GU_PSM_T32:
            return 4 * width * height;

        default:
            return 0;
    }
}

#define TEX_ALIGNMENT (16)
void *getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    unsigned int memSize = getMemorySize(width, height, psm);
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return result;
}

void *getStaticVramBufferBytes(size_t bytes) {
    unsigned int memSize = bytes;
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return (void *) (((unsigned int) result) + ((unsigned int) sceGeEdramGetAddr()));
}

static size_t getStaticVramBytesRemaining(void) {
    if (staticOffset >= VRAM_SIZE) {
        return 0;
    }

    return VRAM_SIZE - staticOffset;
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "oot_port_macros.h"

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    bool cache_valid;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

#define SHADER_PROGRAM_CACHE_CAPACITY 256
#define SHADER_PROGRAM_CACHE_MASK (SHADER_PROGRAM_CACHE_CAPACITY - 1)

struct SamplerState {
    int min_filter;
    int mag_filter;
    int wrap_s;
    int wrap_t;
    uint32_t tex;
};

typedef struct Vertex {
    float u, v;
    unsigned int color;
    float x, y, z;
} Vertex;

typedef struct FogVertex {
    float u, v;
    unsigned int color;
    float x, y, z;
} FogVertex;

typedef struct FogColorVertex {
    unsigned int color;
    float x, y, z;
} FogColorVertex;

typedef char FogVertex_size_check[(sizeof(FogVertex) == 24) ? 1 : -1];
typedef char FogColorVertex_size_check[(sizeof(FogColorVertex) == 16) ? 1 : -1];

typedef struct VertexColor {
    unsigned short a, b;
    unsigned long color;
    unsigned short x, y, z;
} VertexColor;

/* Color combiners retain ShaderProgram pointers, so cache entries must never
 * move or be evicted. Open addressing keeps those pointers stable while
 * avoiding an increasingly expensive linear scan during long play sessions. */
static struct ShaderProgram shader_program_cache[SHADER_PROGRAM_CACHE_CAPACITY];
static uint16_t shader_program_cache_count;
static struct ShaderProgram *cur_shader = NULL;
static struct ShaderProgram *sAppliedShader = NULL;
static struct SamplerState tmu_state[2];
static int active_texture_tile = -1;
static struct SamplerState sAppliedSamplerState;
static bool sAppliedSamplerStateValid;
static bool gl_blend = false;
static bool sDepthTestEnabled = true;
static bool sDepthWriteEnabled = true;
static unsigned int sTextureEnvColor = 0xffffffff;
static void *sDrawBuffer;
static void *sDisplayBuffer;
static void *sDepthBuffer;
static void *sPrimaryBuffer;
static void *sSecondaryBuffer;
static bool sInterlacedOutput;
static bool sCrt240pOutput;
static bool sPauseBgActive;
static bool sPauseBgCaptureRequested;
static bool sPauseBgCaptured;
static void *sPauseBgBuffer;
static bool sHomeMenuBgActive;
static bool sHomeMenuBgCaptureRequested;
static bool sHomeMenuBgCaptured;
static void *sHomeMenuBgBuffer;
static void *sHomeMenuBgBlurScratch;
#if OOT_PSP_USE_INTRAFONT
static intraFont *sHomeMenuFont;
static bool sHomeMenuFontInitTried;
#endif

static struct ShaderProgram *gfx_scegu_probe_shader_cache(uint32_t shader_id, bool *found) {
    size_t cacheIndex = (shader_id * 0x9E3779B1U) >> 24;

    for (size_t probe = 0; probe < SHADER_PROGRAM_CACHE_CAPACITY; probe++) {
        struct ShaderProgram *prg = &shader_program_cache[cacheIndex];

        if (!prg->cache_valid) {
            *found = false;
            return prg;
        }
        if (prg->shader_id == shader_id) {
            *found = true;
            return prg;
        }
        cacheIndex = (cacheIndex + 1) & SHADER_PROGRAM_CACHE_MASK;
    }

    *found = false;
    return NULL;
}

static struct ShaderProgram *gfx_scegu_lookup_shader(uint32_t shader_id) {
    bool found;
    struct ShaderProgram *prg = gfx_scegu_probe_shader_cache(shader_id, &found);

    return found ? prg : NULL;
}

static void *gfx_scegu_vram_cpu_addr(const void *vramBuffer) {
    return (void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) + ((uintptr_t)vramBuffer & 0x00FFFFFFU));
}

static bool gfx_scegu_read_depth(int32_t x, int32_t y, uint16_t *depth) {
    const float scale = (float)gfx_current_dimensions.height / 240.0f;
    const int32_t screenX = (int32_t)(((float)SCR_WIDTH * 0.5f) + ((x - 160) * scale));
    const int32_t screenY =
        (int32_t)((((float)SCR_HEIGHT - gfx_current_dimensions.height) * 0.5f) + (y * scale));
    const volatile uint16_t *depthBuffer;

    if (sDepthBuffer == NULL || screenX < 0 || screenX >= SCR_WIDTH || screenY < 0 || screenY >= SCR_HEIGHT) {
        return false;
    }

    depthBuffer = (const volatile uint16_t *)gfx_scegu_vram_cpu_addr(sDepthBuffer);
    *depth = depthBuffer[(screenY * BUF_WIDTH) + screenX];
    return true;
}

bool gfx_scegu_depth_is_clear(int32_t x, int32_t y) {
    uint16_t depth;

    return gfx_scegu_read_depth(x, y, &depth) && depth == 0;
}

bool gfx_scegu_depth_test(int32_t x, int32_t y, float projectedZ) {
    uint16_t depth;
    uint32_t projectedDepth;

    if (!gfx_scegu_read_depth(x, y, &depth)) {
        return false;
    }

    if (projectedZ < 0.0f) {
        projectedZ = 0.0f;
    } else if (projectedZ > 1.0f) {
        projectedZ = 1.0f;
    }

    /* The PSP depth range is reversed: 0 is the far plane and 0xFFFF is the near plane. */
    projectedDepth = (uint32_t)((1.0f - projectedZ) * 65535.0f);
    return projectedDepth >= depth;
}

static void gfx_scegu_copy_framebuffer_from_vram(void *dst, const void *src) {
    const void *srcAddr = gfx_scegu_vram_cpu_addr(src);

    OotPsp_MemcpyVfpu(dst, srcAddr, FRAMEBUFFER_SIZE);
}

static void gfx_scegu_copy_framebuffer_to_vram(void *dst, const void *src) {
    void *dstAddr = gfx_scegu_vram_cpu_addr(dst);

    OotPsp_MemcpyVfpu(dstAddr, src, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(dstAddr, FRAMEBUFFER_SIZE);
}

static void gfx_scegu_apply_home_menu_2d_view(void) {
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix);
}

static uint16_t gfx_scegu_make_rgb565(unsigned int r, unsigned int g, unsigned int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void gfx_scegu_apply_vismono(uint8_t primR, uint8_t primG, uint8_t primB, uint8_t alpha,
                             uint8_t envR, uint8_t envG, uint8_t envB) {
    void *framebuffer;

    if (alpha == 0) {
        return;
    }

    /* Complete every draw preceding the display-list marker before the CPU
     * filters the active GU buffer. A new direct list preserves the GE state
     * and lets later overlay commands continue in their original order. */
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    framebuffer = gfx_scegu_vram_cpu_addr(sDrawBuffer);

    if (sColorPixelFormat == GU_PSM_8888) {
        uint32_t *pixels = (uint32_t *)framebuffer;

        for (int y = 0; y < SCR_HEIGHT; y++) {
            uint32_t *row = pixels + (y * BUF_WIDTH);

            for (int x = 0; x < SCR_WIDTH; x++) {
                const uint32_t pixel = row[x];
                const unsigned int r = pixel & 0xFF;
                const unsigned int g = (pixel >> 8) & 0xFF;
                const unsigned int b = (pixel >> 16) & 0xFF;
                const unsigned int intensity = ((r * 2) + (g * 4) + b + 3) / 7;
                const unsigned int tintR = envR + (((int)primR - envR) * (int)intensity + 127) / 255;
                const unsigned int tintG = envG + (((int)primG - envG) * (int)intensity + 127) / 255;
                const unsigned int tintB = envB + (((int)primB - envB) * (int)intensity + 127) / 255;
                const unsigned int outR = ((tintR * alpha) + (r * (255 - alpha)) + 127) / 255;
                const unsigned int outG = ((tintG * alpha) + (g * (255 - alpha)) + 127) / 255;
                const unsigned int outB = ((tintB * alpha) + (b * (255 - alpha)) + 127) / 255;

                row[x] = (pixel & 0xFF000000U) | (outB << 16) | (outG << 8) | outR;
            }
        }

        sceGuStart(GU_DIRECT, list);
        return;
    }

    uint16_t *pixels = (uint16_t *)framebuffer;

    if ((primR == 255) && (primG == 255) && (primB == 255) &&
        (envR == 0) && (envG == 0) && (envB == 0) && (alpha == 255)) {
        /* The Zelda flashback uses this full-strength grayscale case. Work in
         * the framebuffer's native component widths so the hot loop needs no
         * RGB888 conversion or alpha interpolation. */
        for (int y = 0; y < SCR_HEIGHT; y++) {
            uint16_t *row = pixels + (y * BUF_WIDTH);

            for (int x = 0; x < SCR_WIDTH; x++) {
                const uint16_t pixel = row[x];
                const unsigned int r5 = (pixel >> 11) & 0x1F;
                const unsigned int g5 = ((pixel >> 5) & 0x3F) >> 1;
                const unsigned int b5 = pixel & 0x1F;
                const unsigned int intensity5 = ((r5 * 2) + (g5 * 4) + b5 + 3) / 7;
                const unsigned int intensity6 = (intensity5 << 1) | (intensity5 >> 4);

                row[x] = (uint16_t)((intensity5 << 11) | (intensity6 << 5) | intensity5);
            }
        }
        sceGuStart(GU_DIRECT, list);
        return;
    }

    for (int y = 0; y < SCR_HEIGHT; y++) {
        uint16_t *row = pixels + (y * BUF_WIDTH);

        for (int x = 0; x < SCR_WIDTH; x++) {
            const uint16_t pixel = row[x];
            const unsigned int r = ((((pixel >> 11) & 0x1F) * 255) + 15) / 31;
            const unsigned int g = ((((pixel >> 5) & 0x3F) * 255) + 31) / 63;
            const unsigned int b = (((pixel & 0x1F) * 255) + 15) / 31;
            const unsigned int intensity = ((r * 2) + (g * 4) + b + 3) / 7;
            const unsigned int tintR = envR + (((int)primR - envR) * (int)intensity + 127) / 255;
            const unsigned int tintG = envG + (((int)primG - envG) * (int)intensity + 127) / 255;
            const unsigned int tintB = envB + (((int)primB - envB) * (int)intensity + 127) / 255;
            const unsigned int outR = ((tintR * alpha) + (r * (255 - alpha)) + 127) / 255;
            const unsigned int outG = ((tintG * alpha) + (g * (255 - alpha)) + 127) / 255;
            const unsigned int outB = ((tintB * alpha) + (b * (255 - alpha)) + 127) / 255;

            row[x] = gfx_scegu_make_rgb565(outR, outG, outB);
        }
    }

    sceGuStart(GU_DIRECT, list);
}

static void gfx_scegu_accum_rgb565(uint16_t color, unsigned int *r, unsigned int *g, unsigned int *b) {
    *r += ((color >> 11) & 0x1F) << 3;
    *g += ((color >> 5) & 0x3F) << 2;
    *b += (color & 0x1F) << 3;
}

static int gfx_scegu_clamp_int(int value, int min, int max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void gfx_scegu_blur_framebuffer(void *framebuffer) {
    static const int offsets[3] = { -3, 0, 3 };
    int x;
    int y;
    int ox;
    int oy;

    OotPsp_MemcpyVfpu(sHomeMenuBgBlurScratch, framebuffer, FRAMEBUFFER_SIZE);

    if (sColorPixelFormat == GU_PSM_8888) {
        uint32_t *pixels = (uint32_t *)framebuffer;
        uint32_t *scratch = (uint32_t *)sHomeMenuBgBlurScratch;

        for (y = 0; y < SCR_HEIGHT; y++) {
            for (x = 0; x < SCR_WIDTH; x++) {
                unsigned int r = 0;
                unsigned int g = 0;
                unsigned int b = 0;
                unsigned int a = 0;

                for (oy = 0; oy < 3; oy++) {
                    int sampleY = gfx_scegu_clamp_int(y + offsets[oy], 0, SCR_HEIGHT - 1);

                    for (ox = 0; ox < 3; ox++) {
                        int sampleX = gfx_scegu_clamp_int(x + offsets[ox], 0, SCR_WIDTH - 1);
                        uint32_t color = pixels[(sampleY * BUF_WIDTH) + sampleX];

                        r += color & 0xFF;
                        g += (color >> 8) & 0xFF;
                        b += (color >> 16) & 0xFF;
                        a += color >> 24;
                    }
                }

                scratch[(y * BUF_WIDTH) + x] = ((a / 9) << 24) | ((b / 9) << 16) | ((g / 9) << 8) | (r / 9);
            }
        }

        OotPsp_MemcpyVfpu(framebuffer, scratch, FRAMEBUFFER_SIZE);
        sceKernelDcacheWritebackRange(framebuffer, FRAMEBUFFER_SIZE);
        return;
    }

    uint16_t *pixels = (uint16_t *)framebuffer;
    uint16_t *scratch = (uint16_t *)sHomeMenuBgBlurScratch;

    for (y = 0; y < SCR_HEIGHT; y++) {
        for (x = 0; x < SCR_WIDTH; x++) {
            unsigned int r = 0;
            unsigned int g = 0;
            unsigned int b = 0;

            for (oy = 0; oy < 3; oy++) {
                int sampleY = gfx_scegu_clamp_int(y + offsets[oy], 0, SCR_HEIGHT - 1);

                for (ox = 0; ox < 3; ox++) {
                    int sampleX = gfx_scegu_clamp_int(x + offsets[ox], 0, SCR_WIDTH - 1);

                    gfx_scegu_accum_rgb565(pixels[(sampleY * BUF_WIDTH) + sampleX], &r, &g, &b);
                }
            }

            scratch[(y * BUF_WIDTH) + x] = gfx_scegu_make_rgb565(r / 9, g / 9, b / 9);
        }
    }

    OotPsp_MemcpyVfpu(framebuffer, scratch, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(framebuffer, FRAMEBUFFER_SIZE);
}

static unsigned int gfx_scegu_rgba(unsigned int r, unsigned int g, unsigned int b, unsigned int a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static float gfx_scegu_home_menu_scale(void) {
    float scaleX = (float)SCR_WIDTH / HOME_MENU_WIDTH;
    float scaleY = (float)SCR_HEIGHT / HOME_MENU_HEIGHT;

    return (scaleX < scaleY) ? scaleX : scaleY;
}

static int gfx_scegu_home_menu_x(float x) {
    float scale = gfx_scegu_home_menu_scale();
    float offset = ((float)SCR_WIDTH - (HOME_MENU_WIDTH * scale)) * 0.5f;

    return (int)(offset + (x * scale) + 0.5f);
}

static int gfx_scegu_home_menu_y(float y) {
    float scale = gfx_scegu_home_menu_scale();
    float offset = ((float)SCR_HEIGHT - (HOME_MENU_HEIGHT * scale)) * 0.5f;

    return (int)(offset + (y * scale) + 0.5f);
}

static void gfx_scegu_draw_rect(int x, int y, int width, int height, unsigned int color) {
    VertexColor *verts;
    int left;
    int top;
    int right;
    int bottom;

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    verts = (VertexColor *)sceGuGetMemory(sizeof(VertexColor) * 2);
    if (verts == NULL) {
        return;
    }

    left = gfx_scegu_home_menu_x((float)x);
    top = gfx_scegu_home_menu_y((float)y);
    right = gfx_scegu_home_menu_x((float)(x + width));
    bottom = gfx_scegu_home_menu_y((float)(y + height));

    verts[0].a = 0;
    verts[0].b = 0;
    verts[0].color = color;
    verts[0].x = (unsigned short)left;
    verts[0].y = (unsigned short)top;
    verts[0].z = 0;
    verts[1].a = 0;
    verts[1].b = 0;
    verts[1].color = color;
    verts[1].x = (unsigned short)right;
    verts[1].y = (unsigned short)bottom;
    verts[1].z = 0;

    gfx_scegu_apply_home_menu_2d_view();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, verts);
}

static const unsigned char *gfx_scegu_get_fallback_glyph(char c) {
    static const unsigned char glyphA[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const unsigned char glyphB[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const unsigned char glyphC[7] = { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F };
    static const unsigned char glyphE[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
    static const unsigned char glyphG[7] = { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F };
    static const unsigned char glyphI[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F };
    static const unsigned char glyphK[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    static const unsigned char glyphL[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const unsigned char glyphM[7] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 };
    static const unsigned char glyphN[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
    static const unsigned char glyphO[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    static const unsigned char glyphP[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 };
    static const unsigned char glyphR[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const unsigned char glyphT[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char glyphX[7] = { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11 };
    static const unsigned char blank[7] = { 0 };

    if ((c >= 'A') && (c <= 'Z')) {
        c = (char)(c - 'A' + 'a');
    }

    switch (c) {
        case 'a':
            return glyphA;
        case 'b':
            return glyphB;
        case 'c':
            return glyphC;
        case 'e':
            return glyphE;
        case 'g':
            return glyphG;
        case 'i':
            return glyphI;
        case 'k':
            return glyphK;
        case 'l':
            return glyphL;
        case 'm':
            return glyphM;
        case 'n':
            return glyphN;
        case 'o':
            return glyphO;
        case 'p':
            return glyphP;
        case 'r':
            return glyphR;
        case 't':
            return glyphT;
        case 'x':
            return glyphX;
        default:
            return blank;
    }
}

static int gfx_scegu_fallback_text_width(const char *text, int scale) {
    int width = 0;

    while (*text != '\0') {
        width += 6 * scale;
        text++;
    }

    if (width > 0) {
        width -= scale;
    }

    return width;
}

static void gfx_scegu_draw_fallback_text_centered(int centerX, int y, const char *text, int scale,
                                                  unsigned int color) {
    int x = centerX - (gfx_scegu_fallback_text_width(text, scale) / 2);

    while (*text != '\0') {
        const unsigned char *glyph = gfx_scegu_get_fallback_glyph(*text);
        int row;

        for (row = 0; row < 7; row++) {
            int col;

            for (col = 0; col < 5; col++) {
                if (glyph[row] & (1 << (4 - col))) {
                    gfx_scegu_draw_rect(x + (col * scale), y + (row * scale), scale, scale, color);
                }
            }
        }

        x += 6 * scale;
        text++;
    }
}

#if OOT_PSP_USE_INTRAFONT
static bool gfx_scegu_ensure_home_menu_font(void) {
    if (!sHomeMenuFontInitTried) {
        sHomeMenuFontInitTried = true;

        if (intraFontInit()) {
            sHomeMenuFont = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
        }

        printf("oot-psp home menu font %s\n", (sHomeMenuFont != NULL) ? "loaded" : "load failed");
    }

    return sHomeMenuFont != NULL;
}
#endif

static void gfx_scegu_draw_home_menu_text(int x, int y, const char *text, float size, unsigned int color,
                                          unsigned int shadowColor, unsigned int options) {
#if OOT_PSP_USE_INTRAFONT
    if (gfx_scegu_ensure_home_menu_font()) {
        float scale = gfx_scegu_home_menu_scale();

        gfx_scegu_apply_home_menu_2d_view();
        intraFontActivate(sHomeMenuFont);
        intraFontSetStyle(sHomeMenuFont, size * scale, color, shadowColor, 0.0f,
                          options | INTRAFONT_STRING_ASCII);
        intraFontPrint(sHomeMenuFont, (float)gfx_scegu_home_menu_x((float)x),
                       (float)gfx_scegu_home_menu_y((float)y), text);
        return;
    }
#endif

    (void)size;
    (void)shadowColor;
    (void)options;
    gfx_scegu_draw_fallback_text_centered(x, y - 14, text, 3, color);
}

static void gfx_scegu_prepare_home_menu_draw(void) {
    gfx_scegu_apply_home_menu_2d_view();
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexEnvColor(0xffffffff);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_FALSE);
}

static void gfx_scegu_render_home_menu_main(int selectedIndex, uint8_t highlightRed, uint8_t highlightGreen,
                                            uint8_t highlightBlue) {
    static const char *items[] = {
        "Resume Game",
        "Controller Mapping",
        "Video Settings",
        "About",
        "Exit Game",
    };
    int i;

    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(0, 0, 0, 96));
    gfx_scegu_draw_rect(112, 34, 256, 210, gfx_scegu_rgba(0, 0, 0, 132));

    for (i = 0; i < 5; i++) {
        int y = 62 + (i * 38);
        unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

        if (selectedIndex == i) {
            gfx_scegu_draw_rect(128, y - 22, 224, 28,
                                gfx_scegu_rgba(highlightRed, highlightGreen, highlightBlue, 205));
            color = gfx_scegu_rgba(255, 255, 245, 255);
        }

        gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, y, items[i], 0.72f, color,
                                      gfx_scegu_rgba(0, 0, 0, 180),
                                      INTRAFONT_ALIGN_CENTER);
    }
}

static void gfx_scegu_draw_home_menu_battery(int batteryPercent) {
    const int x = 392;
    const int y = 8;
    const int width = 58;
    const int height = 18;
    const int fillWidth = width - 4;
    unsigned int fillColor;
    char percentText[8];

    if (batteryPercent < 0) {
        batteryPercent = 0;
    } else if (batteryPercent > 100) {
        batteryPercent = 100;
    }

    if (batteryPercent <= 20) {
        fillColor = gfx_scegu_rgba(178, 62, 54, 255);
    } else if (batteryPercent <= 45) {
        fillColor = gfx_scegu_rgba(194, 148, 56, 255);
    } else {
        fillColor = gfx_scegu_rgba(54, 142, 98, 255);
    }

    gfx_scegu_draw_rect(x, y, width, height, gfx_scegu_rgba(170, 190, 180, 230));
    gfx_scegu_draw_rect(x + 2, y + 2, fillWidth, height - 4, gfx_scegu_rgba(12, 24, 20, 230));
    if (batteryPercent > 0) {
        gfx_scegu_draw_rect(x + 2, y + 2, (fillWidth * batteryPercent) / 100, height - 4, fillColor);
    }
    gfx_scegu_draw_rect(x + width, y + 5, 3, 8, gfx_scegu_rgba(170, 190, 180, 230));

    snprintf(percentText, sizeof(percentText), "%d%%", batteryPercent);
    gfx_scegu_draw_home_menu_text(x + (width / 2), y + 14, percentText, 0.42f,
                                  gfx_scegu_rgba(255, 255, 245, 255), gfx_scegu_rgba(0, 0, 0, 210),
                                  INTRAFONT_ALIGN_CENTER);
}

#define HOME_MENU_ABOUT_QR_MODULE_COUNT 33
#define HOME_MENU_ABOUT_QR_MODULE_SIZE 4
#define HOME_MENU_ABOUT_VISIBLE_LINES 6

static const char* const sHomeMenuAboutQrModules[HOME_MENU_ABOUT_QR_MODULE_COUNT] = {
    "111111101011001101010111001111111",
    "100000100101010100101001101000001",
    "101110101100100110100111001011101",
    "101110101000010001111111001011101",
    "101110100000111100111100101011101",
    "100000101000010011010010101000001",
    "111111101010101010101010101111111",
    "000000001101001111000110100000000",
    "010101111100111010101101111101101",
    "110000001011100111001100111100011",
    "110111111111010100111000001110101",
    "000110001101111110101111101011000",
    "011100101100001010101010011101010",
    "100001001000111001101100000110111",
    "111101101011010000110001111110110",
    "100001010100111010101110101101010",
    "010111110110101011100111101010010",
    "110010010011100000000011100101010",
    "011101101110100011000100100000111",
    "110100010000111100111010111100011",
    "101011101011000101111000110000000",
    "000010010100110000010101100000111",
    "110100101010101010010100111111001",
    "011001000001101101111100101100011",
    "101011101100000000101100111111011",
    "000000001101011111111111100011001",
    "111111101011110110000010101011110",
    "100000101101111100111111100010011",
    "101110100101100101111000111111000",
    "101110101100101110110110101011010",
    "101110100011111011111010110011001",
    "100000101001010000011100101000000",
    "111111100011001001011101110011010",
};

static const char* const sHomeMenuAboutLines[] = {
    "A PlayStation Portable port",
    "of The Legend of Zelda:",
    "Ocarina of Time.",
    "",
    "Ported by Z2442 and the PSP",
    "Homebrew community. Thank you!",
    "To my faimily that put up with",
    "me playing around with two decade",
    "old handheld!",
    "Everyone for all of your help",
    "making this a reality.",
    "I could not done this Alone.",
    "",
    "Special thanks:",
    "Original SM64 Fast3d renderer: Mr.Neo240",
    "R&D Team: M-CID and the PSP community",
    "Daedalus Team: Wally, Re4thewin, Dreamer72",
    "Discord Team: Mr. Iron, Acid_Snake, Krazynez",
    "N64 Decompilation Team",
    "",
    "Scan the QR code to the right",
    "to talk to the team on discord!",
};

static void gfx_scegu_draw_home_menu_about_qr(int x, int y) {
    const int codeSize = HOME_MENU_ABOUT_QR_MODULE_COUNT * HOME_MENU_ABOUT_QR_MODULE_SIZE;
    const int quietZone = HOME_MENU_ABOUT_QR_MODULE_SIZE * 2;
    int row;

    gfx_scegu_draw_rect(x - quietZone, y - quietZone, codeSize + (quietZone * 2), codeSize + (quietZone * 2),
                        gfx_scegu_rgba(255, 255, 255, 255));

    for (row = 0; row < HOME_MENU_ABOUT_QR_MODULE_COUNT; row++) {
        int column;

        for (column = 0; column < HOME_MENU_ABOUT_QR_MODULE_COUNT; column++) {
            if (sHomeMenuAboutQrModules[row][column] == '1') {
                gfx_scegu_draw_rect(x + (column * HOME_MENU_ABOUT_QR_MODULE_SIZE),
                                    y + (row * HOME_MENU_ABOUT_QR_MODULE_SIZE),
                                    HOME_MENU_ABOUT_QR_MODULE_SIZE, HOME_MENU_ABOUT_QR_MODULE_SIZE,
                                    gfx_scegu_rgba(0, 0, 0, 255));
            }
        }
    }
}

static void gfx_scegu_render_about(int scrollOffset) {
    const unsigned int titleColor = gfx_scegu_rgba(255, 255, 245, 255);
    const unsigned int bodyColor = gfx_scegu_rgba(218, 224, 218, 255);
    const unsigned int hintColor = gfx_scegu_rgba(170, 190, 180, 255);
    const unsigned int shadowColor = gfx_scegu_rgba(0, 0, 0, 180);
    const int aboutLineCount = ARRAY_COUNT(sHomeMenuAboutLines);
    int line;

    if (scrollOffset < 0) {
        scrollOffset = 0;
    }
    if (scrollOffset > (aboutLineCount - HOME_MENU_ABOUT_VISIBLE_LINES)) {
        scrollOffset = aboutLineCount - HOME_MENU_ABOUT_VISIBLE_LINES;
    }

    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(0, 0, 0, 112));
    gfx_scegu_draw_rect(20, 20, 440, 232, gfx_scegu_rgba(0, 0, 0, 154));
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 54, "About", 0.82f, titleColor, shadowColor,
                                  INTRAFONT_ALIGN_CENTER);
    gfx_scegu_draw_rect(282, 72, 1, 142, gfx_scegu_rgba(112, 132, 122, 180));

    for (line = 0; line < HOME_MENU_ABOUT_VISIBLE_LINES; line++) {
        gfx_scegu_draw_home_menu_text(48, 96 + (line * 20), sHomeMenuAboutLines[scrollOffset + line], 0.48f,
                                      bodyColor, shadowColor, INTRAFONT_ALIGN_LEFT);
    }

    if (scrollOffset > 0) {
        gfx_scegu_draw_home_menu_text(266, 90, "^", 0.48f, hintColor, shadowColor, INTRAFONT_ALIGN_CENTER);
    }
    if ((scrollOffset + HOME_MENU_ABOUT_VISIBLE_LINES) < aboutLineCount) {
        gfx_scegu_draw_home_menu_text(266, 202, "v", 0.48f, hintColor, shadowColor, INTRAFONT_ALIGN_CENTER);
    }

    gfx_scegu_draw_home_menu_about_qr(306, 82);
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 232, "Up/Down scroll  Circle, Cross, or Start: Back", 0.42f,
                                  hintColor, gfx_scegu_rgba(0, 0, 0, 160), INTRAFONT_ALIGN_CENTER);
}

void gfx_scegu_render_first_boot_progress(uint32_t progressPermille, const char* statusMessage, bool error) {
    const int barX = 72;
    const int barY = 148;
    const int barWidth = 336;
    const int barHeight = 16;
    unsigned int accentColor;
    unsigned int accentHighlight;
    unsigned int statusColor;
    int fillWidth;
    char percentText[16];

    if (progressPermille > 1000) {
        progressPermille = 1000;
    }
    fillWidth = (int)((progressPermille * barWidth) / 1000);
    accentColor = error ? gfx_scegu_rgba(126, 55, 48, 255) : gfx_scegu_rgba(38, 92, 78, 255);
    accentHighlight = error ? gfx_scegu_rgba(188, 88, 72, 255) : gfx_scegu_rgba(72, 140, 116, 255);
    statusColor = error ? gfx_scegu_rgba(255, 210, 196, 255) : gfx_scegu_rgba(218, 224, 218, 255);

    gfx_scegu_prepare_home_menu_draw();
    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(7, 15, 13, 255));
    gfx_scegu_draw_rect(34, 36, 412, 200, gfx_scegu_rgba(0, 0, 0, 154));

    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 78,
                                  error ? "Asset Setup Failed" : "Preparing Game Data",
                                  0.84f, gfx_scegu_rgba(255, 255, 245, 255),
                                  gfx_scegu_rgba(0, 0, 0, 180), INTRAFONT_ALIGN_CENTER);
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 118,
                                  (statusMessage != NULL) ? statusMessage : "Starting asset setup", 0.60f,
                                  statusColor, gfx_scegu_rgba(0, 0, 0, 180), INTRAFONT_ALIGN_CENTER);

    gfx_scegu_draw_rect(barX - 4, barY - 4, barWidth + 8, barHeight + 8, gfx_scegu_rgba(0, 0, 0, 180));
    gfx_scegu_draw_rect(barX, barY, barWidth, barHeight, gfx_scegu_rgba(20, 38, 33, 255));
    if (fillWidth > 0) {
        gfx_scegu_draw_rect(barX, barY, fillWidth, barHeight, accentColor);
        gfx_scegu_draw_rect(barX, barY, fillWidth, 3, accentHighlight);
    }

    snprintf(percentText, sizeof(percentText), "%lu%%", (unsigned long)(progressPermille / 10));
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 194, percentText, 0.58f,
                                  gfx_scegu_rgba(255, 255, 245, 255), gfx_scegu_rgba(0, 0, 0, 180),
                                  INTRAFONT_ALIGN_CENTER);
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 222,
                                  error ? "Check the ROM and restart" : "First launch only - do not power off",
                                  0.48f, gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                  INTRAFONT_ALIGN_CENTER);
}

static void gfx_scegu_render_controller_mapping(int selectedIndex, const char* statusMessage, uint8_t highlightRed,
                                                 uint8_t highlightGreen, uint8_t highlightBlue) {
    char line[80];
    char value[96];
    int bindingCount = OotPspControls_GetBindingCount();
    int deadzoneRow = bindingCount;
    int n64StickScalingRow = bindingCount + 1;
    int saveRow = bindingCount + 2;
    int resetRow = bindingCount + 3;
    int backRow = bindingCount + 4;
    int totalRows = backRow + 1;
    int visibleRows = 7;
    int firstRow = selectedIndex - (visibleRows / 2);
    int row;

    if (visibleRows > totalRows) {
        visibleRows = totalRows;
    }
    if (firstRow < 0) {
        firstRow = 0;
    }
    if ((firstRow + visibleRows) > totalRows) {
        firstRow = totalRows - visibleRows;
    }

    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(0, 0, 0, 112));
    gfx_scegu_draw_rect(34, 20, 412, 232, gfx_scegu_rgba(0, 0, 0, 154));
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 50, "Controller Mapping", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), gfx_scegu_rgba(0, 0, 0, 180),
                                  INTRAFONT_ALIGN_CENTER);

#if OOT_PSP_USE_INTRAFONT
    if (gfx_scegu_ensure_home_menu_font()) {
        intraFontActivate(sHomeMenuFont);

        for (row = firstRow; row < firstRow + visibleRows; row++) {
            int y = 82 + ((row - firstRow) * 22);
            unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

            if (selectedIndex == row) {
                gfx_scegu_draw_rect(54, y - 17, 372, 22,
                                    gfx_scegu_rgba(highlightRed, highlightGreen, highlightBlue, 205));
                color = gfx_scegu_rgba(255, 255, 245, 255);
            }

            if (row < bindingCount) {
                OotPspControls_GetBindingValueText(row, value, sizeof(value));
                snprintf(line, sizeof(line), "%s: %.32s", OotPspControls_GetBindingName(row), value);
            } else if (row == deadzoneRow) {
                snprintf(line, sizeof(line), "Deadzone: %d", OotPspControls_GetDeadzone());
            } else if (row == n64StickScalingRow) {
                snprintf(line, sizeof(line), "N64 Stick Scaling: %s",
                         OotPspControls_IsN64StickScalingEnabled() ? "On" : "Off");
            } else if (row == saveRow) {
                snprintf(line, sizeof(line), "Save controls.ini");
            } else if (row == resetRow) {
                snprintf(line, sizeof(line), "Reset defaults");
            } else {
                snprintf(line, sizeof(line), "Back");
            }

            gfx_scegu_draw_home_menu_text(70, y, line, 0.68f, color, gfx_scegu_rgba(0, 0, 0, 180),
                                          INTRAFONT_ALIGN_LEFT);
        }

        if (firstRow > 0) {
            gfx_scegu_draw_home_menu_text(422, 82, "^", 0.48f, gfx_scegu_rgba(170, 190, 180, 255),
                                          gfx_scegu_rgba(0, 0, 0, 160), INTRAFONT_ALIGN_CENTER);
        }
        if ((firstRow + visibleRows) < totalRows) {
            gfx_scegu_draw_home_menu_text(422, 214, "v", 0.48f, gfx_scegu_rgba(170, 190, 180, 255),
                                          gfx_scegu_rgba(0, 0, 0, 160), INTRAFONT_ALIGN_CENTER);
        }

        if ((statusMessage != NULL) && (statusMessage[0] != '\0')) {
            gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 242, statusMessage, 0.48f,
                                          gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                          INTRAFONT_ALIGN_CENTER);
        } else {
            gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 242,
                                          "Left/Right change  Cross select  Circle back", 0.48f,
                                          gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                          INTRAFONT_ALIGN_CENTER);
        }
        return;
    }
#endif

    gfx_scegu_draw_fallback_text_centered(HOME_MENU_WIDTH / 2, 116, "Controller Mapping", 4,
                                          gfx_scegu_rgba(255, 255, 245, 255));
    gfx_scegu_draw_fallback_text_centered(HOME_MENU_WIDTH / 2, 158, "Back", 4,
                                          gfx_scegu_rgba(218, 224, 218, 255));
}

static void gfx_scegu_render_video_settings(int selectedIndex, const char* statusMessage, uint8_t highlightRed,
                                             uint8_t highlightGreen, uint8_t highlightBlue) {
    char line[80];
    int row;

    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(0, 0, 0, 112));
    gfx_scegu_draw_rect(34, 20, 412, 232, gfx_scegu_rgba(0, 0, 0, 154));
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 48, "Video Settings", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), gfx_scegu_rgba(0, 0, 0, 180),
                                  INTRAFONT_ALIGN_CENTER);

#if OOT_PSP_USE_INTRAFONT
    if (gfx_scegu_ensure_home_menu_font()) {
        for (row = 0; row < 7; row++) {
            int y = 76 + (row * 22);
            unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

            if (selectedIndex == row) {
                gfx_scegu_draw_rect(54, y - 17, 372, 22,
                                    gfx_scegu_rgba(highlightRed, highlightGreen, highlightBlue, 205));
                color = gfx_scegu_rgba(255, 255, 245, 255);
            }

            switch (row) {
                case 0:
                    snprintf(line, sizeof(line), "Output: %s", OotPspVideo_GetOutputName());
                    break;
                case 1:
                    snprintf(line, sizeof(line), "Resolution: %s", OotPspVideo_GetResolutionName());
                    break;
                case 2:
                    snprintf(line, sizeof(line), "Aspect Ratio: %s", OotPspVideo_GetAspectName());
                    break;
                case 3:
                    snprintf(line, sizeof(line), "Apply video mode");
                    break;
                case 4:
                    snprintf(line, sizeof(line), "Save video.ini");
                    break;
                case 5:
                    snprintf(line, sizeof(line), "Reset defaults");
                    break;
                default:
                    snprintf(line, sizeof(line), "Back");
                    break;
            }

            gfx_scegu_draw_home_menu_text(70, y, line, 0.68f, color, gfx_scegu_rgba(0, 0, 0, 180),
                                          INTRAFONT_ALIGN_LEFT);
        }

        if ((statusMessage != NULL) && (statusMessage[0] != '\0')) {
            gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 242, statusMessage, 0.48f,
                                          gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                          INTRAFONT_ALIGN_CENTER);
        } else if (!OotPspVideo_IsTvAvailable()) {
            gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 242, "TV output requires a Slim and video cable",
                                          0.48f, gfx_scegu_rgba(170, 190, 180, 255),
                                          gfx_scegu_rgba(0, 0, 0, 160), INTRAFONT_ALIGN_CENTER);
        } else {
            gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 242,
                                          "Left/Right change  Cross select  Circle back", 0.48f,
                                          gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                          INTRAFONT_ALIGN_CENTER);
        }
        return;
    }
#endif

    gfx_scegu_draw_fallback_text_centered(HOME_MENU_WIDTH / 2, 116, "Video Settings", 4,
                                          gfx_scegu_rgba(255, 255, 245, 255));
    gfx_scegu_draw_fallback_text_centered(HOME_MENU_WIDTH / 2, 158, "Back", 4,
                                          gfx_scegu_rgba(218, 224, 218, 255));
}

static inline uint32_t get_shader_remap(uint32_t id) {
    switch (id) {
        case 153092165:
        case 153092608:
        case 153093005:
        case 153094656:
            return 69;
        default:
            return id;
    }
}

static inline bool is_shader_enabled(uint32_t id) {
    switch (id) {
        case 153092165:
        case 153092608:
        case 153093005:
        case 153094656:
            return false;
        default:
            return true;
    }
}

static struct ShaderProgram *get_shader_from_id(uint32_t id) {
    return gfx_scegu_lookup_shader(id);
}

static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}

static inline int texenv_set_color(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture_color(struct ShaderProgram *prg) {
    int mode;

    if (prg->cc.opt_texture_blend) {
        return GU_TFX_BLEND;
    }

    /*@Hack: lord forgive me for this, but this is easier */
    switch (prg->shader_id) {
        case 0x0000038D: // mario's eyes
        case 0x01045A00: // peach letter
        case 0x01200A00: // intro copyright fade in
            mode = GU_TFX_DECAL;
            break;
        case 0x00000551: // goddard
            mode = GU_TFX_BLEND;
            break;
        default:
            mode = GU_TFX_MODULATE;
            break;
    }

    return mode;
}

static inline int texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    /*@Note: hack shader 0x1A00A6F for Bowser/Peach Paintings (still broken, but just fixed on peach)*/
    return GU_TFX_DECAL;
}

static bool gfx_scegu_shader_uses_texture_alpha(const struct ShaderProgram *prg) {
    if (!prg->cc.opt_alpha) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        switch (prg->cc.c[1][i]) {
            case SHADER_TEXEL0:
            case SHADER_TEXEL0A:
            case SHADER_TEXEL1:
                return true;
        }
    }

    return false;
}

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
    int mode;
    const bool use_texture = prg != NULL && (prg->texture_used[0] || prg->texture_used[1]);

    if (prg == NULL) {
        return;
    }
    if (sAppliedShader == prg) {
        prg->enabled = true;
        return;
    }

    /* Some RDP passes intentionally update only depth while preserving the
     * framebuffer color. The Lens of Truth inverted mask relies on this. */
    sceGuPixelMask((prg->shader_id & SHADER_OPT_DEPTH_ONLY) ? 0xffffffffU : 0);

    if (use_texture) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuTexScale(1.0f, 1.0f);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
    }
/*@Note: Revisit one day! */
#if 0
    if (prg->shader_id & SHADER_OPT_FOG) {
        // Yea this doesnt work at all */
        //sceGuFog(scegu_fog_near, scegu_fog_far, 0x00FF0000);//scegu_fog_color); // color is the same for all verts, only intensity is different
        //sceGuEnable(GU_FOG);
        sceGuEnable(GU_BLEND);
    }
#endif

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit
        // (see mario head)
        //       if there's two colors but no texture, the real color is likely the second one
        /*
        const int hack = (prg->num_inputs > 1) * (4 - (int)prg->texture_used[0]);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, cur_buf_stride, ofs + hack);
        ofs += 4 * prg->num_inputs;
        */
    }

    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        // (horrible) alpha discard
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    } else if (prg->shader_id & SHADER_OPT_ALPHA_THRESHOLD) {
        /* OoT's threshold display lists use zero-alpha texels as holes. This
         * is especially important when a textured rectangle writes depth,
         * as the Lens of Truth mask does. */
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    if (use_texture) {
        switch (prg->mix) {
            case SH_MT_TEXTURE:
                mode = texenv_set_texture(prg);
                break;
            case SH_MT_TEXTURE_TEXTURE:
                mode = texenv_set_texture_texture(prg);
                break;
            case SH_MT_TEXTURE_COLOR:
                mode = texenv_set_texture_color(prg);
                break;
            default:
                mode = texenv_set_color(prg);
                break;
        }

        /* Transition Screens */
        if (prg->shader_id == 0x01A00045) {
            mode = GU_TFX_REPLACE;
        }
        sceGuTexFunc(mode, gfx_scegu_shader_uses_texture_alpha(prg) ? GU_TCC_RGBA : GU_TCC_RGB);
    }

    prg->enabled = true;
    sAppliedShader = prg;
}

static void gfx_scegu_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
        sAppliedShader = NULL;
    }
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    gfx_scegu_apply_shader(cur_shader);
}

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    bool found;
    struct ShaderProgram *prg = gfx_scegu_probe_shader_cache(shader_id, &found);

    if (found) {
        gfx_scegu_load_shader(prg);
        return prg;
    }
    if ((prg == NULL) || (shader_program_cache_count >= SHADER_PROGRAM_CACHE_CAPACITY)) {
        static const char message[] = "Shader program cache exhausted!\n";

        sceIoWrite(1, message, sizeof(message) - 1);
        abort();
    }

    gfx_cc_get_features(shader_id, &ccf);

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0] || ccf.used_textures[1];
    prg->texture_used[1] = false;
    prg->mix = SH_MT_NONE;

    if (prg->texture_used[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (prg->texture_used[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;
    prg->cache_valid = true;
    shader_program_cache_count++;

    gfx_scegu_load_shader(prg);

    return prg;
}

static void gfx_scegu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static uint32_t gfx_scegu_new_texture(void) {
    return texman_create();
}

static uint32_t gfx_cm_to_opengl(uint32_t val, uint32_t mask) {
    if ((val & G_TX_CLAMP) || mask == G_TX_NOMASK)
        return GU_CLAMP;
    return GU_REPEAT;
}

static inline int ispow2(uint32_t x) {
    return (x & (x - 1)) == 0;
}

// compute the next highest power of 2 of 32-bit v
static inline int nextpow2(int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v++;
    return v;
}

static inline void gfx_scegu_apply_tmu_state(const int tile) {
    const struct SamplerState* desired = &tmu_state[tile];

    if (!sAppliedSamplerStateValid || (desired->min_filter != sAppliedSamplerState.min_filter) ||
        (desired->mag_filter != sAppliedSamplerState.mag_filter)) {
        sceGuTexFilter(desired->min_filter, desired->mag_filter);
        sAppliedSamplerState.min_filter = desired->min_filter;
        sAppliedSamplerState.mag_filter = desired->mag_filter;
    }
    if (!sAppliedSamplerStateValid || (desired->wrap_s != sAppliedSamplerState.wrap_s) ||
        (desired->wrap_t != sAppliedSamplerState.wrap_t)) {
        sceGuTexWrap(desired->wrap_s, desired->wrap_t);
        sAppliedSamplerState.wrap_s = desired->wrap_s;
        sAppliedSamplerState.wrap_t = desired->wrap_t;
    }
    sAppliedSamplerStateValid = true;
}

static void gfx_scegu_set_sampler_parameters(const int tile, const bool linear_filter, const uint32_t cms,
                                             const uint32_t cmt, const uint32_t masks, const uint32_t maskt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;

    const int wrap_s = gfx_cm_to_opengl(cms, masks);
    const int wrap_t = gfx_cm_to_opengl(cmt, maskt);

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    if (active_texture_tile == tile) {
        gfx_scegu_apply_tmu_state(tile);
    }
}

static void gfx_scegu_set_texture_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t color = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);

    sTextureEnvColor = color;
    sceGuTexEnvColor(color);
}

static void gfx_scegu_select_texture(int tile, uint32_t texture_id) {
    tmu_state[tile].tex = texture_id;
    texman_bind_tex(texture_id);
    active_texture_tile = tile;
    gfx_scegu_apply_tmu_state(tile);
}

/* Used for rescaling textures into power-of-two dimensions (256 KiB at 32 bpp). */
static unsigned int __attribute__((aligned(16))) scaled[256 * 256];
static void gfx_scegu_resample_32bit(const unsigned int *in, int inwidth, int inheight, unsigned int *out, int outwidth, int outheight) {
    int i, j;
    const unsigned int *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
    int i, j;
    const unsigned short *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_resample_8bit(const unsigned char *in, int inwidth, int inheight, unsigned char *out, int outwidth, int outheight) {
    int i, j;
    const unsigned char *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if (ispow2(width) && ispow2(height)) {
        texman_upload_swizzle(width, height, type, (void *) rgba32_buf);
    } else {
        int scaled_width = nextpow2(width);
        int scaled_height = nextpow2(height);

        if (type == GU_PSM_8888) {
            gfx_scegu_resample_32bit((const unsigned int *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else if ((type == GU_PSM_5551) || (type == GU_PSM_4444)) {
            gfx_scegu_resample_16bit((const unsigned short *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else {
            gfx_scegu_resample_8bit((const unsigned char *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        }
    }
}

static void gfx_scegu_set_depth_test(bool depth_test) {
    sDepthTestEnabled = depth_test;
    sceGuDepthFunc(GU_GEQUAL);

    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
}

static void gfx_scegu_set_depth_mask(bool z_upd) {
    sDepthWriteEnabled = z_upd;
    sceGuDepthMask(z_upd ? GU_FALSE : GU_TRUE);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(32); /* I think we need a little more on psp because of 16bit depth buffer */
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    int originX = (SCR_WIDTH - (int)gfx_current_dimensions.width) / 2;
    int originY = (SCR_HEIGHT - (int)gfx_current_dimensions.height) / 2;

    sceGuViewport(2048 - (SCR_WIDTH / 2) + originX + x + (width / 2),
                  2048 + (SCR_HEIGHT / 2) - originY - y - (height / 2), width, height);
    sceGuScissor(originX + x, SCR_HEIGHT - originY - y - height, originX + x + width,
                 SCR_HEIGHT - originY - y);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    int originX = (SCR_WIDTH - (int)gfx_current_dimensions.width) / 2;
    int originY = (SCR_HEIGHT - (int)gfx_current_dimensions.height) / 2;

    sceGuScissor(originX + x, SCR_HEIGHT - originY - y - height, originX + x + width,
                 SCR_HEIGHT - originY - y);
}

static void gfx_scegu_set_use_alpha(bool use_alpha) {
    gl_blend = use_alpha;
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

static void gfx_scegu_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(Vertex) * 3 * buf_vbo_num_tris);
    void *buf = sceGuGetMemory(sizeof(Vertex) * 3 * buf_vbo_num_tris);
    OotPsp_MemcpyVfpu(buf, buf_vbo, sizeof(Vertex) * 3 * buf_vbo_num_tris);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3 * buf_vbo_num_tris, 0, buf);
}

static void gfx_scegu_draw_fog_triangles(float buf_vbo[], size_t buf_vbo_len,
                                         size_t buf_vbo_num_tris, bool useTextureAlpha,
                                         bool restoreShaderState) {
    struct ShaderProgram *restoreShader = sAppliedShader;
    const size_t vertexCount = 3 * buf_vbo_num_tris;
    const int vertexFormat = (useTextureAlpha ? GU_TEXTURE_32BITF : 0) |
                             GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

    gfx_scegu_reserve_list_memory(buf_vbo_len);
    void *buf = sceGuGetMemory(buf_vbo_len);

    OotPsp_MemcpyVfpu(buf, buf_vbo, buf_vbo_len);

    /* N64 fog is an RDP blend after texture/color combining. Re-draw the same
     * geometry as fog RGB with the VFPU-computed shade alpha. */
    if (useTextureAlpha) {
        const unsigned int fogRgb = ((const FogVertex *)buf_vbo)->color & 0x00ffffff;

        /* With vertex RGB and the texture-environment RGB both set to fog,
         * GU_TFX_BLEND leaves RGB constant while multiplying alpha by texel alpha. */
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexEnvColor(fogRgb | 0xff000000);
        sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
    }
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDepthMask(GU_TRUE);
    if (sDepthTestEnabled && sDepthWriteEnabled) {
        /* Matching the depth written by the base pass also preserves cutout holes. */
        sceGuDepthFunc(GU_EQUAL);
    }
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_TRIANGLES, vertexFormat, vertexCount, 0, buf);

    /* A two-texture material emits its two fog contributions back-to-back.
     * Keep the fog state live between them instead of restoring and then
     * immediately rebuilding it for the second pass. */
    if (!restoreShaderState) {
        return;
    }

    sceGuDepthFunc(GU_GEQUAL);
    sceGuDepthMask(sDepthWriteEnabled ? GU_FALSE : GU_TRUE);
    if (useTextureAlpha) {
        sceGuTexEnvColor(sTextureEnvColor);
    }
    if (restoreShader != NULL) {
        sAppliedShader = NULL;
        gfx_scegu_apply_shader(restoreShader);
    }
    if (!gl_blend) {
        sceGuDisable(GU_BLEND);
    }
}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    VertexColor *quad;
    int originX;
    int originY;

    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    quad = sceGuGetMemory(sizeof(VertexColor) * 2);
    OotPsp_MemcpyVfpu(quad, buf_vbo, sizeof(VertexColor) * 2);

    /* GU_TRANSFORM_2D bypasses the centered 3D viewport, so apply its origin to screen-space rectangles explicitly. */
    originX = (SCR_WIDTH - (int)gfx_current_dimensions.width) / 2;
    originY = (SCR_HEIGHT - (int)gfx_current_dimensions.height) / 2;
    quad[0].x += originX;
    quad[0].y += originY;
    quad[1].x += originX;
    quad[1].y += originY;

    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, quad);
}

static void gfx_scegu_set_mode_geometry(const OotPspVideoMode *mode) {
    sBufferWidth = mode->bufferWidth;
    sScreenWidth = mode->displayWidth;
    sScreenHeight = mode->displayHeight;
}

static bool gfx_scegu_mode_is_interlaced(const OotPspVideoMode *mode) {
    /* Native 320x240 asks the DVE to present that framebuffer directly.  It
     * uses the interlaced TV route, but does not need the 720x503 field-layout
     * conversion used by a full 480i framebuffer. */
    return mode->tvOutput && !mode->crt240p && (mode->dveMode == 0x1D1);
}

static void gfx_scegu_reset_framebuffers(void) {
    sDrawBuffer = sPrimaryBuffer;
    sDisplayBuffer = sSecondaryBuffer;
}

static void gfx_scegu_log_display_mode(const OotPspVideoMode *requestedMode, int result) {
    int activeMode = -1;
    int activeWidth = -1;
    int activeHeight = -1;
    int getModeResult = sceDisplayGetMode(&activeMode, &activeWidth, &activeHeight);

    printf("oot-psp video set result=%d requested=%X/%dx%d active_result=%d active=%X/%dx%d "
           "buffer=%dx%d interlaced=%d crt240p=%d\n",
           result, requestedMode->dveMode, requestedMode->dveWidth, requestedMode->dveHeight, getModeResult,
           activeMode, activeWidth, activeHeight, BUF_WIDTH, SCR_HEIGHT, sInterlacedOutput ? 1 : 0,
           sCrt240pOutput ? 1 : 0);
}

static int gfx_scegu_set_hardware_video_mode(const OotPspVideoMode *mode) {
    if (OotPspDve_IsAvailable()) {
        return OotPspDve_SetVideoOut(mode->dveOutput, mode->dveMode, mode->dveWidth, mode->dveHeight, 1, 15, 0);
    }
    if (mode->tvOutput) {
        return -1;
    }
    return sceDisplaySetMode(0, LCD_SCREEN_WIDTH, LCD_SCREEN_HEIGHT);
}

static void gfx_scegu_configure_gu_buffers(void) {
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(sColorPixelFormat, sDrawBuffer, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, sDisplayBuffer, BUF_WIDTH);
    sceGuDepthBuffer(sDepthBuffer, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xffff, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x55, 0xff);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
    sceGuDepthMask(GU_FALSE);
    sceGuTexEnvColor(0xffffffff);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void gfx_scegu_invalidate_mode_state(void) {
    sPauseBgCaptureRequested = false;
    sPauseBgCaptured = false;
    sHomeMenuBgCaptureRequested = false;
    sHomeMenuBgCaptured = false;
    texman_invalidate_binding();
    active_texture_tile = -1;
    sAppliedShader = NULL;
    sAppliedSamplerStateValid = false;
}

int gfx_scegu_apply_video_mode(void) {
    OotPspVideoMode mode;
    int result;

    OotPspVideo_GetMode(&mode);
    if ((mode.bufferWidth > sMaxBufferWidth) || (mode.displayHeight > sMaxScreenHeight)) {
        printf("oot-psp TV buffers unavailable requested=%dx%d max=%dx%d edram=%lu\n", mode.bufferWidth,
               mode.displayHeight, sMaxBufferWidth, sMaxScreenHeight, (unsigned long)sVramSize);
        return -1000;
    }

    /* Daedalus enters the replacement route with GU output still enabled.
     * Disabling it here leaves some Slim firmwares in LCD scanout mode; using
     * the Display button then only mirrors the old 480x272 surface. */
    sceGuInit();

    /* Match Daedalus's working transition order exactly: select the DVE route
     * first, then rebuild the GU/display buffers for that route.  Calling the
     * DVE sequence after sceGuDispBuffer leaves some hardware with the TV
     * encoder active but the scanout still using the old LCD buffer layout. */
    gfx_scegu_set_mode_geometry(&mode);
    sInterlacedOutput = gfx_scegu_mode_is_interlaced(&mode);
    sCrt240pOutput = mode.crt240p != 0;
    gfx_scegu_reset_framebuffers();
    gfx_scegu_invalidate_mode_state();
    result = gfx_scegu_set_hardware_video_mode(&mode);
    if (result < 0) {
        int failedResult = result;

        OotPspVideo_ForceLcd();
        OotPspVideo_GetMode(&mode);
        gfx_scegu_set_mode_geometry(&mode);
        sInterlacedOutput = false;
        sCrt240pOutput = false;
        gfx_scegu_reset_framebuffers();
        gfx_scegu_invalidate_mode_state();
        if (gfx_scegu_set_hardware_video_mode(&mode) < 0) {
            (void)sceDisplaySetMode(0, LCD_SCREEN_WIDTH, LCD_SCREEN_HEIGHT);
        }
        gfx_scegu_configure_gu_buffers();
        OotPspVideo_CommitMode();
        sceDisplaySetFrameBuf(gfx_scegu_vram_cpu_addr(sDisplayBuffer), BUF_WIDTH, sColorPixelFormat,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
        gfx_set_dimensions((uint32_t)mode.viewportWidth, (uint32_t)mode.viewportHeight);
        gfx_scegu_log_display_mode(&mode, failedResult);
        return failedResult;
    }

    gfx_scegu_configure_gu_buffers();
    OotPspVideo_CommitMode();
    sceDisplaySetFrameBuf(gfx_scegu_vram_cpu_addr(sDisplayBuffer), BUF_WIDTH, sColorPixelFormat,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    gfx_set_dimensions((uint32_t)mode.viewportWidth, (uint32_t)mode.viewportHeight);
    gfx_scegu_log_display_mode(&mode, result);
    return 0;
}

static void gfx_scegu_init(void) {
    OotPspVideoMode mode;
    OotPspVideoMode initialMode;
    void *backgroundStorage;
    void *fbp0;
    void *fbp1;
    void *zbp;
    size_t texmanSize;

    sVramSize = OotPspDve_GetEdramSize();
    if (sVramSize < sceGeEdramGetSize()) {
        sVramSize = sceGeEdramGetSize();
    }
    if (sVramSize < PSP_PHAT_VRAM_SIZE) {
        sVramSize = PSP_PHAT_VRAM_SIZE;
    }
    if (OotPspDve_IsAvailable() && (sVramSize >= (4 * 1024 * 1024))) {
        sMaxBufferWidth = TV_BUFFER_WIDTH;
        sMaxScreenHeight = TV_SCREEN_HEIGHT;
        /* The original Daedalus DVE path requires a 32-bit display buffer on
         * Slim hardware. Keep LCD and TV in the same format so switching modes
         * cannot reinterpret an in-flight framebuffer. */
        sColorPixelFormat = GU_PSM_8888;
        sColorPixelSize = sizeof(uint32_t);
    } else {
        sMaxBufferWidth = LCD_BUFFER_WIDTH;
        sMaxScreenHeight = LCD_SCREEN_HEIGHT;
        sColorPixelFormat = GU_PSM_5650;
        sColorPixelSize = sizeof(uint16_t);
        OotPspVideo_ForceLcd();
    }

    OotPspVideo_GetMode(&mode);
    initialMode = mode;
    initialMode.tvOutput = false;
    initialMode.bufferWidth = LCD_BUFFER_WIDTH;
    initialMode.displayWidth = LCD_SCREEN_WIDTH;
    initialMode.displayHeight = LCD_SCREEN_HEIGHT;
    initialMode.crt240p = 0;
    initialMode.viewportWidth = (mode.viewportWidth == 720) ? 480 : 362;
    initialMode.viewportHeight = LCD_SCREEN_HEIGHT;
    initialMode.dveOutput = 0;
    initialMode.dveMode = 0;
    initialMode.dveWidth = LCD_SCREEN_WIDTH;
    initialMode.dveHeight = LCD_SCREEN_HEIGHT;
    gfx_scegu_set_mode_geometry(&initialMode);
    sFramebufferCapacity =
        (size_t)sMaxBufferWidth * (size_t)sMaxScreenHeight * (size_t)sColorPixelSize;
    backgroundStorage = memalign(64, sFramebufferCapacity * 3);
    if (backgroundStorage == NULL) {
        sceIoWrite(1, "OUT OF FRAMEBUFFER MEMORY!\n", 27);
        sceKernelExitGame();
    }
    sPauseBgBuffer = backgroundStorage;
    sHomeMenuBgBuffer = (uint8_t *)backgroundStorage + sFramebufferCapacity;
    sHomeMenuBgBlurScratch = (uint8_t *)backgroundStorage + (sFramebufferCapacity * 2);

    sceGuInit();
    (void)sceDisplaySetMode(0, LCD_SCREEN_WIDTH, LCD_SCREEN_HEIGHT);
    sDepthTestEnabled = true;
    sDepthWriteEnabled = true;
    sTextureEnvColor = 0xffffffff;
    active_texture_tile = -1;
    sAppliedShader = NULL;
    sAppliedSamplerStateValid = false;
    memset(tmu_state, 0, sizeof(tmu_state));

    fbp0 = getStaticVramBuffer((unsigned int)sMaxBufferWidth, (unsigned int)sMaxScreenHeight, sColorPixelFormat);
    fbp1 = getStaticVramBuffer((unsigned int)sMaxBufferWidth,
                               (unsigned int)((sMaxBufferWidth == TV_BUFFER_WIDTH)
                                                  ? TV_INTERLACED_BUFFER_HEIGHT
                                                  : sMaxScreenHeight),
                               sColorPixelFormat);
    zbp = getStaticVramBuffer((unsigned int)sMaxBufferWidth, (unsigned int)sMaxScreenHeight, GU_PSM_4444);

    sPrimaryBuffer = fbp0;
    sSecondaryBuffer = fbp1;
    sInterlacedOutput = false;
    sCrt240pOutput = false;
    gfx_scegu_reset_framebuffers();
    sDepthBuffer = zbp;

    gfx_scegu_configure_gu_buffers();
    sceDisplaySetFrameBuf(gfx_scegu_vram_cpu_addr(sDisplayBuffer), BUF_WIDTH, sColorPixelFormat,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);

    void *texman_buffer;

    if (sMaxBufferWidth == TV_BUFFER_WIDTH) {
        /* Daedalus fills almost all 4 MiB of EDRAM with its 32-bit TV frame
         * and depth buffers. Its texture allocation therefore falls through
         * to RAM. Keep OOT textures out of the small upper-EDRAM tail too:
         * some Slim firmwares corrupt texture fetches from that region while
         * the DVE is scanning a 720-wide framebuffer. */
        texmanSize = TEXMAN_BUFFER_SIZE;
        texman_buffer = memalign(64, texmanSize);
    } else {
        texmanSize = getStaticVramBytesRemaining();
        if (texmanSize > TEXMAN_BUFFER_SIZE) {
            texmanSize = TEXMAN_BUFFER_SIZE;
        }
        texman_buffer = getStaticVramBufferBytes(texmanSize);
    }
    if (!texman_buffer) {
        char msg[32];
        sprintf(msg, "OUT OF MEMORY!\n");
        sceIoWrite(1, msg, strlen(msg));

        sceKernelExitGame();
    }
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    texman_reset(texman_aligned, texmanSize);
    printf("oot-psp texture cache storage=%s addr=%p size=%lu\n",
           (sMaxBufferWidth == TV_BUFFER_WIDTH) ? "ram" : "edram", texman_aligned,
           (unsigned long)texmanSize);

    if (mode.tvOutput) {
        int result = gfx_scegu_apply_video_mode();

        if (result < 0) {
            printf("oot-psp startup TV mode failed result=%d edram=%lu\n", result, (unsigned long)sVramSize);
        }
    } else {
        gfx_scegu_set_mode_geometry(&mode);
        OotPspVideo_CommitMode();
    }

#if OOT_PSP_USE_INTRAFONT
    /* Load the firmware font while the renderer is initialized, before game
     * assets consume the remaining heap. Menu drawing then only activates the
     * already-resident font and cannot fall into the partial emergency font. */
    gfx_scegu_ensure_home_menu_font();
#endif
}

static void gfx_scegu_start_frame(void) {
    bool hasHomeMenuBackground;
    bool hasPauseBackground;
    bool hasStaticBackground;

    /* GU state can be changed by intraFont and other callback rendering between
     * game frames. Revalidate once per frame, then suppress redundant texture
     * image/filter/wrap commands inside the frame. */
    texman_invalidate_binding();
    active_texture_tile = -1;
    sAppliedShader = NULL;
    sAppliedSamplerStateValid = false;

    if (sHomeMenuBgCaptureRequested) {
        gfx_scegu_copy_framebuffer_from_vram(sHomeMenuBgBuffer,
                                             sInterlacedOutput ? sPrimaryBuffer : sDisplayBuffer);
        gfx_scegu_blur_framebuffer(sHomeMenuBgBuffer);
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = true;
    }

    if (sPauseBgCaptureRequested) {
        gfx_scegu_copy_framebuffer_from_vram(sPauseBgBuffer,
                                             sInterlacedOutput ? sPrimaryBuffer : sDisplayBuffer);
        sPauseBgCaptureRequested = false;
        sPauseBgCaptured = true;
    }

    hasHomeMenuBackground = sHomeMenuBgActive && sHomeMenuBgCaptured;
    hasPauseBackground = sPauseBgActive && sPauseBgCaptured;
    hasStaticBackground = hasHomeMenuBackground || hasPauseBackground;

    if (hasHomeMenuBackground) {
        gfx_scegu_copy_framebuffer_to_vram(sDrawBuffer, sHomeMenuBgBuffer);
    } else if (hasPauseBackground) {
        gfx_scegu_copy_framebuffer_to_vram(sDrawBuffer, sPauseBgBuffer);
    }

    sceGuStart(GU_DIRECT, list);
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearDepth(0);

    if (hasStaticBackground) {
        sceGuClear(GU_DEPTH_BUFFER_BIT);
    } else {
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    }

    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);

    // Identity every frame? unsure.
    //sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *) identity_matrix);

#if 0
    const int DitherMatrix[2][16] = { { 0, 8, 0, 8,
                         8, 0, 8, 0,
                         0, 8, 0, 8,
                         8, 0, 8, 0 },
                        { 8, 8, 8, 8,
                          0, 8, 0, 8,
                          8, 8, 8, 8,
                          0, 8, 0, 8 } };

    extern int gDoDither;
    extern int gFrame;

    sceGuDisable(GU_DITHER);
    if(gDoDither){
        // every frame
        sceGuSetDither((const ScePspIMatrix4 *)DitherMatrix[(gFrame&1)]);
        sceGuEnable(GU_DITHER);
    }
#endif
}

void gfx_scegu_on_resize(void) {
}

static void gfx_scegu_present_interlaced(void) {
    const uintptr_t src = (uintptr_t)gfx_scegu_vram_cpu_addr(sPrimaryBuffer);
    const uintptr_t dst = (uintptr_t)gfx_scegu_vram_cpu_addr(sSecondaryBuffer);
    const int rowBytes = BUF_WIDTH * sColorPixelSize;

    /* The DVE's 480i modes expose two 240-line fields in one 503-line
     * framebuffer. Odd source lines occupy rows 0..239; even lines start at
     * row 262. Native 320x240 bypasses this conversion entirely. */
    sceGuStart(GU_DIRECT, list);
    sceGuCopyImage(sColorPixelFormat, 0, 0, TV_SCREEN_WIDTH, TV_SCREEN_HEIGHT / 2, BUF_WIDTH * 2,
                   (void *)(src + (uintptr_t)rowBytes), 0, 0, BUF_WIDTH, (void *)dst);
    sceGuTexSync();
    sceGuCopyImage(sColorPixelFormat, 0, 0, TV_SCREEN_WIDTH, TV_SCREEN_HEIGHT / 2, BUF_WIDTH * 2,
                   (void *)src, 0, 0, BUF_WIDTH,
                   (void *)(dst + ((uintptr_t)BUF_WIDTH * TV_INTERLACED_FIELD_OFFSET * sColorPixelSize)));
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    sDrawBuffer = sPrimaryBuffer;
    sDisplayBuffer = sSecondaryBuffer;
}

static void gfx_scegu_end_frame(void) {
    void *previousDrawBuffer = sDrawBuffer;

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    if (sInterlacedOutput) {
        gfx_scegu_present_interlaced();
    } else {
        sDrawBuffer = sceGuSwapBuffers();
        sDisplayBuffer = previousDrawBuffer;
    }
}

static void gfx_scegu_finish_render(void) {
    /* There should be something here! */
}

// clang-format off
struct GfxRenderingAPI gfx_scegu_api = {
    gfx_scegu_z_is_from_0_to_1,
    gfx_scegu_unload_shader,
    gfx_scegu_load_shader,
    gfx_scegu_create_and_load_new_shader,
    gfx_scegu_lookup_shader,
    gfx_scegu_shader_get_info,
    gfx_scegu_new_texture,
    gfx_scegu_select_texture,
    gfx_scegu_upload_texture,
    gfx_scegu_set_sampler_parameters,
    gfx_scegu_set_texture_env_color,
    gfx_scegu_set_depth_test,
    gfx_scegu_set_depth_mask,
    gfx_scegu_set_zmode_decal,
    gfx_scegu_set_viewport,
    gfx_scegu_set_scissor,
    gfx_scegu_set_use_alpha,
    gfx_scegu_draw_triangles,
    gfx_scegu_draw_fog_triangles,
    gfx_scegu_init,
    gfx_scegu_on_resize,
    gfx_scegu_start_frame,
    gfx_scegu_end_frame,
    gfx_scegu_finish_render
};

void gfx_scegu_request_pause_background(void) {
    sPauseBgCaptureRequested = true;
}

void gfx_scegu_set_pause_background_active(bool active) {
    sPauseBgActive = active;

    if (!active) {
        sPauseBgCaptureRequested = false;
        sPauseBgCaptured = false;
    }
}

void gfx_scegu_request_home_menu_background(void) {
    sHomeMenuBgCaptureRequested = true;
}

void gfx_scegu_set_home_menu_background_active(bool active) {
    sHomeMenuBgActive = active;

    if (!active) {
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = false;
    }
}

void gfx_scegu_render_home_menu(int selectedIndex, int screen, int submenuSelectedIndex, const char* statusMessage,
                                uint8_t highlightRed, uint8_t highlightGreen, uint8_t highlightBlue,
                                int batteryPercent) {
    gfx_scegu_prepare_home_menu_draw();

    if (screen == 1) {
        gfx_scegu_render_controller_mapping(submenuSelectedIndex, statusMessage, highlightRed, highlightGreen,
                                             highlightBlue);
    } else if (screen == 2) {
        gfx_scegu_render_video_settings(submenuSelectedIndex, statusMessage, highlightRed, highlightGreen,
                                        highlightBlue);
    } else if (screen == 3) {
        gfx_scegu_render_about(submenuSelectedIndex);
    } else {
        gfx_scegu_render_home_menu_main(selectedIndex, highlightRed, highlightGreen, highlightBlue);
    }

    gfx_scegu_draw_home_menu_battery(batteryPercent);
}

#endif // TARGET_SCEGU
