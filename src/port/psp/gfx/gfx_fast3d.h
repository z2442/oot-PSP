#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stdint.h>

#include "ultra64.h"

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name,
              bool start_in_fullscreen);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);
void gfx_set_dimensions(uint32_t width, uint32_t height);
void gfx_render_callback_frame(void (*draw_callback)(void *arg), void *arg);
void gfx_invalidate_render_state(void);

#ifdef __cplusplus
}
#endif

#endif
