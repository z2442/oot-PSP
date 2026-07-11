#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "ultra64.h"
#include "ultra64/gs2dex.h"

#ifdef GU_PI
#undef GU_PI
#endif
#include <pspfpu.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include "pspmath.h"

#include "gfx_fast3d.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#if defined(TARGET_PSP)
#include "buffers.h"
#endif
#include "oot_port_macros.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_compat.h"
#include "oot_psp_gfx_ext.h"
#include "oot_psp_memory.h"
#include "segmented_address.h"
#include "sys_matrix.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define INFO_MSG(x) printf("%s %s\n", __FILE__ ":" TOSTRING(__LINE__), x)
#define _UNUSED(x) (void)(x)

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (1024)
#define MAX_LIGHTS 7
#define MAX_VERTICES 64
#define GFX_TLUT_SIZE_BYTES 512
#define GFX_CI4_TLUT_SIZE_BYTES 32
#define MODELVIEW_STACK_SIZE 11

#define PSP_NATIVE_ADDR_START 0x08800000U
#define PSP_NATIVE_ADDR_END 0x0C000000U
#define PSP_SEGMENTED_COLLISION_OFFSET_MAX 0x00010000U
#define PSP_ASSET_SYMBOL_GIDENTITYMTX 0x0E000001U

/* Pixel Formats */
#define GU_PSM_5650		(0) /* Display, Texture, Palette */
#define GU_PSM_5551		(1) /* Display, Texture, Palette */
#define GU_PSM_4444		(2) /* Display, Texture, Palette */
#define GU_PSM_8888		(3) /* Display, Texture, Palette */
#define GU_PSM_T4		(4) /* Texture */
#define GU_PSM_T8		(5) /* Texture */
#define GU_PSM_T16		(6) /* Texture */
#define GU_PSM_T32		(7) /* Texture */
extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
extern float identity_matrix[4][4];
extern Mtx D_01000000;

#if defined(TARGET_PSP)
struct GfxCmdSnapshot {
    uintptr_t addr;
    uint32_t w0;
    uint32_t w1;
};
#endif

struct RGBA {
    uint8_t r, g, b, a;
} __attribute__((packed, aligned(4)));

struct XYWidthHeight {
    uint16_t x, y, width, height;
} __attribute__((packed, aligned(4)));

struct LoadedVertex {
    float x, y, z, w;
    float _x, _y, _z, _w;
    float u, v;
    struct RGBA color;
    uint32_t clip_rej;
} __attribute__((packed, aligned(16)));

#if defined(TARGET_PSP)
typedef char LoadedVertex_vfpu_size_check[(sizeof(struct LoadedVertex) == 48) ? 1 : -1];
typedef char Vtx_vfpu_size_check[(sizeof(Vtx) == 16) ? 1 : -1];
/* Keep the assembly entry point to four arguments so it does not depend on PSP EABI extension registers. */
struct GfxVfpuTransformMatrices {
    const float (*model)[4];
    const float (*projection)[4];
};
extern uint32_t gfx_clip_to_hyperplane_vfpu(struct LoadedVertex *dest, const struct LoadedVertex *source,
                                            const float plane[4], uint32_t inCount);
extern void gfx_transform_vertices_vfpu(struct LoadedVertex* dest, const Vtx* source, uint32_t count,
                                        const struct GfxVfpuTransformMatrices* matrices);
#endif

typedef struct VertexColor {
	unsigned short u, v;
	struct RGBA color;
	unsigned short x, y, z;
} VertexColor __attribute__((aligned(16)));

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    uint16_t width, height;
    uint32_t row_stride_bytes;
    uint8_t source_nibble_offset;
#if defined(TARGET_PSP)
    uint32_t source_key;
    const uint8_t* palette_addr;
    uint32_t palette_key;
    uint16_t upload_width, upload_height;
#endif
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    uint8_t masks, maskt;
#if defined(TARGET_PSP)
    uint8_t mirror_s, mirror_t;
#endif
    bool linear_filter;
} __attribute__((packed, aligned(4)));
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t used_textures[2];
    int8_t active_texture;
    uint8_t vertex_color_source[2];
    bool texture_blend;
} __attribute__((packed, aligned(4)));

#define COLOR_COMBINER_CACHE_COUNT 64
typedef struct ColorCombinerCacheEntry {
    struct ColorCombiner combiner;
    bool valid;
} ColorCombinerCacheEntry;
static ColorCombinerCacheEntry color_combiner_cache[COLOR_COMBINER_CACHE_COUNT];

struct TriPipelineState {
    struct ColorCombiner *comb;
    bool use_alpha;
    bool used_textures[2];
    bool use_texture;
    bool two_texture_blend;
    bool color_mul_env;
    bool color_mul_prim;
    float tex_u_scale[2], tex_v_scale[2];
    float tex_u_bias[2], tex_v_bias[2];
    float tex_u_nominal_span[2], tex_v_nominal_span[2];
    bool tex_u_scale_to_primitive[2], tex_v_scale_to_primitive[2];
} __attribute__((packed, aligned(4)));

typedef struct TextureTileState {
    uint8_t fmt;
    uint8_t siz;
    uint8_t cms, cmt;
    uint8_t masks, maskt;
    uint16_t uls, ult, lrs, lrt; // U10.2
    uint32_t line_size_bytes;
} TextureTileState;

static struct RSP {
    float modelview_matrix_stack[MODELVIEW_STACK_SIZE][4][4]__attribute__((aligned(16)));

    float P_matrix[4][4] __attribute__((aligned(16)));
    uint8_t modelview_matrix_stack_size;
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    uint32_t rdp_half_1;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    void *segments[NUM_SEGMENTS];
#if defined(TARGET_PSP)
    struct GfxCmdSnapshot segment_cmd[NUM_SEGMENTS];
#endif
    
    struct VertexColor loaded_vertices_2D[4];
    struct LoadedVertex loaded_vertices[MAX_VERTICES];
} rsp  __attribute__((aligned(16)));

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t fmt;
        uint8_t siz;
        uint32_t width;
        uint8_t tile_number;
#if defined(TARGET_PSP)
        struct GfxCmdSnapshot image_cmd;
#endif
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        uint32_t source_size_bytes;
        uint32_t row_stride_bytes;
        uint8_t source_nibble_offset;
#if defined(TARGET_PSP)
        struct GfxCmdSnapshot image_cmd;
        struct GfxCmdSnapshot load_cmd;
#endif
    } loaded_texture[2];
    TextureTileState texture_tile[2];
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    bool combine_color_mul_env;
    bool combine_color_mul_prim;
    bool combine_two_texture_blend;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp  __attribute__((aligned(4)));

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct ColorCombiner *color_combiner;
    uint32_t color_combiner_id;
    bool color_combiner_valid;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    bool tri_pipeline_dirty;
    bool backend_state_dirty;
    struct TriPipelineState tri_pipeline;
} rendering_state __attribute__((aligned(16)));

struct GfxDimensions gfx_current_dimensions __attribute__((aligned(4)));

static inline TextureTileState* gfx_get_texture_tile(int tile) {
    return &rdp.texture_tile[tile != 0];
}

static inline bool gfx_get_render_tile_slot(uint8_t tile, int* slot) {
    if (tile == G_TX_RENDERTILE) {
        *slot = 0;
        return true;
    }

    if (tile == 1) {
        *slot = 1;
        return true;
    }

    return false;
}

static bool dropped_frame;
static const struct RGBA white_color = {0xff, 0xff, 0xff, 0xff};
static OotPspHudAnchor sHudAnchor;
static bool sHudViewportFullscreen = true;
static float sNdcAspectScale = 1.0f;
static float sWidescreenMarginPixels;
static float sHudAnchorOffsetPixels;
static float sHudAnchorOffsetNdc;
static float sUploadedProjection[4][4] __attribute__((aligned(16)));
static bool sUploadedProjectionValid;

#if defined(TARGET_PSP)
static uint8_t sInvalidTextureBuf[256] __attribute__((aligned(16))) = { 0xff, 0xff, 0xff, 0xff };
static uint8_t sInvalidPaletteBuf[512] __attribute__((aligned(16))) = { 0xff, 0xff };
static struct GfxCmdSnapshot sCurrentCmd;
extern u8 __bss_start[];

static inline bool gfx_addr_is_native(uintptr_t addr) {
    return (addr >= PSP_NATIVE_ADDR_START) && (addr < PSP_NATIVE_ADDR_END);
}

static inline bool gfx_native_range_contains(uintptr_t value, size_t size) {
    uintptr_t end;

    if (size == 0) {
        return false;
    }

    end = value + size;
    if (end < value) {
        return false;
    }

    return (value >= PSP_NATIVE_ADDR_START) && (end <= PSP_NATIVE_ADDR_END);
}

