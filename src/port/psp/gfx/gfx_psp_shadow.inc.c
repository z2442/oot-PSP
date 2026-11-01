/* Included by gfx_scegu.c: shares its list allocator and cached GU state. */
typedef struct ShadowVertex {
    uint16_t u, v;
    uint32_t color;
    uint16_t x, y, z, padding;
} ShadowVertex;
typedef char ShadowVertex_size_check[(sizeof(ShadowVertex) == 16) ? 1 : -1];

static uint32_t sShadowStampAtlas[128 * 16] __attribute__((aligned(64)));
static bool sShadowStampAtlasReady;

static void gfx_scegu_init_shadow_atlas(void) {
    static const uint8_t heights[7] = {7, 8, 12, 6, 14, 3, 3};
    static const uint8_t topOffsets[7] = {3, 4, 6, 2, 7, 1, 1};
    static const uint8_t widths[7][14] = {
        {2, 3, 4, 4, 4, 3, 2},
        {2, 3, 4, 4, 4, 4, 3, 2},
        {2, 4, 5, 5, 6, 6, 6, 6, 5, 5, 4, 2},
        {1, 2, 3, 3, 2, 1},
        {2, 4, 5, 6, 7, 8, 8, 8, 8, 7, 6, 5, 4, 2},
        {3, 3, 3}, {1, 1, 1},
    };
    if (sShadowStampAtlasReady) return;
    /* These immutable profiles replace the game's per-frame pixel loops. */
    for (int type = 0; type < 7; type++) {
        for (int row = 0; row < heights[type]; row++) {
            const int width = widths[type][row];
            const int end = width + (type >= 5 ? 1 : (type == 0 || type == 3 ? -1 : 0));
            for (int x = -width; x < end; x++) {
                sShadowStampAtlas[(8 - topOffsets[type] + row) * 128 + type * 16 + 8 + x] = 0xffffffffU;
            }
        }
    }
    sceKernelDcacheWritebackRange(sShadowStampAtlas, sizeof(sShadowStampAtlas));
    sShadowStampAtlasReady = true;
}

static unsigned int gfx_scegu_shadow_sprite(ShadowVertex* out, int x, int y, int width, int height,
                                           int u, int v, uint32_t color) {
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > 64 ? 64 : x + width;
    int bottom = y + height > 64 ? 64 : y + height;
    if (left >= right || top >= bottom) return 0;
    out[0] = (ShadowVertex){u + left - x, v + top - y, color, left, top, 0, 0};
    out[1] = (ShadowVertex){u + right - x, v + bottom - y, color, right, bottom, 0, 0};
    return 2;
}

static unsigned int gfx_scegu_shadow_stamps(ShadowVertex* vertices, const OotPspShadowMask* mask) {
    unsigned int count = 0;
    const unsigned int stampCount = mask->count < OOT_PSP_SHADOW_MAX_STAMPS
                                       ? mask->count : OOT_PSP_SHADOW_MAX_STAMPS;
    for (unsigned int i = 0; i < stampCount; i++) {
        const OotPspShadowStamp* stamp = &mask->stamps[i];
        if (stamp->type >= 7) continue;
        int x = stamp->x, y = stamp->y;
        if (mask->flags & OOT_PSP_SHADOW_WRAP_ROWS) {
            /* Boss code clips linear pixel indices, so crossing X=0/64
             * wraps into the neighboring row. Preserve that edge behavior. */
            const int row = x >= 0 ? x / 64 : (x - 63) / 64;
            x -= row * 64;
            y += row;
            for (int offset = -1; offset <= 1; offset++) {
                count += gfx_scegu_shadow_sprite(vertices + count, x + offset * 64 - 8,
                                                 y - offset - 8, 16, 16, stamp->type * 16, 0, 0xffffffffU);
            }
        } else {
            count += gfx_scegu_shadow_sprite(vertices + count, x - 8, y - 8, 16, 16,
                                             stamp->type * 16, 0, 0xffffffffU);
        }
    }
    return count;
}

static void gfx_scegu_shadow_draw(const ShadowVertex* vertices, unsigned int count) {
    if (count) {
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       count, NULL, vertices);
    }
}

static void gfx_scegu_shadow_copy(struct PSP_Texture* texture) {
    sceGuTexSync();
    sceGuCopyImage(GU_PSM_8888, 0, 0, 64, 64, 64, gfx_scegu_vram_cpu_addr(sShadowMaskBuffer),
                   0, 0, 64, texture->location);
    sceGuTexSync();
    sceGuTexFlush();
}

