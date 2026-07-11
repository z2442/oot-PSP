#ifndef OOT_PORT_PSP_OOT_PSP_RENDERER_H
#define OOT_PORT_PSP_OOT_PSP_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "sched.h"

void OotPspRenderer_Init(void);
void OotPspRenderer_RenderDisplayList(Gfx* dl);
void OotPspRenderer_RenderTask(const OSTask* task);
bool OotPspRenderer_IsDepthClear(int32_t x, int32_t y);
bool OotPspRenderer_DepthTest(int32_t x, int32_t y, float projectedZ);
void OotPspRenderer_SetJpegBackgroundResolution(bool active, uint32_t width, uint32_t height);
void OotPspRenderer_RequestPauseBackground(void);
void OotPspRenderer_SetPauseBackgroundActive(bool active);
void OotPspRenderer_RequestHomeMenuBackground(void);
void OotPspRenderer_SetHomeMenuBackgroundActive(bool active);
void OotPspRenderer_RenderHomeMenu(int selectedIndex, int screen, int controlSelectedIndex,
                                   const char* statusMessage);
void OotPspRenderer_RenderFirstBootProgress(uint32_t progressPermille, const char* statusMessage, bool error);

#endif