static inline uintptr_t gfx_bswap32(uintptr_t value) {
    uint32_t v = (uint32_t)value;

    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) | ((v & 0x00FF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static inline bool gfx_normalize_native_range(uintptr_t value, size_t size, uintptr_t* normalized) {
    uintptr_t candidate;

    if (gfx_native_range_contains(value, size)) {
        *normalized = value;
        return true;
    }

    candidate = value & 0x0FFFFFFFU;
    if ((candidate != value) && gfx_native_range_contains(candidate, size)) {
        *normalized = candidate;
        return true;
    }

    if (value >= 0x00010000U) {
        candidate = gfx_bswap32(value);
        if (gfx_native_range_contains(candidate, size)) {
            *normalized = candidate;
            return true;
        }

        candidate &= 0x0FFFFFFFU;
        if (gfx_native_range_contains(candidate, size)) {
            *normalized = candidate;
            return true;
        }
    }

    return false;
}

static inline bool gfx_normalize_native_addr(uintptr_t value, uintptr_t* normalized) {
    uintptr_t candidate;

    if (gfx_addr_is_native(value)) {
        *normalized = value;
        return true;
    }

    candidate = value & 0x0FFFFFFFU;
    if ((candidate != value) && gfx_addr_is_native(candidate)) {
        *normalized = candidate;
        return true;
    }

    if (value >= 0x00010000U) {
        candidate = gfx_bswap32(value);
        if (gfx_addr_is_native(candidate)) {
            *normalized = candidate;
            return true;
        }

        candidate &= 0x0FFFFFFFU;
        if (gfx_addr_is_native(candidate)) {
            *normalized = candidate;
            return true;
        }
    }

    return false;
}

static bool gfx_range_contains(uintptr_t value, size_t size, uintptr_t rangeStart, uintptr_t rangeEnd) {
    uintptr_t end;

    if (size == 0) {
        return true;
    }

    end = value + size;
    if (end < value) {
        return false;
    }

    return (value >= rangeStart) && (end <= rangeEnd);
}

static bool gfx_is_graph_pool_range(uintptr_t value, size_t size) {
    for (size_t i = 0; i < ARRAY_COUNT(gGfxPools); i++) {
        uintptr_t poolStart = (uintptr_t)&gGfxPools[i];
        uintptr_t poolEnd = poolStart + sizeof(gGfxPools[i]);

        if (gfx_range_contains(value, size, poolStart, poolEnd)) {
            return true;
        }
    }

    return false;
}

static bool gfx_is_static_prx_range(uintptr_t value, size_t size) {
    return gfx_range_contains(value, size, PSP_NATIVE_ADDR_START, (uintptr_t)__bss_start);
}

static bool gfx_is_loaded_external_range(uintptr_t value, size_t size) {
    u32 flags;

    return OotPsp_GetLoadedExternalAssetRangeFlags((const void*)value, size, &flags);
}

static bool gfx_is_valid_native_dl_range(uintptr_t value, size_t size) {
    if (!gfx_native_range_contains(value, size)) {
        return false;
    }

    if (gfx_is_static_prx_range(value, size)) {
        return true;
    }

    if (gfx_is_graph_pool_range(value, size)) {
        return true;
    }

    if (OotPsp_IsSystemHeapRange((const void*)value, size)) {
        return true;
    }

    if (gfx_is_loaded_external_range(value, size)) {
        return true;
    }

    return false;
}

static bool gfx_is_valid_native_read_range(uintptr_t value, size_t size) {
    if (!gfx_native_range_contains(value, size)) {
        return false;
    }

    /*
     * Keep this provenance set in sync with gfx_is_valid_native_dl_range().
     * Display lists and texture staging data allocated from sPspSystemHeap can
     * numerically look like segments 8-B.  If the heap is omitted here,
     * seg_addr() translates a valid pointer through the active N64 segment and
     * sends the renderer into unrelated frame data.
     */
    if (gfx_is_static_prx_range(value, size) || gfx_is_graph_pool_range(value, size) ||
        OotPsp_IsSystemHeapRange((const void*)value, size)) {
        return true;
    }

    if (OotPsp_IsRuntimeByteRange((const void*)value, size)) {
        return true;
    }

    return gfx_is_loaded_external_range(value, size);
}

static void gfx_log_bad_data_source(const char* context, const void* addr, size_t sizeBytes) {
    static s32 sBadDataSourceLogCount = 0;
    uintptr_t value = (uintptr_t)addr;
    uint8_t segment = value >> 24;
    const struct GfxCmdSnapshot emptyCmd = { 0 };
    const struct GfxCmdSnapshot* segmentCmd = &emptyCmd;

    if (segment < NUM_SEGMENTS) {
        segmentCmd = &rsp.segment_cmd[segment];
    }

    if (sBadDataSourceLogCount < 32) {
        printf("oot-psp gfx bad data source context=%s addr=%08lx size=%lu cur=%08lx/%08lx/%08lx "
               "seg=%u segbase=%08lx segcmd=%08lx/%08lx/%08lx "
               "seg1=%08lx seg2=%08lx seg3=%08lx seg4=%08lx seg6=%08lx seg8=%08lx seg9=%08lx sega=%08lx "
               "segc=%08lx segd=%08lx segacmd=%08lx/%08lx/%08lx\n",
               context, (unsigned long)value, (unsigned long)sizeBytes, (unsigned long)sCurrentCmd.addr,
               (unsigned long)sCurrentCmd.w0, (unsigned long)sCurrentCmd.w1, segment,
               (segment < NUM_SEGMENTS) ? (unsigned long)(uintptr_t)rsp.segments[segment] : 0,
               (unsigned long)segmentCmd->addr, (unsigned long)segmentCmd->w0, (unsigned long)segmentCmd->w1,
               (unsigned long)(uintptr_t)rsp.segments[1], (unsigned long)(uintptr_t)rsp.segments[2],
               (unsigned long)(uintptr_t)rsp.segments[3], (unsigned long)(uintptr_t)rsp.segments[4],
               (unsigned long)(uintptr_t)rsp.segments[6], (unsigned long)(uintptr_t)rsp.segments[8],
               (unsigned long)(uintptr_t)rsp.segments[9], (unsigned long)(uintptr_t)rsp.segments[10],
               (unsigned long)(uintptr_t)rsp.segments[12], (unsigned long)(uintptr_t)rsp.segments[13],
               (unsigned long)rsp.segment_cmd[10].addr, (unsigned long)rsp.segment_cmd[10].w0,
               (unsigned long)rsp.segment_cmd[10].w1);
    } else if (sBadDataSourceLogCount == 32) {
        printf("oot-psp gfx bad data source logs suppressed\n");
    }

    sBadDataSourceLogCount++;
}

static bool gfx_normalize_read_source(const void* addr, size_t sizeBytes, const char* context, const void** normalized) {
    uintptr_t normalizedValue;

    if (gfx_normalize_native_range((uintptr_t)addr, sizeBytes, &normalizedValue) &&
        gfx_is_valid_native_read_range(normalizedValue, sizeBytes)) {
        *normalized = (const void*)normalizedValue;
        return true;
    }

    gfx_log_bad_data_source(context, addr, sizeBytes);
    return false;
}

static void gfx_log_bad_texture_source(int tile, const char* context, const uint8_t* addr, uint32_t sizeBytes) {
    static s32 sBadTextureSourceLogCount = 0;

    if (sBadTextureSourceLogCount < 16) {
        const struct GfxCmdSnapshot emptyCmd = { 0 };
        const struct GfxCmdSnapshot* imageCmd = &rdp.texture_to_load.image_cmd;
        const struct GfxCmdSnapshot* loadCmd = &emptyCmd;

        if (tile >= 0 && tile < 2) {
            imageCmd = &rdp.loaded_texture[tile].image_cmd;
            loadCmd = &rdp.loaded_texture[tile].load_cmd;
        }

        printf("oot-psp gfx bad texture source context=%s tile=%d addr=%08lx size=%lu cur=%08lx/%08lx/%08lx "
               "setimg=%08lx/%08lx/%08lx load=%08lx/%08lx/%08lx tex=%08lx fmt=%u siz=%u width=%lu "
               "loadslot=%u seg1=%08lx seg4=%08lx seg8=%08lx seg9=%08lx sega=%08lx segd=%08lx "
               "seg8cmd=%08lx/%08lx/%08lx seg9cmd=%08lx/%08lx/%08lx segacmd=%08lx/%08lx/%08lx\n",
               context, tile, (unsigned long)(uintptr_t)addr, (unsigned long)sizeBytes,
               (unsigned long)sCurrentCmd.addr, (unsigned long)sCurrentCmd.w0, (unsigned long)sCurrentCmd.w1,
               (unsigned long)imageCmd->addr, (unsigned long)imageCmd->w0, (unsigned long)imageCmd->w1,
               (unsigned long)loadCmd->addr, (unsigned long)loadCmd->w0, (unsigned long)loadCmd->w1,
               (unsigned long)(uintptr_t)rdp.texture_to_load.addr, rdp.texture_to_load.fmt, rdp.texture_to_load.siz,
               (unsigned long)rdp.texture_to_load.width, rdp.texture_to_load.tile_number,
               (unsigned long)(uintptr_t)rsp.segments[1], (unsigned long)(uintptr_t)rsp.segments[4],
               (unsigned long)(uintptr_t)rsp.segments[8], (unsigned long)(uintptr_t)rsp.segments[9],
               (unsigned long)(uintptr_t)rsp.segments[10], (unsigned long)(uintptr_t)rsp.segments[13],
               (unsigned long)rsp.segment_cmd[8].addr, (unsigned long)rsp.segment_cmd[8].w0,
               (unsigned long)rsp.segment_cmd[8].w1, (unsigned long)rsp.segment_cmd[9].addr,
               (unsigned long)rsp.segment_cmd[9].w0, (unsigned long)rsp.segment_cmd[9].w1,
               (unsigned long)rsp.segment_cmd[10].addr, (unsigned long)rsp.segment_cmd[10].w0,
               (unsigned long)rsp.segment_cmd[10].w1);
    } else if (sBadTextureSourceLogCount == 16) {
        printf("oot-psp gfx bad texture source logs suppressed\n");
    }

    sBadTextureSourceLogCount++;
}

static bool gfx_normalize_texture_source(const uint8_t** addr, uint32_t sizeBytes) {
    uintptr_t normalized;

    if (gfx_normalize_native_range((uintptr_t)*addr, sizeBytes, &normalized) &&
        gfx_is_valid_native_read_range(normalized, sizeBytes)) {
        *addr = (const uint8_t*)normalized;
        return true;
    }

    return false;
}

static bool gfx_texture_load_slot(const char* context, int* slot) {
    if (rdp.texture_to_load.tile_number < 2) {
        *slot = rdp.texture_to_load.tile_number;
        return true;
    }

    gfx_log_bad_texture_source(rdp.texture_to_load.tile_number, context, rdp.texture_to_load.addr, 1);
    return false;
}

static void gfx_set_invalid_loaded_texture(int tile) {
    rdp.loaded_texture[tile].addr = sInvalidTextureBuf;
    rdp.loaded_texture[tile].size_bytes = 8;
    rdp.loaded_texture[tile].source_size_bytes = 8;
    rdp.loaded_texture[tile].row_stride_bytes = 2;
    rdp.loaded_texture[tile].source_nibble_offset = 0;
}

static void gfx_set_invalid_texture_tile(int tile) {
    TextureTileState* tileState = gfx_get_texture_tile(tile);

    tileState->uls = 0;
    tileState->ult = 0;
    tileState->lrs = 0;
    tileState->lrt = 0;
    tileState->line_size_bytes = 2;
}

static void gfx_validate_palette_source(const char* context) {
    if (gfx_normalize_texture_source(&rdp.palette, sizeof(sInvalidPaletteBuf))) {
        return;
    }

    gfx_log_bad_texture_source(-1, context, rdp.palette, sizeof(sInvalidPaletteBuf));
    rdp.palette = sInvalidPaletteBuf;
}

static uint32_t gfx_loaded_texture_source_size(int tile) {
    return rdp.loaded_texture[tile].source_size_bytes != 0 ? rdp.loaded_texture[tile].source_size_bytes
                                                           : rdp.loaded_texture[tile].size_bytes;
}

static bool gfx_validate_texture_source(int tile, const char* context) {
    const uint8_t* addr = rdp.loaded_texture[tile].addr;
    uint32_t sizeBytes = gfx_loaded_texture_source_size(tile);

    if (gfx_normalize_texture_source(&addr, sizeBytes)) {
        rdp.loaded_texture[tile].addr = addr;
        return true;
    }

    gfx_log_bad_texture_source(tile, context, addr, sizeBytes);
    gfx_set_invalid_loaded_texture(tile);
    gfx_set_invalid_texture_tile(tile);
    return false;
}
#endif

static uint32_t gfx_texture_source_span_size(int tile) {
#if defined(TARGET_PSP)
    return gfx_loaded_texture_source_size(tile);
#else
    return rdp.loaded_texture[tile].source_size_bytes != 0 ? rdp.loaded_texture[tile].source_size_bytes
                                                           : rdp.loaded_texture[tile].size_bytes;
#endif
}

typedef enum GfxTextureSwapMode {
    GFX_TEXTURE_SWAP_NONE,
    GFX_TEXTURE_SWAP_DIRECT,
    GFX_TEXTURE_SWAP_MAPPED,
} GfxTextureSwapMode;

typedef struct GfxTextureSwapState {
    GfxTextureSwapMode mode;
    uintptr_t rangeStart;
    uintptr_t rangeEnd;
} GfxTextureSwapState;

static GfxTextureSwapState gfx_texture_source_swap_state(const uint8_t* addr, uint32_t sizeBytes) {
    GfxTextureSwapState state = { GFX_TEXTURE_SWAP_NONE, 0, 0 };
#if defined(TARGET_PSP)
    u32 loadedFlags;

    /*
     * Extracted OoT textures are declared as u64 words. On PSP those words are
     * stored little-endian, so byte-oriented texture import must flip bytes back
     * within each 64-bit word. Loaded external assets need asset-offset-aware
     * mapping; static PRX data can use a direct native-address XOR.
     */
    if ((addr == sInvalidTextureBuf) || (addr == sInvalidPaletteBuf)) {
        return state;
    }

    if (OotPsp_GetLoadedExternalAssetRangeFlags(addr, sizeBytes, &loadedFlags) ||
        OotPsp_GetLoadedExternalAssetRangeFlags(addr, 1, &loadedFlags)) {
        state.mode = ((loadedFlags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0) ? GFX_TEXTURE_SWAP_MAPPED
                                                                          : GFX_TEXTURE_SWAP_NONE;
        if (state.mode == GFX_TEXTURE_SWAP_MAPPED) {
            OotPsp_GetNativeExternalTextureMappingRange(addr, &state.rangeStart, &state.rangeEnd);
        }
        return state;
    }

    state.mode = OotPsp_IsRuntimeByteRange(addr, sizeBytes) ? GFX_TEXTURE_SWAP_NONE : GFX_TEXTURE_SWAP_DIRECT;
#else
    _UNUSED(addr);
    _UNUSED(sizeBytes);
#endif
    return state;
}

static inline uint8_t gfx_read_texture_source_u8(const uint8_t* addr, uint32_t offset,
                                                 GfxTextureSwapState* swapState) {
#if defined(TARGET_PSP)
    uintptr_t source = (uintptr_t)addr + offset;

    if (swapState->mode == GFX_TEXTURE_SWAP_MAPPED) {
        uintptr_t relative;
        uintptr_t mappedRelative;

        if ((source < swapState->rangeStart) || (source >= swapState->rangeEnd)) {
            OotPsp_GetNativeExternalTextureMappingRange((const void*)source, &swapState->rangeStart,
                                                        &swapState->rangeEnd);
        }

        relative = source - swapState->rangeStart;
        mappedRelative = relative ^ 7U;
        if ((swapState->rangeStart != 0) &&
            (mappedRelative < (swapState->rangeEnd - swapState->rangeStart))) {
            source = swapState->rangeStart + mappedRelative;
        } else {
            source ^= 7U;
        }
    } else if (swapState->mode == GFX_TEXTURE_SWAP_DIRECT) {
        source ^= 7U;
    }

    return *(const uint8_t*)source;
#else
    _UNUSED(swapState);

    return addr[offset];
#endif
}

static inline uint16_t gfx_read_texture_source_be16(const uint8_t* addr, uint32_t offset,
                                                    GfxTextureSwapState* swapState) {
    uint16_t hi = gfx_read_texture_source_u8(addr, offset, swapState);
    uint16_t lo = gfx_read_texture_source_u8(addr, offset + 1, swapState);

    return (hi << 8) | lo;
}

#if defined(TARGET_PSP)
typedef struct psp_fast_t {
  float u,v;
  struct RGBA color;
  float x,y,z;
} psp_fast_t;
typedef struct psp_uv_t {
  float u,v;
  uint8_t alpha;
} psp_uv_t;
static psp_fast_t buf_vbo[MAX_BUFFERED  * 3] __attribute__ ((aligned (32))); // 3 vertices in a triangle and 26 floats per vtx
static psp_uv_t buf_vbo_tex1[MAX_BUFFERED * 3] __attribute__((aligned(32)));
#else
static float buf_vbo[MAX_BUFFERED * (26 * 3)] // 3 vertices in a triangle and 26 floats per vtx
#endif
static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#if defined(TARGET_PSP)
static uint8_t psp_texture_stage_buf[256 * 256 * 4] __attribute__((aligned(16)));

static inline uint32_t gfx_next_power_of_two(uint32_t value) {
    if (value <= 1) {
        return 1;
    }

    return 1U << (32 - __builtin_clz(value - 1));
}

static inline bool gfx_is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static inline size_t gfx_gu_texture_bytes_per_pixel(unsigned int type) {
    if (type == GU_PSM_T8) {
        return 1;
    }

    if ((type == GU_PSM_5551) || (type == GU_PSM_4444)) {
        return 2;
    }

    return 4;
}

static inline void gfx_copy_psp_mirrored_row(uint8_t *dst, const uint8_t *src, uint32_t width,
                                             size_t bytes_per_pixel) {
    src += (size_t)width * bytes_per_pixel;

    switch (bytes_per_pixel) {
        case 1:
            while (width-- != 0) {
                *dst++ = *--src;
            }
            break;

        case 2:
            while (width-- != 0) {
                src -= 2;
                dst[0] = src[0];
                dst[1] = src[1];
                dst += 2;
            }
            break;

        default:
            while (width-- != 0) {
                src -= 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                dst += 4;
            }
            break;
    }
}

static inline void gfx_copy_psp_texture_texel(uint8_t *dst, const uint8_t *src, size_t bytes_per_pixel) {
    switch (bytes_per_pixel) {
        case 1:
            dst[0] = src[0];
            break;

        case 2:
            dst[0] = src[0];
            dst[1] = src[1];
            break;

        default:
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
            break;
    }
}

static void gfx_fill_psp_texture_horizontal_padding(uint8_t *row, uint32_t content_x, uint32_t content_width,
                                                    uint32_t upload_width, size_t bytes_per_pixel) {
    if (content_width == 0) {
        return;
    }

    if (content_x != 0) {
        const uint8_t *edge = row + (size_t)content_x * bytes_per_pixel;
        uint8_t *dst = row;

        for (uint32_t x = 0; x < content_x; x++) {
            gfx_copy_psp_texture_texel(dst, edge, bytes_per_pixel);
            dst += bytes_per_pixel;
        }
    }

    uint32_t content_right = content_x + content_width;

    if (content_right < upload_width) {
        const uint8_t *edge = row + (size_t)(content_right - 1) * bytes_per_pixel;
        uint8_t *dst = row + (size_t)content_right * bytes_per_pixel;

        for (uint32_t x = content_right; x < upload_width; x++) {
            gfx_copy_psp_texture_texel(dst, edge, bytes_per_pixel);
            dst += bytes_per_pixel;
        }
    }
}

static void gfx_fill_psp_texture_vertical_padding(uint32_t content_y, uint32_t content_height,
                                                  uint32_t upload_height, size_t row_bytes) {
    if (content_height == 0) {
        return;
    }

    if (content_y != 0) {
        const uint8_t *edge_row = psp_texture_stage_buf + (size_t)content_y * row_bytes;

        for (uint32_t y = 0; y < content_y; y++) {
            OotPsp_MemcpyVfpu(psp_texture_stage_buf + (size_t)y * row_bytes, edge_row, row_bytes);
        }
    }

    uint32_t content_bottom = content_y + content_height;

    if (content_bottom < upload_height) {
        const uint8_t *edge_row = psp_texture_stage_buf + (size_t)(content_bottom - 1) * row_bytes;

        for (uint32_t y = content_bottom; y < upload_height; y++) {
            OotPsp_MemcpyVfpu(psp_texture_stage_buf + (size_t)y * row_bytes, edge_row, row_bytes);
        }
    }
}

static const uint8_t *gfx_prepare_psp_texture_for_upload(const uint8_t *src, uint32_t width, uint32_t height, unsigned int type, bool mirror_s, bool mirror_t, uint32_t *upload_width, uint32_t *upload_height, bool *applied_mirror_s, bool *applied_mirror_t) {
    const size_t bytes_per_pixel = gfx_gu_texture_bytes_per_pixel(type);
    const uint32_t pot_width = gfx_next_power_of_two(width);
    const uint32_t pot_height = gfx_next_power_of_two(height);
    bool use_mirror_s = mirror_s;
    bool use_mirror_t = mirror_t;
    uint32_t source_x_offset;
    uint32_t source_y_offset;

    *upload_width = pot_width * (use_mirror_s ? 2 : 1);
    *upload_height = pot_height * (use_mirror_t ? 2 : 1);

    if (!use_mirror_s && !use_mirror_t && pot_width == width && pot_height == height) {
        return src;
    }

    if ((size_t)(*upload_width) * (*upload_height) * bytes_per_pixel > sizeof(psp_texture_stage_buf)) {
        use_mirror_s = false;
        use_mirror_t = false;
        *upload_width = pot_width;
        *upload_height = pot_height;
    }

    if ((size_t)(*upload_width) * (*upload_height) * bytes_per_pixel > sizeof(psp_texture_stage_buf)) {
        *upload_width = width;
        *upload_height = height;
        *applied_mirror_s = false;
        *applied_mirror_t = false;
        return src;
    }

    *applied_mirror_s = use_mirror_s;
    *applied_mirror_t = use_mirror_t;

    source_x_offset = use_mirror_s ? pot_width : 0;
    source_y_offset = use_mirror_t ? pot_height : 0;

    const size_t src_row_bytes = (size_t)width * bytes_per_pixel;
    const size_t dst_row_bytes = (size_t)(*upload_width) * bytes_per_pixel;
    const uint32_t content_x_offset = use_mirror_s ? (pot_width - width) : source_x_offset;
    const uint32_t content_width = width * (use_mirror_s ? 2 : 1);
    const uint32_t content_y_offset = use_mirror_t ? (pot_height - height) : source_y_offset;
    const uint32_t content_height = height * (use_mirror_t ? 2 : 1);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_row = src + (size_t)y * src_row_bytes;
        uint8_t *dst_base = psp_texture_stage_buf + (size_t)(source_y_offset + y) * dst_row_bytes;
        uint8_t *dst_row = dst_base + (size_t)source_x_offset * bytes_per_pixel;

        OotPsp_MemcpyVfpu(dst_row, src_row, src_row_bytes);

        if (use_mirror_s) {
            gfx_copy_psp_mirrored_row(dst_base + (size_t)(pot_width - width) * bytes_per_pixel,
                                      src_row, width, bytes_per_pixel);
        }

        gfx_fill_psp_texture_horizontal_padding(dst_base, content_x_offset, content_width, *upload_width,
                                                bytes_per_pixel);
    }

    if (use_mirror_t) {
        for (uint32_t y = 0; y < height; y++) {
            OotPsp_MemcpyVfpu(psp_texture_stage_buf + (size_t)(pot_height - 1 - y) * dst_row_bytes,
                              psp_texture_stage_buf + (size_t)(source_y_offset + y) * dst_row_bytes,
                              dst_row_bytes);
        }
    }

    gfx_fill_psp_texture_vertical_padding(content_y_offset, content_height, *upload_height, dst_row_bytes);

    return psp_texture_stage_buf;
}

static void gfx_upload_texture_slow(int tile, const uint8_t *buf, uint32_t width, uint32_t height, unsigned int type,
                                    bool mirror_s, bool mirror_t) {
    uint32_t upload_width = width;
    uint32_t upload_height = height;
    bool applied_mirror_s = false;
    bool applied_mirror_t = false;
    const uint8_t *upload_buf = gfx_prepare_psp_texture_for_upload(buf, width, height, type, mirror_s, mirror_t, &upload_width, &upload_height, &applied_mirror_s, &applied_mirror_t);

    rendering_state.textures[tile]->mirror_s = applied_mirror_s;
    rendering_state.textures[tile]->mirror_t = applied_mirror_t;
    rendering_state.textures[tile]->upload_width = upload_width;
    rendering_state.textures[tile]->upload_height = upload_height;

    gfx_rapi->upload_texture(upload_buf, upload_width, upload_height, type);
}

static inline __attribute__((always_inline)) void gfx_upload_texture(int tile, const uint8_t *buf, uint32_t width,
                                                                     uint32_t height, unsigned int type) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    const bool mirror_s = (tileState->cms & G_TX_MIRROR) != 0 && tileState->masks != G_TX_NOMASK;
    const bool mirror_t = (tileState->cmt & G_TX_MIRROR) != 0 && tileState->maskt != G_TX_NOMASK;

    if (__builtin_expect(!mirror_s && !mirror_t && gfx_is_power_of_two(width) && gfx_is_power_of_two(height), 1)) {
        rendering_state.textures[tile]->mirror_s = false;
        rendering_state.textures[tile]->mirror_t = false;
        rendering_state.textures[tile]->upload_width = width;
        rendering_state.textures[tile]->upload_height = height;
        gfx_rapi->upload_texture(buf, width, height, type);
        return;
    }

    gfx_upload_texture_slow(tile, buf, width, height, type, mirror_s, mirror_t);
}
#else
static inline void gfx_upload_texture(int tile, const uint8_t *buf, uint32_t width, uint32_t height,
                                      unsigned int type) {
    _UNUSED(tile);
    gfx_rapi->upload_texture(buf, width, height, type);
}
#endif

#if defined(TARGET_PSP)
#include <pspthreadman.h>
__attribute__((unused))
static unsigned long get_time(void) {
    return sceKernelGetSystemTimeWide();
}
#else
#include <time.h>
__attribute__((unused))
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif


//******************* Clipping things

// Bits for clipping
// +-+-+-
// xxyyzz
#define Z_NEG  (0x01)
#define Z_POS  (0x02)
#define Y_NEG  (0x04)
#define Y_POS  (0x08)
#define X_NEG  (0x10)
#define X_POS  (0x20)

// Test all but Z_NEG (for No Near Plane microcodes)
#define CLIP_TEST_FLAGS ( X_POS | X_NEG | Y_POS | Y_NEG | Z_POS | Z_NEG)
//#define CLIP_TEST_FLAGS ( Z_POS | Z_NEG ) /* Faster but worse */

static inline float vec3_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2]);
}

static inline float vec4_dot(const float *lhs, const float *rhs){
#if defined(TARGET_PSP)
    float out;

    __asm__ volatile(
        ".set push\n"
        ".set noreorder\n"
        "lv.q C000, 0(%[lhs])\n"
        "lv.q C010, 0(%[rhs])\n"
        "vdot.q S020, C000, C010\n"
        "sv.s S020, 0(%[out])\n"
        ".set pop\n"
        :
        : [lhs] "r"(lhs), [rhs] "r"(rhs), [out] "r"(&out)
        : "memory");

    return out;
#else
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2])+ (lhs[3]*rhs[3]);
#endif
}

void gfx_clip_interpolate_vert(struct LoadedVertex* out, const struct  LoadedVertex* lhs, const struct LoadedVertex* rhs, const float factor )
{
    // modelview-space position emitted to the PSP for projection
    out->x = lhs->x + (rhs->x - lhs->x) * factor;
    out->y = lhs->y + (rhs->y - lhs->y) * factor;
    out->z = lhs->z + (rhs->z - lhs->z) * factor;
    // projected w retained for combiner LOD behavior
    out->w = lhs->w + (rhs->w - lhs->w) * factor;
    // clip-space position used by the frustum clipper
    out->_x = lhs->_x + (rhs->_x - lhs->_x) * factor;
    out->_y = lhs->_y + (rhs->_y - lhs->_y) * factor;
    out->_z = lhs->_z + (rhs->_z - lhs->_z) * factor;
    out->_w = lhs->_w + (rhs->_w - lhs->_w) * factor;
    // color
    out->color.r = lhs->color.r + (rhs->color.r - lhs->color.r) * factor;
    out->color.g = lhs->color.g + (rhs->color.g - lhs->color.g) * factor;
    out->color.b = lhs->color.b + (rhs->color.b - lhs->color.b) * factor;
    out->color.a = lhs->color.a + (rhs->color.a - lhs->color.a) * factor;
    // texture
    out->u = lhs->u + (rhs->u - lhs->u) * factor;
    out->v = lhs->v + (rhs->v - lhs->v) * factor;
}

