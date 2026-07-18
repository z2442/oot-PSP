#include "oot_psp_performance.h"

#if defined(OOTDEBUG)

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspthreadman.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_backend.h"

#define OOT_PSP_PERFORMANCE_LOG_PATH "oot-psp-performance.csv"
#define OOT_PSP_PERFORMANCE_WINDOW_FRAMES 60

typedef struct OotPspPerformanceFrame {
    uint64_t actorUpdateUsec;
    uint64_t actorDrawUsec;
    uint64_t rendererUsec;
    uint64_t pacingUsec;
    uint64_t displayListUsec;
    uint64_t submitUsec;
    uint32_t commandCount;
    uint32_t inputTriangleCount;
    uint32_t outputTriangleCount;
    uint32_t flushCount;
    uint32_t drawCallCount;
    uint32_t opaqueDrawCallCount;
    uint32_t opaqueTriangleCount;
    uint32_t translucentDrawCallCount;
    uint32_t translucentTriangleCount;
    uint32_t maxBatchTriangles;
    uint32_t textureUploadCount;
} OotPspPerformanceFrame;

typedef struct OotPspPerformanceWindow {
    int sceneId;
    int roomId;
    uint32_t frameCount;
    uint64_t totalUsec;
    uint64_t totalUsecMax;
    uint64_t gameUsec;
    uint64_t gameUsecMax;
    uint64_t actorUpdateUsec;
    uint64_t actorDrawUsec;
    uint64_t rendererUsec;
    uint64_t rendererUsecMax;
    uint64_t pacingUsec;
    uint64_t displayListUsec;
    uint64_t displayListUsecMax;
    uint64_t submitUsec;
    uint64_t audioUsec;
    uint64_t commandCount;
    uint64_t inputTriangleCount;
    uint64_t outputTriangleCount;
    uint64_t flushCount;
    uint64_t drawCallCount;
    uint64_t opaqueDrawCallCount;
    uint64_t opaqueTriangleCount;
    uint64_t translucentDrawCallCount;
    uint64_t translucentTriangleCount;
    uint64_t textureUploadCount;
    uint32_t maxBatchTriangles;
    uint64_t wallStartUsec;
    uint64_t idleStartClocks;
    uint64_t mainStartClocks;
    uint64_t audioProducerStartClocks;
    uint64_t audioOutputStartClocks;
    OotPspAudioProfileCounters audioProfileStart;
} OotPspPerformanceWindow;

static OotPspPerformanceFrame sFrame;
static OotPspPerformanceWindow sWindow;
static char sLogPath[384];
static bool sLogInitialized;
static bool sLogUnavailable;
static SceUID sMainThreadId = -1;

static uint64_t OotPspPerformance_ClockValue(const SceKernelSysClock* clock) {
    return ((uint64_t)clock->hi << 32) | clock->low;
}

static uint64_t OotPspPerformance_IdleClocks(void) {
    SceKernelSystemStatus status;

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferSystemStatus(&status) < 0) {
        return 0;
    }
    return OotPspPerformance_ClockValue(&status.idleClocks);
}

static uint64_t OotPspPerformance_ThreadRunClocks(SceUID threadId) {
    SceKernelThreadRunStatus status;

    if (threadId < 0) {
        return 0;
    }
    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(threadId, &status) < 0) {
        return 0;
    }
    return OotPspPerformance_ClockValue(&status.runClocks);
}

static void OotPspPerformance_Write(const char* text, int length, int flags) {
    SceUID fd;

    if (sLogUnavailable || text == NULL || length <= 0) {
        return;
    }

    fd = sceIoOpen(sLogPath, PSP_O_WRONLY | PSP_O_CREAT | flags, 0777);
    if (fd < 0) {
        sLogUnavailable = true;
        return;
    }
    sceIoWrite(fd, text, length);
    sceIoClose(fd);
}

