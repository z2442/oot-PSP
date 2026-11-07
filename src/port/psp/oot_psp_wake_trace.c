#include "oot_psp_wake_trace.h"

#if OOT_PSP_WAKE_DIAGNOSTICS

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_backend.h"
#include "oot_psp_video_thread.h"

#define WAKE_TRACE_THREADS 128
#define WAKE_TRACE_INTERVAL_US 3000000U

typedef struct WakeThread {
    SceUID id;
    char name[32];
    u64 runUsec;
    u32 intrPreempts;
    u32 releases;
    int priority;
} WakeThread;

typedef struct WakeSnapshot {
    u64 wallUsec;
    u64 idleUsec;
    u32 idleExits;
    u32 switches;
    u32 vfpuSwitches;
    int systemResult;
    int listResult;
    int listedCount;
    int count;
    int cpuMHz;
    int busMHz;
    u32 presentations;
    u32 powerFlags, resumingUsec, resumeCompleteUsec;
    OotPspAudioWakeStatus audio;
    WakeThread threads[WAKE_TRACE_THREADS];
} WakeSnapshot;

static WakeSnapshot sPrevious, sCurrent, sBeforeStart, sBeforeEnd;
static int sInitialized, sHaveBaseline, sPostSamples, sCycle, sLogStarted;
static u32 sNextSampleUsec;
static volatile u32 sPowerFlags, sResumingUsec, sResumeCompleteUsec;

void OotPspWakeTrace_NotifyPowerEvent(int flags) {
    if ((flags & PSP_POWER_CB_SUSPENDING) && !(flags & PSP_POWER_CB_RESUMING)) {
        sPowerFlags = 0;
        sResumingUsec = sResumeCompleteUsec = 0;
    }
    if ((flags & PSP_POWER_CB_RESUMING) && !(sPowerFlags & PSP_POWER_CB_RESUMING)) {
        sResumingUsec = sceKernelGetSystemTimeLow();
    }
    if ((flags & PSP_POWER_CB_RESUME_COMPLETE) && !(sPowerFlags & PSP_POWER_CB_RESUME_COMPLETE)) {
        sResumeCompleteUsec = sceKernelGetSystemTimeLow();
    }
    sPowerFlags |= flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE);
}

static u64 WakeTrace_Clock(const SceKernelSysClock* clock) {
    return ((u64)clock->hi << 32) | clock->low;
}

static void WakeTrace_Capture(WakeSnapshot* snapshot) {
    SceUID ids[WAKE_TRACE_THREADS];
    SceKernelSystemStatus system;
    int i, count;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->wallUsec = sceKernelGetSystemTimeWide();
    snapshot->cpuMHz = scePowerGetCpuClockFrequencyInt();
    snapshot->busMHz = scePowerGetBusClockFrequencyInt();
    snapshot->presentations = OotPspVideoThread_GetCompletedFrames();
    snapshot->powerFlags = sPowerFlags;
    snapshot->resumingUsec = sResumingUsec;
    snapshot->resumeCompleteUsec = sResumeCompleteUsec;
    OotPspAudioBackend_GetWakeStatus(&snapshot->audio);
    memset(&system, 0, sizeof(system));
    system.size = sizeof(system);
    snapshot->systemResult = sceKernelReferSystemStatus(&system);
    if (snapshot->systemResult >= 0) {
        snapshot->idleUsec = WakeTrace_Clock(&system.idleClocks);
        snapshot->idleExits = system.comesOutOfIdleCount;
        snapshot->switches = system.threadSwitchCount;
        snapshot->vfpuSwitches = system.vfpuSwitchCount;
    }
    snapshot->listResult = sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, WAKE_TRACE_THREADS,
                                                       &snapshot->listedCount);
    if (snapshot->listResult < 0) {
        return;
    }
    count = snapshot->listedCount;
    if (count > WAKE_TRACE_THREADS) {
        count = WAKE_TRACE_THREADS;
    }
    for (i = 0; i < count; i++) {
        SceKernelThreadInfo info;
        WakeThread* thread;

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(ids[i], &info) < 0) {
            continue;
        }
        thread = &snapshot->threads[snapshot->count++];
        thread->id = ids[i];
        memcpy(thread->name, info.name, sizeof(thread->name) - 1);
        thread->runUsec = WakeTrace_Clock(&info.runClocks);
        thread->intrPreempts = info.intrPreemptCount;
        thread->releases = info.releaseCount;
        thread->priority = info.currentPriority;
    }
}

