#include "sched.h"

#include "array_count.h"
#include "oot_psp_performance.h"
#include "oot_psp_renderer.h"
#include "oot_psp_video.h"
#include "oot_psp_frame_interpolation.h"

#include <pspkernel.h>
#include <string.h>

#define OOT_PSP_VI_RATE_HZ          60U
#define OOT_PSP_FRAME_BASE_USEC     16666U
#define OOT_PSP_FRAME_REMAINDER     40U

/*
 * Graphics pacing state.
 *
 * Uses the PSP's inexpensive 32-bit microsecond timer.
 *
 * sceKernelGetSystemTimeLow() wraps approximately every 71 minutes,
 * but signed timestamp subtraction handles that safely for the small
 * intervals used here.
 */
static u32 sNextGfxPresentationUsec;
static u32 sGfxPacingRemainder;
static s32 sGfxPacingInitialized;


/*
 * Wrap-safe time difference.
 *
 * Positive:
 *     a is after b
 *
 * Negative:
 *     a is before b
 */
static inline s32 SchedPsp_TimeDiff(u32 a, u32 b) {
    return (s32)(a - b);
}


/*
 * Return the N64 VI update rate associated with this task.
 *
 * Menus can use 1; normal OoT gameplay uses 3 (20 Hz):
 *
 *     60 / 1 = 60 Hz
 *
 * updateRate == 2:
 *
 *     60 / 2 = 30 Hz
 */
static inline u32 SchedPsp_GetUpdateRate(const OSScTask* task) {
    if (task != NULL &&
        task->framebuffer != NULL &&
        task->framebuffer->updateRate > 0) {
        return (u32)task->framebuffer->updateRate;
    }

    return 1U;
}


/*
 * Calculate the duration of this VI interval.
 *
 * 1,000,000 / 60 is:
 *
 *     16666 remainder 40
 *
 * Instead of performing a 64-bit division every frame, we use:
 *
 *     16666 us
 *
 * and accumulate the fractional remainder.
 *
 * The resulting sequence for 60 Hz is effectively:
 *
 *     16666
 *     16667
 *     16667
 *     16666
 *     16667
 *     16667
 *     ...
 *
 * which averages exactly 60 Hz over time.
 */
static inline u32 SchedPsp_GetFrameUsec(u32 updateRate) {

    u32 frameUsec =
        OOT_PSP_FRAME_BASE_USEC * updateRate;

    sGfxPacingRemainder +=
        OOT_PSP_FRAME_REMAINDER * updateRate;

    if (sGfxPacingRemainder >= OOT_PSP_VI_RATE_HZ) {
        const u32 extraUsec =
            sGfxPacingRemainder / OOT_PSP_VI_RATE_HZ;

        frameUsec += extraUsec;

        sGfxPacingRemainder -=
            extraUsec * OOT_PSP_VI_RATE_HZ;
    }

    return frameUsec;
}


/*
 * Pace a graphics presentation before rendering.
 *
 * IMPORTANT:
 *
 * There is intentionally NO catch-up behavior here.
 *
 * If a presentation starts early:
 *
 *     sleep until the expected VI deadline
 *
 * If a presentation starts late:
 *
 *     discard the missed deadline immediately
 *     restart pacing from "now"
 *
 * This prevents a slow frame from causing several subsequent frames to
 * run back-to-back at 100% CPU trying to catch up.
 */