//*****************************************************************************
//
//	The following clipping code was taken from The Irrlicht Engine.
//	See http://irrlicht.sourceforge.net/ for more information.
//	Copyright (C) 2002-2006 Nikolaus Gebhardt/Alten Thomas
//
//*****************************************************************************
static const float NDCPlane[6][4] __attribute__((aligned(16))) =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -1.f },	// left
	{ -1.f,  0.f,  0.f, -1.f },	// right
	{  0.f,  1.f,  0.f, -1.f },	// bottom
	{  0.f, -1.f,  0.f, -1.f },	// top
	{  0.f,  0.f, -1.f, -1.f }	// far
};

static uint32_t clipToHyperPlane( struct LoadedVertex *dest, const struct LoadedVertex *source, uint32_t inCount, const float plane[4] )
{
#if defined(TARGET_PSP)
	if (inCount == 0) {
		return 0;
	}

	return gfx_clip_to_hyperplane_vfpu(dest, source, plane, inCount);
#else
	uint32_t outCount;
	struct LoadedVertex *out;

	const struct LoadedVertex *a;
	const struct LoadedVertex *b;

	float aDotPlane;
	float bDotPlane;

	out = dest;
	outCount = 0;
	b = source;
	bDotPlane = vec4_dot(&b->_x, plane);
    size_t i;

#define EPSILON 0.00000001
	for(i = 1; i < inCount + 1; ++i)
	{
		a = &source[i%inCount];
		aDotPlane = vec4_dot(&a->_x, plane);

		// current point inside
		if ( aDotPlane <= EPSILON )
		{
			// last point outside
			if ( bDotPlane > EPSILON )
			{
				// intersect line segment with plane
                const float dot_projected = bDotPlane - aDotPlane;
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );
				out += 1;
				outCount += 1;
			}
			// copy current to out
			*out = *a;
			b = out;

			out += 1;
			outCount += 1;
		}
		else
		{
			// current point outside

			if ( bDotPlane <= EPSILON )
			{
				// previous was inside
				// intersect line segment with plane
                const float dot_projected = bDotPlane - aDotPlane;
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );

				out += 1;
				outCount += 1;
			}
			b = a;
		}

        bDotPlane = aDotPlane;
	}

	return outCount;
#endif
}

uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn )
{
	uint32_t vOut;

	vOut = vIn;

	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[2] );		// right
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[1] );		// left
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[4] );		// top
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[3] );		// bottom
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[0] );		// near
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[5] );		// far

	return vOut;
}

static struct LoadedVertex temp_a[12];
static struct LoadedVertex temp_b[12];
/* The display-list renderer is single-threaded and the clipper already uses
 * the static temp_a/temp_b pair. Keep retessellation scratch beside it so the
 * common, unclipped triangle path does not reserve over 1 KiB of stack. */
static struct LoadedVertex sClippedVertices[21] __attribute__((aligned(16)));
static struct LoadedVertex* sClippedVertexPtrs[21];

void gfx_clip_single_vert( struct LoadedVertex *p_p_vertices, size_t *p_num_vertices, struct LoadedVertex *v_arr[3])
{
	//
	//	At this point all vertices are lit/projected and have both transformed and projected
	//	vertex positions. For the best results we clip against the projected vertex positions,
	//	but use the resulting intersections to interpolate the transformed positions. 
	//	The clipping is more efficient in normalised device coordinates, but rendering these
	//	directly prevents the PSP performing perspective correction. We could invert the projection
	//	matrix and use this to back-project the clip planes into world coordinates, but this
	//	suffers from various precision issues. Carrying around both sets of coordinates gives
	//	us the best of both worlds :)
	//
    size_t clipped_vertices_num = 0;

    temp_a[ 0 ] = *v_arr[ 0 ];
    temp_a[ 1 ] = *v_arr[ 1 ];
    temp_a[ 2 ] = *v_arr[ 2 ];

    uint32_t out = clip_to_frustum( temp_a, temp_b, 3 );
    if( out < 3 ){
        *p_num_vertices = 0;
        return;
    }

    // Retesselate
    for( uint32_t j = 0; j <= out - 3; ++j )
    {            
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ 0 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 1 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 2 ] );
    }

	*p_num_vertices = clipped_vertices_num;
}

//******************* End Clipping things


static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        //int num = buf_vbo_num_tris;
        //unsigned long t0 = get_time();
#if defined(TARGET_PSP)
        const bool twoTextureBlend = rendering_state.tri_pipeline.two_texture_blend &&
                                     rendering_state.textures[0] != NULL && rendering_state.textures[1] != NULL;

        if (twoTextureBlend) {
            gfx_rapi->set_use_alpha(true);
        }
#endif
        gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
#if defined(TARGET_PSP)
        if (twoTextureBlend) {
            for (size_t i = 0; i < buf_num_vert; i++) {
                buf_vbo[i].u = buf_vbo_tex1[i].u;
                buf_vbo[i].v = buf_vbo_tex1[i].v;
                buf_vbo[i].color.a = buf_vbo_tex1[i].alpha;
            }

            /*
             * The GU has one texture unit. Emulate the RGB part of:
             *   (TEXEL1 - TEXEL0) * ENV_ALPHA + TEXEL0
             * with two alpha-blended passes. The per-pass alpha values are
             * chosen so PRIM_ALPHA still controls the final surface opacity.
             */
            gfx_rapi->select_texture(1, rendering_state.textures[1]->texture_id);
            gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
            gfx_rapi->set_use_alpha(rendering_state.alpha_blend);
            gfx_rapi->select_texture(0, rendering_state.textures[0]->texture_id);
        }
#endif
        buf_vbo_len = 0;
        buf_num_vert = 0;
        buf_vbo_num_tris = 0;
        //unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static uint8_t gfx_cc_pick_vertex_color_source(const uint8_t components[4], const uint8_t input_mapping[4],
                                               uint8_t input_count) {
    if (input_count == 0) {
        return CC_0;
    }

#if defined(TARGET_PSP)
    /* PSP GU has one primary color; approximate UI foreground/background texture blends with the foreground. */
    if ((components[0] == CC_PRIM) && (components[1] == CC_ENV) && (components[3] == CC_ENV) &&
        ((components[2] == CC_TEXEL0) || (components[2] == CC_TEXEL0A) || (components[2] == CC_TEXEL1))) {
        return CC_PRIM;
    }
#endif

    return input_mapping[input_count - 1];
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    uint8_t shader_input_count[2] = {0};
    struct CCFeatures cc_features;
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
        shader_input_count[i] = next_input_number - SHADER_INPUT_1;
    }
    gfx_cc_get_features(shader_id, &cc_features);
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
#if defined(TARGET_PSP)
    const bool collapse_tex1 = cc_features.used_textures[0] && cc_features.used_textures[1];

    comb->used_textures[0] = cc_features.used_textures[0];
    comb->used_textures[1] = cc_features.used_textures[1] && !collapse_tex1;
    comb->active_texture = cc_features.used_textures[0] ? 0 : (cc_features.used_textures[1] ? 1 : -1);
#else
    comb->used_textures[0] = cc_features.used_textures[0];
    comb->used_textures[1] = cc_features.used_textures[1];
    comb->active_texture =
        (cc_features.used_textures[0] && cc_features.used_textures[1]) ? (cc_features.do_single[1] ? 1 : 0) :
        (cc_features.used_textures[0] ? 0 : (cc_features.used_textures[1] ? 1 : -1));
#endif
    comb->vertex_color_source[0] =
        gfx_cc_pick_vertex_color_source(c[0], shader_input_mapping[0], shader_input_count[0]);
    comb->vertex_color_source[1] =
        gfx_cc_pick_vertex_color_source(c[1], shader_input_mapping[1], shader_input_count[1]);
    comb->texture_blend = cc_features.opt_texture_blend;
    if (cc_features.opt_texture_blend) {
        comb->vertex_color_source[0] = cc_features.opt_texture_blend_shade ? CC_SHADE : CC_ENV;
    }
}

static inline struct RGBA gfx_get_vertex_color(const struct ColorCombiner *comb, bool use_alpha, const struct RGBA *shade_color, float lod_w, bool allow_lod) {
    switch (comb->vertex_color_source[use_alpha ? 1 : 0]) {
        case CC_PRIM:
            return rdp.prim_color;
        case CC_SHADE:
            return *shade_color;
        case CC_ENV:
            return rdp.env_color;
        case CC_LOD:
            if (allow_lod) {
                float distance_frac = (lod_w - 3000.0f) / 3000.0f;
                if (distance_frac < 0.0f) distance_frac = 0.0f;
                if (distance_frac > 1.0f) distance_frac = 1.0f;
                const uint8_t lod = distance_frac * 255.0f;
                return (struct RGBA){lod, lod, lod, lod};
            }
            break;
    }
    return white_color;
}

static inline uint8_t gfx_color_mul_channel(uint8_t lhs, uint8_t rhs) {
    return ((uint16_t)lhs * (uint16_t)rhs + 127) / 255;
}

static inline void gfx_color_mul_env(struct RGBA* color) {
    color->r = gfx_color_mul_channel(color->r, rdp.env_color.r);
    color->g = gfx_color_mul_channel(color->g, rdp.env_color.g);
    color->b = gfx_color_mul_channel(color->b, rdp.env_color.b);
}

static inline void gfx_color_mul_prim(struct RGBA* color) {
    color->r = gfx_color_mul_channel(color->r, rdp.prim_color.r);
    color->g = gfx_color_mul_channel(color->g, rdp.prim_color.g);
    color->b = gfx_color_mul_channel(color->b, rdp.prim_color.b);
}

#if defined(TARGET_PSP)
static inline void gfx_two_texture_blend_pass_alphas(uint8_t surfaceAlpha, uint8_t mixAlpha, uint8_t* baseAlpha,
                                                     uint8_t* overlayAlpha) {
    uint32_t overlay = ((uint32_t)surfaceAlpha * mixAlpha + 127) / 255;
    uint32_t denominator = 255 - overlay;
    uint32_t base = 0;

    if (denominator != 0) {
        base = ((uint32_t)surfaceAlpha * (255 - mixAlpha) + denominator / 2) / denominator;
        if (base > 255) {
            base = 255;
        }
    }

    *baseAlpha = base;
    *overlayAlpha = overlay;
}
#endif

static inline struct RGBA gfx_get_vertex_rgba(const struct ColorCombiner *comb, bool use_alpha,
                                              const struct RGBA *shade_color, float lod_w, bool allow_lod) {
    struct RGBA color = gfx_get_vertex_color(comb, false, shade_color, lod_w, allow_lod);

    if (use_alpha) {
        color.a = gfx_get_vertex_color(comb, true, shade_color, lod_w, allow_lod).a;
    }

    return color;
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    size_t cacheIndex = (cc_id ^ (cc_id >> 12) ^ (cc_id >> 24)) & (COLOR_COMBINER_CACHE_COUNT - 1);
    ColorCombinerCacheEntry* entry = &color_combiner_cache[cacheIndex];

    if (entry->valid && entry->combiner.cc_id == cc_id) {
        return &entry->combiner;
    }

    /* Buffered vertices can still reference this cache slot. Flush before a
     * collision replaces it; unlike the old linear pool this cache cannot
     * overflow when a scene uses more than 64 distinct combine modes. */
    gfx_flush();
    gfx_generate_cc(&entry->combiner, cc_id);
    entry->valid = true;
    return &entry->combiner;
}

extern int gfx_vram_space_available(void);
extern void texman_clear(void);
extern void texman_upload(int width, int height, unsigned int type, const void* buffer);
extern int texman_vram_space_available(unsigned int size);
extern int texman_texture_slot_available(void);

static uint32_t gfx_texture_import_width(int tile);
static uint32_t gfx_texture_import_height(int tile);

#if defined(TARGET_PSP)
static unsigned int gfx_texture_cache_upload_type(uint32_t fmt, uint32_t siz) {
    if (((fmt == G_IM_FMT_RGBA) && (siz == G_IM_SIZ_16b)) || (fmt == G_IM_FMT_CI)) {
        return GU_PSM_5551;
    }

    if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_8b) {
        return GU_PSM_T8;
    }
    if (((fmt == G_IM_FMT_IA) && (siz == G_IM_SIZ_8b)) ||
        ((fmt == G_IM_FMT_I) && (siz == G_IM_SIZ_4b))) {
        return GU_PSM_4444;
    }

    return GU_PSM_8888;
}

static unsigned int gfx_texture_cache_upload_size(uint16_t width, uint16_t height, uint32_t fmt, uint32_t siz,
                                                  bool mirror_s, bool mirror_t) {
    const unsigned int type = gfx_texture_cache_upload_type(fmt, siz);
    const size_t bytes_per_pixel = gfx_gu_texture_bytes_per_pixel(type);
    const uint32_t pot_width = gfx_next_power_of_two(width);
    const uint32_t pot_height = gfx_next_power_of_two(height);
    uint32_t upload_width = pot_width * (mirror_s ? 2 : 1);
    uint32_t upload_height = pot_height * (mirror_t ? 2 : 1);
    size_t upload_size = (size_t)upload_width * upload_height * bytes_per_pixel;

    if ((mirror_s || mirror_t) && upload_size > sizeof(psp_texture_stage_buf)) {
        upload_width = pot_width;
        upload_height = pot_height;
        upload_size = (size_t)upload_width * upload_height * bytes_per_pixel;
    }

    if (upload_size > sizeof(psp_texture_stage_buf)) {
        upload_size = (size_t)width * height * bytes_per_pixel;
    }

    if (upload_size > (size_t)0xFFFFFFFFU) {
        return 0xFFFFFFFFU;
    }

    return (unsigned int)upload_size;
}
#endif

static void gfx_texture_cache_clear(void) {
    gfx_flush();
    texman_clear();
    gfx_texture_cache.pool_pos = 0;
    memset(gfx_texture_cache.pool, 0, sizeof(gfx_texture_cache.pool));
    memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
    memset(rendering_state.textures, 0, sizeof(rendering_state.textures));
    rendering_state.tri_pipeline_dirty = true;
    rendering_state.backend_state_dirty = true;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (uintptr_t)orig_addr;
    struct TextureHashmapNode **node;
#if defined(TARGET_PSP)
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    const uint32_t source_span_size = gfx_texture_source_span_size(tile);
    const uint32_t source_key = OotPsp_GetExternalAssetRangeSerial(orig_addr, source_span_size);
    const bool dynamic_source = (source_key == 0) && (orig_addr != sInvalidTextureBuf) &&
                                (orig_addr != sInvalidPaletteBuf) &&
                                OotPsp_IsRuntimeByteRange(orig_addr, source_span_size);
    const bool uses_palette = fmt == G_IM_FMT_CI;
    const uint32_t palette_size = siz == G_IM_SIZ_4b ? GFX_CI4_TLUT_SIZE_BYTES : GFX_TLUT_SIZE_BYTES;
    const uint8_t* palette_addr = uses_palette ? rdp.palette : NULL;
    const uint32_t palette_key =
        uses_palette ? OotPsp_GetExternalAssetRangeSerial(palette_addr, palette_size) : 0;
    const uint8_t mirror_s = (tileState->cms & G_TX_MIRROR) != 0 && tileState->masks != G_TX_NOMASK;
    const uint8_t mirror_t = (tileState->cmt & G_TX_MIRROR) != 0 && tileState->maskt != G_TX_NOMASK;

    hash ^= (size_t)source_key * 2654435761U;
    hash ^= (size_t)palette_addr >> 4;
    hash ^= (size_t)palette_key * 2246822519U;
#else
    const bool dynamic_source = false;
#endif
    hash = (hash >> 5) & 0x3ff;
    node = &gfx_texture_cache.hashmap[hash];
    const uint16_t width = gfx_texture_import_width(tile);
    const uint16_t height = gfx_texture_import_height(tile);

    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz
            && (*node)->width == width && (*node)->height == height
            && (*node)->row_stride_bytes == rdp.loaded_texture[tile].row_stride_bytes
            && (*node)->source_nibble_offset == rdp.loaded_texture[tile].source_nibble_offset
#if defined(TARGET_PSP)
            && (*node)->source_key == source_key && (*node)->palette_addr == palette_addr
            && (*node)->palette_key == palette_key && (*node)->mirror_s == mirror_s && (*node)->mirror_t == mirror_t
#endif
        ) {
            *n = *node;
            gfx_rapi->set_sampler_parameters(tile, (*node)->linear_filter, (*node)->cms, (*node)->cmt,
                                             (*node)->masks, (*node)->maskt);
            /* Dynamic sources are uploaded into the existing cache slot, so
             * select it before texman_upload. Static hits are selected once by
             * pipeline setup below instead of binding twice. */
            if (dynamic_source) {
                gfx_rapi->select_texture(tile, (*node)->texture_id);
            }
            return !dynamic_source;
        }
        node = &(*node)->next;
    }
#if defined(TARGET_PSP)
    if (!texman_vram_space_available(gfx_texture_cache_upload_size(width, height, fmt, siz, mirror_s, mirror_t)) ||
        !texman_texture_slot_available()) {
#else
    if (!gfx_vram_space_available() || !texman_texture_slot_available()) {
#endif
        gfx_texture_cache_clear();
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        gfx_texture_cache_clear();
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    /*@Note: unneeded due to sequential GE flow */
    //gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0, G_TX_NOMASK, G_TX_NOMASK);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->masks = G_TX_NOMASK;
    (*node)->maskt = G_TX_NOMASK;
    (*node)->linear_filter = false;
#if defined(TARGET_PSP)
    (*node)->source_key = source_key;
    (*node)->palette_addr = palette_addr;
    (*node)->palette_key = palette_key;
    (*node)->upload_width = width;
    (*node)->upload_height = height;
    (*node)->mirror_s = mirror_s;
    (*node)->mirror_t = mirror_t;
#endif
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->width = width;
    (*node)->height = height;
    (*node)->row_stride_bytes = rdp.loaded_texture[tile].row_stride_bytes;
    (*node)->source_nibble_offset = rdp.loaded_texture[tile].source_nibble_offset;
    *n = *node;
    return false;
}

static uint32_t gfx_texture_tile_width(int tile) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);

    return (tileState->lrs - tileState->uls + 4) >> G_TEXTURE_IMAGE_FRAC;
}

static uint32_t gfx_texture_tile_height(int tile) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);

    return (tileState->lrt - tileState->ult + 4) >> G_TEXTURE_IMAGE_FRAC;
}

static uint32_t gfx_texture_row_bytes(uint32_t width, uint32_t siz) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return (width + 1) >> 1;
        case G_IM_SIZ_8b:
            return width;
        case G_IM_SIZ_16b:
            return width << 1;
        case G_IM_SIZ_32b:
            return width << 2;
        default:
            return width;
    }
}

#if defined(TARGET_PSP)
static uint32_t gfx_texture_import_max_texels(uint8_t fmt, uint8_t siz) {
    switch (fmt) {
        case G_IM_FMT_RGBA:
            if (siz == G_IM_SIZ_16b) {
                return 4096;
            }
            if (siz == G_IM_SIZ_32b) {
                return 4096 / 4;
            }
            break;
        case G_IM_FMT_IA:
            if (siz == G_IM_SIZ_4b) {
                return 32768 / 4;
            }
            if (siz == G_IM_SIZ_8b) {
                return 16384 / 4;
            }
            if (siz == G_IM_SIZ_16b) {
                return 8192 / 4;
            }
            break;
        case G_IM_FMT_CI:
            if (siz == G_IM_SIZ_4b) {
                return 32768 / 4;
            }
            if (siz == G_IM_SIZ_8b) {
                return 16384 / 4;
            }
            break;
        case G_IM_FMT_I:
            if (siz == G_IM_SIZ_4b) {
                return 32768 / 4;
            }
            if (siz == G_IM_SIZ_8b) {
                return 16384 / 4;
            }
            break;
    }

    return 0;
}

