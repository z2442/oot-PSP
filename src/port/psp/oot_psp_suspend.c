#include "oot_psp_suspend.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>

#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_backend.h"
#include "oot_psp_renderer.h"
#include "oot_psp_wake_trace.h"

static volatile int sRequested;
static volatile int sInProgress;
static volatile int sWaitingForResume;
static volatile int sResumeStarted;
static volatile int sResumeComplete;

int OotPspSuspend_Init(void) {
    return scePowerLock(0) >= 0;
}

void OotPspSuspend_NotifyPowerEvent(int flags) {
#if OOT_PSP_WAKE_DIAGNOSTICS
    OotPspWakeTrace_NotifyPowerEvent(flags);
#endif
    if (sWaitingForResume && (flags & PSP_POWER_CB_RESUMING)) {
        sResumeStarted = 1;
    }
    if (sWaitingForResume && (flags & PSP_POWER_CB_RESUME_COMPLETE)) {
        sResumeStarted = 1;
        sResumeComplete = 1;
    }
    if (!sInProgress && (flags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING))) {
        sRequested = 1;
    }
}

static void OotPspSuspend_WaitForRetry(const char* message) {
    SceCtrlData pad = { 0 };
    int released = 0;

    OotPspRenderer_RenderSuspendMessage(message, "Cross: retry   Home: quit");
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_HOME) {
            sceKernelExitGame();
        }
        if (!(pad.Buttons & PSP_CTRL_CROSS)) {
            released = 1;
        } else if (released) {
            return;
        }
        sceKernelDelayThread(16000);
    }
}

static int OotPspSuspend_WaitForResumeComplete(void) {
    unsigned int start = sceKernelGetSystemTimeLow();

    while (!sResumeComplete) {
        if ((sceKernelGetSystemTimeLow() - start) >= 5000000U) {
            return 0;
        }
        sceKernelDelayThread(1000);
    }
    return 1;
}

void OotPspSuspend_Poll(void) {
    int prepared;
    int didSleep = 0;
    const char* failureReason = NULL;

#if OOT_PSP_WAKE_DIAGNOSTICS
    OotPspWakeTrace_Poll();
#endif
    if (!sRequested) {
        return;
    }
    sInProgress = 1;
    sRequested = 0;
    prepared = OotPspRenderer_PrepareSuspend();
    if (!prepared) {
        failureReason = "Could not save graphics state";
    } else {
        prepared = OotPspAudioBackend_PrepareSuspend();
        if (!prepared) {
            failureReason = OotPspAudioBackend_GetSuspendError();
        }
    }
    if (prepared) {
        OotPspRenderer_RenderSuspendMessage("Preparing to sleep", "Please wait");
        prepared = OotPspRenderer_PrepareSuspend();
        if (!prepared) {
            failureReason = "Could not save graphics state";
        } else {
            prepared = OotPsp_AssetPrepareSuspend();
            if (!prepared) {
                failureReason = "Could not save volatile memory";
            }
        }
    }
    if (prepared) {
#if OOT_PSP_WAKE_DIAGNOSTICS
        OotPspWakeTrace_Checkpoint("before_sleep");
#endif
        sResumeStarted = 0;
        sResumeComplete = 0;
        sWaitingForResume = 1;
        /* A switch request made under the power lock fires on unlock. Do not
         * issue a second software suspend request: it could sleep again after
         * the first wake. RESUMING only starts recovery: firmware may still
         * change bus/peripheral state for hundreds of milliseconds after it. */
        if (scePowerUnlock(0) >= 0) {
            while (!sResumeStarted) {
                sceKernelDelayThread(1000);
            }
            while (scePowerLock(0) < 0) {
                sceKernelDelayThread(1000);
            }
            /* Restore clocks, graphics and assets, then verify the ME after
             * firmware finishes. Missing completion notifications stay on
             * the retry screen rather than releasing audio into a timeout
             * followed by permanent CPU mixing. */
            while (!OotPspSuspend_WaitForResumeComplete()) {
                OotPspSuspend_WaitForRetry("PSP wake did not finish");
            }
            didSleep = 1;
#if OOT_PSP_WAKE_DIAGNOSTICS
            OotPspWakeTrace_Checkpoint("firmware_complete");
#endif
            scePowerSetClockFrequency(333, 333, 166);
            while (!OotPspRenderer_Resume()) {
                OotPspSuspend_WaitForRetry("Graphics state did not restore");
            }
        } else {
            prepared = 0;
            failureReason = "Power switch unlock failed";
        }
        sWaitingForResume = 0;
    } else {
        printf("oot-psp sleep cancelled: %s\n", failureReason);
    }

    /* Also rolls back partial prepare failures. Never release readers while
     * their retained pointers refer to missing or unverified volatile data. */
    while (!OotPsp_AssetResume()) {
        OotPspSuspend_WaitForRetry("Restore failed. Check the Memory Stick.");
    }
#if OOT_PSP_WAKE_DIAGNOSTICS
    if (didSleep) {
        OotPspWakeTrace_Checkpoint("assets_restored");
    }
#endif
    while (!OotPspAudioBackend_Resume(didSleep)) {
        OotPspSuspend_WaitForRetry(OotPspAudioBackend_GetSuspendError());
    }
#if OOT_PSP_WAKE_DIAGNOSTICS
    if (didSleep) {
        OotPspWakeTrace_Checkpoint("me_restarted");
        OotPspWakeTrace_Resume();
    }
#else
    (void)didSleep;
#endif
    if (!prepared) {
        OotPspRenderer_RenderSuspendMessage("Sleep cancelled", failureReason);
        sceKernelDelayThread(1000000);
    }
    sInProgress = 0;
}
