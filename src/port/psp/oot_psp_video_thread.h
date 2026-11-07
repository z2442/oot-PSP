#ifndef OOT_PSP_VIDEO_THREAD_H
#define OOT_PSP_VIDEO_THREAD_H

#include <stdint.h>

/* One finished GU list may be in flight. Only the game/render thread submits
 * or waits; the callback owns GU/display state until Wait returns. */
void OotPspVideoThread_Submit(void (*completeFrame)(void));
void OotPspVideoThread_Wait(void);

#if OOT_PSP_WAKE_DIAGNOSTICS
uint32_t OotPspVideoThread_GetCompletedFrames(void);
#endif

#endif