static const WakeThread* WakeTrace_FindThread(const WakeSnapshot* snapshot, const WakeThread* thread) {
    int i;

    for (i = 0; i < snapshot->count; i++) {
        if (snapshot->threads[i].id == thread->id && !strcmp(snapshot->threads[i].name, thread->name)) {
            return &snapshot->threads[i];
        }
    }
    return NULL;
}

static void WakeTrace_Write(SceUID fd, const char* text) {
    int remaining = strlen(text);

    while (remaining > 0) {
        int written = sceIoWrite(fd, text, remaining);

        if (written <= 0) {
            break;
        }
        text += written;
        remaining -= written;
    }
}

static void WakeTrace_ReportHardware(SceUID fd, const char* label, const u32* registers, s32 result) {
    char line[320];

    snprintf(line, sizeof(line),
             "%s result=%ld cpu_div=0x%08lX me_div=0x%08lX bus_div=0x%08lX "
             "bus_enable=0x%08lX reset=0x%08lX tachyon=0x%08lX\n",
             label, (long)result, (unsigned long)registers[0], (unsigned long)registers[1],
             (unsigned long)registers[2], (unsigned long)registers[3],
             (unsigned long)registers[4], (unsigned long)registers[5]);
    WakeTrace_Write(fd, line);
}

