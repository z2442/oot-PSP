#include "oot_psp_renderer.h"

#include <stdbool.h>

#include "gfx/gfx_fast3d.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_psp.h"

extern struct GfxRenderingAPI gfx_scegu_api;
extern void gfx_scegu_request_pause_background(void);
extern void gfx_scegu_set_pause_background_active(bool active);
extern void gfx_scegu_request_home_menu_background(void);
extern void gfx_scegu_set_home_menu_background_active(bool active);
extern void gfx_scegu_render_home_menu(int selectedIndex, int screen, int controlSelectedIndex,
                                       const char* statusMessage);

static bool sInitialized;

typedef struct OotPspHomeMenuRenderArgs {
    int selectedIndex;
    int screen;
    int controlSelectedIndex;
    const char* statusMessage;
} OotPspHomeMenuRenderArgs;

static void OotPspRenderer_DrawHomeMenu(void* arg) {
    const OotPspHomeMenuRenderArgs* menu = (const OotPspHomeMenuRenderArgs*)arg;

    gfx_scegu_render_home_menu(menu->selectedIndex, menu->screen, menu->controlSelectedIndex, menu->statusMessage);
}

void OotPspRenderer_Init(void) {
    if (sInitialized) {
        return;
    }

    gfx_init(&gfx_psp_window_api, &gfx_scegu_api, "oot-psp-port", false);
    sInitialized = true;
}

void OotPspRenderer_RenderDisplayList(Gfx* dl) {
    OotPspRenderer_Init();
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
}

void OotPspRenderer_RenderTask(const OSTask* task) {
    Gfx* dl;

    if (task == NULL || task->t.data_ptr == NULL) {
        return;
    }

    dl = (Gfx*)task->t.data_ptr;
    OotPspRenderer_RenderDisplayList(dl);
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

void OotPspRenderer_RenderHomeMenu(int selectedIndex, int screen, int controlSelectedIndex,
                                   const char* statusMessage) {
    OotPspHomeMenuRenderArgs args;

    OotPspRenderer_Init();
    args.selectedIndex = selectedIndex;
    args.screen = screen;
    args.controlSelectedIndex = controlSelectedIndex;
    args.statusMessage = statusMessage;

    gfx_render_callback_frame(OotPspRenderer_DrawHomeMenu, &args);
}
