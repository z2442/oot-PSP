#include "oot_psp_renderer.h"

#include <stdbool.h>

#include "gfx/gfx_fast3d.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_psp.h"
#include "oot_psp_performance.h"
#include "oot_psp_video.h"

extern struct GfxRenderingAPI gfx_scegu_api;
extern void gfx_scegu_request_pause_background(void);
extern void gfx_scegu_set_pause_background_active(bool active);
extern void gfx_scegu_request_home_menu_background(void);
extern void gfx_scegu_set_home_menu_background_active(bool active);
extern int gfx_scegu_apply_video_mode(void);
extern void gfx_scegu_render_home_menu(int selectedIndex, int screen, int submenuSelectedIndex,
                                       const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen,
                                       uint8_t highlightBlue, int batteryPercent);
extern void gfx_scegu_render_first_boot_progress(uint32_t progressPermille, const char* statusMessage, bool error);
extern bool gfx_scegu_depth_is_clear(int32_t x, int32_t y);
extern bool gfx_scegu_depth_test(int32_t x, int32_t y, float projectedZ);

static bool sInitialized;

typedef struct OotPspHomeMenuRenderArgs {
    int selectedIndex;
    int screen;
    int submenuSelectedIndex;
    const char* statusMessage;
    uint8_t highlightRed;
    uint8_t highlightGreen;
    uint8_t highlightBlue;
    int batteryPercent;
} OotPspHomeMenuRenderArgs;

typedef struct OotPspFirstBootProgressRenderArgs {
    uint32_t progressPermille;
    const char* statusMessage;
    bool error;
} OotPspFirstBootProgressRenderArgs;

static void OotPspRenderer_DrawHomeMenu(void* arg) {
    const OotPspHomeMenuRenderArgs* menu = (const OotPspHomeMenuRenderArgs*)arg;

    gfx_scegu_render_home_menu(menu->selectedIndex, menu->screen, menu->submenuSelectedIndex, menu->statusMessage,
                               menu->highlightRed, menu->highlightGreen, menu->highlightBlue, menu->batteryPercent);
}

static void OotPspRenderer_DrawFirstBootProgress(void* arg) {
    const OotPspFirstBootProgressRenderArgs* progress = (const OotPspFirstBootProgressRenderArgs*)arg;

    gfx_scegu_render_first_boot_progress(progress->progressPermille, progress->statusMessage, progress->error);
}

void OotPspRenderer_Init(void) {
    if (sInitialized) {
        return;
    }

    gfx_init(&gfx_psp_window_api, &gfx_scegu_api, "oot-psp-port", false);
    sInitialized = true;
}

void OotPspRenderer_RenderDisplayList(Gfx* dl) {
#if defined(OOTDEBUG)
    uint64_t startUsec;
#endif

    OotPspRenderer_Init();
#if defined(OOTDEBUG)
    startUsec = OotPspPerformance_Now();
#endif
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
#if defined(OOTDEBUG)
    OotPspPerformance_RecordRenderer(OotPspPerformance_Now() - startUsec);
#endif
}

void OotPspRenderer_RenderTask(const OSTask* task) {
    Gfx* dl;

    if (task == NULL || task->t.data_ptr == NULL) {
        return;
    }

    dl = (Gfx*)task->t.data_ptr;
    OotPspRenderer_RenderDisplayList(dl);
}

bool OotPspRenderer_IsDepthClear(int32_t x, int32_t y) {
    OotPspRenderer_Init();
    return gfx_scegu_depth_is_clear(x, y);
}

bool OotPspRenderer_DepthTest(int32_t x, int32_t y, float projectedZ) {
    OotPspRenderer_Init();
    return gfx_scegu_depth_test(x, y, projectedZ);
}

void OotPspRenderer_SetJpegBackgroundResolution(bool active, uint32_t width, uint32_t height) {
    OotPspVideoMode mode;

    OotPspRenderer_Init();
    OotPspVideo_GetActiveMode(&mode);

    if (!active || (width == 0) || (height == 0) || (width > (uint32_t)mode.viewportWidth) ||
        (height > (uint32_t)mode.viewportHeight)) {
        width = (uint32_t)mode.viewportWidth;
        height = (uint32_t)mode.viewportHeight;
    }

    gfx_set_dimensions(width, height);
}

void OotPspRenderer_RequestPauseBackground(void) {
    gfx_scegu_request_pause_background();
}

void OotPspRenderer_SetPauseBackgroundActive(bool active) {
    gfx_scegu_set_pause_background_active(active);
}

void OotPspRenderer_RequestHomeMenuBackground(void) {
    gfx_scegu_request_home_menu_background();
}

void OotPspRenderer_SetHomeMenuBackgroundActive(bool active) {
    gfx_scegu_set_home_menu_background_active(active);
}

int OotPspRenderer_ApplyVideoSettings(void) {
    int result;

    OotPspRenderer_Init();
    result = gfx_scegu_apply_video_mode();
    /* The DVE transition reinitializes the GU and can invalidate texture data
     * already resident in the upper half of Slim EDRAM. Rebuild the cache on
     * the next clean frame for both the selected route and the LCD fallback. */
    gfx_invalidate_texture_cache();
    gfx_invalidate_render_state();
    return result;
}

void OotPspRenderer_RenderHomeMenu(int selectedIndex, int screen, int submenuSelectedIndex,
                                   const char* statusMessage, uint8_t highlightRed, uint8_t highlightGreen,
                                   uint8_t highlightBlue, int batteryPercent) {
    OotPspHomeMenuRenderArgs args;

    OotPspRenderer_Init();
    args.selectedIndex = selectedIndex;
    args.screen = screen;
    args.submenuSelectedIndex = submenuSelectedIndex;
    args.statusMessage = statusMessage;
    args.highlightRed = highlightRed;
    args.highlightGreen = highlightGreen;
    args.highlightBlue = highlightBlue;
    args.batteryPercent = batteryPercent;

    gfx_render_callback_frame(OotPspRenderer_DrawHomeMenu, &args);
}

void OotPspRenderer_RenderFirstBootProgress(uint32_t progressPermille, const char* statusMessage, bool error) {
    OotPspFirstBootProgressRenderArgs args;

    OotPspRenderer_Init();
    args.progressPermille = progressPermille;
    args.statusMessage = statusMessage;
    args.error = error;

    gfx_render_callback_frame(OotPspRenderer_DrawFirstBootProgress, &args);
}