static void OotPspPerformance_InitLog(void) {
    static const char header[] =
        "scene,room,frames,total_avg_us,total_max_us,game_avg_us,game_max_us,actor_update_avg_us,"
        "actor_draw_avg_us,renderer_avg_us,renderer_max_us,display_list_avg_us,display_list_max_us,"
        "submit_avg_us,pacing_avg_us,audio_avg_us,commands_avg,input_triangles_avg,output_triangles_avg,"
        "flushes_avg,draw_calls_avg,opaque_draw_calls_avg,opaque_triangles_avg,translucent_draw_calls_avg,"
        "translucent_triangles_avg,max_batch_triangles,texture_uploads_avg,cpu_busy_tenths_percent,"
        "main_cpu_avg_us,audio_producer_cpu_avg_us,audio_output_cpu_avg_us,other_cpu_avg_us,cpu_clock_mhz,"
        "bus_clock_mhz,audio_updates_avg,audio_wait_avg_us,audio_prepare_avg_us,audio_synth_avg_us,"
        "audio_sequence_avg_us,audio_command_build_avg_us,audio_submit_profile_avg_us,audio_abi_commands_avg,"
        "audio_sample_dmas_avg,audio_me_submits_avg,audio_cpu_mixes_avg,audio_me_failures_window,"
        "audio_me_active,audio_me_state,audio_me_progress\n";
    char resolvedPath[384];
    const char* path;

    if (sLogInitialized) {
        return;
    }

    path = OotPsp_ResolveRootPath(OOT_PSP_PERFORMANCE_LOG_PATH, resolvedPath, sizeof(resolvedPath));
    snprintf(sLogPath, sizeof(sLogPath), "%s", path);
    OotPspPerformance_Write(header, sizeof(header) - 1, PSP_O_TRUNC);
    sLogInitialized = true;
}

static uint64_t OotPspPerformance_Average(uint64_t total) {
    return sWindow.frameCount != 0 ? total / sWindow.frameCount : 0;
}