static void WakeTrace_Report(SceUID fd, const char* label, const WakeSnapshot* start, const WakeSnapshot* end) {
    char line[512];
    u64 wall = end->wallUsec - start->wallUsec;
    u64 idle = end->idleUsec - start->idleUsec;
    int valid = wall > 0 && start->systemResult >= 0 && end->systemResult >= 0 &&
                end->idleUsec >= start->idleUsec && idle <= wall;
    int i;

    snprintf(line, sizeof(line),
             "cycle=%d phase=%s wall_us=%llu cpu_busy_tenths_pct=%lld valid=%d "
             "cpu_mhz=%d->%d bus_mhz=%d->%d idle_exits=%lu switches=%lu vfpu_switches=%lu "
             "threads=%d/%d list_result=%d system_result=%d\n",
             sCycle, label, (unsigned long long)wall,
             valid ? (long long)((wall - idle) * 1000 / wall) : -1LL, valid,
             start->cpuMHz, end->cpuMHz, start->busMHz, end->busMHz,
             (unsigned long)(end->idleExits - start->idleExits),
             (unsigned long)(end->switches - start->switches),
             (unsigned long)(end->vfpuSwitches - start->vfpuSwitches),
             end->count, end->listedCount, end->listResult, end->systemResult);
    WakeTrace_Write(fd, line);
    WakeTrace_ReportHardware(fd, "hardware_start", start->audio.hardware, start->audio.hardwareResult);
    WakeTrace_ReportHardware(fd, "hardware_end", end->audio.hardware, end->audio.hardwareResult);
    snprintf(line, sizeof(line),
             "me_cpu config=0x%08lX->0x%08lX status=0x%08lX->0x%08lX "
             "power_flags=0x%08lX->0x%08lX resuming_us=%lu complete_us=%lu\n",
             (unsigned long)start->audio.meConfig, (unsigned long)end->audio.meConfig,
             (unsigned long)start->audio.meStatus, (unsigned long)end->audio.meStatus,
             (unsigned long)start->powerFlags, (unsigned long)end->powerFlags,
             (unsigned long)end->resumingUsec, (unsigned long)end->resumeCompleteUsec);
    WakeTrace_Write(fd, line);
    snprintf(line, sizeof(line), "me_stack sp=0x%08lX->0x%08lX\n",
             (unsigned long)start->audio.meStackPointer, (unsigned long)end->audio.meStackPointer);
    WakeTrace_Write(fd, line);
    snprintf(line, sizeof(line), "synth_inputs ticks/notes/reverbs/frames/downsample "
             "cpu=%ld/%ld/%ld/%ld/%ld me=%ld/%ld/%ld/%ld/%ld\n",
             (long)end->audio.cpuSynthInputs[0], (long)end->audio.cpuSynthInputs[1],
             (long)end->audio.cpuSynthInputs[2], (long)end->audio.cpuSynthInputs[3],
             (long)end->audio.cpuSynthInputs[4], (long)end->audio.meSynthInputs[0],
             (long)end->audio.meSynthInputs[1], (long)end->audio.meSynthInputs[2],
             (long)end->audio.meSynthInputs[3], (long)end->audio.meSynthInputs[4]);
    WakeTrace_Write(fd, line);
    if (end->audio.fallbackCount != 0) {
        snprintf(line, sizeof(line), "last_fallback state=%lu progress=%lu elapsed_us=%lu commands=%ld\n",
                 (unsigned long)end->audio.fallbackState, (unsigned long)end->audio.fallbackProgress,
                 (unsigned long)end->audio.fallbackElapsedUsec, (long)end->audio.fallbackCmdCount);
        WakeTrace_Write(fd, line);
        WakeTrace_ReportHardware(fd, "fallback_hardware", end->audio.fallbackHardware,
                                 end->audio.fallbackHardwareResult);
        if (end->audio.fallbackProgress & 0x80000000U) {
            snprintf(line, sizeof(line), "fallback_mixer command_index=%lu opcode=%lu "
                     "word0=0x%08lX word1=0x%08lX valid=%lu\n",
                     (unsigned long)((end->audio.fallbackProgress & 0x7FFFFFFFU) >> 8),
                     (unsigned long)(end->audio.fallbackProgress & 0xFF),
                     (unsigned long)end->audio.fallbackWord0, (unsigned long)end->audio.fallbackWord1,
                     (unsigned long)end->audio.fallbackWordsValid);
            WakeTrace_Write(fd, line);
        }
    }
    snprintf(line, sizeof(line), "video presentations=%lu fps_tenths=%llu\n",
             (unsigned long)(end->presentations - start->presentations),
             wall ? (unsigned long long)((u64)(end->presentations - start->presentations) * 10000000 / wall) : 0);
    WakeTrace_Write(fd, line);
    snprintf(line, sizeof(line),
             "audio me_active=%lu->%lu state=%lu->%lu progress=%lu->%lu irq_ready=%lu->%lu "
             "irq_count=%lu->%lu fallback_count=%lu->%lu hardware_src=%lu->%lu boots=%lu->%lu\n",
             (unsigned long)start->audio.meActive, (unsigned long)end->audio.meActive,
             (unsigned long)start->audio.meState, (unsigned long)end->audio.meState,
             (unsigned long)start->audio.meProgress, (unsigned long)end->audio.meProgress,
             (unsigned long)start->audio.interruptReady, (unsigned long)end->audio.interruptReady,
             (unsigned long)start->audio.interruptCount, (unsigned long)end->audio.interruptCount,
             (unsigned long)start->audio.fallbackCount, (unsigned long)end->audio.fallbackCount,
             (unsigned long)start->audio.hardwareSrc, (unsigned long)end->audio.hardwareSrc,
             (unsigned long)start->audio.bootCount, (unsigned long)end->audio.bootCount);
    WakeTrace_Write(fd, line);
    for (i = 0; i < end->count; i++) {
        const WakeThread* thread = &end->threads[i];
        const WakeThread* old = WakeTrace_FindThread(start, thread);
        int comparable = old && wall > 0 && thread->runUsec >= old->runUsec;
        u64 run = comparable ? thread->runUsec - old->runUsec : 0;

        snprintf(line, sizeof(line),
                 "thread id=%d name=\"%s\" cpu_us=%llu cpu_tenths_pct=%lld comparable=%d priority=%d "
                 "intr_preempts=%lu releases=%lu\n",
                 thread->id, thread->name, (unsigned long long)run,
                 comparable ? (long long)(run * 1000 / wall) : -1LL, comparable, thread->priority,
                 (unsigned long)(old ? thread->intrPreempts - old->intrPreempts : 0),
                 (unsigned long)(old ? thread->releases - old->releases : 0));
        WakeTrace_Write(fd, line);
    }
}