static void SchedPsp_PaceGfxTask(u32 updateRate) {
    const u32 now = sceKernelGetSystemTimeLow();
    const u32 frameUsec = SchedPsp_GetFrameUsec(updateRate);

    /*
     * The first presentation establishes our pacing timeline.
     */
    if (!sGfxPacingInitialized) {
        sNextGfxPresentationUsec = now;
        sGfxPacingInitialized = 1;
    }

    {
        const s32 waitUsec =
            SchedPsp_TimeDiff(
                sNextGfxPresentationUsec,
                now);

        if (waitUsec > 0) {
            /*
             * Presentation reached the scheduler early.
             *
             * Sleep only for the remaining amount of this VI.
             */
            sceKernelDelayThread((u32)waitUsec);
        } else if (waitUsec < 0) {
            /*
             * Presentation missed its deadline.
             *
             * NO CATCH-UP.
             *
             * Throw away all accumulated pacing debt and begin the
             * next interval relative to the current time.
             */
            sNextGfxPresentationUsec = now;
        }
    }

    /*
     * Establish the target for the next presented frame.
     *
     * If we were on time, this advances the existing timeline.
     *
     * If we were late, sNextGfxPresentationUsec was reset to 'now',
     * so the next target becomes:
     *
     *     now + frameUsec
     */
    sNextGfxPresentationUsec += frameUsec;
}


void Sched_Notify(Scheduler* sc) {
    OSScTask* task;

    if (sc == NULL) {
        return;
    }

    while (osRecvMesg(
               &sc->cmdQueue,
               (OSMesg*)&task,
               OS_MESG_NOBLOCK) == 0) {

        if (task == NULL) {
            continue;
        }

        if (task->list.t.type == M_GFXTASK) {
            const int presents = (task->flags & OS_SC_SWAPBUFFER) && task->framebuffer != NULL;
            const u32 updateRate = SchedPsp_GetUpdateRate(task);
            /* Keep the original simulation interval. Only replay the immutable
             * display list; game and draw callbacks must run once per tick. */
            const u32 passes = presents && OotPspVideo_GetFrameRate() == 60 &&
                                       OotPspFrameInterpolation_HasSample() ? updateRate : 1;
            u32 pass;

            for (pass = 0; pass < passes; pass++) {
                if (presents) {
#if defined(OOTDEBUG)
                    const u64 paceStartUsec = OotPspPerformance_Now();
#endif
                    /* Pace before rendering so each intermediate frame has its
                     * own deadline. Overruns rebase without running catch-up ticks. */
                    SchedPsp_PaceGfxTask(updateRate / passes);
#if defined(OOTDEBUG)
                    OotPspPerformance_RecordPacing(OotPspPerformance_Now() - paceStartUsec);
#endif
                }
                OotPspFrameInterpolation_SetFraction((float)(pass + 1) / (float)passes);
                OotPspRenderer_RenderTask(&task->list);
            }
            OotPspFrameInterpolation_SetFraction(1.0f);
            if (presents) {
                osViSwapBuffer(task->framebuffer->swapBuffer);
            }
        }

        /*
         * Notify whoever submitted the task that it has completed.
         */
        if (task->msgQueue != NULL) {
            osSendMesg(
                task->msgQueue,
                task->msg,
                OS_MESG_NOBLOCK);
        }
    }
}


void Sched_Init(
    Scheduler* sc,
    void* stack,
    OSPri priority,
    u8 viModeType,
    UNK_TYPE arg4,
    IrqMgr* irqMgr
) {
    if (sc == NULL) {
        return;
    }

    memset(sc, 0, sizeof(*sc));

    osCreateMesgQueue(
        &sc->interruptQueue,
        sc->interruptMsgBuf,
        ARRAY_COUNT(sc->interruptMsgBuf));

    osCreateMesgQueue(
        &sc->cmdQueue,
        sc->cmdMsgBuf,
        ARRAY_COUNT(sc->cmdMsgBuf));

    sc->retraceCount = 1;
    sc->isFirstSwap = true;

    /*
     * Start with a fresh graphics pacing timeline.
     */
    sNextGfxPresentationUsec = 0;
    sGfxPacingRemainder = 0;
    sGfxPacingInitialized = 0;

    OotPspRenderer_Init();

    (void)stack;
    (void)priority;
    (void)viModeType;
    (void)arg4;
    (void)irqMgr;
}


void Sched_FlushTaskQueue(void) {
}
