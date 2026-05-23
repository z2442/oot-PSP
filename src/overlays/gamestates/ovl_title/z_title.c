/*
 * File: z_title.c
 * Overlay: ovl_title
 * Description: Displays the Nintendo Logo
 */

#include "libu64/gfxprint.h"
#if PLATFORM_N64
#include "cic6105.h"
#include "n64dd.h"
#endif

#include "alloca.h"
#include "build.h"
#include "console_logo_state.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "padmgr.h"
#include "printf.h"
#include "regs.h"
#include "segment_symbols.h"
#include "sequence.h"
#include "sys_matrix.h"
#include "sys_debug_controller.h"
#include "sys_freeze.h"
#include "title_setup_state.h"
#include "versions.h"
#include "actor.h"
#include "environment.h"
#include "save.h"
#if PLATFORM_PSP
#include "oot_psp_asset_loader.h"
#include "oot_psp_compat.h"
#endif

#include "assets/textures/nintendo_rogo_static/nintendo_rogo_static.h"

#if DEBUG_FEATURES
void ConsoleLogo_PrintBuildInfo(Gfx** gfxP) {
    Gfx* gfx;
    GfxPrint* printer;

    gfx = *gfxP;
    gfx = Gfx_SetupDL_28(gfx);
    printer = alloca(sizeof(GfxPrint));
    GfxPrint_Init(printer);
    GfxPrint_Open(printer, gfx);
    GfxPrint_SetColor(printer, 255, 155, 255, 255);
    GfxPrint_SetPos(printer, 9, 21);
    GfxPrint_Printf(printer, "NOT MARIO CLUB VERSION");
    GfxPrint_SetColor(printer, 255, 255, 255, 255);
    GfxPrint_SetPos(printer, 7, 23);
    GfxPrint_Printf(printer, "[Creator:%s]", gBuildCreator);
    GfxPrint_SetPos(printer, 7, 24);
    GfxPrint_Printf(printer, "[Date:%s]", gBuildDate);
    gfx = GfxPrint_Close(printer);
    GfxPrint_Destroy(printer);
    *gfxP = gfx;
}
#endif

void ConsoleLogo_Calc(ConsoleLogoState* this) {
#if !PLATFORM_GC
    if ((this->coverAlpha == 0) && (this->visibleDuration != 0)) {
        this->unk_1D4--;
        this->visibleDuration--;
        if (this->unk_1D4 == 0) {
            this->unk_1D4 = 400;
        }
    } else {
        this->coverAlpha += this->addAlpha;
        if (this->coverAlpha <= 0) {
            this->coverAlpha = 0;
            this->addAlpha = 3;
        } else if (this->coverAlpha >= 255) {
            this->coverAlpha = 255;
            this->exit = true;
        }
    }
    this->uls = this->ult & 0x7F;
    this->ult++;
#else
    this->exit = true;
#endif
}

void ConsoleLogo_SetupView(ConsoleLogoState* this, f32 x, f32 y, f32 z) {
    View* view = &this->view;
    Vec3f eye;
    Vec3f lookAt;
    Vec3f up;

    eye.x = x;
    eye.y = y;
    eye.z = z;
    up.x = up.z = 0.0f;
    up.y = 1.0f;
    lookAt.x = lookAt.y = lookAt.z = 0.0f;

    View_SetPerspective(view, 30.0f, 10.0f, 12800.0f);
    View_LookAt(view, &eye, &lookAt, &up);
    View_Apply(view, VIEW_ALL);
}

#if PLATFORM_PSP
typedef struct ConsoleLogoPspTexture {
    /* 0x00 */ const u8* data;
    /* 0x04 */ s32 byteSwap;
} ConsoleLogoPspTexture;

