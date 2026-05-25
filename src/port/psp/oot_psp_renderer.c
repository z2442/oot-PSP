#include "oot_psp_renderer.h"

#include <stdbool.h>

#include "gfx/gfx_fast3d.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_psp.h"

extern struct GfxRenderingAPI gfx_scegu_api;
extern void gfx_scegu_request_pause_background(void);
extern void gfx_scegu_set_pause_background_active(bool active);

static bool sInitialized;

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
