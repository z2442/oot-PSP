#ifndef OOT_PORT_PSP_OOT_PSP_RENDERER_H
#define OOT_PORT_PSP_OOT_PSP_RENDERER_H

#include "sched.h"

void OotPspRenderer_Init(void);
void OotPspRenderer_RenderDisplayList(Gfx* dl);
void OotPspRenderer_RenderTask(const OSTask* task);

#endif