static SceUID WakeTrace_Open(void) {
    char pathBuffer[384];
    const char* path = OotPsp_ResolveRootPath("oot-psp-wake.log", pathBuffer, sizeof(pathBuffer));
    int flags = PSP_O_WRONLY | PSP_O_CREAT | (sLogStarted ? PSP_O_APPEND : PSP_O_TRUNC);
    SceUID fd = sceIoOpen(path, flags, 0777);

    if (fd < 0) {
        printf("oot-psp wake trace open failed: 0x%08X\n", (unsigned int)fd);
        return fd;
    }
    if (!sLogStarted) {
        WakeTrace_Write(fd, "OOT PSP wake trace v8; percentages use tenths (150 = 15%).\n");
        sLogStarted = 1;
    }
    return fd;
}

void OotPspWakeTrace_Checkpoint(const char* stage) {
    OotPspAudioWakeStatus audio;
    char line[256];
    SceUID fd = WakeTrace_Open();

    if (fd < 0) {
        return;
    }
    OotPspAudioBackend_GetWakeStatus(&audio);
    snprintf(line, sizeof(line), "recovery cycle=%d stage=%s time_us=%lu "
             "me_state=%lu progress=%lu boots=%lu power_flags=0x%08lX\n",
             sCycle + 1, stage, (unsigned long)sceKernelGetSystemTimeLow(),
             (unsigned long)audio.meState, (unsigned long)audio.meProgress,
             (unsigned long)audio.bootCount, (unsigned long)sPowerFlags);
    WakeTrace_Write(fd, line);
    sceIoClose(fd);
}

static void WakeTrace_SaveSample(void) {
    SceUID fd = WakeTrace_Open();

    if (fd < 0) {
        return;
    }
    if (sPostSamples == 3) {
        if (sHaveBaseline) {
            WakeTrace_Report(fd, "before_sleep", &sBeforeStart, &sBeforeEnd);
        } else {
            WakeTrace_Write(fd, "No baseline: run at least 10 seconds before sleeping.\n");
        }
    }
    WakeTrace_Report(fd, sPostSamples == 3 ? "wake_0_3s" : sPostSamples == 2 ? "wake_3_6s" : "wake_6_9s",
                     &sPrevious, &sCurrent);
    sceIoClose(fd);
}

void OotPspWakeTrace_Poll(void) {
    u32 now = sceKernelGetSystemTimeLow();

    if (sInitialized && (s32)(now - sNextSampleUsec) < 0) {
        return;
    }
    WakeTrace_Capture(&sCurrent);
    if (sInitialized) {
        if (sPostSamples) {
            WakeTrace_SaveSample();
            if (sPostSamples == 3) {
                sHaveBaseline = 0;
            }
            sPostSamples--;
        } else {
            sBeforeStart = sPrevious;
            sBeforeEnd = sCurrent;
            sHaveBaseline = 1;
        }
    }
    sPrevious = sCurrent;
    sInitialized = 1;
    sNextSampleUsec = now + WAKE_TRACE_INTERVAL_US;
}

void OotPspWakeTrace_Resume(void) {
    /* Never calculate CPU usage across suspended time or recovery screens. */
    WakeTrace_Capture(&sPrevious);
    sInitialized = 1;
    sNextSampleUsec = sceKernelGetSystemTimeLow() + WAKE_TRACE_INTERVAL_US;
    sPostSamples = 3;
    sCycle++;
}

#endif
