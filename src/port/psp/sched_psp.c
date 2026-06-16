#include "sched.h"

#include "array_count.h"
#include "oot_psp_renderer.h"

#include <pspkernel.h>

#define OOT_PSP_VI_RATE_HZ 60
#define OOT_PSP_MAX_PACING_LAG_USEC 250000ULL

static u64 sNextGfxCompletionUsec;

static s32 SchedPsp_GetUpdateRate(OSScTask* task) {
    s32 updateRate = 1;

    if (task != NULL && task->framebuffer != NULL && task->framebuffer->updateRate > 0) {
        updateRate = task->framebuffer->updateRate;
    }

    return updateRate;
}

static void SchedPsp_PaceGfxTask(OSScTask* task) {
    u64 now = sceKernelGetSystemTimeWide();
    u64 frameUsec = (1000000ULL * (u64)SchedPsp_GetUpdateRate(task)) / OOT_PSP_VI_RATE_HZ;

    if (frameUsec == 0) {
        frameUsec = 1;
    }

    if (sNextGfxCompletionUsec == 0 || now > sNextGfxCompletionUsec + OOT_PSP_MAX_PACING_LAG_USEC) {
        sNextGfxCompletionUsec = now;
    }

    if (now < sNextGfxCompletionUsec) {
        sceKernelDelayThread((u32)(sNextGfxCompletionUsec - now));
        now = sceKernelGetSystemTimeWide();
    }

    sNextGfxCompletionUsec += frameUsec;

    if (now > sNextGfxCompletionUsec + OOT_PSP_MAX_PACING_LAG_USEC) {
        sNextGfxCompletionUsec = now + frameUsec;
    }
}

void Sched_Notify(Scheduler* sc) {
    OSScTask* task;

    if (sc == NULL) {
        return;
    }

    while (osRecvMesg(&sc->cmdQueue, (OSMesg*)&task, OS_MESG_NOBLOCK) == 0) {
        if (task == NULL) {
            continue;
        }

        if (task->list.t.type == M_GFXTASK) {
            OotPspRenderer_RenderTask(&task->list);

            if ((task->flags & OS_SC_SWAPBUFFER) && task->framebuffer != NULL) {
                osViSwapBuffer(task->framebuffer->swapBuffer);
            }

            SchedPsp_PaceGfxTask(task);
        }

        if (task->msgQueue != NULL) {
            osSendMesg(task->msgQueue, task->msg, OS_MESG_NOBLOCK);
        }
    }
}

void Sched_Init(Scheduler* sc, void* stack, OSPri priority, u8 viModeType, UNK_TYPE arg4, IrqMgr* irqMgr) {
    if (sc == NULL) {
        return;
    }

    memset(sc, 0, sizeof(*sc));
    osCreateMesgQueue(&sc->interruptQueue, sc->interruptMsgBuf, ARRAY_COUNT(sc->interruptMsgBuf));
    osCreateMesgQueue(&sc->cmdQueue, sc->cmdMsgBuf, ARRAY_COUNT(sc->cmdMsgBuf));
    sc->retraceCount = 1;
    sc->isFirstSwap = true;

    OotPspRenderer_Init();

    (void)stack;
    (void)priority;
    (void)viModeType;
    (void)arg4;
    (void)irqMgr;
}

void Sched_FlushTaskQueue(void) {
}