static ConsoleLogoPspTexture ConsoleLogo_GetPspTexture(void* texture) {
    ConsoleLogoPspTexture source;
    u32 loadedFlags;

    source.data = texture;
    source.byteSwap = false;

    if (OotPsp_GetLoadedExternalAssetRangeFlags(source.data, 1, &loadedFlags)) {
        source.byteSwap = (loadedFlags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0;
    } else if (!OotPsp_IsRuntimeByteRange(source.data, 1)) {
        source.byteSwap = true;
    }

    return source;
}

static u8 ConsoleLogo_ReadPspTextureByte(ConsoleLogoPspTexture texture, size_t offset) {
    const u8* source = texture.data + offset;

    if (texture.byteSwap) {
        source = (const u8*)((uintptr_t)source ^ 7U);
    }

    return *source;
}

static u8 ConsoleLogo_ReadI8Texel(ConsoleLogoPspTexture texture, s16 width, s16 height, s16 x, s16 y) {
    x %= width;
    y %= height;

    if (x < 0) {
        x += width;
    }
    if (y < 0) {
        y += height;
    }

    return ConsoleLogo_ReadPspTextureByte(texture, y * width + x);
}

static void ConsoleLogo_BuildPspWordmarkTexture(ConsoleLogoState* this, u8* dst) {
    ConsoleLogoPspTexture wordmarkTex = ConsoleLogo_GetPspTexture(nintendo_rogo_static_Tex_000000);
    ConsoleLogoPspTexture effectTex = ConsoleLogo_GetPspTexture(nintendo_rogo_static_Tex_001800);
    s16 scrollS = (this->uls >> G_TEXTURE_IMAGE_FRAC) & 0x1F;
    s16 scrollT = (this->ult >> G_TEXTURE_IMAGE_FRAC) & 0x1F;
    s16 x;
    s16 y;

    for (y = 0; y < nintendo_rogo_static_Tex_000000_HEIGHT; y++) {
        for (x = 0; x < nintendo_rogo_static_Tex_000000_WIDTH; x++) {
            u8 alpha = ConsoleLogo_ReadI8Texel(wordmarkTex, nintendo_rogo_static_Tex_000000_WIDTH,
                                               nintendo_rogo_static_Tex_000000_HEIGHT, x, y);
            u8 effect = ConsoleLogo_ReadI8Texel(effectTex, nintendo_rogo_static_Tex_001800_WIDTH,
                                                nintendo_rogo_static_Tex_001800_HEIGHT, (x >> 2) + scrollS,
                                                y + scrollT);

            *dst++ = 128 + (effect >> 1);
            *dst++ = alpha;
        }
    }
}

static void ConsoleLogo_DrawPspWordmark(ConsoleLogoState* this) {
    Gfx* gfx = this->state.gfxCtx->polyOpa.p;
    u8* texture = GRAPH_ALLOC(this->state.gfxCtx, nintendo_rogo_static_Tex_000000_WIDTH *
                                                       nintendo_rogo_static_Tex_000000_HEIGHT * 2);
    u8* textureSlice;
    u16 idx;
    u16 y;

    if (texture == NULL) {
        return;
    }

    ConsoleLogo_BuildPspWordmarkTexture(this, texture);

    gDPPipeSync(gfx++);
    gDPSetCycleType(gfx++, G_CYC_1CYCLE);
    gDPSetRenderMode(gfx++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
    gDPSetCombineMode(gfx++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetPrimColor(gfx++, 0, 0, 170, 255, 255, 255);

    for (idx = 0, y = 94, textureSlice = texture; idx < 16;
         idx++, y += 2, textureSlice += nintendo_rogo_static_Tex_000000_WIDTH * 2 * 2) {
        gDPLoadTextureBlock(gfx++, textureSlice, G_IM_FMT_IA, G_IM_SIZ_16b, nintendo_rogo_static_Tex_000000_WIDTH, 2,
                            0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                            G_TX_NOLOD, G_TX_NOLOD);
        gSPTextureRectangle(gfx++, 97 << 2, y << 2, 289 << 2, (y + 2) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10,
                            1 << 10);
    }

    this->state.gfxCtx->polyOpa.p = gfx;
}
#endif

void ConsoleLogo_Draw(ConsoleLogoState* this) {
    static s16 sTitleRotY = 0;
    static Lights1 sTitleLights = gdSPDefLights1(100, 100, 100, 255, 255, 255, 69, 69, 69);

    u16 y;
    u16 idx;
    s32 pad1;
    Vec3f v3;
    Vec3f v1;
    Vec3f v2;
    s32 pad2;
    s32 pad3;

    OPEN_DISPS(this->state.gfxCtx, "../z_title.c", 395);

    v3.x = 69;
    v3.y = 69;
    v3.z = 69;
    v2.x = -4949.148;
    v2.y = 4002.5417;
    v1.x = 0;
    v1.y = 0;
    v1.z = 0;
    v2.z = 1119.0837;

    func_8002EABC(&v1, &v2, &v3, this->state.gfxCtx);
    gSPSetLights1(POLY_OPA_DISP++, sTitleLights);
    ConsoleLogo_SetupView(this, 0, 150.0, 300.0);
    Gfx_SetupDL_25Opa(this->state.gfxCtx);
    Matrix_Translate(-53.0, -5.0, 0, MTXMODE_NEW);
    Matrix_Scale(1.0, 1.0, 1.0, MTXMODE_APPLY);
    Matrix_RotateZYX(0, sTitleRotY, 0, MTXMODE_APPLY);

    MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, this->state.gfxCtx, "../z_title.c", 424);
    gSPDisplayList(POLY_OPA_DISP++, gNintendo64LogoDL);
#if PLATFORM_PSP
    /*
     * The PSP backend imports this boot logo's I8 texture with alpha, so a
     * single pass reads too translucent. Layer it locally instead of changing
     * global I8 alpha handling, which is shared by HUD and effect materials.
     */
    gSPDisplayList(POLY_OPA_DISP++, gNintendo64LogoDL);
    gSPDisplayList(POLY_OPA_DISP++, gNintendo64LogoDL);
    gSPDisplayList(POLY_OPA_DISP++, gNintendo64LogoDL);
#endif
    Gfx_SetupDL_39Opa(this->state.gfxCtx);
#if PLATFORM_PSP
    ConsoleLogo_DrawPspWordmark(this);
#else
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_2CYCLE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_PASS, G_RM_CLD_SURF2);
    gDPSetCombineLERP(POLY_OPA_DISP++, TEXEL1, PRIMITIVE, ENV_ALPHA, TEXEL0, 0, 0, 0, TEXEL0, PRIMITIVE, ENVIRONMENT,
                      COMBINED, ENVIRONMENT, COMBINED, 0, PRIMITIVE, 0);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 170, 255, 255, 255);
    gDPSetEnvColor(POLY_OPA_DISP++, 0, 0, 255, 128);

    gDPLoadMultiBlock(POLY_OPA_DISP++, nintendo_rogo_static_Tex_001800, 0x100, 1, G_IM_FMT_I, G_IM_SIZ_8b, 32, 32, 0,
                      G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 5, 5, 2, 11);

    for (idx = 0, y = 94; idx < 16; idx++, y += 2) {
        gDPLoadTextureBlock(POLY_OPA_DISP++, &((u8*)nintendo_rogo_static_Tex_000000)[0x180 * idx], G_IM_FMT_I,
                            G_IM_SIZ_8b, 192, 2, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                            G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

        gDPSetTileSize(POLY_OPA_DISP++, 1, this->uls, (this->ult & 0x7F) - (idx << 2), 0, 0);
        gSPTextureRectangle(POLY_OPA_DISP++, 97 << 2, y << 2, 289 << 2, (y + 2) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10,
                            1 << 10);
    }
#endif

    Environment_FillScreen(this->state.gfxCtx, 0, 0, 0, (s16)this->coverAlpha, FILL_SCREEN_XLU);

    sTitleRotY += 300;

    CLOSE_DISPS(this->state.gfxCtx, "../z_title.c", 483);
}

void ConsoleLogo_Main(GameState* thisx) {
    ConsoleLogoState* this = (ConsoleLogoState*)thisx;

    OPEN_DISPS(this->state.gfxCtx, "../z_title.c", 494);

    gSPSegment(POLY_OPA_DISP++, 0, NULL);
    gSPSegment(POLY_OPA_DISP++, 1, this->staticSegment);
    Gfx_SetupFrame(this->state.gfxCtx, 0, 0, 0);
    ConsoleLogo_Calc(this);
    ConsoleLogo_Draw(this);

#if DEBUG_FEATURES
    if (gIsCtrlr2Valid) {
        Gfx* gfx = POLY_OPA_DISP;

        ConsoleLogo_PrintBuildInfo(&gfx);
        POLY_OPA_DISP = gfx;
    }
#endif

#if PLATFORM_IQUE
    this->exit = true;
#endif

    if (this->exit) {
        gSaveContext.seqId = (u8)NA_BGM_DISABLED;
        gSaveContext.natureAmbienceId = 0xFF;
        gSaveContext.gameMode = GAMEMODE_TITLE_SCREEN;
        this->state.running = false;
        SET_NEXT_GAMESTATE(&this->state, TitleSetup_Init, TitleSetupState);
    }

    CLOSE_DISPS(this->state.gfxCtx, "../z_title.c", 541);
}

void ConsoleLogo_Destroy(GameState* thisx) {
    ConsoleLogoState* this = (ConsoleLogoState*)thisx;

#if PLATFORM_N64
    if (this->unk_1E0) {
        if (func_801C7818() != 0) {
            Freeze_CurrentThread();
        }
        func_801C7268();
    }
#endif

    Sram_InitSram(&this->state, &this->sramCtx);

#if PLATFORM_N64
    func_800014E8();
#endif
}

void ConsoleLogo_Init(GameState* thisx) {
    ConsoleLogoState* this = (ConsoleLogoState*)thisx;
#if !PLATFORM_PSP
    u32 size = (uintptr_t)_nintendo_rogo_staticSegmentRomEnd - (uintptr_t)_nintendo_rogo_staticSegmentRomStart;
#endif

#if PLATFORM_N64
    if ((D_80121210 != 0) && (D_80121211 != 0) && (D_80121212 == 0)) {
        if (func_801C7658() != 0) {
            Freeze_CurrentThread();
        }
        this->unk_1E0 = true;
    } else {
        this->unk_1E0 = false;
    }
#endif

#if PLATFORM_PSP
    this->staticSegment = (void*)nintendo_rogo_static_Tex_000000;
#else
    this->staticSegment = GAME_STATE_ALLOC(&this->state, size, "../z_title.c", 611);
    PRINTF("z_title.c\n");
    ASSERT(this->staticSegment != NULL, "this->staticSegment != NULL", "../z_title.c", 614);
    DMA_REQUEST_SYNC(this->staticSegment, (uintptr_t)_nintendo_rogo_staticSegmentRomStart, size, "../z_title.c", 615);
#endif
    R_UPDATE_RATE = 1;
    Matrix_Init(&this->state);
    View_Init(&this->view, this->state.gfxCtx);
    this->state.main = ConsoleLogo_Main;
    this->state.destroy = ConsoleLogo_Destroy;
    this->exit = false;

#if OOT_VERSION < GC_US || PLATFORM_IQUE
    if (!(gPadMgr.validCtrlrsMask & 1)) {
        gSaveContext.fileNum = 0xFEDC;
    } else {
        gSaveContext.fileNum = 0xFF;
    }
#else
    gSaveContext.fileNum = 0xFF;
#endif

    Sram_Alloc(&this->state, &this->sramCtx);
    this->ult = 0;
    this->unk_1D4 = 0x14;
    this->coverAlpha = 255;
    this->addAlpha = -3;
    this->visibleDuration = 0x3C;
}