static uint32_t gfx_texture_width_from_row_bytes(uint32_t rowBytes, uint32_t siz) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return rowBytes << 1;
        case G_IM_SIZ_8b:
            return rowBytes;
        case G_IM_SIZ_16b:
            return rowBytes >> 1;
        case G_IM_SIZ_32b:
            return rowBytes >> 2;
        default:
            return rowBytes;
    }
}

static bool gfx_try_loaded_texture_dimensions(int tile, uint32_t* width, uint32_t* height) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    uint32_t rowBytes = tileState->line_size_bytes;
    uint32_t rowTexels;
    uint32_t sizeBytes = rdp.loaded_texture[tile].size_bytes;

    if ((rowBytes == 0) || (sizeBytes < rowBytes)) {
        return false;
    }

    rowTexels = gfx_texture_width_from_row_bytes(rowBytes, tileState->siz);
    if (rowTexels <= rdp.loaded_texture[tile].source_nibble_offset) {
        return false;
    }

    *width = rowTexels - rdp.loaded_texture[tile].source_nibble_offset;
    *height = sizeBytes / rowBytes;
    return *height != 0;
}

static bool gfx_texture_tile_dimensions_fit_import(int tile, uint32_t width, uint32_t height) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    uint32_t maxTexels = gfx_texture_import_max_texels(tileState->fmt, tileState->siz);

    if ((width == 0) || (height == 0) || (maxTexels == 0) || (height > (UINT32_MAX / width))) {
        return false;
    }

    return (width * height) <= maxTexels;
}

static bool gfx_texture_tile_dimensions_fit_source(int tile, uint32_t width, uint32_t height) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    uint32_t textureWidth = width + rdp.loaded_texture[tile].source_nibble_offset;
    uint32_t rowBytes;
    uint32_t spanBytes;

    if ((width == 0) || (height == 0) || (textureWidth < width)) {
        return false;
    }

    rowBytes = gfx_texture_row_bytes(textureWidth, tileState->siz);
    if ((rowBytes == 0) || (height > (UINT32_MAX / rowBytes))) {
        return false;
    }

    spanBytes = rowBytes * height;
    return spanBytes <= gfx_loaded_texture_source_size(tile);
}

static void gfx_texture_import_dimensions(int tile, uint32_t* width, uint32_t* height) {
    uint32_t tileWidth = gfx_texture_tile_width(tile);
    uint32_t tileHeight = gfx_texture_tile_height(tile);

    if (gfx_texture_tile_dimensions_fit_import(tile, tileWidth, tileHeight) &&
        gfx_texture_tile_dimensions_fit_source(tile, tileWidth, tileHeight)) {
        *width = tileWidth;
        *height = tileHeight;
        return;
    }

    if (gfx_try_loaded_texture_dimensions(tile, width, height)) {
        return;
    }

    *width = tileWidth;
    *height = tileHeight;
}
#else
static void gfx_texture_import_dimensions(int tile, uint32_t* width, uint32_t* height) {
    *width = gfx_texture_tile_width(tile);
    *height = gfx_texture_tile_height(tile);
}
#endif

static uint32_t gfx_texture_import_width(int tile) {
    uint32_t width;
    uint32_t height;

    gfx_texture_import_dimensions(tile, &width, &height);
    return width;
}

static uint32_t gfx_texture_import_height(int tile) {
    uint32_t width;
    uint32_t height;

    gfx_texture_import_dimensions(tile, &width, &height);
    return height;
}

#if defined(TARGET_PSP)
static void gfx_validate_texture_tile_dimensions(int tile, const char* context) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t textureWidth = width + rdp.loaded_texture[tile].source_nibble_offset;
    bool widthOverflow = textureWidth < width;
    uint32_t rowBytes = widthOverflow ? UINT32_MAX : gfx_texture_row_bytes(textureWidth, tileState->siz);
    uint32_t spanBytes = (height > 0 && rowBytes <= (UINT32_MAX / height)) ? rowBytes * height : UINT32_MAX;
    bool texelOverflow = height != 0 && width > (UINT32_MAX / height);
    uint32_t texels = texelOverflow ? UINT32_MAX : width * height;
    uint32_t maxTexels = gfx_texture_import_max_texels(tileState->fmt, tileState->siz);

    if ((width == 0) || (height == 0) || widthOverflow || texelOverflow || (maxTexels == 0) ||
        (texels > maxTexels)) {
        gfx_log_bad_texture_source(tile, context, rdp.loaded_texture[tile].addr, spanBytes);
        gfx_set_invalid_loaded_texture(tile);
        gfx_set_invalid_texture_tile(tile);
    }
}
#endif

static uint32_t gfx_texture_byte_offset(uint32_t texel, uint32_t siz) {
    if (siz == G_IM_SIZ_4b) {
        return texel >> 1;
    }

    return gfx_texture_row_bytes(texel, siz);
}

static const uint8_t* gfx_texture_row(int tile, uint32_t y, uint32_t fallbackRowBytes) {
    uint32_t stride = rdp.loaded_texture[tile].row_stride_bytes;

    if (stride == 0) {
        stride = fallbackRowBytes;
    }

    return rdp.loaded_texture[tile].addr + (size_t)y * stride;
}

static inline uint16_t gfx_rgba16_to_gu5551(uint16_t color) {
    return ((color & 0x0001U) << 15) | ((color & 0x003EU) << 9) |
           ((color & 0x07C0U) >> 1) | ((color & 0xF800U) >> 11);
}

static void import_texture_rgba16(int tile) {
    uint16_t rgba16_buf[4096] __attribute__ ((aligned(4)));    
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_16b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint16_t col16 = gfx_read_texture_source_be16(row, 2 * x, &swapState);
            rgba16_buf[i] = gfx_rgba16_to_gu5551(col16);
        }
    }

    gfx_upload_texture(tile, (const uint8_t *)rgba16_buf, width, height, GU_PSM_5551);
}

static void import_texture_rgba32(int tile) {
    uint8_t rgba32_buf[4096] __attribute__ ((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_32b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    if ((swapState.mode == GFX_TEXTURE_SWAP_NONE) &&
        (rdp.loaded_texture[tile].row_stride_bytes == 0 || rdp.loaded_texture[tile].row_stride_bytes == rowBytes)) {
        gfx_upload_texture(tile, rdp.loaded_texture[tile].addr, width, height, GU_PSM_8888);
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < rowBytes; x++) {
            rgba32_buf[y * rowBytes + x] = gfx_read_texture_source_u8(row, x, &swapState);
        }
    }

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static uint8_t gfx_texture_read_4b(int tile, uint32_t x, const uint8_t* row,
                                   GfxTextureSwapState* swapState) {
    uint32_t texel = rdp.loaded_texture[tile].source_nibble_offset + x;
    uint8_t byte = gfx_read_texture_source_u8(row, texel >> 1, swapState);

    return (byte >> (4 - (texel & 1) * 4)) & 0xf;
}

static void import_texture_ia4(int tile) {
    uint8_t rgba32_buf[32768] __attribute__ ((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width + rdp.loaded_texture[tile].source_nibble_offset, G_IM_SIZ_4b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t part = gfx_texture_read_4b(tile, x, row, &swapState);
            uint8_t intensity = part >> 1;
            uint8_t alpha = part & 1;
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            rgba32_buf[4*i + 0] = SCALE_3_8(r);
            rgba32_buf[4*i + 1] = SCALE_3_8(g);
            rgba32_buf[4*i + 2] = SCALE_3_8(b);
            rgba32_buf[4*i + 3] = alpha ? 255 : 0;
        }
    }

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ia8(int tile) {
    uint16_t rgba16_buf[4096] __attribute__((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_8b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t texel = gfx_read_texture_source_u8(row, x, &swapState);
            uint16_t intensity = texel >> 4;
            uint16_t alpha = texel & 0xF;

            rgba16_buf[i] = (alpha << 12) | (intensity << 8) | (intensity << 4) | intensity;
        }
    }

    gfx_upload_texture(tile, (const uint8_t*)rgba16_buf, width, height, GU_PSM_4444);
}

static void import_texture_ia16(int tile) {
    uint8_t rgba32_buf[8192];
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_16b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t intensity = gfx_read_texture_source_u8(row, 2 * x, &swapState);
            uint8_t alpha = gfx_read_texture_source_u8(row, 2 * x + 1, &swapState);
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            rgba32_buf[4*i + 0] = r;
            rgba32_buf[4*i + 1] = g;
            rgba32_buf[4*i + 2] = b;
            rgba32_buf[4*i + 3] = alpha;
        }
    }

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_i4(int tile) {
    uint16_t rgba16_buf[8192] __attribute__((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width + rdp.loaded_texture[tile].source_nibble_offset, G_IM_SIZ_4b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint16_t intensity = gfx_texture_read_4b(tile, x, row, &swapState);

            rgba16_buf[i] = (intensity << 12) | (intensity << 8) | (intensity << 4) | intensity;
        }
    }

    gfx_upload_texture(tile, (const uint8_t*)rgba16_buf, width, height, GU_PSM_4444);
}

static void import_texture_i8(int tile) {
#if defined(TARGET_PSP)
    uint8_t i8_buf[4096] __attribute__ ((aligned(4)));
#else
    uint8_t rgba32_buf[16384];
#endif
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_8b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

#if defined(TARGET_PSP)
    if ((swapState.mode == GFX_TEXTURE_SWAP_NONE) &&
        (rdp.loaded_texture[tile].row_stride_bytes == 0 || rdp.loaded_texture[tile].row_stride_bytes == rowBytes)) {
        gfx_upload_texture(tile, rdp.loaded_texture[tile].addr, width, height, GU_PSM_T8);
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            i8_buf[y * width + x] = gfx_read_texture_source_u8(row, x, &swapState);
        }
    }

    gfx_upload_texture(tile, i8_buf, width, height, GU_PSM_T8);
#else
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t intensity = gfx_read_texture_source_u8(row, x, &swapState);
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            rgba32_buf[4*i + 0] = r;
            rgba32_buf[4*i + 1] = g;
            rgba32_buf[4*i + 2] = b;
            rgba32_buf[4*i + 3] = intensity;
        }
    }

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
#endif
}


static void import_texture_ci4(int tile) {
    uint16_t rgba16_buf[8192] __attribute__((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width + rdp.loaded_texture[tile].source_nibble_offset, G_IM_SIZ_4b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

#if defined(TARGET_PSP)
    gfx_validate_palette_source("import_texture_ci4");
#endif
    GfxTextureSwapState paletteSwapState = gfx_texture_source_swap_state(rdp.palette, GFX_CI4_TLUT_SIZE_BYTES);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t idx = gfx_texture_read_4b(tile, x, row, &swapState);
            uint16_t col16 = gfx_read_texture_source_be16(rdp.palette, idx * 2, &paletteSwapState);
            rgba16_buf[i] = gfx_rgba16_to_gu5551(col16);
        }
    }

    gfx_upload_texture(tile, (const uint8_t*)rgba16_buf, width, height, GU_PSM_5551);
}

static void import_texture_ci8(int tile) {
    uint16_t rgba16_buf[4096] __attribute__((aligned(4)));
    uint32_t width = gfx_texture_import_width(tile);
    uint32_t height = gfx_texture_import_height(tile);
    uint32_t rowBytes = gfx_texture_row_bytes(width, G_IM_SIZ_8b);
    GfxTextureSwapState swapState =
        gfx_texture_source_swap_state(rdp.loaded_texture[tile].addr, gfx_texture_source_span_size(tile));

#if defined(TARGET_PSP)
    gfx_validate_palette_source("import_texture_ci8");
#endif
    GfxTextureSwapState paletteSwapState = gfx_texture_source_swap_state(rdp.palette, GFX_TLUT_SIZE_BYTES);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = gfx_texture_row(tile, y, rowBytes);

        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            uint8_t idx = gfx_read_texture_source_u8(row, x, &swapState);
            uint16_t col16 = gfx_read_texture_source_be16(rdp.palette, idx * 2, &paletteSwapState);
            rgba16_buf[i] = gfx_rgba16_to_gu5551(col16);
        }
    }

    gfx_upload_texture(tile, (const uint8_t*)rgba16_buf, width, height, GU_PSM_5551);
}

static void import_texture(int tile) {
    const TextureTileState* tileState = gfx_get_texture_tile(tile);
    uint8_t fmt = tileState->fmt;
    uint8_t siz = tileState->siz;

#if defined(TARGET_PSP)
    gfx_validate_texture_source(tile, "import_texture");
    gfx_validate_texture_tile_dimensions(tile, "import_texture-dimensions");
#endif
    
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[tile].addr, fmt, siz)) {
        return;
    }
    
    //int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    //int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
}

static inline void gfx_mark_tri_pipeline_dirty(void) {
    rendering_state.tri_pipeline_dirty = true;
}

static inline bool gfx_blend_cycle_uses_framebuffer(uint32_t other_mode_l, uint32_t m2a_shift, uint32_t m2b_shift) {
    uint32_t m2a = (other_mode_l >> m2a_shift) & 3;
    uint32_t m2b = (other_mode_l >> m2b_shift) & 3;

    return (m2a == G_BL_CLR_MEM) && ((m2b == G_BL_1MA) || (m2b == G_BL_1));
}

static void gfx_prepare_texture_coord_state(struct TriPipelineState* state, int coordSlot, int textureSlot,
                                            bool linear_filter) {
    const TextureTileState* tileState = gfx_get_texture_tile(textureSlot);
    const float filter_bias = linear_filter ? 16.0f : 0.0f;

    state->tex_u_nominal_span[coordSlot] = 8.0f * (tileState->lrs - tileState->uls + 4);
    state->tex_v_nominal_span[coordSlot] = 8.0f * (tileState->lrt - tileState->ult + 4);

    if (state->tex_u_nominal_span[coordSlot] == 0.0f || state->tex_v_nominal_span[coordSlot] == 0.0f) {
        return;
    }

    state->tex_u_scale[coordSlot] = 1.0f / state->tex_u_nominal_span[coordSlot];
    state->tex_v_scale[coordSlot] = 1.0f / state->tex_v_nominal_span[coordSlot];
    state->tex_u_bias[coordSlot] = (filter_bias - tileState->uls * 8.0f) * state->tex_u_scale[coordSlot];
    state->tex_v_bias[coordSlot] = (filter_bias - tileState->ult * 8.0f) * state->tex_v_scale[coordSlot];
    state->tex_u_scale_to_primitive[coordSlot] = tileState->masks == G_TX_NOMASK;
    state->tex_v_scale_to_primitive[coordSlot] = tileState->maskt == G_TX_NOMASK;
#if defined(TARGET_PSP)
    {
        const struct TextureHashmapNode* texture_node = rendering_state.textures[textureSlot];
        const uint32_t tile_width = (tileState->lrs - tileState->uls + 4) / 4;
        const uint32_t tile_height = (tileState->lrt - tileState->ult + 4) / 4;

        if (texture_node != NULL && tile_width != 0 && tile_height != 0) {
            if (texture_node->upload_width != 0) {
                const float upload_width = texture_node->upload_width;
                const float u_factor = (float)tile_width / upload_width;

                state->tex_u_scale[coordSlot] *= u_factor;
                state->tex_u_bias[coordSlot] *= u_factor;
            }

            if (texture_node->upload_height != 0) {
                const float upload_height = texture_node->upload_height;
                const float v_factor = (float)tile_height / upload_height;

                state->tex_v_scale[coordSlot] *= v_factor;
                state->tex_v_bias[coordSlot] *= v_factor;
            }

            if (texture_node->mirror_s) {
                state->tex_u_bias[coordSlot] += 0.5f;
            }

            if (texture_node->mirror_t) {
                state->tex_v_bias[coordSlot] += 0.5f;
            }
        }
    }
#endif
}