static void OotPspPerformance_FlushWindow(void) {
    char line[1152];
    int length;
    uint64_t wallUsec;
    uint64_t idleClocks;
    uint64_t mainClocks;
    uint64_t audioProducerClocks;
    uint64_t audioOutputClocks;
    uint64_t busyClocks;
    uint64_t knownClocks;
    uint64_t otherClocks;
    uint64_t cpuBusyTenthsPercent;
    OotPspAudioProfileCounters audioProfile;

    if (sWindow.frameCount == 0) {
        return;
    }

    OotPspPerformance_InitLog();
    wallUsec = OotPspPerformance_Now() - sWindow.wallStartUsec;
    idleClocks = OotPspPerformance_IdleClocks() - sWindow.idleStartClocks;
    mainClocks = OotPspPerformance_ThreadRunClocks(sMainThreadId) - sWindow.mainStartClocks;
    OotPspAudioBackend_GetThreadRunClocks(&audioProducerClocks, &audioOutputClocks);
    OotPspAudioBackend_GetProfileCounters(&audioProfile);
    audioProducerClocks -= sWindow.audioProducerStartClocks;
    audioOutputClocks -= sWindow.audioOutputStartClocks;
    busyClocks = idleClocks < wallUsec ? wallUsec - idleClocks : 0;
    knownClocks = mainClocks + audioProducerClocks + audioOutputClocks;
    otherClocks = knownClocks < busyClocks ? busyClocks - knownClocks : 0;
    cpuBusyTenthsPercent = wallUsec != 0 ? (busyClocks * 1000) / wallUsec : 0;
    length = snprintf(
        line, sizeof(line),
        "%d,%d,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u\n",
        sWindow.sceneId, sWindow.roomId, (unsigned int)sWindow.frameCount,
        (unsigned long long)OotPspPerformance_Average(sWindow.totalUsec),
        (unsigned long long)sWindow.totalUsecMax,
        (unsigned long long)OotPspPerformance_Average(sWindow.gameUsec),
        (unsigned long long)sWindow.gameUsecMax,
        (unsigned long long)OotPspPerformance_Average(sWindow.actorUpdateUsec),
        (unsigned long long)OotPspPerformance_Average(sWindow.actorDrawUsec),
        (unsigned long long)OotPspPerformance_Average(sWindow.rendererUsec),
        (unsigned long long)sWindow.rendererUsecMax,
        (unsigned long long)OotPspPerformance_Average(sWindow.displayListUsec),
        (unsigned long long)sWindow.displayListUsecMax,
        (unsigned long long)OotPspPerformance_Average(sWindow.submitUsec),
        (unsigned long long)OotPspPerformance_Average(sWindow.pacingUsec),
        (unsigned long long)OotPspPerformance_Average(sWindow.audioUsec),
        (unsigned long long)OotPspPerformance_Average(sWindow.commandCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.inputTriangleCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.outputTriangleCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.flushCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.drawCallCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.opaqueDrawCallCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.opaqueTriangleCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.translucentDrawCallCount),
        (unsigned long long)OotPspPerformance_Average(sWindow.translucentTriangleCount),
        (unsigned int)sWindow.maxBatchTriangles,
        (unsigned long long)OotPspPerformance_Average(sWindow.textureUploadCount),
        (unsigned long long)cpuBusyTenthsPercent,
        (unsigned long long)(mainClocks / sWindow.frameCount),
        (unsigned long long)(audioProducerClocks / sWindow.frameCount),
        (unsigned long long)(audioOutputClocks / sWindow.frameCount),
        (unsigned long long)(otherClocks / sWindow.frameCount), scePowerGetCpuClockFrequencyInt(),
        scePowerGetBusClockFrequencyInt(),
        (unsigned long long)OotPspPerformance_Average((u32)(audioProfile.updates - sWindow.audioProfileStart.updates)),
        (unsigned long long)OotPspPerformance_Average((u32)(audioProfile.waitUsec - sWindow.audioProfileStart.waitUsec)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.prepareUsec - sWindow.audioProfileStart.prepareUsec)),
        (unsigned long long)OotPspPerformance_Average((u32)(audioProfile.synthUsec - sWindow.audioProfileStart.synthUsec)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.sequenceUsec - sWindow.audioProfileStart.sequenceUsec)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.commandBuildUsec - sWindow.audioProfileStart.commandBuildUsec)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.submitUsec - sWindow.audioProfileStart.submitUsec)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.abiCommands - sWindow.audioProfileStart.abiCommands)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.sampleDmas - sWindow.audioProfileStart.sampleDmas)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.meSubmits - sWindow.audioProfileStart.meSubmits)),
        (unsigned long long)OotPspPerformance_Average(
            (u32)(audioProfile.cpuMixes - sWindow.audioProfileStart.cpuMixes)),
        (unsigned int)(audioProfile.meFailures - sWindow.audioProfileStart.meFailures),
        (unsigned int)audioProfile.meActive, (unsigned int)audioProfile.meState,
        (unsigned int)audioProfile.meProgress);
    if (length > 0) {
        if ((size_t)length >= sizeof(line)) {
            length = sizeof(line) - 1;
        }
        OotPspPerformance_Write(line, length, PSP_O_APPEND);
    }
    memset(&sWindow, 0, sizeof(sWindow));
}

uint64_t OotPspPerformance_Now(void) {
    return sceKernelGetSystemTimeWide();
}

void OotPspPerformance_BeginFrame(int sceneId, int roomId) {
    uint64_t audioProducerClocks;
    uint64_t audioOutputClocks;

    if (sWindow.frameCount != 0 && (sWindow.sceneId != sceneId || sWindow.roomId != roomId)) {
        OotPspPerformance_FlushWindow();
    }
    if (sWindow.frameCount == 0) {
        if (sMainThreadId < 0) {
            sMainThreadId = sceKernelGetThreadId();
        }
        sWindow.sceneId = sceneId;
        sWindow.roomId = roomId;
        sWindow.wallStartUsec = OotPspPerformance_Now();
        sWindow.idleStartClocks = OotPspPerformance_IdleClocks();
        sWindow.mainStartClocks = OotPspPerformance_ThreadRunClocks(sMainThreadId);
        OotPspAudioBackend_GetThreadRunClocks(&audioProducerClocks, &audioOutputClocks);
        sWindow.audioProducerStartClocks = audioProducerClocks;
        sWindow.audioOutputStartClocks = audioOutputClocks;
        OotPspAudioBackend_GetProfileCounters(&sWindow.audioProfileStart);
    }
    memset(&sFrame, 0, sizeof(sFrame));
}

void OotPspPerformance_RecordActorUpdate(uint64_t usec) {
    sFrame.actorUpdateUsec += usec;
}