void gfx_scegu_upload_shadow_mask(int tile, unsigned int textureId, const OotPspShadowMask* mask) {
    struct PSP_Texture* texture = texman_prepare_gpu_texture(textureId, 64, 64, GU_PSM_8888);
    if (texture == NULL) {
        /* The interpreter reserves the full RGBA allocation before this call. */
        abort();
    }
    gfx_scegu_init_shadow_atlas();
    const unsigned int status = sceGuGetAllStatus();
    struct ShaderProgram* restoreShader = sAppliedShader;
    const unsigned int stampCount = mask->count < OOT_PSP_SHADOW_MAX_STAMPS
                                       ? mask->count : OOT_PSP_SHADOW_MAX_STAMPS;
    const unsigned int copies = mask->flags & OOT_PSP_SHADOW_WRAP_ROWS ? 3 : 1;
    const size_t vertexBytes = sizeof(ShadowVertex) * (2 + 8 + stampCount * copies * 2 + 128);
    gfx_scegu_reserve_list_memory(vertexBytes);
    ShadowVertex* vertices = sceGuGetMemory(vertexBytes);
    gfx_scegu_shadow_sprite(vertices, 0, 0, 64, 64, 0, 0, 0);
    unsigned int borderCount = 0;
    ShadowVertex* border = vertices + 2;
    if (mask->flags & OOT_PSP_SHADOW_BORDER) {
        borderCount += gfx_scegu_shadow_sprite(border + borderCount, 0, 0, 64, 1, 0, 0, 0xffffffffU);
        borderCount += gfx_scegu_shadow_sprite(border + borderCount, 0, 63, 64, 1, 0, 0, 0xffffffffU);
        borderCount += gfx_scegu_shadow_sprite(border + borderCount, 0, 1, 1, 62, 0, 0, 0xffffffffU);
        borderCount += gfx_scegu_shadow_sprite(border + borderCount, 63, 1, 1, 62, 0, 0, 0xffffffffU);
    }
    ShadowVertex* stamps = border + borderCount;
    const unsigned int vertexCount = gfx_scegu_shadow_stamps(stamps, mask);

    sceGuDrawBufferList(GU_PSM_8888, sShadowMaskBuffer, 64);
    sceGuScissor(0, 0, 64, 64);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuPixelMask(0);
    /* GE framebuffer alpha comes from stencil replacement, not fragment
     * alpha. Alpha testing discards transparent atlas texels before stamping. */
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
    gfx_scegu_shadow_draw(vertices, 2);
    sceGuStencilFunc(GU_ALWAYS, 255, 0xff);
    gfx_scegu_shadow_draw(border, borderCount);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, 128, 16, 128, sShadowStampAtlas);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    sceGuEnable(GU_ALPHA_TEST);
    gfx_scegu_shadow_draw(stamps, vertexCount);
    gfx_scegu_shadow_copy(texture);

    if (mask->flags & OOT_PSP_SHADOW_GRADIENT) {
        /* Preserve Link's debug row fade using a second GPU pass. Sampling
         * the copied mask avoids reading and writing the same render target. */
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
        gfx_scegu_shadow_draw(vertices, 2);
        sceGuTexImage(0, 64, 64, 64, texture->location);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuEnable(GU_ALPHA_TEST);
        ShadowVertex* rows = stamps + vertexCount;
        for (int y = 0; y < 64; y++) {
            const uint8_t value = mask->gradientBase - y * mask->gradientStep;
            const uint32_t color = (uint32_t)value * 0x01010101U;
            gfx_scegu_shadow_sprite(rows + y * 2, 0, y, 64, 1, 0, y, color);
            sceGuStencilFunc(GU_ALWAYS, value, 0xff);
            gfx_scegu_shadow_draw(rows + y * 2, 2);
        }
        gfx_scegu_shadow_copy(texture);
    }

    sceGuDrawBufferList(sColorPixelFormat, sDrawBuffer, BUF_WIDTH);
    sceGuScissor(sScissorBounds[0], sScissorBounds[1], sScissorBounds[2], sScissorBounds[3]);
    sceGuDepthMask(sDepthWriteEnabled ? GU_FALSE : GU_TRUE);
    sceGuTexEnvColor(sTextureEnvColor);
    sAppliedShader = NULL;
    gfx_scegu_apply_shader(restoreShader);
    sceGuSetAllStatus(status);
    texman_invalidate_binding();
    sAppliedSamplerStateValid = false;
    gfx_scegu_select_texture(tile, textureId);
}
