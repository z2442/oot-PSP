#include "oot_psp_video_thread.h"

#include <pspkernel.h>
#include <stdbool.h>

static SceUID sRequestSema = -1;
static SceUID sCompleteSema = -1;
static bool sInitializationAttempted;
static bool sThreadReady;
static bool sFramePending;
static void (*sCompleteFrame)(void);

static int OotPspVideoThread_Run(SceSize args, void* argp) {
    (void)args;
    (void)argp;

    for (;;) {
        sceKernelWaitSema(sRequestSema, 1, NULL);
        sCompleteFrame();
        sceKernelSignalSema(sCompleteSema, 1);
    }
    return 0;
}

static void OotPspVideoThread_Init(void) {
    SceUID thread;

    if (sInitializationAttempted) {
        return;
    }
    sInitializationAttempted = true;
    sRequestSema = sceKernelCreateSema("OOT PSP VideoReq", 0, 0, 1, NULL);
    if (sRequestSema < 0) {
        return;
    }
    sCompleteSema = sceKernelCreateSema("OOT PSP VideoDone", 0, 0, 1, NULL);
    if (sCompleteSema < 0) {
        sceKernelDeleteSema(sRequestSema);
        return;
    }

    /* Above the game (0x20), below AudioOut (0x1e). The worker sleeps during
     * GE/vblank waits, leaving Allegrex available for the following tick. */
    thread = sceKernelCreateThread("OOT PSP Video", OotPspVideoThread_Run, 0x1f, 0x10000,
                                   PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (thread >= 0) {
        if (sceKernelStartThread(thread, 0, NULL) >= 0) {
            sThreadReady = true;
            return;
        }
        sceKernelDeleteThread(thread);
    }
    sceKernelDeleteSema(sCompleteSema);
    sceKernelDeleteSema(sRequestSema);
}

void OotPspVideoThread_Wait(void) {
    if (sFramePending) {
        sceKernelWaitSema(sCompleteSema, 1, NULL);
        sFramePending = false;
    }
}

void OotPspVideoThread_Submit(void (*completeFrame)(void)) {
    OotPspVideoThread_Wait();
    OotPspVideoThread_Init();
    if (!sThreadReady) {
        /* Preserve presentation on systems without another thread/semaphore. */
        completeFrame();
        return;
    }
    sCompleteFrame = completeFrame;
    sFramePending = true;
    sceKernelSignalSema(sRequestSema, 1);
}
