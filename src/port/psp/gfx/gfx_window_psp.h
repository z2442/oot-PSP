#ifndef OOT_PORT_PSP_GFX_WINDOW_PSP_H
#define OOT_PORT_PSP_GFX_WINDOW_PSP_H

#include <stdbool.h>

#include "gfx_window_manager_api.h"

extern struct GfxWindowManagerAPI gfx_psp_window_api;

bool OotPspWindow_ShouldQuit(void);

#endif