static void gfx_prepare_tri_pipeline_state(void) {
    if (!rendering_state.tri_pipeline_dirty) {
        return;
    }

    const bool backend_state_dirty = rendering_state.backend_state_dirty;
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (backend_state_dirty || depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (backend_state_dirty || z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }

    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (backend_state_dirty || zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }

    if (backend_state_dirty || rdp.viewport_or_scissor_changed) {
        if (backend_state_dirty || memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (backend_state_dirty || memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint32_t cc_id = rdp.combine_mode;

    uint32_t alpha_compare = rdp.other_mode_l & (3 << G_MDSFT_ALPHACOMPARE);
    bool alpha_blend = (rdp.other_mode_l & FORCE_BL) &&
                       (gfx_blend_cycle_uses_framebuffer(rdp.other_mode_l, 22, 18) ||
                        gfx_blend_cycle_uses_framebuffer(rdp.other_mode_l, 20, 16));
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = alpha_compare == G_AC_DITHER;
    bool use_alpha = alpha_blend || texture_edge || (alpha_compare != G_AC_NONE);

    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;

    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }

    if (backend_state_dirty || !rendering_state.color_combiner_valid || rendering_state.color_combiner_id != cc_id) {
        rendering_state.color_combiner = gfx_lookup_or_create_color_combiner(cc_id);
        rendering_state.color_combiner_id = cc_id;
        rendering_state.color_combiner_valid = true;
    }

    struct ColorCombiner *comb = rendering_state.color_combiner;
    struct ShaderProgram *prg = comb->prg;
    if (backend_state_dirty || prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(backend_state_dirty ? NULL : rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (backend_state_dirty || alpha_blend != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(alpha_blend);
        rendering_state.alpha_blend = alpha_blend;
    }
    if (comb->texture_blend) {
        gfx_flush();
        gfx_rapi->set_texture_env_color(rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a);
    }

    const bool used_textures[2] = {
        comb->used_textures[0],
        comb->used_textures[1] || rdp.combine_two_texture_blend,
    };
    const bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    const int active_texture = comb->active_texture;

    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            const TextureTileState* tileState = gfx_get_texture_tile(i);

            if (rendering_state.textures[i] == NULL) {
                rdp.textures_changed[i] = true;
            }

            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            if (backend_state_dirty ||
                linear_filter != rendering_state.textures[i]->linear_filter ||
                tileState->cms != rendering_state.textures[i]->cms ||
                tileState->cmt != rendering_state.textures[i]->cmt ||
                tileState->masks != rendering_state.textures[i]->masks ||
                tileState->maskt != rendering_state.textures[i]->maskt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, tileState->cms, tileState->cmt,
                                                 tileState->masks, tileState->maskt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = tileState->cms;
                rendering_state.textures[i]->cmt = tileState->cmt;
                rendering_state.textures[i]->masks = tileState->masks;
                rendering_state.textures[i]->maskt = tileState->maskt;
            }
        }
    }

    if ((active_texture >= 0) && (rendering_state.textures[active_texture] != NULL)) {
        const TextureTileState* tileState = gfx_get_texture_tile(active_texture);

        gfx_flush();
        gfx_rapi->set_sampler_parameters(active_texture, linear_filter, tileState->cms, tileState->cmt,
                                         tileState->masks, tileState->maskt);
        gfx_rapi->select_texture(active_texture, rendering_state.textures[active_texture]->texture_id);
    }

    struct TriPipelineState *state = &rendering_state.tri_pipeline;
    state->comb = comb;
    state->use_alpha = use_alpha;
    state->used_textures[0] = used_textures[0];
    state->used_textures[1] = used_textures[1];
    state->use_texture = used_textures[0] || used_textures[1];
    state->two_texture_blend = rdp.combine_two_texture_blend;
    state->color_mul_env = rdp.combine_color_mul_env;
    state->color_mul_prim = rdp.combine_color_mul_prim;
    for (int i = 0; i < 2; i++) {
        state->tex_u_scale[i] = 0.0f;
        state->tex_v_scale[i] = 0.0f;
        state->tex_u_bias[i] = 0.0f;
        state->tex_v_bias[i] = 0.0f;
        state->tex_u_nominal_span[i] = 0.0f;
        state->tex_v_nominal_span[i] = 0.0f;
        state->tex_u_scale_to_primitive[i] = false;
        state->tex_v_scale_to_primitive[i] = false;
    }
    if (state->use_texture) {
        int base_texture = state->two_texture_blend ? 0 : active_texture;

        if (base_texture < 0) {
            base_texture = used_textures[0] ? 0 : 1;
        }

        gfx_prepare_texture_coord_state(state, 0, base_texture, linear_filter);
        if (state->two_texture_blend) {
            gfx_prepare_texture_coord_state(state, 1, 1, linear_filter);
        }
    }

    rendering_state.tri_pipeline_dirty = false;
    rendering_state.backend_state_dirty = false;
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

static uint8_t gfx_clamp_num_lights(uint32_t num_lights) {
    const uint32_t max_lights_with_ambient = MAX_LIGHTS + 1;

    return (num_lights > max_lights_with_ambient) ? max_lights_with_ambient : num_lights;
}

static void gfx_normalize_vector(float v[3]) {
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if(dot > 0.00001f){
        const float scale = 1.0f / pspFpuSqrt(dot);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    __asm__ volatile(
        "lv.q C000, 0(%[b])\n"
        "lv.q C010, 16(%[b])\n"
        "lv.q C020, 32(%[b])\n"
        "lv.q C030, 48(%[b])\n"
        "lv.q C100, 0(%[a])\n"
        "lv.q C110, 16(%[a])\n"
        "lv.q C120, 32(%[a])\n"
        "lv.q C130, 48(%[a])\n"
        "vscl.q C200, C000, S100\n"
        "vscl.q C210, C010, S101\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C020, S102\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C030, S103\n"
        "vadd.q C200, C200, C210\n"
        "sv.q C200, 0(%[res])\n"
        "vscl.q C200, C000, S110\n"
        "vscl.q C210, C010, S111\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C020, S112\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C030, S113\n"
        "vadd.q C200, C200, C210\n"
        "sv.q C200, 16(%[res])\n"
        "vscl.q C200, C000, S120\n"
        "vscl.q C210, C010, S121\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C020, S122\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C030, S123\n"
        "vadd.q C200, C200, C210\n"
        "sv.q C200, 32(%[res])\n"
        "vscl.q C200, C000, S130\n"
        "vscl.q C210, C010, S131\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C020, S132\n"
        "vadd.q C200, C200, C210\n"
        "vscl.q C210, C030, S133\n"
        "vadd.q C200, C200, C210\n"
        "sv.q C200, 48(%[res])\n"
        :
        : [res] "r"(res), [a] "r"(a), [b] "r"(b)
        : "memory");
}

static inline void gfx_transform_vec4(float out[4], const float matrix[4][4], const float in[4]) {
    out[0] = (in[0] * matrix[0][0]) + (in[1] * matrix[1][0]) + (in[2] * matrix[2][0]) +
             (in[3] * matrix[3][0]);
    out[1] = (in[0] * matrix[0][1]) + (in[1] * matrix[1][1]) + (in[2] * matrix[2][1]) +
             (in[3] * matrix[3][1]);
    out[2] = (in[0] * matrix[0][2]) + (in[1] * matrix[1][2]) + (in[2] * matrix[2][2]) +
             (in[3] * matrix[3][2]);
    out[3] = (in[0] * matrix[0][3]) + (in[1] * matrix[1][3]) + (in[2] * matrix[2][3]) +
             (in[3] * matrix[3][3]);
}

static inline void gfx_upload_projection_matrix(const float matrix[4][4]) {
    if (sUploadedProjectionValid && memcmp(sUploadedProjection, matrix, sizeof(sUploadedProjection)) == 0) {
        return;
    }

    sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4*)matrix);
    memcpy(sUploadedProjection, matrix, sizeof(sUploadedProjection));
    sUploadedProjectionValid = true;
}

static bool gfx_hud_anchor_enabled(void);

static inline __attribute__((always_inline)) void gfx_apply_projection_matrix(void) {
    float projection[4][4];

    if (!gfx_hud_anchor_enabled() || !sHudViewportFullscreen) {
        gfx_upload_projection_matrix(rsp.P_matrix);
        return;
    }

    memcpy(projection, rsp.P_matrix, sizeof(projection));
    {
        for (size_t i = 0; i < 4; i++) {
            projection[i][0] = (projection[i][0] * sNdcAspectScale) +
                               (projection[i][3] * sHudAnchorOffsetNdc);
        }
    }

    gfx_upload_projection_matrix(projection);
}

static void gfx_apply_modelview_matrix(void) {
    /*
     * F3DEX2 transforms vertices when SPVertex runs. Link's flexible limb display
     * lists rely on that: they load a few vertices under one segment-D matrix,
     * switch matrices, then emit triangles using both batches. Keep the PSP GU
     * model matrix at identity and store modelview-transformed positions per
     * loaded vertex so later matrix commands cannot retarget old vertices. The
     * rendering backend restores the identity model matrix once per frame.
     */
    rsp.lights_changed = true;
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4] __attribute__((aligned(16)));
#if defined(TARGET_PSP)
    const void* normalizedSource;

    if (!gfx_normalize_read_source(addr, sizeof(Mtx), "matrix", &normalizedSource)) {
        return;
    }
    addr = (const int32_t*)normalizedSource;
#endif
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (parameters & G_MTX_PROJECTION) {
        /*
         * Pending vertices still use the projection currently installed in the
         * backend. Submit them before replacing it. Model-view changes do not
         * need this: SPVertex has already transformed and lit those vertices.
         */
        gfx_flush();
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
        gfx_apply_projection_matrix();
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < MODELVIEW_STACK_SIZE) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        gfx_apply_modelview_matrix();
    }
}

static void gfx_sp_pop_matrix(uint32_t count) {
    if (count == 0) {
        return;
    }

    while (count--) {
        if (rsp.modelview_matrix_stack_size > 1) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    gfx_apply_modelview_matrix();
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * sNdcAspectScale;
}

static float gfx_hud_anchor_direction(void) {
    if (sHudAnchor == OOT_PSP_HUD_ANCHOR_LEFT) {
        return -1.0f;
    }
    if (sHudAnchor == OOT_PSP_HUD_ANCHOR_RIGHT) {
        return 1.0f;
    }
    return 0.0f;
}

static bool gfx_hud_anchor_enabled(void) {
    return sHudAnchor != OOT_PSP_HUD_ANCHOR_NONE;
}

static void gfx_update_screen_metrics(void) {
    const float width = (float)gfx_current_dimensions.width;
    const float height = gfx_current_dimensions.height != 0 ? (float)gfx_current_dimensions.height : 1.0f;
    float margin = (width - (height * (4.0f / 3.0f))) * 0.5f;

    gfx_current_dimensions.aspect_ratio = width / height;
    sNdcAspectScale = width != 0.0f ? (height * (4.0f / 3.0f)) / width : 1.0f;

    if (margin < 0.0f) {
        margin = 0.0f;
    }
    sWidescreenMarginPixels = margin;
    sHudAnchorOffsetPixels = gfx_hud_anchor_direction() * sWidescreenMarginPixels;
    sHudAnchorOffsetNdc = width != 0.0f ? 2.0f * sHudAnchorOffsetPixels / width : 0.0f;
}

static float gfx_widescreen_margin_pixels(void) {
    return sWidescreenMarginPixels;
}

static float gfx_hud_anchor_offset_pixels(void) {
    return sHudAnchorOffsetPixels;
}

static void gfx_apply_unmasked_texture_axis(const struct LoadedVertex *const vertices[], size_t n_vertices,
                                            bool use_u, float nominal_span, float *scale, float *bias) {
    float min_coord;
    float max_coord;
    float span;

    if (n_vertices == 0 || nominal_span <= 0.0f) {
        return;
    }

    min_coord = use_u ? vertices[0]->u : vertices[0]->v;
    max_coord = min_coord;

    for (size_t i = 1; i < n_vertices; i++) {
        const float coord = use_u ? vertices[i]->u : vertices[i]->v;

        if (coord < min_coord) {
            min_coord = coord;
        }
        if (coord > max_coord) {
            max_coord = coord;
        }
    }

    span = max_coord - min_coord;
    if (span <= nominal_span + 1.0f) {
        return;
    }

    /* Unmasked axes can use oversize UVs to stretch one tile, not repeat it. */
    *scale = 1.0f / (span + 1.0f);
    *bias = -min_coord * *scale;
}

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

static bool gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
#if !defined(TARGET_PSP)
    float temp_vec[4] __attribute__((aligned(16)));
    float model_vec[4] __attribute__((aligned(16)));
    float proj_vec[4] __attribute__((aligned(16)));
#endif
    const float hudAnchorOffsetNdc = sHudViewportFullscreen ? sHudAnchorOffsetNdc : 0.0f;
    const uint32_t geometryMode = rsp.geometry_mode;
    const uint8_t directionalLightCount = rsp.current_num_lights - 1;
#if defined(TARGET_PSP)
    const void* normalizedSource;

    if (n_vertices == 0) {
        return true;
    }

    if ((dest_index >= MAX_VERTICES) || (n_vertices > (MAX_VERTICES - dest_index))) {
        gfx_log_bad_data_source("vertex-count", vertices, n_vertices * sizeof(Vtx));
        return false;
    }

    if (!gfx_normalize_read_source(vertices, n_vertices * sizeof(Vtx), "vertex", &normalizedSource)) {
        return false;
    }
    vertices = (const Vtx*)normalizedSource;

    {
        const struct GfxVfpuTransformMatrices matrices = {
            rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
            rsp.P_matrix,
        };

        gfx_transform_vertices_vfpu(&rsp.loaded_vertices[dest_index], vertices, n_vertices, &matrices);
    }
#endif
    if ((geometryMode & G_LIGHTING) && rsp.lights_changed) {
        static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
        static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};

        for (int i = 0; i < directionalLightCount; i++) {
            calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
        }
        calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
        calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
        rsp.lights_changed = false;
    }

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];

#if defined(TARGET_PSP)
        float w = d->_w;
        float x = gfx_adjust_x_for_aspect_ratio(d->_x);
        const float y = d->_y;
        const float z = d->_z;
#else
        temp_vec[0] = v->ob[0];
        temp_vec[1] = v->ob[1];
        temp_vec[2] = v->ob[2];
        temp_vec[3] = 1.0f;

        gfx_transform_vec4(model_vec, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], temp_vec);
        gfx_transform_vec4(proj_vec, rsp.P_matrix, model_vec);

        float w = proj_vec[3];
        float x = gfx_adjust_x_for_aspect_ratio(proj_vec[0]);
        const float y = proj_vec[1];
        const float z = proj_vec[2];
#endif

        if (hudAnchorOffsetNdc != 0.0f) {
            x += hudAnchorOffsetNdc * w;
        }

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (geometryMode & G_LIGHTING) {
            unsigned int r = rsp.current_lights[directionalLightCount].col[0];
            unsigned int g = rsp.current_lights[directionalLightCount].col[1];
            unsigned int b = rsp.current_lights[directionalLightCount].col[2];
            
            for (int i = 0; i < directionalLightCount; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity *= (1.0f / 127.0f);
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;
            
            if (geometryMode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx * (1.0f / 127.0f) + 1.0f) * 0.25f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty * (1.0f / 127.0f) + 1.0f) * 0.25f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= X_POS;
        if (x > w) d->clip_rej |= X_NEG;
        if (y < -w) d->clip_rej |= Y_POS;
        if (y > w) d->clip_rej |= Y_NEG;
        if (z < -w) d->clip_rej |= Z_POS;
        if (z > w) d->clip_rej |= Z_NEG;

#if !defined(TARGET_PSP)
        d->x = model_vec[0];
        d->y = model_vec[1];
        d->z = model_vec[2];
#endif
        d->w = w;

        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;

        /*@Note: this is a trainwreck*/
        /*if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
            //d->color.r = d->color.r + (rdp.fog_color.r - d->color.r) * (fog_z/255);
            //d->color.g = d->color.g + (rdp.fog_color.g - d->color.g) * (fog_z/255);
            //d->color.b = d->color.b + (rdp.fog_color.b - d->color.b) * (fog_z/255);
            
            d->color.r = d->color.r + (255 - d->color.r) * (fog_z/255);
            d->color.g = d->color.g + (0 - d->color.g) * (fog_z/255);
            d->color.b = d->color.b + (0 - d->color.b) * (fog_z/255);
            //d->color.r = 255-fog_z;
            //d->color.g = 255-fog_z;
            //d->color.b = 255-fog_z;
            d->color.a = 255;
        } else {
            d->color.a = v->cn[3];
        }*/
        d->color.a = v->cn[3];
    }

    return true;
}

#if defined(TARGET_PSP) && defined(F3DEX_GBI_2)
static inline void *seg_addr(uintptr_t w1);

static bool gfx_decode_vertex_cmd_f3dex2(uint32_t w0, uint32_t* n, uint32_t* destIndex) {
    const uint32_t count = (w0 >> 12) & 0xFF;
    const uint32_t end = (w0 >> 1) & 0x7F;

    if (((w0 & 0x00F00F01U) != 0) || (count == 0) || (count > MAX_VERTICES) || (end > MAX_VERTICES) ||
        (end < count)) {
        return false;
    }

    *n = count;
    *destIndex = end - count;
    return true;
}

static bool gfx_sp_vertex_f3dex2(uint32_t w0, uintptr_t rawAddr) {
    uint32_t n;
    uint32_t destIndex;

    if (!gfx_decode_vertex_cmd_f3dex2(w0, &n, &destIndex)) {
        n = (w0 >> 12) & 0xFF;
        gfx_log_bad_data_source("vertex-cmd", (const void*)rawAddr, n * sizeof(Vtx));
        return false;
    }

    return gfx_sp_vertex(n, destIndex, seg_addr(rawAddr));
}
#endif

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};
    const uint32_t clip_flags = v1->clip_rej | v2->clip_rej | v3->clip_rej;

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        return;
    }
    const uint32_t cull_mode = rsp.geometry_mode & G_CULL_BOTH;
#if defined(TARGET_PSP)
    /* Fully visible vertices have positive W, so the sign of this homogeneous
     * cross product matches the perspective-divided screen-space triangle.
     * Reject backfaces before color/UV generation without three divides. Keep
     * partially clipped triangles on the conservative path below. */
    if ((cull_mode != 0) && (clip_flags == 0)) {
        const float dx1 = v1->_x * v2->_w - v2->_x * v1->_w;
        const float dy1 = v1->_y * v2->_w - v2->_y * v1->_w;
        const float dx2 = v3->_x * v2->_w - v2->_x * v3->_w;
        const float dy2 = v3->_y * v2->_w - v2->_y * v3->_w;
        const float cross = dx1 * dy2 - dy1 * dx2;

        if (((cull_mode == G_CULL_FRONT) && (cross <= 0.0f)) ||
            ((cull_mode == G_CULL_BACK) && (cross >= 0.0f)) ||
            (cull_mode == G_CULL_BOTH)) {
            return;
        }
    }
