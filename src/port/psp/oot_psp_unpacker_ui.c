#include "oot_psp_unpacker_ui.h"
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>
#if OOT_PSP_USE_INTRAFONT
#include <intraFont.h>
#else
#define INTRAFONT_ALIGN_CENTER 0
#endif

#define HOME_MENU_WIDTH 480
#define HOME_MENU_HEIGHT 272
static unsigned int sList[16384] __attribute__((aligned(16)));
static bool sInitialized;
static void* sDrawBuffer;
#if OOT_PSP_USE_INTRAFONT
static intraFont* sFont;
#endif

static unsigned int gfx_scegu_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
static void gfx_scegu_prepare_home_menu_draw(void) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}
static void gfx_scegu_draw_rect(int x, int y, int width, int height, unsigned int color) {
    struct Vertex { unsigned int color; short x, y, z; };
    struct Vertex* v = sceGuGetMemory(2 * sizeof(*v));
    if (!v || width <= 0 || height <= 0) return;
    v[0] = (struct Vertex){color, x, y, 0};
    v[1] = (struct Vertex){color, x + width, y + height, 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
}
static void gfx_scegu_draw_home_menu_text(int x, int y, const char* text, float size,
                                         unsigned color, unsigned shadow, unsigned align) {
#if OOT_PSP_USE_INTRAFONT
    if (sFont) {
        intraFontSetStyle(sFont, size, color, shadow, 0, align);
        intraFontPrint(sFont, x, y, text);
        return;
    }
#endif
    /* The graphical panel and bar remain available if flash fonts are absent. */
    sceGuFinish();
    sceGuSync(0, 0);
    pspDebugScreenSetOffset((int)(uintptr_t)sDrawBuffer);
    pspDebugScreenSetTextColor(color);
    pspDebugScreenSetBackColor(0xFF000000);
    pspDebugScreenSetXY((x - (int)strlen(text) * 3) / 7, (y - 8) / 8);
    pspDebugScreenPrintf("%s", text);
    sceGuStart(GU_DIRECT, sList);
    gfx_scegu_prepare_home_menu_draw();
}
#include "oot_psp_first_boot_ui.inc.c"

void OotPspUnpackerUI_Draw(uint32_t progress, const char* status, bool error) {
    if (!sInitialized) {
        sceGuInit();
        sceGuStart(GU_DIRECT, sList);
        sceGuDrawBuffer(GU_PSM_8888, NULL, 512);
        sceGuDispBuffer(480, 272, (void*)(512 * 272 * 4), 512);
        sceGuOffset(2048 - 240, 2048 - 136);
        sceGuViewport(2048, 2048, 480, 272);
        sceGuScissor(0, 0, 480, 272);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuFinish(); sceGuSync(0, 0);
        sceDisplayWaitVblankStart(); sceGuDisplay(GU_TRUE);
#if OOT_PSP_USE_INTRAFONT
        if (intraFontInit()) sFont = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
#endif
        sInitialized = true;
    }
    sceGuStart(GU_DIRECT, sList);
    gfx_scegu_render_first_boot_progress(progress, status, error);
    sceGuFinish(); sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sDrawBuffer = sceGuSwapBuffers();
}

void OotPspUnpackerUI_Shutdown(void) {
    if (!sInitialized) return;
    sceGuSync(0, 0);
#if OOT_PSP_USE_INTRAFONT
    if (sFont) { intraFontUnload(sFont); sFont = NULL; }
    intraFontShutdown();
#endif
    sceGuTerm();
    sInitialized = false;
}
