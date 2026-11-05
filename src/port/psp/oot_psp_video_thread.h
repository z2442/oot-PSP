#ifndef OOT_PSP_VIDEO_THREAD_H
#define OOT_PSP_VIDEO_THREAD_H

/* One finished GU list may be in flight. Only the game/render thread submits
 * or waits; the callback owns GU/display state until Wait returns. */
void OotPspVideoThread_Submit(void (*completeFrame)(void));
void OotPspVideoThread_Wait(void);

#endif