#else
    if (cull_mode != 0) {
        const float inv_w1 = 1.0f / v1->_w;
        const float inv_w2 = 1.0f / v2->_w;
        const float inv_w3 = 1.0f / v3->_w;
        float dx1 = v1->_x * inv_w1 - v2->_x * inv_w2;
        float dy1 = v1->_y * inv_w1 - v2->_y * inv_w2;
        float dx2 = v3->_x * inv_w3 - v2->_x * inv_w2;
        float dy2 = v3->_y * inv_w3 - v2->_y * inv_w2;
        float cross = dx1 * dy2 - dy1 * dx2;
        
        if ((v1->_w < 0) ^ (v2->_w < 0) ^ (v3->_w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        switch (cull_mode) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }
#endif

    /* Setup to clip but if we dont, we preload correct values and fix up pointers; */
    struct LoadedVertex **clipped_vertices = v_arr;
    size_t clipped_vertices_num = 3;
    /* A triangle clipped against six planes can become a nine-vertex polygon. */
    if (clip_flags & CLIP_TEST_FLAGS) {
        gfx_clip_single_vert(sClippedVertices, &clipped_vertices_num, v_arr);

        if (!clipped_vertices_num) {
            /* No idea if this is possible */
            return;
        }
        size_t i;
        for (i = 0; i < clipped_vertices_num; i++) {
            sClippedVertexPtrs[i] = &sClippedVertices[i];
        }
        clipped_vertices = sClippedVertexPtrs;
    }

    gfx_prepare_tri_pipeline_state();
    const struct TriPipelineState *state = &rendering_state.tri_pipeline;
    struct ColorCombiner *comb = state->comb;
    const bool use_alpha = state->use_alpha;
    const bool use_texture = state->use_texture;
    float tex_u_scale[2] = { state->tex_u_scale[0], state->tex_u_scale[1] };
    float tex_v_scale[2] = { state->tex_v_scale[0], state->tex_v_scale[1] };
    float tex_u_bias[2] = { state->tex_u_bias[0], state->tex_u_bias[1] };
    float tex_v_bias[2] = { state->tex_v_bias[0], state->tex_v_bias[1] };
    const uint32_t shader_program_id = rendering_state.shader_program->shader_id;

    if (use_texture) {
        const struct LoadedVertex *uv_vertices[3] = {v1, v2, v3};
        const int coord_count = state->two_texture_blend ? 2 : 1;

        for (int coord = 0; coord < coord_count; coord++) {
            if (state->tex_u_scale_to_primitive[coord]) {
                gfx_apply_unmasked_texture_axis(uv_vertices, 3, true, state->tex_u_nominal_span[coord],
                                                &tex_u_scale[coord], &tex_u_bias[coord]);
            }
            if (state->tex_v_scale_to_primitive[coord]) {
                gfx_apply_unmasked_texture_axis(uv_vertices, 3, false, state->tex_v_nominal_span[coord],
                                                &tex_v_scale[coord], &tex_v_bias[coord]);
            }
        }
    }
    
    const size_t new_tri_count = clipped_vertices_num / 3;

    if (new_tri_count == 0) {
        return;
    }

#if defined(TARGET_PSP)
    if ((buf_num_vert + clipped_vertices_num) > (sizeof(buf_vbo) / sizeof(buf_vbo[0]))) {
        gfx_flush();
    }
#endif

    if ((buf_vbo_num_tris + new_tri_count) > MAX_BUFFERED) {
        gfx_flush();
    }

    size_t i;
    for (i = 0; i < clipped_vertices_num; i++) {
        const struct LoadedVertex *vertex = clipped_vertices[i];
        psp_fast_t *out = &buf_vbo[buf_num_vert];

        out->x = vertex->x;
        out->y = vertex->y;
        out->z = vertex->z;
        
        if (use_texture) {
            out->u = vertex->u * tex_u_scale[0] + tex_u_bias[0];
            out->v = vertex->v * tex_v_scale[0] + tex_v_bias[0];
            if (state->two_texture_blend) {
                buf_vbo_tex1[buf_num_vert].u = vertex->u * tex_u_scale[1] + tex_u_bias[1];
                buf_vbo_tex1[buf_num_vert].v = vertex->v * tex_v_scale[1] + tex_v_bias[1];
            }
        } else {
            out->u = 0.0f;
            out->v = 0.0f;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = clipped_vertices[i].color.a / 255.0f; // fog factor (not alpha)
        }
        */
        out->color = gfx_get_vertex_rgba(comb, use_alpha, &vertex->color, vertex->w, true);
        if (state->color_mul_env) {
            gfx_color_mul_env(&out->color);
        }
        if (state->color_mul_prim) {
            gfx_color_mul_prim(&out->color);
        }

        /*@Note: Blue Star color */
        if (shader_program_id == 0x01200200) {
            out->color = clipped_vertices[0]->color;
            if (rdp.env_color.a != 255) {
                out->color.a = rdp.env_color.a;
            }
        }
        if (state->two_texture_blend) {
            uint8_t baseAlpha;
            uint8_t overlayAlpha;

            gfx_two_texture_blend_pass_alphas(out->color.a, rdp.env_color.a, &baseAlpha, &overlayAlpha);
            out->color.a = baseAlpha;
            buf_vbo_tex1[buf_num_vert].alpha = overlayAlpha;
        }
        if (shader_program_id == 0x01A00045) {
            /* Matches the old code, which only updated the temporary pointer after the copy. */
        }
        buf_num_vert++;
        buf_vbo_len += sizeof(psp_fast_t);
    }
    buf_vbo_num_tris += new_tri_count;
    if (buf_vbo_num_tris >= MAX_BUFFERED) {
        gfx_flush();
    }
}

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};

    gfx_prepare_tri_pipeline_state();
    const struct TriPipelineState *state = &rendering_state.tri_pipeline;
    struct ColorCombiner *comb = state->comb;
    const bool use_alpha = state->use_alpha;
    const bool use_texture = state->use_texture;
    const int texture_tile = state->two_texture_blend ? 0 : (comb->active_texture >= 0 ? comb->active_texture : 0);
    const TextureTileState* tileState = gfx_get_texture_tile(texture_tile);
    //uint32_t tex_width = (tileState->lrs - tileState->uls + 4) / 4;
    //uint32_t tex_height = (tileState->lrt - tileState->ult + 4) / 4;

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
            int32_t u = (v_arr[i]->u - tileState->uls * 8) / 32;
            int32_t v = (v_arr[i]->v - tileState->ult * 8) / 32;
#if defined(TARGET_PSP)
            const int active_texture = comb->active_texture;
            const struct TextureHashmapNode *texture_node =
                active_texture >= 0 ? rendering_state.textures[active_texture] : NULL;

            if (texture_node != NULL) {
                if (texture_node->mirror_s) {
                    u += texture_node->upload_width / 2;
                }

                if (texture_node->mirror_t) {
                    v += texture_node->upload_height / 2;
                }
            }
