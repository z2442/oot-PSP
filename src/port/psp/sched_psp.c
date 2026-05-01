#include "sched.h"

#include "array_count.h"
#include "oot_psp_renderer.h"

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