void OotPspPerformance_RecordActorDraw(uint64_t usec) {
    sFrame.actorDrawUsec += usec;
}

void OotPspPerformance_RecordRenderer(uint64_t usec) {
    sFrame.rendererUsec += usec;
}

void OotPspPerformance_RecordPacing(uint64_t usec) {
    sFrame.pacingUsec += usec;
}

void OotPspPerformance_RecordGfx(uint64_t displayListUsec, uint64_t submitUsec, uint32_t commandCount,
                                 uint32_t inputTriangleCount, uint32_t outputTriangleCount, uint32_t flushCount,
                                 uint32_t drawCallCount, uint32_t opaqueDrawCallCount,
                                 uint32_t opaqueTriangleCount, uint32_t translucentDrawCallCount,
                                 uint32_t translucentTriangleCount, uint32_t maxBatchTriangles,
                                 uint32_t textureUploadCount) {
    sFrame.displayListUsec += displayListUsec;
    sFrame.submitUsec += submitUsec;
    sFrame.commandCount += commandCount;
    sFrame.inputTriangleCount += inputTriangleCount;
    sFrame.outputTriangleCount += outputTriangleCount;
    sFrame.flushCount += flushCount;
    sFrame.drawCallCount += drawCallCount;
    sFrame.opaqueDrawCallCount += opaqueDrawCallCount;
    sFrame.opaqueTriangleCount += opaqueTriangleCount;
    sFrame.translucentDrawCallCount += translucentDrawCallCount;
    sFrame.translucentTriangleCount += translucentTriangleCount;
    if (maxBatchTriangles > sFrame.maxBatchTriangles) {
        sFrame.maxBatchTriangles = maxBatchTriangles;
    }
    sFrame.textureUploadCount += textureUploadCount;
}

void OotPspPerformance_EndFrame(uint64_t gameUsec, uint64_t audioUsec, uint64_t totalUsec) {
    sWindow.frameCount++;
    sWindow.totalUsec += totalUsec;
    sWindow.gameUsec += gameUsec;
    sWindow.actorUpdateUsec += sFrame.actorUpdateUsec;
    sWindow.actorDrawUsec += sFrame.actorDrawUsec;
    sWindow.rendererUsec += sFrame.rendererUsec;
    sWindow.pacingUsec += sFrame.pacingUsec;
    sWindow.displayListUsec += sFrame.displayListUsec;
    sWindow.submitUsec += sFrame.submitUsec;
    sWindow.audioUsec += audioUsec;
    sWindow.commandCount += sFrame.commandCount;
    sWindow.inputTriangleCount += sFrame.inputTriangleCount;
    sWindow.outputTriangleCount += sFrame.outputTriangleCount;
    sWindow.flushCount += sFrame.flushCount;
    sWindow.drawCallCount += sFrame.drawCallCount;
    sWindow.opaqueDrawCallCount += sFrame.opaqueDrawCallCount;
    sWindow.opaqueTriangleCount += sFrame.opaqueTriangleCount;
    sWindow.translucentDrawCallCount += sFrame.translucentDrawCallCount;
    sWindow.translucentTriangleCount += sFrame.translucentTriangleCount;
    sWindow.textureUploadCount += sFrame.textureUploadCount;

    if (totalUsec > sWindow.totalUsecMax) {
        sWindow.totalUsecMax = totalUsec;
    }
    if (gameUsec > sWindow.gameUsecMax) {
        sWindow.gameUsecMax = gameUsec;
    }
    if (sFrame.rendererUsec > sWindow.rendererUsecMax) {
        sWindow.rendererUsecMax = sFrame.rendererUsec;
    }
    if (sFrame.displayListUsec > sWindow.displayListUsecMax) {
        sWindow.displayListUsecMax = sFrame.displayListUsec;
    }
    if (sFrame.maxBatchTriangles > sWindow.maxBatchTriangles) {
        sWindow.maxBatchTriangles = sFrame.maxBatchTriangles;
    }

    if (sWindow.frameCount >= OOT_PSP_PERFORMANCE_WINDOW_FRAMES) {
        OotPspPerformance_FlushWindow();
    }
}

void OotPspPerformance_Flush(void) {
    OotPspPerformance_FlushWindow();
}

#endif