#endif
            /*
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            */
            tri_buf[tri_num_vert].u = u;
            tri_buf[tri_num_vert].v = v;
        } else {
            tri_buf[tri_num_vert].u = 0;
            tri_buf[tri_num_vert].v = 0;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            tri_buf[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            tri_buf[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        */
        tri_buf[tri_num_vert].color = gfx_get_vertex_rgba(comb, use_alpha, &v_arr[i]->color, 0.0f, false);
        if (state->color_mul_env) {
            gfx_color_mul_env(&tri_buf[tri_num_vert].color);
        }
        if (state->color_mul_prim) {
            gfx_color_mul_prim(&tri_buf[tri_num_vert].color);
        }
        tri_num_vert++;
    }
    gfx_scegu_draw_triangles_2d((float*)&tri_buf[0],0,1);
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    uint32_t geometryMode = (rsp.geometry_mode & ~clear) | set;

    if (geometryMode == rsp.geometry_mode) {
        return;
    }
    rsp.geometry_mode = geometryMode;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
#if defined(TARGET_PSP)
    const void* normalizedSource;

    if (!gfx_normalize_read_source(viewport, sizeof(Vp_t), "viewport", &normalizedSource)) {
        rdp.viewport.x = 0;
        rdp.viewport.y = 0;
        rdp.viewport.width = gfx_current_dimensions.width;
        rdp.viewport.height = gfx_current_dimensions.height;
        sHudViewportFullscreen = true;
        rdp.viewport_or_scissor_changed = true;
        gfx_mark_tri_pipeline_dirty();
        return;
    }
    viewport = (const Vp_t*)normalizedSource;
#endif

    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    sHudViewportFullscreen =
        (x <= 0.0f) && (y <= 0.0f) && (width >= SCREEN_WIDTH) && (height >= SCREEN_HEIGHT);
    
    if (gfx_hud_anchor_enabled() && !sHudViewportFullscreen) {
        width *= RATIO_Y;
        x = (x * RATIO_Y) + gfx_widescreen_margin_pixels() + gfx_hud_anchor_offset_pixels();
    } else {
        width *= RATIO_X;
        x *= RATIO_X;
    }
    height *= RATIO_Y;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
#if defined(TARGET_PSP)
                const void* normalizedSource;

                if (!gfx_normalize_read_source(data, sizeof(Light_t), "light", &normalizedSource)) {
                    break;
                }
                data = normalizedSource;
#endif
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
#if defined(TARGET_PSP)
        {
            const void* normalizedSource;

            if (!gfx_normalize_read_source(data, sizeof(Light_t), "light", &normalizedSource)) {
                break;
            }
            data = normalizedSource;
        }
#endif
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = gfx_clamp_num_lights(data / 24 + 1); // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = gfx_clamp_num_lights((data - 0x80000000U) / 32);
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
        {
            uint8_t segment = offset >> 2;
            uintptr_t base = data;
            uintptr_t normalized;

            if (segment < NUM_SEGMENTS) {
                if (gfx_normalize_native_addr(base, &normalized)) {
                    base = normalized;
                }
                rsp.segments[segment] = (void*)base;
                rsp.segment_cmd[segment] = sCurrentCmd;
            }
            break;
        }
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(tile);
    _UNUSED(on);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);

    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    
    struct XYWidthHeight scissor = { x, y, width, height };

    if (memcmp(&rdp.scissor, &scissor, sizeof(scissor)) == 0) {
        return;
    }
    rdp.scissor = scissor;
    
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.fmt = format;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width;
#if defined(TARGET_PSP)
    rdp.texture_to_load.image_cmd = sCurrentCmd;
#endif
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    _UNUSED(shiftt);
    _UNUSED(shifts);
    int renderSlot;

    if (gfx_get_render_tile_slot(tile, &renderSlot)) {
        TextureTileState* tileState = gfx_get_texture_tile(renderSlot);
        const uint32_t lineSizeBytes = line * 8;
        bool changed = (tileState->fmt != fmt) || (tileState->siz != siz) ||
                       (tileState->cms != cms) || (tileState->cmt != cmt) ||
                       (tileState->masks != masks) || (tileState->maskt != maskt) ||
                       (tileState->line_size_bytes != lineSizeBytes);

        if (tile == G_TX_RENDERTILE) {
            SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        }
        tileState->fmt = fmt;
        tileState->siz = siz;
        tileState->cms = cms;
        tileState->cmt = cmt;
        tileState->masks = masks;
        tileState->maskt = maskt;
        tileState->line_size_bytes = lineSizeBytes;
#if defined(TARGET_PSP)
        if (renderSlot == 1 && tmem == 0 && rdp.loaded_texture[0].addr != NULL) {
            changed = changed || rdp.textures_changed[0] ||
                      (rdp.loaded_texture[1].addr != rdp.loaded_texture[0].addr) ||
                      (rdp.loaded_texture[1].size_bytes != rdp.loaded_texture[0].size_bytes) ||
                      (rdp.loaded_texture[1].source_size_bytes != rdp.loaded_texture[0].source_size_bytes) ||
                      (rdp.loaded_texture[1].row_stride_bytes != rdp.loaded_texture[0].row_stride_bytes) ||
                      (rdp.loaded_texture[1].source_nibble_offset != rdp.loaded_texture[0].source_nibble_offset);
            /*
             * Many scrolling-surface display lists load one image into TMEM 0,
             * then define tile 1 as another view of that same TMEM data. The
             * PSP backend uses separate texture objects, so mirror the source
             * load into slot 1 while keeping tile 1's own descriptor above.
             */
            rdp.loaded_texture[1] = rdp.loaded_texture[0];
        }
#endif
        if (changed) {
            rdp.textures_changed[renderSlot] = true;
            gfx_mark_tri_pipeline_dirty();
        }
    }
    
    if (tile == G_TX_LOADTILE) {
        /*
         * Two-texture display lists often place tile 1 at TMEM 0x80 for
         * smaller tiles and 0x100 for larger blocks. The PSP backend only
         * tracks two texture slots, so treat any non-zero TMEM base as slot 1.
         */
        rdp.texture_to_load.tile_number = tmem != 0;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    int renderSlot;

    if (gfx_get_render_tile_slot(tile, &renderSlot)) {
        TextureTileState* tileState = gfx_get_texture_tile(renderSlot);

        if ((tileState->uls == uls) && (tileState->ult == ult) &&
            (tileState->lrs == lrs) && (tileState->lrt == lrt)) {
            return;
        }

        tileState->uls = uls;
        tileState->ult = ult;
        tileState->lrs = lrs;
        tileState->lrt = lrt;
        rdp.textures_changed[renderSlot] = true;
        gfx_mark_tri_pipeline_dirty();
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    _UNUSED(high_index);

#if defined(TARGET_PSP)
    int loadSlot;

    if (tile != G_TX_LOADTILE || rdp.texture_to_load.siz != G_IM_SIZ_16b ||
        !gfx_texture_load_slot("load-tlut-slot", &loadSlot)) {
        gfx_log_bad_texture_source(-1, "load-tlut-command", rdp.texture_to_load.addr, GFX_TLUT_SIZE_BYTES);
        return;
    }
#else
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
#endif
    rdp.palette = rdp.texture_to_load.addr;
#if defined(TARGET_PSP)
    rdp.loaded_texture[loadSlot].image_cmd = rdp.texture_to_load.image_cmd;
    rdp.loaded_texture[loadSlot].load_cmd = sCurrentCmd;
#endif
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    _UNUSED(dxt);

    if (tile == 1) return;
#if defined(TARGET_PSP)
    int loadSlot;

    if (tile != G_TX_LOADTILE || uls != 0 || ult != 0 ||
        !gfx_texture_load_slot("load-block-slot", &loadSlot)) {
        gfx_log_bad_texture_source(-1, "load-block-command", rdp.texture_to_load.addr, 1);
        return;
    }
#else
    int loadSlot = rdp.texture_to_load.tile_number;

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
#endif
    
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift = 0;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    rdp.loaded_texture[loadSlot].size_bytes = size_bytes;
    rdp.loaded_texture[loadSlot].source_size_bytes = size_bytes;
    rdp.loaded_texture[loadSlot].row_stride_bytes = 0;
    rdp.loaded_texture[loadSlot].source_nibble_offset = 0;
#if defined(TARGET_PSP)
    rdp.loaded_texture[loadSlot].image_cmd = rdp.texture_to_load.image_cmd;
    rdp.loaded_texture[loadSlot].load_cmd = sCurrentCmd;
    if (size_bytes > 4096U) {
        gfx_log_bad_texture_source(loadSlot, "load-block-size", rdp.texture_to_load.addr, size_bytes);
        gfx_set_invalid_loaded_texture(loadSlot);
        gfx_set_invalid_texture_tile(loadSlot);
    } else {
        rdp.loaded_texture[loadSlot].addr = rdp.texture_to_load.addr;
    }
#else
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[loadSlot].addr = rdp.texture_to_load.addr;
#endif
    
    rdp.textures_changed[loadSlot] = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (tile == 1) return;
#if defined(TARGET_PSP)
    int loadSlot;

    if (tile != G_TX_LOADTILE || !gfx_texture_load_slot("load-tile-slot", &loadSlot)) {
        gfx_log_bad_texture_source(-1, "load-tile-command", rdp.texture_to_load.addr, 1);
        return;
    }
#else
    int loadSlot = rdp.texture_to_load.tile_number;

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
#endif

    uint32_t source_uls = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t source_ult = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t source_lrs = lrs >> G_TEXTURE_IMAGE_FRAC;
    uint32_t source_lrt = lrt >> G_TEXTURE_IMAGE_FRAC;
#if defined(TARGET_PSP)
    if (source_lrs < source_uls || source_lrt < source_ult) {
        gfx_log_bad_texture_source(loadSlot, "load-tile-bounds", rdp.texture_to_load.addr, 1);
        gfx_set_invalid_loaded_texture(loadSlot);
        gfx_set_invalid_texture_tile(loadSlot);
        rdp.textures_changed[loadSlot] = true;
        gfx_mark_tri_pipeline_dirty();
        return;
    }
#endif
    uint32_t width = source_lrs - source_uls + 1;
    uint32_t height = source_lrt - source_ult + 1;
    uint32_t source_width = rdp.texture_to_load.width;

    if (source_width == 0 || source_width < source_lrs + 1) {
        source_width = source_lrs + 1;
    }

    uint32_t source_stride = gfx_texture_row_bytes(source_width, rdp.texture_to_load.siz);
    uint32_t source_x_offset = gfx_texture_byte_offset(source_uls, rdp.texture_to_load.siz);
    uint32_t row_bytes = gfx_texture_row_bytes(width + (rdp.texture_to_load.siz == G_IM_SIZ_4b ? (source_uls & 1) : 0),
                                               rdp.texture_to_load.siz);
    uint32_t size_bytes = row_bytes * height;
    uint32_t source_size_bytes = height > 0 ? ((height - 1) * source_stride) + row_bytes : 0;
    const uint8_t* source_addr = rdp.texture_to_load.addr + (size_t)source_ult * source_stride + source_x_offset;

    rdp.loaded_texture[loadSlot].size_bytes = size_bytes;
    rdp.loaded_texture[loadSlot].source_size_bytes = source_size_bytes;
    rdp.loaded_texture[loadSlot].row_stride_bytes = source_stride;
    rdp.loaded_texture[loadSlot].source_nibble_offset =
        rdp.texture_to_load.siz == G_IM_SIZ_4b ? (source_uls & 1) : 0;
#if defined(TARGET_PSP)
    rdp.loaded_texture[loadSlot].image_cmd = rdp.texture_to_load.image_cmd;
    rdp.loaded_texture[loadSlot].load_cmd = sCurrentCmd;
#endif

#if defined(TARGET_PSP)
    if (size_bytes > 4096U) {
        gfx_log_bad_texture_source(loadSlot, "load-tile-size", source_addr, size_bytes);
        gfx_set_invalid_loaded_texture(loadSlot);
        gfx_set_invalid_texture_tile(loadSlot);
    } else {
        rdp.loaded_texture[loadSlot].addr = source_addr;
        gfx_get_texture_tile(loadSlot)->uls = uls;
        gfx_get_texture_tile(loadSlot)->ult = ult;
        gfx_get_texture_tile(loadSlot)->lrs = lrs;
        gfx_get_texture_tile(loadSlot)->lrt = lrt;
    }
#else
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[loadSlot].addr = source_addr;
    gfx_get_texture_tile(loadSlot)->uls = uls;
    gfx_get_texture_tile(loadSlot)->ult = ult;
    gfx_get_texture_tile(loadSlot)->lrs = lrs;
    gfx_get_texture_tile(loadSlot)->lrt = lrt;
#endif

    rdp.textures_changed[loadSlot] = true;
    gfx_mark_tri_pipeline_dirty();
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static bool gfx_cc_is_two_cycle_texture_tint(uint32_t a0, uint32_t b0, uint32_t c0, uint32_t d0, uint32_t a1,
                                             uint32_t b1, uint32_t c1, uint32_t d1) {
    return ((a0 == G_CCMUX_TEXEL0) || (a0 == G_CCMUX_TEXEL1)) &&
           ((b0 == G_CCMUX_PRIMITIVE) || (b0 == G_CCMUX_TEXEL0)) &&
           ((c0 == G_CCMUX_ENV_ALPHA) || (c0 == G_CCMUX_PRIM_LOD_FRAC)) && (d0 == G_CCMUX_TEXEL0) &&
           (a1 == G_CCMUX_PRIMITIVE) && (b1 == G_CCMUX_ENVIRONMENT) && (c1 == G_CCMUX_COMBINED) &&
           (d1 == G_CCMUX_ENVIRONMENT);
}

static bool gfx_cc_is_two_cycle_texture_shade_prim_tint(uint32_t a0, uint32_t b0, uint32_t c0, uint32_t d0,
                                                        uint32_t a1, uint32_t b1, uint32_t c1, uint32_t d1) {
    return (a0 == G_CCMUX_TEXEL1) && (b0 == G_CCMUX_TEXEL0) && (c0 == G_CCMUX_PRIM_LOD_FRAC) &&
           (d0 == G_CCMUX_TEXEL0) && (a1 == G_CCMUX_SHADE) && (b1 == G_CCMUX_PRIMITIVE) &&
           (c1 == G_CCMUX_COMBINED) && (d1 == G_CCMUX_PRIMITIVE);
}

static bool gfx_cc_is_two_cycle_texture_blend_mul_shade(uint32_t a0, uint32_t b0, uint32_t c0, uint32_t d0,
                                                        uint32_t a1, uint32_t b1, uint32_t c1, uint32_t d1) {
    return (a0 == G_CCMUX_TEXEL1) && (b0 == G_CCMUX_TEXEL0) && (c0 == G_CCMUX_ENV_ALPHA) &&
           (d0 == G_CCMUX_TEXEL0) && (a1 == G_CCMUX_COMBINED) && (b1 == (G_CCMUX_0 & 0xF)) &&
           (c1 == G_CCMUX_SHADE) && (d1 == (G_CCMUX_0 & 0x7));
}

static bool gfx_cc_is_texture_prim_env_blend(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a == G_CCMUX_PRIMITIVE) && (b == G_CCMUX_ENVIRONMENT) && (c == G_CCMUX_TEXEL0) &&
           (d == G_CCMUX_ENVIRONMENT);
}

static bool gfx_cc_is_color_mul(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t lhs, uint32_t rhs) {
    return (b == (G_CCMUX_0 & 0xF)) && (d == (G_CCMUX_0 & 0x7)) &&
           (((a == lhs) && (c == rhs)) || ((a == rhs) && (c == lhs)));
}

static bool gfx_cc_is_one(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a == G_ACMUX_0) && (b == G_ACMUX_0) && (c == G_ACMUX_0) && (d == G_ACMUX_1);
}

static bool gfx_cc_is_combined_mul_primitive(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a == G_ACMUX_COMBINED) && (b == G_ACMUX_0) && (c == G_ACMUX_PRIMITIVE) &&
           (d == G_ACMUX_0);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, bool color_mul_env, bool color_mul_prim,
                                    bool texture_blend, bool texture_blend_shade, bool two_texture_blend) {
    uint32_t combineMode = rgb | (alpha << 12);

    if (texture_blend) {
        combineMode |= texture_blend_shade ? SHADER_OPT_TEXTURE_BLEND_SHADE : SHADER_OPT_TEXTURE_BLEND;
    }
    if ((rdp.combine_mode == combineMode) && (rdp.combine_color_mul_env == color_mul_env) &&
        (rdp.combine_color_mul_prim == color_mul_prim) &&
        (rdp.combine_two_texture_blend == two_texture_blend)) {
        return;
    }
    if (rdp.combine_two_texture_blend != two_texture_blend) {
        /* Do not let vertices from the one-pass and two-pass paths share a batch. */
        gfx_flush();
    }
    rdp.combine_mode = combineMode;
    rdp.combine_color_mul_env = color_mul_env;
    rdp.combine_color_mul_prim = color_mul_prim;
    rdp.combine_two_texture_blend = two_texture_blend;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((rdp.prim_color.r == r) && (rdp.prim_color.g == g) && (rdp.prim_color.b == b) &&
        (rdp.prim_color.a == a)) {
        return;
    }

    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static bool gfx_rectangle_covers_width(int32_t ulx, int32_t lrx) {
    return (ulx <= 0) && (lrx >= ((SCREEN_WIDTH - 1) << 2));
}

static bool gfx_rectangle_covers_screen(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    return gfx_rectangle_covers_width(ulx, lrx) && (uly <= 0) && (lry >= ((SCREEN_HEIGHT - 1) << 2));
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, bool force_fullscreen,
                               bool force_full_width) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
        gfx_mark_tri_pipeline_dirty();
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    if (force_fullscreen) {
        ulxf = 0.0f;
        ulyf = 0.0f;
        lrxf = gfx_current_dimensions.width;
        lryf = gfx_current_dimensions.height;
    } else {
        const float halfWidth = (float)gfx_current_dimensions.width * 0.5f;
        const float halfHeight = (float)gfx_current_dimensions.height * 0.5f;
        const float hudOffset = gfx_hud_anchor_offset_pixels();

        ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
        lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;

        if (force_full_width) {
            ulxf = 0.0f;
            lrxf = gfx_current_dimensions.width;
        } else {
            ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
            lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
            ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
            lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
            ulxf = (ulxf * halfWidth) + halfWidth + hudOffset;
            lrxf = (lrxf * halfWidth) + halfWidth + hudOffset;
        }

        ulyf = (ulyf * halfHeight) + halfHeight;
        lryf = (lryf * halfHeight) + halfHeight;
    }
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    
    ul->x = (unsigned short)ulxf;
    ul->y = (unsigned short)ulyf;

    lr->x = (unsigned short)lrxf;
    lr->y = (unsigned short)lryf;

    // The coordinates for texture rectangles shall bypass the 3D viewport/scissor state.
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    struct XYWidthHeight scissor_saved = rdp.scissor;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.scissor = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    gfx_mark_tri_pipeline_dirty();
    
    gfx_sp_tri1_2d(0, 1, 2);
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.scissor = scissor_saved;
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
        gfx_mark_tri_pipeline_dirty();
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    _UNUSED(tile);

    uint32_t saved_combine_mode = rdp.combine_mode;
    bool saved_combine_color_mul_env = rdp.combine_color_mul_env;
    bool saved_combine_color_mul_prim = rdp.combine_color_mul_prim;
    bool saved_combine_two_texture_blend = rdp.combine_two_texture_blend;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0),
                                false, false, false, false, false);
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    /*@Note: fix this */
    #if 0
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    #endif
    
    gfx_draw_rectangle(ulx, uly, lrx, lry, false, false);
    rdp.combine_mode = saved_combine_mode;
    rdp.combine_color_mul_env = saved_combine_color_mul_env;
    rdp.combine_color_mul_prim = saved_combine_color_mul_prim;
    rdp.combine_two_texture_blend = saved_combine_two_texture_blend;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    bool use_fill_color = (mode == G_CYC_COPY || mode == G_CYC_FILL);
    
    if (use_fill_color) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    for (int i = 0; i < 2; i++) {
        struct VertexColor* v = &rsp.loaded_vertices_2D[i];
        v->color = use_fill_color ? rdp.fill_color : rdp.prim_color;
    }
    
    uint32_t saved_combine_mode = rdp.combine_mode;
    bool saved_combine_color_mul_env = rdp.combine_color_mul_env;
    bool saved_combine_color_mul_prim = rdp.combine_color_mul_prim;
    bool saved_combine_two_texture_blend = rdp.combine_two_texture_blend;
    if (use_fill_color) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE), false, false,
                                false, false, false);
    }
    gfx_draw_rectangle(ulx, uly, lrx, lry, gfx_rectangle_covers_screen(ulx, uly, lrx, lry),
                       gfx_rectangle_covers_width(ulx, lrx));
    if (use_fill_color) {
        rdp.combine_mode = saved_combine_mode;
        rdp.combine_color_mul_env = saved_combine_color_mul_env;
        rdp.combine_color_mul_prim = saved_combine_color_mul_prim;
        rdp.combine_two_texture_blend = saved_combine_two_texture_blend;
        gfx_mark_tri_pipeline_dirty();
    }
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    _UNUSED(size);
    _UNUSED(width);

    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    if ((rdp.other_mode_l == (uint32_t)om) && (rdp.other_mode_h == (uint32_t)(om >> 32))) {
        return;
    }
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_other_mode(uint32_t mode_h, uint32_t mode_l) {
    if ((rdp.other_mode_h == mode_h) && (rdp.other_mode_l == mode_l)) {
        return;
    }
    rdp.other_mode_h = mode_h;
    rdp.other_mode_l = mode_l;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_noop(uint32_t tag) {
    if ((tag & 0xFFFFFF00U) == OOT_PSP_HUD_ANCHOR_TAG) {
        OotPspHudAnchor anchor = tag & 0xFF;

        if ((anchor <= OOT_PSP_HUD_ANCHOR_RIGHT) && (anchor != sHudAnchor)) {
            gfx_flush();
            sHudAnchor = anchor;
            gfx_update_screen_metrics();
            gfx_apply_projection_matrix();
        }
    }
}

static inline bool gfx_addr_looks_segmented(uintptr_t addr) {
    uint8_t segment = addr >> 24;
    uintptr_t offset = addr & 0x00FFFFFFU;
    bool segmentMapped;

    if ((segment == 0) || (segment >= NUM_SEGMENTS)) {
        return false;
    }

    if (!gfx_addr_is_native(addr)) {
        return true;
    }

    segmentMapped = rsp.segments[segment] != NULL;
#if defined(TARGET_PSP)
    segmentMapped = segmentMapped || (gSegments[segment] != 0);
#endif

    /*
     * A zero-offset reference is the canonical dynamic-segment form used by
     * Link's eye and mouth textures (0x08000000 and 0x09000000).  Those exact
     * values can fall inside PSP RAM, but an active matching segment makes the
     * command's intent unambiguous.
     */
    if ((offset == 0) && segmentMapped) {
        return true;
    }

    /*
     * Provenance has to win over the low-offset collision heuristic.  Native
     * pointers allocated near the start of PSP RAM can have values such as
     * 0x0900AAC0; interpreting those as segment 9 first adds the active segment
     * base and sends display-list execution into unrelated graph-pool data.
     */
    if (gfx_is_valid_native_read_range(addr, 1)) {
        return false;
    }

    /*
     * Other small segment offsets also overlap PSP RAM.  Once known native
     * allocations have been excluded, an active matching segment makes the
     * low-offset form explicit.
     */
    if (offset < PSP_SEGMENTED_COLLISION_OFFSET_MAX) {
        if (segmentMapped) {
            return true;
        }
    }

    /*
     * PSP RAM and N64 segments 8-B share the same numeric address window.  A
     * fixed offset cutoff cannot separate them: large room/object offsets are
     * valid segmented addresses.  Treat only ranges with known PSP provenance
     * as native and let every other mapped value use segment translation.
     */
    return true;
}

static void gfx_log_unmapped_segment(uintptr_t addr) {
    static s32 sUnmappedSegmentLogCount = 0;

    if (sUnmappedSegmentLogCount < 16) {
        printf("oot-psp gfx unmapped segment addr=%08lx segment=%lu offset=%06lx\n", (unsigned long)addr,
               (unsigned long)(addr >> 24), (unsigned long)(addr & 0x00FFFFFFU));
    } else if (sUnmappedSegmentLogCount == 16) {
        printf("oot-psp gfx unmapped segment logs suppressed\n");
    }

    sUnmappedSegmentLogCount++;
}

static void gfx_log_segment_translation(uintptr_t addr, void* base, void* translated) {
    static s32 sSegmentTranslateLogCount = 0;
    uint8_t segment = addr >> 24;

    if ((segment != 0x0C) && (segment != 0x0D)) {
        return;
    }

    if (sSegmentTranslateLogCount < 32) {
        printf("oot-psp gfx segment translate addr=%08lx segment=%u offset=%06lx base=%08lx out=%08lx\n",
               (unsigned long)addr, segment, (unsigned long)(addr & 0x00FFFFFFU), (unsigned long)(uintptr_t)base,
               (unsigned long)(uintptr_t)translated);
    } else if (sSegmentTranslateLogCount == 32) {
        printf("oot-psp gfx segment translate logs suppressed\n");
    }

    sSegmentTranslateLogCount++;
}

static void gfx_log_bad_dl_cursor(const char* reason, uintptr_t raw, uintptr_t translated) {
    static s32 sBadDlCursorLogCount = 0;

    if (sBadDlCursorLogCount < 32) {
        printf("oot-psp gfx bad dl cursor reason=%s raw=%08lx translated=%08lx cur=%08lx/%08lx/%08lx "
               "seg1=%08lx seg2=%08lx seg3=%08lx seg4=%08lx seg6=%08lx seg8=%08lx seg9=%08lx sega=%08lx "
               "segc=%08lx segd=%08lx segacmd=%08lx/%08lx/%08lx\n",
               reason, (unsigned long)raw, (unsigned long)translated, (unsigned long)sCurrentCmd.addr,
               (unsigned long)sCurrentCmd.w0, (unsigned long)sCurrentCmd.w1,
               (unsigned long)(uintptr_t)rsp.segments[1], (unsigned long)(uintptr_t)rsp.segments[2],
               (unsigned long)(uintptr_t)rsp.segments[3], (unsigned long)(uintptr_t)rsp.segments[4],
               (unsigned long)(uintptr_t)rsp.segments[6], (unsigned long)(uintptr_t)rsp.segments[8],
               (unsigned long)(uintptr_t)rsp.segments[9], (unsigned long)(uintptr_t)rsp.segments[10],
               (unsigned long)(uintptr_t)rsp.segments[12], (unsigned long)(uintptr_t)rsp.segments[13],
               (unsigned long)rsp.segment_cmd[10].addr, (unsigned long)rsp.segment_cmd[10].w0,
               (unsigned long)rsp.segment_cmd[10].w1);
    } else if (sBadDlCursorLogCount == 32) {
        printf("oot-psp gfx bad dl cursor logs suppressed\n");
    }

    sBadDlCursorLogCount++;
}

static bool gfx_validate_dl_cursor(uintptr_t raw, Gfx** cmdP) {
    uintptr_t translated = (uintptr_t)*cmdP;
    uintptr_t normalized;

    if (!gfx_normalize_native_range(translated, sizeof(Gfx), &normalized)) {
        gfx_log_bad_dl_cursor("unmapped", raw, translated);
        return false;
    }

    if ((normalized & (sizeof(Gfx) - 1)) != 0) {
        gfx_log_bad_dl_cursor("unaligned", raw, normalized);
        return false;
    }

    if (!gfx_is_valid_native_dl_range(normalized, sizeof(Gfx))) {
        gfx_log_bad_dl_cursor("non-dl-native-range", raw, normalized);
        return false;
    }

    *cmdP = (Gfx*)normalized;
    return true;
}

static inline void* gfx_runtime_symbol_addr(uintptr_t addr) {
    if (addr == (uintptr_t)&D_01000000 && rsp.segments[1] != NULL) {
        return rsp.segments[1];
    }

    switch (addr) {
        case PSP_ASSET_SYMBOL_GIDENTITYMTX:
            return &gIdentityMtx;
        default:
            return NULL;
    }
}

static inline void *seg_addr(uintptr_t w1) {
    void* runtimeSymbol = gfx_runtime_symbol_addr(w1);

    if (runtimeSymbol != NULL) {
        return runtimeSymbol;
    }

    if (gfx_addr_looks_segmented(w1)) {
        uint8_t segment = w1 >> 24;
        uintptr_t offset = w1 & 0x00FFFFFFU;

        if (rsp.segments[segment] != NULL) {
            uintptr_t baseValue = (uintptr_t)rsp.segments[segment];
            uintptr_t translatedValue = baseValue + offset;
            uintptr_t normalized;
            void* translated;

#if defined(TARGET_PSP)
            if (!gfx_addr_is_native(baseValue) && gfx_addr_looks_segmented(baseValue) &&
                gfx_addr_looks_segmented(translatedValue) &&
                (translatedValue != w1)) {
                translated = seg_addr(translatedValue);
                if (translated != (void*)translatedValue) {
                    gfx_log_segment_translation(w1, rsp.segments[segment], translated);
                    return translated;
                }
            }
#endif

            if (gfx_normalize_native_addr(translatedValue, &normalized)) {
                translatedValue = normalized;
            }

            translated = (void*)translatedValue;

            gfx_log_segment_translation(w1, rsp.segments[segment], translated);
            return translated;
        }

#if defined(TARGET_PSP)
        if (gSegments[segment] != 0) {
            uintptr_t translatedValue = gSegments[segment] + offset + K0BASE;
            uintptr_t normalized;
            void* translated;

            if (gfx_normalize_native_addr(translatedValue, &normalized)) {
                translatedValue = normalized;
            }

            translated = (void*)translatedValue;
            gfx_log_segment_translation(w1, (void*)(gSegments[segment] + K0BASE), translated);
            return translated;
        }
#endif

        gfx_log_unmapped_segment(w1);
    }

    {
        uintptr_t normalized;

        if (gfx_normalize_native_addr(w1, &normalized)) {
            return (void*)normalized;
        }
    }

    return (void*)w1;
}

#if defined(TARGET_PSP) && defined(F3DEX_GBI_2)
#define GFX_S2DEX_BG_MAX_UPLOAD_WIDTH 256U
#define GFX_S2DEX_BG_MAX_UPLOAD_HEIGHT 256U

static void gfx_s2dex_bg_compute_upload_size(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t* contentWidth,
                                             uint32_t* contentHeight, uint32_t* uploadWidth,
                                             uint32_t* uploadHeight) {
    *contentWidth = sourceWidth;
    *contentHeight = sourceHeight;

    if ((*contentWidth > GFX_S2DEX_BG_MAX_UPLOAD_WIDTH) || (*contentHeight > GFX_S2DEX_BG_MAX_UPLOAD_HEIGHT)) {
        if ((uint64_t)sourceWidth * GFX_S2DEX_BG_MAX_UPLOAD_HEIGHT >
            (uint64_t)sourceHeight * GFX_S2DEX_BG_MAX_UPLOAD_WIDTH) {
            *contentWidth = GFX_S2DEX_BG_MAX_UPLOAD_WIDTH;
            *contentHeight = ((uint64_t)sourceHeight * GFX_S2DEX_BG_MAX_UPLOAD_WIDTH + (sourceWidth / 2)) /
                             sourceWidth;
        } else {
            *contentHeight = GFX_S2DEX_BG_MAX_UPLOAD_HEIGHT;
            *contentWidth = ((uint64_t)sourceWidth * GFX_S2DEX_BG_MAX_UPLOAD_HEIGHT + (sourceHeight / 2)) /
                            sourceHeight;
        }

        if (*contentWidth == 0) {
            *contentWidth = 1;
        }
        if (*contentHeight == 0) {
            *contentHeight = 1;
        }
    }

    *uploadWidth = gfx_next_power_of_two(*contentWidth);
    *uploadHeight = gfx_next_power_of_two(*contentHeight);
}

static bool gfx_s2dex_bg_upload_rgba16_texture(const uint8_t* source, uint32_t width, uint32_t height,
                                               uint32_t rowBytes, uint32_t sourceSpan, uint32_t contentWidth,
                                               uint32_t contentHeight, uint32_t uploadWidth, uint32_t uploadHeight,
                                               struct TextureHashmapNode* node) {
    const size_t uploadSize = (size_t)uploadWidth * uploadHeight * sizeof(uint16_t);
    GfxTextureSwapState swapState;
    uint16_t* dst;

    if (uploadSize > sizeof(psp_texture_stage_buf)) {
        gfx_log_bad_texture_source(0, "s2dex-bg-upload-size", source, sourceSpan);
        return false;
    }

    memset(psp_texture_stage_buf, 0, uploadSize);
    dst = (uint16_t*)psp_texture_stage_buf;
    swapState = gfx_texture_source_swap_state(source, sourceSpan);

    for (uint32_t y = 0; y < contentHeight; y++) {
        uint32_t sourceY = (uint64_t)y * height / contentHeight;
        const uint8_t* row = source + (size_t)sourceY * rowBytes;
        uint16_t* dstRow = dst + (size_t)y * uploadWidth;

        for (uint32_t x = 0; x < contentWidth; x++) {
            uint32_t sourceX = (uint64_t)x * width / contentWidth;
            uint16_t col16 = gfx_read_texture_source_be16(row, sourceX * sizeof(uint16_t), &swapState);
            const uint8_t a = col16 & 1;
            const uint8_t r = (col16 >> 11) & 0x1f;
            const uint8_t g = (col16 >> 6) & 0x1f;
            const uint8_t b = (col16 >> 1) & 0x1f;

            dstRow[x] = (a << 15) | (b << 10) | (g << 5) | r;
        }
    }

    texman_upload(uploadWidth, uploadHeight, GU_PSM_5551, psp_texture_stage_buf);
    node->upload_width = uploadWidth;
    node->upload_height = uploadHeight;
    node->mirror_s = false;
    node->mirror_t = false;
    return true;
}

static bool gfx_s2dex_bg_prepare_texture(const uint8_t* source, uint32_t width, uint32_t height, uint8_t fmt,
                                         uint8_t siz, uint32_t* contentWidth, uint32_t* contentHeight) {
    const int tile = 0;
    uint32_t rowBytes;
    uint32_t sourceSpan;
    uint32_t uploadWidth;
    uint32_t uploadHeight;
    unsigned int uploadSize;

    if ((source == NULL) || (width == 0) || (height == 0) || (height > (UINT32_MAX / width))) {
        gfx_log_bad_texture_source(tile, "s2dex-bg-dimensions", source, 1);
        return false;
    }

    rowBytes = gfx_texture_row_bytes(width, siz);
    if ((rowBytes == 0) || (height > (UINT32_MAX / rowBytes))) {
        gfx_log_bad_texture_source(tile, "s2dex-bg-rowbytes", source, rowBytes);
        return false;
    }

    sourceSpan = rowBytes * height;
    if ((fmt != G_IM_FMT_RGBA) || (siz != G_IM_SIZ_16b)) {
        gfx_log_bad_texture_source(tile, "s2dex-bg-format", source, sourceSpan);
        return false;
    }

    if (!gfx_normalize_texture_source(&source, sourceSpan)) {
        gfx_log_bad_texture_source(tile, "s2dex-bg-source", source, sourceSpan);
        return false;
    }

    gfx_s2dex_bg_compute_upload_size(width, height, contentWidth, contentHeight, &uploadWidth, &uploadHeight);

    if ((size_t)uploadWidth * uploadHeight * sizeof(uint16_t) > sizeof(psp_texture_stage_buf)) {
        gfx_log_bad_texture_source(tile, "s2dex-bg-too-large", source, sourceSpan);
        return false;
    }

    uploadSize = uploadWidth * uploadHeight * sizeof(uint16_t);
    if (!texman_vram_space_available(uploadSize) || !texman_texture_slot_available()) {
        gfx_texture_cache_clear();
    }

    gfx_flush();

    rdp.texture_to_load.addr = source;
    rdp.texture_to_load.fmt = fmt;
    rdp.texture_to_load.siz = siz;
    rdp.texture_to_load.width = width;
    rdp.texture_to_load.tile_number = tile;
    rdp.texture_to_load.image_cmd = sCurrentCmd;

    rdp.loaded_texture[tile].addr = source;
    rdp.loaded_texture[tile].size_bytes = sourceSpan;
    rdp.loaded_texture[tile].source_size_bytes = sourceSpan;
    rdp.loaded_texture[tile].row_stride_bytes = rowBytes;
    rdp.loaded_texture[tile].source_nibble_offset = 0;
    rdp.loaded_texture[tile].image_cmd = sCurrentCmd;
    rdp.loaded_texture[tile].load_cmd = sCurrentCmd;

    TextureTileState* tileState = gfx_get_texture_tile(tile);

    tileState->fmt = fmt;
    tileState->siz = siz;
    tileState->cms = G_TX_CLAMP;
    tileState->cmt = G_TX_CLAMP;
    tileState->masks = G_TX_NOMASK;
    tileState->maskt = G_TX_NOMASK;
    tileState->line_size_bytes = rowBytes;
    tileState->uls = 0;
    tileState->ult = 0;
    tileState->lrs = (width - 1) << G_TEXTURE_IMAGE_FRAC;
    tileState->lrt = (height - 1) << G_TEXTURE_IMAGE_FRAC;

    if (!gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], source, fmt, siz)) {
        if (!gfx_s2dex_bg_upload_rgba16_texture(source, width, height, rowBytes, sourceSpan, *contentWidth,
                                                *contentHeight, uploadWidth, uploadHeight,
                                                rendering_state.textures[tile])) {
            rendering_state.textures[tile] = NULL;
            rdp.textures_changed[tile] = true;
            return false;
        }

        gfx_rapi->select_texture(tile, rendering_state.textures[tile]->texture_id);
        gfx_rapi->set_sampler_parameters(tile, false, tileState->cms, tileState->cmt,
                                         tileState->masks, tileState->maskt);
    }

    rdp.textures_changed[tile] = false;
    gfx_mark_tri_pipeline_dirty();
    return true;
}

static uint32_t gfx_s2dex_bg_texcoord_span(uint32_t frameSpan, uint16_t scale, bool scaled) {
    if (scaled && (scale != 0)) {
        return (uint32_t)(((uint64_t)frameSpan * scale) >> 7);
    }

    return frameSpan * 8;
}

static void gfx_sp_s2dex_bg_rect(uint32_t opcode, const void* bgAddr) {
    const void* normalizedBg;
    const uObjBg* bg;
    const uint8_t* source;
    uint32_t imageWidth;
    uint32_t imageHeight;
    int32_t frameX;
    int32_t frameY;
    uint32_t frameW;
    uint32_t frameH;
    uint32_t texSpanS;
    uint32_t texSpanT;
    uint32_t texStartS;
    uint32_t texStartT;
    uint32_t contentWidth;
    uint32_t contentHeight;
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    uint32_t savedCombineMode;
    bool savedColorMulEnv;
    bool savedColorMulPrim;
    bool savedTwoTextureBlend;
    const bool scaled = opcode == G_BG_1CYC;

    if (!gfx_normalize_read_source(bgAddr, sizeof(uObjBg), "s2dex-bg", &normalizedBg)) {
        return;
    }

    if (((uintptr_t)normalizedBg & (sizeof(uint32_t) - 1)) != 0) {
        gfx_log_bad_data_source("s2dex-bg-align", bgAddr, sizeof(uObjBg));
        return;
    }

    bg = (const uObjBg*)normalizedBg;
    imageWidth = bg->b.imageW >> 2;
    imageHeight = bg->b.imageH >> 2;
    source = (const uint8_t*)seg_addr((uintptr_t)bg->b.imagePtr);

    if (!gfx_s2dex_bg_prepare_texture(source, imageWidth, imageHeight, bg->b.imageFmt, bg->b.imageSiz, &contentWidth,
                                      &contentHeight)) {
        return;
    }

    frameX = bg->b.frameX;
    frameY = bg->b.frameY;
    frameW = scaled ? bg->s.frameW : bg->b.frameW;
    frameH = scaled ? bg->s.frameH : bg->b.frameH;
    texSpanS = gfx_s2dex_bg_texcoord_span(frameW, bg->s.scaleW, scaled);
    texSpanT = gfx_s2dex_bg_texcoord_span(frameH, bg->s.scaleH, scaled);
    texStartS = (uint64_t)bg->b.imageX * contentWidth / imageWidth;
    texStartT = (uint64_t)bg->b.imageY * contentHeight / imageHeight;
    texSpanS = (uint64_t)texSpanS * contentWidth / imageWidth;
    texSpanT = (uint64_t)texSpanT * contentHeight / imageHeight;

    ul->u = texStartS;
    ul->v = texStartT;
    ul->color = white_color;
    lr->u = texStartS + texSpanS;
    lr->v = texStartT + texSpanT;
    lr->color = white_color;

    savedCombineMode = rdp.combine_mode;
    savedColorMulEnv = rdp.combine_color_mul_env;
    savedColorMulPrim = rdp.combine_color_mul_prim;
    savedTwoTextureBlend = rdp.combine_two_texture_blend;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0), false,
                                false, false, false, false);
    }

    gfx_draw_rectangle(frameX, frameY, frameX + frameW, frameY + frameH, false, false);

    rdp.combine_mode = savedCombineMode;
    rdp.combine_color_mul_env = savedColorMulEnv;
    rdp.combine_color_mul_prim = savedColorMulPrim;
    rdp.combine_two_texture_blend = savedTwoTextureBlend;
    gfx_mark_tri_pipeline_dirty();
}
#endif

static bool gfx_translate_dl_cursor(Gfx** cmdP) {
    uintptr_t raw = (uintptr_t)*cmdP;
    uintptr_t normalized;

    if (gfx_normalize_native_range(raw, sizeof(Gfx), &normalized) &&
        gfx_is_valid_native_dl_range(normalized, sizeof(Gfx))) {
        *cmdP = (Gfx*)normalized;
        return true;
    }

    if (gfx_addr_looks_segmented(raw)) {
        void* translated = seg_addr(raw);

        if (translated == (void*)raw) {
            gfx_log_bad_dl_cursor("untranslated-segment", raw, (uintptr_t)translated);
            return false;
        }

        *cmdP = (Gfx*)translated;
    } else {
        if (gfx_normalize_native_range(raw, sizeof(Gfx), &normalized)) {
            *cmdP = (Gfx*)normalized;
        }
    }

    return gfx_validate_dl_cursor(raw, cmdP);
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void gfx_run_dl(Gfx* cmd) {
    if (!gfx_translate_dl_cursor(&cmd)) {
        return;
    }

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
#if defined(TARGET_PSP)
        sCurrentCmd.addr = (uintptr_t)cmd;
        sCurrentCmd.w0 = cmd->words.w0;
        sCurrentCmd.w1 = cmd->words.w1;
#endif
        
        switch (opcode) {
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case (uint8_t)G_RDPHALF_1:
                rsp.rdp_half_1 = cmd->words.w1;
                break;
            case (uint8_t)G_BRANCH_Z:
                /*
                 * The PSP path does not retain the RSP's fixed-point screen Z.
                 * Follow the detailed branch so conditional scene chunks are
                 * rendered instead of falling through to an empty display list.
                 */
                if (rsp.rdp_half_1 != 0) {
                    Gfx* branchTarget = (Gfx*)seg_addr(rsp.rdp_half_1);

                    if (!gfx_translate_dl_cursor(&branchTarget)) {
                        return;
                    }

                    cmd = branchTarget;
                    --cmd;
                }
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
#if defined(TARGET_PSP)
                if (!gfx_sp_vertex_f3dex2(cmd->words.w0, cmd->words.w1)) {
                    return;
                }
#else
                if (!gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1))) {
                    return;
                }
#endif
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                if (!gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1))) {
                    return;
                }
#else
                if (!gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1))) {
                    return;
                }
#endif
                break;
            case G_DL:
            {
                void* target = seg_addr(cmd->words.w1);

                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)target);
                } else {
                    Gfx* jumpTarget = (Gfx*)target;

                    if (!gfx_translate_dl_cursor(&jumpTarget)) {
                        return;
                    }

                    cmd = jumpTarget;
                    --cmd; // increase after break
                }
                break;
            }
#if defined(TARGET_PSP) && defined(F3DEX_GBI_2)
            case (uint8_t)G_BG_1CYC:
            case (uint8_t)G_BG_COPY:
                gfx_sp_s2dex_bg_rect(opcode, seg_addr(cmd->words.w1));
                break;
#endif
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI_2) || defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
#ifdef F3DEX_GBI_2
            case (uint8_t)G_QUAD:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
            case G_NOOP:
                gfx_dp_noop(cmd->words.w1);
                break;
            case G_RDPSETOTHERMODE:
                gfx_dp_set_other_mode(cmd->words.w0 & 0x00FFFFFFU, cmd->words.w1);
                break;
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12) + 1, seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
            {
                uint32_t rgbA0 = C0(20, 4);
                uint32_t rgbB0 = C1(28, 4);
                uint32_t rgbC0 = C0(15, 5);
                uint32_t rgbD0 = C1(15, 3);
                uint32_t alphaA0 = C0(12, 3);
                uint32_t alphaB0 = C1(12, 3);
                uint32_t alphaC0 = C0(9, 3);
                uint32_t alphaD0 = C1(9, 3);
                uint32_t rgbA1 = C0(5, 4);
                uint32_t rgbB1 = C1(24, 4);
                uint32_t rgbC1 = C0(0, 5);
                uint32_t rgbD1 = C1(6, 3);
                uint32_t alphaA1 = C1(21, 3);
                uint32_t alphaB1 = C1(3, 3);
                uint32_t alphaC1 = C1(18, 3);
                uint32_t alphaD1 = C1(0, 3);
                bool colorMulTexelShade = gfx_cc_is_color_mul(rgbA0, rgbB0, rgbC0, rgbD0, G_CCMUX_TEXEL0,
                                                              G_CCMUX_SHADE);
                bool colorMulEnv = colorMulTexelShade && gfx_cc_is_color_mul(rgbA1, rgbB1, rgbC1, rgbD1,
                                                                              G_CCMUX_ENVIRONMENT,
                                                                              G_CCMUX_COMBINED);
                bool colorMulPrim = colorMulTexelShade && gfx_cc_is_color_mul(rgbA1, rgbB1, rgbC1, rgbD1,
                                                                               G_CCMUX_COMBINED,
                                                                               G_CCMUX_PRIMITIVE);
                bool colorMulShadePrim = gfx_cc_is_color_mul(rgbA0, rgbB0, rgbC0, rgbD0, G_CCMUX_SHADE,
                                                              G_CCMUX_PRIMITIVE);
                bool textureBlend = gfx_cc_is_texture_prim_env_blend(rgbA0, rgbB0, rgbC0, rgbD0);
                bool textureBlendShade = false;
                bool twoTextureBlend = false;
                uint32_t rgbComb = color_comb(rgbA0, rgbB0, rgbC0, rgbD0);
                uint32_t alphaComb = color_comb(alphaA0, alphaB0, alphaC0, alphaD0);

#if defined(TARGET_PSP)
                if (colorMulShadePrim) {
                    /*
                     * The GU has only one vertex-color input. Keep SHADE as that input and apply the constant
                     * PRIMITIVE tint on the CPU; selecting the last combiner input instead turns dark untextured
                     * geometry (such as the Temple of Time ceiling and entrance recess) primitive white.
                     */
                    rgbComb = color_comb(G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_SHADE);
                    colorMulPrim = true;
                }

                if (((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) &&
                    gfx_cc_is_one(alphaA0, alphaB0, alphaC0, alphaD0) &&
                    gfx_cc_is_combined_mul_primitive(alphaA1, alphaB1, alphaC1, alphaD1)) {
                    alphaComb = color_comb(G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_PRIMITIVE);
                }

                if (gfx_cc_is_two_cycle_texture_blend_mul_shade(rgbA0, rgbB0, rgbC0, rgbD0, rgbA1, rgbB1, rgbC1,
                                                               rgbD1)) {
                    // Draw this as two passes on the single-texture GU backend.
                    twoTextureBlend = true;
                    rgbComb = color_comb(G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0);
                    if (gfx_cc_is_combined_mul_primitive(alphaA1, alphaB1, alphaC1, alphaD1)) {
                        alphaComb = color_comb(G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_PRIMITIVE);
                    }
                }

                if (gfx_cc_is_two_cycle_texture_tint(rgbA0, rgbB0, rgbC0, rgbD0, rgbA1, rgbB1, rgbC1, rgbD1)) {
                    /*
                     * The second RDP cycle tints the first cycle between ENVIRONMENT and PRIMITIVE:
                     *
                     *   (PRIMITIVE - ENVIRONMENT) * COMBINED + ENVIRONMENT
                     *
                     * The GU cannot execute both cycles, so use TEXEL0 as the approximation for COMBINED and
                     * let GU_TFX_BLEND preserve both tint endpoints.  Modulating TEXEL0 by PRIMITIVE (the old
                     * fallback) discarded ENVIRONMENT completely, which made spiritual stones and other gems
                    * too dark and gave them the wrong hue.
                     */
                    rgbComb = color_comb(G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_PRIMITIVE, G_CCMUX_0);
                    if (gfx_cc_is_combined_mul_primitive(alphaA1, alphaB1, alphaC1, alphaD1) &&
                        !gfx_cc_is_one(alphaA0, alphaB0, alphaC0, alphaD0)) {
                        /* Translucent gems use the first-cycle texture result as their coverage. */
                        alphaComb = color_comb(G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_PRIMITIVE, G_ACMUX_0);
                    } else if (gfx_cc_is_one(alphaA1, alphaB1, alphaC1, alphaD1)) {
                        alphaComb = color_comb(G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1);
                    }
                    textureBlend = true;
                }

                if (gfx_cc_is_two_cycle_texture_shade_prim_tint(rgbA0, rgbB0, rgbC0, rgbD0, rgbA1, rgbB1,
                                                                rgbC1, rgbD1)) {
                    /*
                     * Recovery hearts tint their texture between PRIMITIVE and per-vertex SHADE in cycle two.
                     * Preserve both colored endpoints with the GU blend unit instead of displaying cycle one's
                     * grayscale texture.  The GU blend direction is reversed, but both endpoints are heart colors.
                     */
                    rgbComb = color_comb(G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0);
                    textureBlend = true;
                    textureBlendShade = true;
                }
#endif

                gfx_dp_set_combine_mode(rgbComb, alphaComb, colorMulEnv, colorMulPrim, textureBlend,
                                        textureBlendShade,
                                        twoTextureBlend);
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            }
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    rsp.rdp_half_1 = 0;
    sHudAnchor = OOT_PSP_HUD_ANCHOR_NONE;
    sHudViewportFullscreen = true;
    gfx_update_screen_metrics();
    memset(rsp.segments, 0, sizeof(rsp.segments));
#if defined(TARGET_PSP)
    memset(rsp.segment_cmd, 0, sizeof(rsp.segment_cmd));
    memset(&sCurrentCmd, 0, sizeof(sCurrentCmd));
#endif
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_set_dimensions(uint32_t width, uint32_t height) {
    if ((width == 0) || (height == 0) ||
        ((gfx_current_dimensions.width == width) && (gfx_current_dimensions.height == height))) {
        return;
    }

    gfx_current_dimensions.width = width;
    gfx_current_dimensions.height = height;
    gfx_update_screen_metrics();
    gfx_rapi->on_resize();
}


float times[30];
float time_avg;
float time_first_200;
int total_frame_counter;
int frame_counter;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();
    rendering_state.color_combiner_valid = false;
    rendering_state.tri_pipeline_dirty = true;
    rendering_state.backend_state_dirty = true;

    int i;
    for(i=0;i<30;i++){
        times[i] = 0.0f;
    }
    frame_counter = 0;
    time_avg = 0.0f;
    time_first_200 = 0;
    total_frame_counter = 0;

    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (size_t i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }

    memcpy(rsp.P_matrix, identity_matrix, sizeof(identity_matrix));
    memcpy(rsp.modelview_matrix_stack[0], identity_matrix, sizeof(identity_matrix));

    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_update_screen_metrics();
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

unsigned int total_t0, total_t1;

void gfx_start_frame(void) {
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
    gfx_wapi->handle_events();
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
    
    //INFO_MSG("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
    sUploadedProjectionValid = false;
    gfx_run_dl(commands);
    gfx_flush();
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
    //double t1 = gfx_wapi->get_time();

}

void gfx_end_frame(void) {
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }

}

void gfx_invalidate_render_state(void) {
    gfx_flush();

    if (gfx_rapi != NULL) {
        gfx_rapi->unload_shader(NULL);
    }

    rendering_state.shader_program = NULL;
    rendering_state.color_combiner_valid = false;
    rendering_state.tri_pipeline_dirty = true;
    rendering_state.backend_state_dirty = true;
    sUploadedProjectionValid = false;
}

void gfx_render_callback_frame(void (*draw_callback)(void *arg), void *arg) {
    gfx_start_frame();

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }

    dropped_frame = false;
    gfx_rapi->start_frame();
    sUploadedProjectionValid = false;

    if (draw_callback != NULL) {
        draw_callback(arg);
    }

    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
    gfx_end_frame();
    gfx_invalidate_render_state();
}
