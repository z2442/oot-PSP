#include "oot_psp_audio_backend.h"

#include "attributes.h"
#include "audio.h"
#include "dma.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_commands.h"
#include "oot_psp_mixer.h"

#include <me-core-mapper/me-core.h>
#include <pspaudio.h>
#include <pspdmac.h>
#include <pspintrman.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#define OOT_PSP_AUDIO_CHANNELS 2
#ifndef OOT_PSP_AUDIO_SOURCE_FREQUENCY
#define OOT_PSP_AUDIO_SOURCE_FREQUENCY 22050
#endif
#ifndef OOT_PSP_AUDIO_HARDWARE_SRC
#define OOT_PSP_AUDIO_HARDWARE_SRC 1
#endif
#ifndef OOT_PSP_AUDIO_DIAGNOSTICS
#define OOT_PSP_AUDIO_DIAGNOSTICS 0
#endif
#define OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY OOT_PSP_AUDIO_SOURCE_FREQUENCY
#define OOT_PSP_AUDIO_OUTPUT_FREQUENCY 44100
#define OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES 768
#define OOT_PSP_AUDIO_RING_FRAMES 16384
#define OOT_PSP_AUDIO_RING_MASK (OOT_PSP_AUDIO_RING_FRAMES - 1)
#define OOT_PSP_AUDIO_CACHE_LINE_SIZE 64
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS 24
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK ((1U << OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS) - 1)
#define OOT_PSP_AUDIO_RESAMPLE_STEP_2X (1U << (OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS - 1))
#define OOT_PSP_AUDIO_LERP_FRAC_BITS 15

static volatile s32 sOotPspAudioInitialized = false;

#define OOT_PSP_AUDIO_TARGET_CHUNKS 10
#define OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS 8
#define OOT_PSP_AUDIO_URGENT_CHUNKS 6

#if OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS > OOT_PSP_AUDIO_TARGET_CHUNKS
#error OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS must not exceed OOT_PSP_AUDIO_TARGET_CHUNKS
#endif
#if OOT_PSP_AUDIO_URGENT_CHUNKS > OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS
#error OOT_PSP_AUDIO_URGENT_CHUNKS must not exceed OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS
#endif

/* PSP priorities are lower-is-higher. Only the output submitter needs to outrank the game thread. */
#define OOT_PSP_AUDIO_GAME_THREAD_PRIORITY     0x20
#define OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY   (OOT_PSP_AUDIO_GAME_THREAD_PRIORITY - 2)
#define OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY OOT_PSP_AUDIO_GAME_THREAD_PRIORITY

#define OOT_PSP_AUDIO_MAX_UPDATES_NORMAL 8
#define OOT_PSP_AUDIO_MAX_UPDATES_CATCHUP 3
/* Initial and dynamically sized AI buffers can be shorter than one nominal
 * source chunk, so allow one extra update while priming the high-water mark. */
#define OOT_PSP_AUDIO_MAX_UPDATES_PRIME (OOT_PSP_AUDIO_TARGET_CHUNKS + 1)
#define OOT_PSP_AUDIO_UPDATE_USEC (1000000U / 60U)
#define OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)
#define OOT_PSP_AUDIO_ME_TIMEOUT_US 250000
#define OOT_PSP_AUDIO_ME_POLL_USEC 100
#define OOT_PSP_AUDIO_ME_PROGRESS_ENTERED 0x10
#define OOT_PSP_AUDIO_ME_PROGRESS_BOOT_RELEASED 0x20
#define OOT_PSP_AUDIO_ME_PROGRESS_READY 0x100
#define OOT_PSP_AUDIO_PRODUCER_IO_BACKOFF_USEC 1000
#define OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC \
    ((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * 1000000U) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY)
#define OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN 1
#define OOT_PSP_AUDIO_DMA_COPY_MIN_BYTES 1024
#define OOT_PSP_AUDIO_MAX_ME_WRITE_RANGES 512

#if OOT_PSP_AUDIO_DIAGNOSTICS
#define OOT_PSP_AUDIO_DIAGNOSTIC_THREAD_PRIORITY 0x40
#define OOT_PSP_AUDIO_DIAGNOSTIC_POLL_USEC 100000
#define OOT_PSP_AUDIO_DIAGNOSTIC_REPORT_USEC 1000000
#define OOT_PSP_AUDIO_DIAGNOSTIC_SLOW_ME_WAIT_USEC 5000
#define OOT_PSP_AUDIO_DIAGNOSTIC_LATE_USEC 1000
#endif

typedef enum {
    OOT_PSP_AUDIO_ME_STATE_BOOTING,
    OOT_PSP_AUDIO_ME_STATE_IDLE,
    OOT_PSP_AUDIO_ME_STATE_RUN,
    OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER,
    OOT_PSP_AUDIO_ME_STATE_STOP,
    OOT_PSP_AUDIO_ME_STATE_HALTED,
    OOT_PSP_AUDIO_ME_STATE_FAULT,
} OotPspAudioMeState;

#if OOT_PSP_AUDIO_DIAGNOSTICS
typedef enum {
    OOT_PSP_AUDIO_OUTPUT_STATE_STOPPED,
    OOT_PSP_AUDIO_OUTPUT_STATE_STARTING,
    OOT_PSP_AUDIO_OUTPUT_STATE_PRIMING,
    OOT_PSP_AUDIO_OUTPUT_STATE_PREPARE,
    OOT_PSP_AUDIO_OUTPUT_STATE_WAIT_HARDWARE,
    OOT_PSP_AUDIO_OUTPUT_STATE_ERROR_BACKOFF,
} OotPspAudioOutputState;
#endif

enum {
    OOT_PSP_AUDIO_ME_SHARED_STATE,
    OOT_PSP_AUDIO_ME_SHARED_CMD_LIST,
    OOT_PSP_AUDIO_ME_SHARED_CMD_COUNT,
    OOT_PSP_AUDIO_ME_SHARED_PROGRESS,
    OOT_PSP_AUDIO_ME_SHARED_COMPLETION_INTERRUPT_ENABLED,
    OOT_PSP_AUDIO_ME_SHARED_QUEUE_SRC,
    OOT_PSP_AUDIO_ME_SHARED_QUEUE_FRAMES,
    OOT_PSP_AUDIO_ME_SHARED_QUEUE_WRITE_POS,
    OOT_PSP_AUDIO_ME_SHARED_QUEUE_RESULT_WRITE_POS,
    OOT_PSP_AUDIO_ME_SHARED_COUNT,
};

static volatile u32 sAudioMeSharedStorage[OOT_PSP_AUDIO_ME_SHARED_COUNT] __attribute__((aligned(64), section(".uncached")));

#define sAudioMeShared \
    ((volatile u32*)(UNCACHED_USER_MASK | (u32)(uintptr_t)sAudioMeSharedStorage))
#define sAudioMeState    sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_STATE]
#define sAudioMeCmdList  sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_CMD_LIST]
#define sAudioMeCmdCount sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_CMD_COUNT]
#define sAudioMeProgress sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_PROGRESS]
#define sAudioMeCompletionInterruptEnabled \
    sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_COMPLETION_INTERRUPT_ENABLED]
#define sAudioMeQueueSrc                   sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_SRC]
#define sAudioMeQueueFrames                sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_FRAMES]
#define sAudioMeQueueWritePos              sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_WRITE_POS]
#define sAudioMeQueueResultWritePos        sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_RESULT_WRITE_POS]

#if OOT_PSP_AUDIO_DIAGNOSTICS
static volatile OotPspMixerOpcodeProfile sAudioMeOpcodeProfileStorage
    __attribute__((aligned(64), section(".uncached")));
#define sAudioMeOpcodeProfile                                                                    \
    ((volatile OotPspMixerOpcodeProfile*)(UNCACHED_USER_MASK |                                    \
                                           (u32)(uintptr_t)&sAudioMeOpcodeProfileStorage))
#define OOT_PSP_AUDIO_ME_OPCODE_PROFILE sAudioMeOpcodeProfile
#else
#define OOT_PSP_AUDIO_ME_OPCODE_PROFILE NULL
#endif

#if (OOT_PSP_AUDIO_RING_FRAMES & (OOT_PSP_AUDIO_RING_FRAMES - 1)) != 0
#error OOT_PSP_AUDIO_RING_FRAMES must be a power of two
#endif

#if (OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES & 63) != 0
#error OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES must be a multiple of 64
#endif

static s16 sAudioRing[OOT_PSP_AUDIO_RING_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 sAudioMix[2][OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * OOT_PSP_AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static u8 sAudioExternalPool[OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE] __attribute__((aligned(64)));

static volatile u32 sAudioReadPos;
static volatile u32 sAudioWritePos;
static volatile u32 sAudioOutputFrames;
static volatile u32 sAudioPendingOutputFrames;
static volatile s32 sAudioOutputThreadRunning;
static volatile s32 sAudioProducerThreadRunning;
static volatile s32 sAudioPlaybackPrimed;
static volatile s32 sAudioMeBootStarted;
static volatile s32 sAudioMeBootResult;
static volatile s32 sAudioMeInitialized;
static volatile s32 sAudioMeCommandPending;
static volatile u32 sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;
static volatile u32 sAudioSourceChunkFrames;
#if OOT_PSP_AUDIO_DIAGNOSTICS
static volatile u32 sAudioDiagnosticProducerState = OOT_PSP_AUDIO_PRODUCER_STATE_STOPPED;
static volatile u32 sAudioDiagnosticOutputState = OOT_PSP_AUDIO_OUTPUT_STATE_STOPPED;
static volatile s32 sAudioDiagnosticMeWaiting;
static volatile SceUID sAudioDiagnosticMeWaiterThreadId = -1;
static volatile u32 sAudioDiagnosticMeWaitStartUsec;
static volatile u32 sAudioDiagnosticUpdates;
static volatile u32 sAudioDiagnosticMeSubmits;
static volatile u32 sAudioDiagnosticCpuMixes;
static volatile u32 sAudioDiagnosticMeFallbacks;
static volatile u32 sAudioDiagnosticMeTimeouts;
static volatile u32 sAudioDiagnosticMeWaits;
static volatile u32 sAudioDiagnosticMeWaitTotalUsec;
static volatile u32 sAudioDiagnosticMeWaitMaxUsec;
static volatile u32 sAudioDiagnosticMeWaitLastUsec;
static volatile u32 sAudioDiagnosticUnderruns;
static volatile u32 sAudioDiagnosticOutputErrors;
static volatile u32 sAudioDiagnosticIoBackoffs;
static volatile u32 sAudioDiagnosticRingFull;
static volatile u32 sAudioDiagnosticCatchups;
static volatile u32 sAudioDiagnosticProducerLate;
static volatile u32 sAudioDiagnosticProducerLateMaxUsec;
#endif
#if defined(OOTDEBUG) || OOT_PSP_AUDIO_DIAGNOSTICS
static volatile u32 sAudioProfileUpdates;
static volatile u32 sAudioProfileWaitUsec;
static volatile u32 sAudioProfilePrepareUsec;
static volatile u32 sAudioProfileSynthUsec;
static volatile u32 sAudioProfileSubmitUsec;
static volatile u32 sAudioProfileSequenceUsec;
static volatile u32 sAudioProfileCommandBuildUsec;
static volatile u32 sAudioProfileAbiCommands;
static volatile u32 sAudioProfileSampleDmas;
#endif
#if defined(OOTDEBUG)
static volatile u32 sAudioProfileMeSubmits;
static volatile u32 sAudioProfileCpuMixes;
static volatile u32 sAudioProfileMeFailures;
#endif

static const Acmd* sAudioMePendingCmdList;
static s32 sAudioMePendingCmdCount;
static u32 sAudioMePendingStartTime;
static const s16* sAudioMePendingQueueSrc;
static u32 sAudioMePendingQueueFrames;
static u32 sAudioMePendingQueueWritePos;
static u32 sAudioResampleFrac;
static u32 sAudioResampleStep;
static s16 sAudioLastLeft;
static s16 sAudioLastRight;

typedef struct {
    void* address;
    u32 size;
} OotPspAudioCacheRange;

static OotPspAudioCacheRange sAudioMeWriteRanges[OOT_PSP_AUDIO_MAX_ME_WRITE_RANGES];
static u32 sAudioMeWriteRangeCount;
static s32 sAudioMeWriteRangeOverflow;

static SceUID sAudioOutputThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
#if OOT_PSP_AUDIO_DIAGNOSTICS
static SceUID sAudioDiagnosticThreadId = -1;
static volatile s32 sAudioDiagnosticThreadRunning;
#endif
static SceUID sAudioMeLockSema = -1;
static SceUID sAudioMeCompletionSema = -1;
static s32 sAudioMeCompletionInterruptReady;
static s32 sAudioOutputChannel = -1;
static s32 sAudioHardwareSrc;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);
static s32 OotPspAudioBackend_EnsureMeLockSema(void);
static s32 OotPspAudioBackend_EnsureMeCompletionInterrupt(void);
static void OotPspAudioBackend_MeInvalidateInputs(const Acmd* cmdList, s32 cmdCount,
                                                  const void* privateOutput, u32 privateOutputBytes);
static void OotPspAudioBackend_MeWritebackOutputs(const Acmd* cmdList, s32 cmdCount,
                                                  const void* privateOutput, u32 privateOutputBytes);
static void OotPspAudioBackend_MeQueueBuffer(s32 invalidateSource);
static void OotPspAudioBackend_QueueCpuCopy(const s16* samples, u32 frames, u32 writePos);
static u32 OotPspAudioBackend_FreeFrames(void);

#if OOT_PSP_AUDIO_DIAGNOSTICS
#define AUDIO_DIAG_SET(variable, value) ((variable) = (value))
#define AUDIO_DIAG_INCREMENT(variable) ((variable)++)
#else
#define AUDIO_DIAG_SET(variable, value) ((void)0)
#define AUDIO_DIAG_INCREMENT(variable) ((void)0)
#endif

__attribute__((noinline, aligned(4))) void meLibOnException(void) {
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    if (sAudioMeCompletionInterruptEnabled) {
        meLibSendExternalSoftInterrupt();
    }
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    if (sAudioMeCompletionInterruptEnabled) {
        meLibSendExternalSoftInterrupt();
    }
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
    /*
     * Publish a checkpoint before touching any optional ME facilities.  The
     * library's exception-handler setup is intentionally omitted: audio only
     * sends completion interrupts from the ME to the Allegrex, while that
     * setup configures the opposite direction and can stall on real hardware.
     */
    sAudioMeProgress = OOT_PSP_AUDIO_ME_PROGRESS_ENTERED;
    meLibSync();

    do {
        meLibDelayPipeline();
    } while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_BOOTING);

    sAudioMeProgress = OOT_PSP_AUDIO_ME_PROGRESS_BOOT_RELEASED;
    meLibSync();
    OotPspMixer_InitVme();
    sAudioMeProgress = OOT_PSP_AUDIO_ME_PROGRESS_READY;
    meLibSync();

    while (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_STOP) {
        if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
            const Acmd* cmdList = (const Acmd*)(uintptr_t)sAudioMeCmdList;
            s32 cmdCount = (s32)sAudioMeCmdCount;
            const void* privateOutput = (const void*)(uintptr_t)sAudioMeQueueSrc;
            u32 privateOutputBytes = sAudioMeQueueFrames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);

            sAudioMeProgress = 1;
            OotPspAudioBackend_MeInvalidateInputs(cmdList, cmdCount, privateOutput, privateOutputBytes);
            sAudioMeProgress = 2;
            OotPspMixer_ExecuteCommandListMe(cmdList, cmdCount, OOT_PSP_AUDIO_ME_OPCODE_PROFILE);
            if (sAudioMeQueueFrames != 0) {
                sAudioMeProgress = 3;
                /* A_SAVEBUFF just produced this PCM in the ME cache. Copy it
                 * into the ring before writeback instead of flushing and
                 * immediately reloading the same samples. */
                OotPspAudioBackend_MeQueueBuffer(false);
            }
            sAudioMeProgress = 4;
            OotPspAudioBackend_MeWritebackOutputs(cmdList, cmdCount, privateOutput, privateOutputBytes);
            meLibSync();
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
            meLibSync();
            if (sAudioMeCompletionInterruptEnabled) {
                meLibSendExternalSoftInterrupt();
            }
        } else if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER) {
            sAudioMeProgress = 1;
            OotPspAudioBackend_MeQueueBuffer(true);
            sAudioMeProgress = 2;
            meLibSync();
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
        } else {
            meLibDelayPipeline();
        }
    }

    OotPspMixer_ShutdownVme();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_HALTED;
    meLibSync();
    meLibHalt();
}

/*
 * Match the working me-core samples: initialize the library once, near the
 * beginning of main, before the renderer and audio systems are brought up.
 * The ME waits in meLibOnProcess until the late audio initialization changes
 * the shared state from BOOTING to IDLE.
 */
s32 OotPspAudioBackend_BootMe(void) {
    if (sAudioMeBootStarted) {
        return sAudioMeBootResult;
    }

    sAudioMeCmdList = 0;
    sAudioMeCmdCount = 0;
    sAudioMeProgress = 0;
    sAudioMeQueueSrc = 0;
    sAudioMeQueueFrames = 0;
    sAudioMeQueueWritePos = 0;
    sAudioMeQueueResultWritePos = 0;
#if OOT_PSP_AUDIO_DIAGNOSTICS
    memset((void*)sAudioMeOpcodeProfile, 0, sizeof(*sAudioMeOpcodeProfile));
    sAudioMeOpcodeProfile->currentOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    sAudioMeOpcodeProfile->lastJobSlowOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    sAudioMeOpcodeProfile->maxJobSlowOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
#endif
    sAudioMeCompletionInterruptEnabled = false;
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_BOOTING;
    meLibSync();

    sAudioMeBootStarted = true;
    sAudioMeBootResult = meLibDefaultInit();
    return sAudioMeBootResult;
}

static s32 OotPspAudioBackend_InitMe(void) {
    s32 ret;
    u32 readyStart;

    if (sAudioMeInitialized) {
        return 0;
    }

    ret = OotPspAudioBackend_EnsureMeLockSema();
    if (ret < 0) {
        return ret;
    }
    ret = OotPspAudioBackend_BootMe();
    if (ret < 0) {
        return ret;
    }

    /* A failed registration is non-fatal; command waits retain their polling fallback. */
    OotPspAudioBackend_EnsureMeCompletionInterrupt();

    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;

    /*
     * Do not publish the backend as active until the ME has finished mixer
     * setup and entered its command loop.  Previously the first audio job
     * could be submitted while the ME was still stuck in startup, leading to
     * one timeout followed by permanent CPU mixing.
     */
    readyStart = sceKernelGetSystemTimeLow();
    while (sAudioMeProgress != OOT_PSP_AUDIO_ME_PROGRESS_READY) {
        if ((sceKernelGetSystemTimeLow() - readyStart) >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_STOP;
            meLibSync();
            AUDIO_DIAG_INCREMENT(sAudioDiagnosticMeTimeouts);
            return -1;
        }
        sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
    }

    sAudioMeInitialized = true;
    return 0;
}

static void OotPspAudioBackend_InvalidateRange(const void* address, u32 size) {
    uintptr_t start;
    uintptr_t end;

    if ((address == NULL) || (size == 0)) {
        return;
    }

    start = (uintptr_t)address & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    end = ((uintptr_t)address + size + OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1) &
          ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    sceKernelDcacheInvalidateRange((void*)start, end - start);
}

static void OotPspAudioBackend_WritebackRange(const void* address, u32 size) {
    uintptr_t start;
    uintptr_t end;

    if ((address == NULL) || (size == 0)) {
        return;
    }

    start = (uintptr_t)address & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    end = ((uintptr_t)address + size + OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1) &
          ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    sceKernelDcacheWritebackRange((void*)start, end - start);
}

static void OotPspAudioBackend_WritebackBoundaryRange(const void* address, u32 size) {
    uintptr_t start;
    uintptr_t last;
    uintptr_t startLine;
    uintptr_t lastLine;

    if ((address == NULL) || (size == 0)) {
        return;
    }

    start = (uintptr_t)address;
    last = start + size - 1;
    startLine = start & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    lastLine = last & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);

    if ((start & (OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1)) != 0) {
        OotPspAudioBackend_WritebackRange((const void*)startLine, OOT_PSP_AUDIO_CACHE_LINE_SIZE);
    }

    if (((last + 1) & (OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1)) != 0) {
        if (lastLine != startLine) {
            OotPspAudioBackend_WritebackRange((const void*)lastLine, OOT_PSP_AUDIO_CACHE_LINE_SIZE);
        } else if ((start & (OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1)) == 0) {
            OotPspAudioBackend_WritebackRange((const void*)lastLine, OOT_PSP_AUDIO_CACHE_LINE_SIZE);
        }
    }
}

static void OotPspAudioBackend_MeInvalidateRange(const void* address, u32 size) {
    uintptr_t start;
    uintptr_t end;

    if ((address == NULL) || (size == 0)) {
        return;
    }

    start = (uintptr_t)address & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    end = ((uintptr_t)address + size + OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1) &
          ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    meLibDcacheInvalidateRange((u32)start, end - start);
}

static void OotPspAudioBackend_MeWritebackRange(const void* address, u32 size) {
    uintptr_t start;
    uintptr_t end;

    if ((address == NULL) || (size == 0)) {
        return;
    }

    start = (uintptr_t)address & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    end = ((uintptr_t)address + size + OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1) &
          ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    meLibDcacheWritebackRange((u32)start, end - start);
}

static u32 OotPspAudioBackend_CommandDmaSize(u32 w0) {
    return ((w0 >> 16) & 0xFF) << 4;
}

static s32 OotPspAudioBackend_RangeIsWithin(const void* address, u32 size, const void* container,
                                            u32 containerSize) {
    uintptr_t start = (uintptr_t)address;
    uintptr_t containerStart = (uintptr_t)container;

    if ((address == NULL) || (size == 0) || (container == NULL) || (containerSize == 0) ||
        (start < containerStart)) {
        return false;
    }

    return (start - containerStart <= containerSize) && (size <= containerSize - (start - containerStart));
}

static void OotPspAudioBackend_ResetMeWriteRanges(void) {
    sAudioMeWriteRangeCount = 0;
    sAudioMeWriteRangeOverflow = false;
}

static void OotPspAudioBackend_RecordMeWriteRange(void* address, u32 size) {
    uintptr_t start;
    uintptr_t end;

    if ((address == NULL) || (size == 0) || sAudioMeWriteRangeOverflow) {
        return;
    }

    start = (uintptr_t)address & ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);
    end = ((uintptr_t)address + size + OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1) &
          ~(OOT_PSP_AUDIO_CACHE_LINE_SIZE - 1);

    if (sAudioMeWriteRangeCount != 0) {
        OotPspAudioCacheRange* previous = &sAudioMeWriteRanges[sAudioMeWriteRangeCount - 1];
        uintptr_t previousStart = (uintptr_t)previous->address;
        uintptr_t previousEnd = previousStart + previous->size;

        if ((start <= previousEnd) && (end >= previousStart)) {
            if (start < previousStart) {
                previousStart = start;
            }
            if (end < previousEnd) {
                end = previousEnd;
            }
            previous->address = (void*)previousStart;
            previous->size = end - previousStart;
            return;
        }
    }

    if (sAudioMeWriteRangeCount >= OOT_PSP_AUDIO_MAX_ME_WRITE_RANGES) {
        sAudioMeWriteRangeOverflow = true;
        return;
    }

    sAudioMeWriteRanges[sAudioMeWriteRangeCount].address = (void*)start;
    sAudioMeWriteRanges[sAudioMeWriteRangeCount].size = end - start;
    sAudioMeWriteRangeCount++;
}

static void OotPspAudioBackend_WritebackMeInputs(const Acmd* cmdList, s32 cmdCount, const void* privateOutput,
                                                 u32 privateOutputBytes) {
    void* filterLut = NULL;
    s32 i;

    OotPspAudioBackend_ResetMeWriteRanges();

    for (i = 0; i < cmdCount; i++) {
        u32 w0 = cmdList[i].words.w0;
        u32 w1 = cmdList[i].words.w1;

        switch (w0 >> 24) {
            case A_ADPCM:
            case A_RESAMPLE:
            case A_S8DEC:
                OotPspAudioBackend_RecordMeWriteRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case A_FILTER:
                if (((w0 >> 16) & 0xFF) > 1) {
                    filterLut = (void*)(uintptr_t)w1;
                } else {
                    OotPspAudioBackend_RecordMeWriteRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                    if (filterLut != NULL) {
                        OotPspAudioBackend_RecordMeWriteRange(filterLut, 8 * sizeof(s16));
                    }
                }
                break;

            case A_SAVEBUFF:
                if (!OotPspAudioBackend_RangeIsWithin((void*)(uintptr_t)w1,
                                                      OotPspAudioBackend_CommandDmaSize(w0), privateOutput,
                                                      privateOutputBytes)) {
                    OotPspAudioBackend_RecordMeWriteRange((void*)(uintptr_t)w1,
                                                          OotPspAudioBackend_CommandDmaSize(w0));
                }
                break;

            default:
                break;
        }
    }

    /* The Allegrex and ME have separate data caches. A busy mix can contain
     * hundreds of commands, and issuing a kernel range writeback for every
     * tiny state/sample input costs far more than flushing the PSP's small
     * data cache once. This still makes every command-owned input visible to
     * the ME; the precise range list above is retained so only ME-written
     * state is invalidated when the job completes. */
    sceKernelDcacheWritebackAll();
}

static void OotPspAudioBackend_MeInvalidateInputs(const Acmd* cmdList, s32 cmdCount, const void* privateOutput,
                                                  u32 privateOutputBytes) {
    void* filterLut = NULL;
    s32 i;

    OotPspAudioBackend_MeInvalidateRange(cmdList, cmdCount * sizeof(Acmd));
    OotPspAudioBackend_MeInvalidateRange(privateOutput, privateOutputBytes);

    for (i = 0; i < cmdCount; i++) {
        u32 w0 = cmdList[i].words.w0;
        u32 w1 = cmdList[i].words.w1;

        switch (w0 >> 24) {
            case A_ADPCM:
            case A_RESAMPLE:
            case A_S8DEC:
                OotPspAudioBackend_MeInvalidateRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case A_FILTER:
                if (((w0 >> 16) & 0xFF) > 1) {
                    filterLut = (void*)(uintptr_t)w1;
                    OotPspAudioBackend_MeInvalidateRange(filterLut, 8 * sizeof(s16));
                } else {
                    OotPspAudioBackend_MeInvalidateRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                    if (filterLut != NULL) {
                        OotPspAudioBackend_MeInvalidateRange(filterLut, 8 * sizeof(s16));
                    }
                }
                break;

            case A_LOADADPCM:
            case OOT_PSP_A_LOAD_ADPCM_CACHED:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, w0 & 0xFFFFFF);
                break;

            case A_LOADBUFF:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case A_SAVEBUFF:
                if (!OotPspAudioBackend_RangeIsWithin((void*)(uintptr_t)w1,
                                                      OotPspAudioBackend_CommandDmaSize(w0), privateOutput,
                                                      privateOutputBytes)) {
                    OotPspAudioBackend_MeInvalidateRange((void*)(uintptr_t)w1,
                                                         OotPspAudioBackend_CommandDmaSize(w0));
                }
                break;

            case A_SETLOOP:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
            case OOT_PSP_A_REVERB_SAVE:
            case OOT_PSP_A_REVERB_LOAD:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1,
                                                     sizeof(OotPspAudioReverbDownsampleCmd));
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_MeWritebackOutputs(const Acmd* cmdList, s32 cmdCount, const void* privateOutput,
                                                  u32 privateOutputBytes) {
    void* filterLut = NULL;
    s32 i;

    for (i = 0; i < cmdCount; i++) {
        u32 w0 = cmdList[i].words.w0;
        u32 w1 = cmdList[i].words.w1;

        switch (w0 >> 24) {
            case A_ADPCM:
            case A_RESAMPLE:
            case A_S8DEC:
                OotPspAudioBackend_MeWritebackRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case A_FILTER:
                if (((w0 >> 16) & 0xFF) > 1) {
                    filterLut = (void*)(uintptr_t)w1;
                } else {
                    OotPspAudioBackend_MeWritebackRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                    if (filterLut != NULL) {
                        OotPspAudioBackend_MeWritebackRange(filterLut, 8 * sizeof(s16));
                    }
                }
                break;

            case A_SAVEBUFF:
                if (!OotPspAudioBackend_RangeIsWithin((void*)(uintptr_t)w1,
                                                      OotPspAudioBackend_CommandDmaSize(w0), privateOutput,
                                                      privateOutputBytes)) {
                    OotPspAudioBackend_MeWritebackRange((void*)(uintptr_t)w1,
                                                        OotPspAudioBackend_CommandDmaSize(w0));
                }
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_InvalidateMeWritesFromCommands(const Acmd* cmdList, s32 cmdCount,
                                                              const void* privateOutput,
                                                              u32 privateOutputBytes) {
    void* filterLut = NULL;
    s32 i;

    for (i = 0; i < cmdCount; i++) {
        u32 w0 = cmdList[i].words.w0;
        u32 w1 = cmdList[i].words.w1;

        switch (w0 >> 24) {
            case A_ADPCM:
            case A_RESAMPLE:
            case A_S8DEC:
                OotPspAudioBackend_InvalidateRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case A_FILTER:
                if (((w0 >> 16) & 0xFF) > 1) {
                    filterLut = (void*)(uintptr_t)w1;
                } else {
                    OotPspAudioBackend_InvalidateRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                    if (filterLut != NULL) {
                        OotPspAudioBackend_InvalidateRange(filterLut, 8 * sizeof(s16));
                    }
                }
                break;

            case A_SAVEBUFF:
                if (!OotPspAudioBackend_RangeIsWithin((void*)(uintptr_t)w1,
                                                      OotPspAudioBackend_CommandDmaSize(w0), privateOutput,
                                                      privateOutputBytes)) {
                    OotPspAudioBackend_InvalidateRange((void*)(uintptr_t)w1,
                                                       OotPspAudioBackend_CommandDmaSize(w0));
                }
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_InvalidateMeWrites(const Acmd* cmdList, s32 cmdCount, const void* privateOutput,
                                                  u32 privateOutputBytes) {
    u32 i;

    if (sAudioMeWriteRangeOverflow) {
        OotPspAudioBackend_InvalidateMeWritesFromCommands(cmdList, cmdCount, privateOutput, privateOutputBytes);
    } else {
        for (i = 0; i < sAudioMeWriteRangeCount; i++) {
            sceKernelDcacheInvalidateRange(sAudioMeWriteRanges[i].address, sAudioMeWriteRanges[i].size);
        }
    }

    OotPspAudioBackend_ResetMeWriteRanges();
}

static void OotPspAudioBackend_InvalidateQueuedRingFrames(u32 writePos, u32 frames) {
    while (frames != 0) {
        u32 todo = frames;
        u32 untilWrap = OOT_PSP_AUDIO_RING_FRAMES - writePos;

        if (todo > untilWrap) {
            todo = untilWrap;
        }

        OotPspAudioBackend_InvalidateRange(&sAudioRing[writePos * OOT_PSP_AUDIO_CHANNELS],
                                           todo * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
        frames -= todo;
        writePos = (writePos + todo) & OOT_PSP_AUDIO_RING_MASK;
    }
}

static void OotPspAudioBackend_PublishPendingMeQueue(void) {
    u32 expectedWritePos;
    u32 resultWritePos;

    if ((sAudioMePendingQueueSrc == NULL) || (sAudioMePendingQueueFrames == 0)) {
        return;
    }

    resultWritePos = sAudioMeQueueResultWritePos & OOT_PSP_AUDIO_RING_MASK;
    expectedWritePos = (sAudioMePendingQueueWritePos + sAudioMePendingQueueFrames) & OOT_PSP_AUDIO_RING_MASK;
    if (resultWritePos != expectedWritePos) {
        OotPspAudioBackend_QueueCpuCopy(sAudioMePendingQueueSrc, sAudioMePendingQueueFrames,
                                       sAudioMePendingQueueWritePos);
        return;
    }

    OotPspAudioBackend_InvalidateQueuedRingFrames(sAudioMePendingQueueWritePos,
                                                  sAudioMePendingQueueFrames);
    /* Publish only after Allegrex can see every ring line written by the ME. */
    sAudioWritePos = resultWritePos;
}

static void OotPspAudioBackend_FallbackFromMe(const Acmd* cmdList, s32 cmdCount, const s16* queueSrc,
                                              u32 queueFrames, u32 queueWritePos) {
    sAudioMeInitialized = false;
    AUDIO_DIAG_INCREMENT(sAudioDiagnosticCpuMixes);
    AUDIO_DIAG_INCREMENT(sAudioDiagnosticMeFallbacks);
#if defined(OOTDEBUG)
    sAudioProfileMeFailures++;
    sAudioProfileCpuMixes++;
#endif
    OotPspAudioBackend_ResetMeWriteRanges();
    OotPspMixer_InvalidateStateCache();
    OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
    if ((queueSrc != NULL) && (queueFrames != 0)) {
        OotPspAudioBackend_QueueCpuCopy(queueSrc, queueFrames, queueWritePos);
    }
}

static void OotPspAudioBackend_ClearPendingMeCommand(void) {
    sAudioMeCommandPending = false;
    sAudioMePendingCmdList = NULL;
    sAudioMePendingCmdCount = 0;
    sAudioMePendingStartTime = 0;
    sAudioMePendingQueueSrc = NULL;
    sAudioMePendingQueueFrames = 0;
    sAudioMePendingQueueWritePos = 0;
}

static s32 OotPspAudioBackend_IsValidUid(SceUID uid) {
    return uid > 0;
}

static s32 OotPspAudioBackend_EnsureMeLockSema(void) {
    SceUID sema;

    if (OotPspAudioBackend_IsValidUid(sAudioMeLockSema)) {
        return 0;
    }

    sema = sceKernelCreateSema("OOT PSP AudioME", 0, 1, 1, NULL);
    if (!OotPspAudioBackend_IsValidUid(sema)) {
        sAudioMeLockSema = -1;
        return sema;
    }

    sAudioMeLockSema = sema;
    return 0;
}

static void OotPspAudioBackend_MeCompletionInterrupt(UNUSED int subIntr, UNUSED void* arg) {
    if (OotPspAudioBackend_IsValidUid(sAudioMeCompletionSema)) {
        sceKernelSignalSema(sAudioMeCompletionSema, 1);
    }
}

static s32 OotPspAudioBackend_EnsureMeCompletionInterrupt(void) {
    SceUID sema;
    s32 ret;

    if (sAudioMeCompletionInterruptReady) {
        return 0;
    }

    if (!OotPspAudioBackend_IsValidUid(sAudioMeCompletionSema)) {
        sema = sceKernelCreateSema("OOT PSP AudioMEDone", 0, 0, 1, NULL);
        if (!OotPspAudioBackend_IsValidUid(sema)) {
            sAudioMeCompletionSema = -1;
            return sema;
        }
        sAudioMeCompletionSema = sema;
    }

    ret = sceKernelRegisterSubIntrHandler(PSP_MECODEC_INT, 0, OotPspAudioBackend_MeCompletionInterrupt, NULL);
    if (ret < 0) {
        return ret;
    }

    ret = sceKernelEnableSubIntr(PSP_MECODEC_INT, 0);
    if (ret < 0) {
        sceKernelReleaseSubIntrHandler(PSP_MECODEC_INT, 0);
        return ret;
    }

    sAudioMeCompletionInterruptEnabled = true;
    meLibSync();
    sAudioMeCompletionInterruptReady = true;
    return 0;
}

static void OotPspAudioBackend_DrainMeCompletion(void) {
    if (!sAudioMeCompletionInterruptReady) {
        return;
    }

    while (sceKernelPollSema(sAudioMeCompletionSema, 1) == 0) {
    }
}

static void OotPspAudioBackend_LockMe(void) {
    if (OotPspAudioBackend_IsValidUid(sAudioMeLockSema)) {
        sceKernelWaitSema(sAudioMeLockSema, 1, NULL);
    }
}

static void OotPspAudioBackend_UnlockMe(void) {
    if (OotPspAudioBackend_IsValidUid(sAudioMeLockSema)) {
        sceKernelSignalSema(sAudioMeLockSema, 1);
    }
}

#if OOT_PSP_AUDIO_DIAGNOSTICS
static void OotPspAudioBackend_EndDiagnosticMeWait(u32 startUsec, s32 waited) {
    u32 elapsed;

    if (!waited) {
        return;
    }

    elapsed = sceKernelGetSystemTimeLow() - startUsec;
    sAudioDiagnosticMeWaiting = false;
    sAudioDiagnosticMeWaitLastUsec = elapsed;
    sAudioDiagnosticMeWaitTotalUsec += elapsed;
    sAudioDiagnosticMeWaits++;
    if (elapsed > sAudioDiagnosticMeWaitMaxUsec) {
        sAudioDiagnosticMeWaitMaxUsec = elapsed;
    }
}
#endif

static void OotPspAudioBackend_WaitForCommandsLocked(void) {
#if OOT_PSP_AUDIO_DIAGNOSTICS
    u32 diagnosticWaitStart = 0;
    s32 diagnosticWaited = false;
#endif

    if (!sAudioMeCommandPending) {
        return;
    }

#if OOT_PSP_AUDIO_DIAGNOSTICS
    if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
        diagnosticWaitStart = sceKernelGetSystemTimeLow();
        diagnosticWaited = true;
        sAudioDiagnosticMeWaitStartUsec = diagnosticWaitStart;
        sAudioDiagnosticMeWaiterThreadId = sceKernelGetThreadId();
        sAudioDiagnosticMeWaiting = true;
    }
#endif

    while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
        if (sAudioMeCompletionInterruptReady) {
            u32 elapsed = sceKernelGetSystemTimeLow() - sAudioMePendingStartTime;
            SceUInt timeout;

            if (elapsed >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
                break;
            }

            timeout = OOT_PSP_AUDIO_ME_TIMEOUT_US - elapsed;
            sceKernelWaitSema(sAudioMeCompletionSema, 1, &timeout);
            continue;
        }

        if ((sceKernelGetSystemTimeLow() - sAudioMePendingStartTime) >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
            break;
        }
        sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
    }

    if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
        u32 interruptTime;

        AUDIO_DIAG_INCREMENT(sAudioDiagnosticMeTimeouts);
        meLibEmitSoftwareInterrupt();
        interruptTime = sceKernelGetSystemTimeLow();
        while ((sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) &&
               ((sceKernelGetSystemTimeLow() - interruptTime) < 10000)) {
            if (sAudioMeCompletionInterruptReady) {
                u32 elapsed = sceKernelGetSystemTimeLow() - interruptTime;
                SceUInt timeout;

                if (elapsed >= 10000) {
                    break;
                }
                timeout = 10000 - elapsed;
                sceKernelWaitSema(sAudioMeCompletionSema, 1, &timeout);
            } else {
                sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
            }
        }

        if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
#if OOT_PSP_AUDIO_DIAGNOSTICS
            OotPspAudioBackend_EndDiagnosticMeWait(diagnosticWaitStart, diagnosticWaited);
#endif
            OotPspAudioBackend_FallbackFromMe(sAudioMePendingCmdList, sAudioMePendingCmdCount,
                                              sAudioMePendingQueueSrc, sAudioMePendingQueueFrames,
                                              sAudioMePendingQueueWritePos);
            OotPspAudioBackend_ClearPendingMeCommand();
            return;
        }
    }

#if OOT_PSP_AUDIO_DIAGNOSTICS
    OotPspAudioBackend_EndDiagnosticMeWait(diagnosticWaitStart, diagnosticWaited);
#endif

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        OotPspAudioBackend_FallbackFromMe(sAudioMePendingCmdList, sAudioMePendingCmdCount,
                                          sAudioMePendingQueueSrc, sAudioMePendingQueueFrames,
                                          sAudioMePendingQueueWritePos);
        OotPspAudioBackend_ClearPendingMeCommand();
        return;
    }

    OotPspAudioBackend_InvalidateMeWrites(
        sAudioMePendingCmdList, sAudioMePendingCmdCount, sAudioMePendingQueueSrc,
        sAudioMePendingQueueFrames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    OotPspAudioBackend_PublishPendingMeQueue();
    OotPspAudioBackend_ClearPendingMeCommand();
}

void OotPspAudioBackend_WaitForCommands(void) {
    OotPspAudioBackend_LockMe();
    OotPspAudioBackend_WaitForCommandsLocked();
    OotPspAudioBackend_UnlockMe();
}

static void OotPspAudioBackend_SubmitCommandsInternal(const Acmd* cmdList, s32 cmdCount, const s16* queueSrc,
                                                      u32 queueFrames) {
    u32 queueWritePos = sAudioWritePos;

    if ((cmdList == NULL) || (cmdCount <= 0)) {
        return;
    }

    if ((queueSrc == NULL) || (queueFrames == 0)) {
        queueSrc = NULL;
        queueFrames = 0;
    }

    OotPspAudioBackend_LockMe();
    OotPspAudioBackend_WaitForCommandsLocked();
    queueWritePos = sAudioWritePos;
    if (queueFrames > OotPspAudioBackend_FreeFrames()) {
        /* The producer reserves maxAiBufferLength before synthesis, so this
         * can only be reached by an out-of-band caller. Preserve mixer state
         * even though that caller supplied more PCM than the ring can hold. */
        queueSrc = NULL;
        queueFrames = 0;
    }

    if (!sAudioMeInitialized) {
        AUDIO_DIAG_INCREMENT(sAudioDiagnosticCpuMixes);
#if defined(OOTDEBUG)
        sAudioProfileCpuMixes++;
#endif
        OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
        if (queueFrames != 0) {
            OotPspAudioBackend_QueueCpuCopy(queueSrc, queueFrames, queueWritePos);
        }
        OotPspAudioBackend_UnlockMe();
        return;
    }

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        OotPspAudioBackend_FallbackFromMe(cmdList, cmdCount, queueSrc, queueFrames, queueWritePos);
        OotPspAudioBackend_UnlockMe();
        return;
    }

    /*
     * The CPU only writes back command-owned memory. The ME invalidates its
     * cache before consuming commands, then publishes persistent mixer state
     * and queues the private PCM output before publishing IDLE.
     */
    OotPspAudioBackend_WritebackMeInputs(cmdList, cmdCount, queueSrc,
                                         queueFrames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    OotPspAudioBackend_DrainMeCompletion();
    sAudioMeCmdList = (u32)(uintptr_t)cmdList;
    sAudioMeCmdCount = (u32)cmdCount;
    sAudioMeQueueSrc = (u32)(uintptr_t)queueSrc;
    sAudioMeQueueFrames = queueFrames;
    sAudioMeQueueWritePos = queueWritePos;
    sAudioMeQueueResultWritePos = queueWritePos;
    sAudioMeProgress = 0;
    sAudioMePendingCmdList = cmdList;
    sAudioMePendingCmdCount = cmdCount;
    sAudioMePendingStartTime = sceKernelGetSystemTimeLow();
    sAudioMePendingQueueSrc = queueSrc;
    sAudioMePendingQueueFrames = queueFrames;
    sAudioMePendingQueueWritePos = queueWritePos;
    sAudioMeCommandPending = true;
    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_RUN;
    AUDIO_DIAG_INCREMENT(sAudioDiagnosticMeSubmits);
#if defined(OOTDEBUG)
    sAudioProfileMeSubmits++;
#endif
    OotPspAudioBackend_UnlockMe();
}

void OotPspAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount) {
    OotPspAudioBackend_SubmitCommandsInternal(cmdList, cmdCount, NULL, 0);
}

void OotPspAudioBackend_SubmitCommandsAndQueue(const Acmd* cmdList, s32 cmdCount, const void* buf, u32 size) {
    const u32 frameBytes = sizeof(s16) * OOT_PSP_AUDIO_CHANNELS;

    OotPspAudioBackend_SubmitCommandsInternal(cmdList, cmdCount, buf, size / frameBytes);
}

void OotPspAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount) {
    OotPspAudioBackend_SubmitCommands(cmdList, cmdCount);
    OotPspAudioBackend_WaitForCommands();
}

static u32 OotPspAudioBackend_CalculateSourceChunkFrames(u32 frequency) {
    return (((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * frequency) + OOT_PSP_AUDIO_OUTPUT_FREQUENCY - 1) /
            OOT_PSP_AUDIO_OUTPUT_FREQUENCY);
}

static u32 OotPspAudioBackend_SourceChunkFrames(void) {
    return sAudioSourceChunkFrames;
}

static u32 OotPspAudioBackend_TargetBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * OOT_PSP_AUDIO_TARGET_CHUNKS;
}

static u32 OotPspAudioBackend_IoBackoffFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS;
}

static u32 OotPspAudioBackend_UrgentBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * OOT_PSP_AUDIO_URGENT_CHUNKS;
}

static u32 OotPspAudioBackend_CalculateResampleStep(u32 frequency) {
    u32 whole = frequency / OOT_PSP_AUDIO_OUTPUT_FREQUENCY;
    u32 remainder = frequency % OOT_PSP_AUDIO_OUTPUT_FREQUENCY;
    u32 fraction = 0;
    s32 i;

    for (i = 0; i < OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS; i++) {
        remainder <<= 1;
        fraction <<= 1;
        if (remainder >= OOT_PSP_AUDIO_OUTPUT_FREQUENCY) {
            remainder -= OOT_PSP_AUDIO_OUTPUT_FREQUENCY;
            fraction++;
        }
    }

    return (whole << OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS) | fraction;
}

static u32 OotPspAudioBackend_BufferedFrames(void) {
    return (sAudioWritePos - sAudioReadPos) & OOT_PSP_AUDIO_RING_MASK;
}

static u32 OotPspAudioBackend_PendingMeQueueFrames(void) {
    return sAudioMeCommandPending ? sAudioMePendingQueueFrames : 0;
}

static u32 OotPspAudioBackend_FreeFrames(void) {
    u32 freeFrames = (OOT_PSP_AUDIO_RING_FRAMES - 1) - OotPspAudioBackend_BufferedFrames();
    u32 pendingFrames = OotPspAudioBackend_PendingMeQueueFrames();

    return pendingFrames < freeFrames ? freeFrames - pendingFrames : 0;
}

static u32 OotPspAudioBackend_RestFrames(void) {
    s32 rest;

    if ((sAudioOutputChannel < 0) && !sAudioHardwareSrc) {
        return 0;
    }

    rest = sAudioOutputFrames;
    return (rest > 0) ? (u32)rest : 0;
}

static u32 OotPspAudioBackend_TotalBufferedFrames(void) {
    return OotPspAudioBackend_BufferedFrames() + OotPspAudioBackend_PendingMeQueueFrames() +
           OotPspAudioBackend_RestFrames() + sAudioPendingOutputFrames;
}

static u32 OotPspAudioBackend_ReportableFrames(void) {
    u32 buffered = OotPspAudioBackend_TotalBufferedFrames();
    u32 targetFrames = OotPspAudioBackend_TargetBufferFrames();

    if (buffered <= targetFrames) {
        return 0;
    }

    return buffered - targetFrames;
}

static s16 OotPspAudioBackend_LerpSample(s16 current, s16 next, u32 frac) {
    s32 scaledFrac = frac >> (OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS - OOT_PSP_AUDIO_LERP_FRAC_BITS);
    s32 delta = ((s32)next - current) * scaledFrac;
    s32 sign = delta >> 31;

    delta += ((1 << (OOT_PSP_AUDIO_LERP_FRAC_BITS - 1)) ^ sign) - sign;

    return current + (delta >> OOT_PSP_AUDIO_LERP_FRAC_BITS);
}

static s16 OotPspAudioBackend_LerpHalfSample(s16 current, s16 next) {
    s32 delta = (s32)next - current;

    delta += 1 | (delta >> 31);

    return current + (delta >> 1);
}

static s32 OotPspAudioBackend_OutputMix(s16* mix) {
    sceKernelDcacheWritebackRange(mix, sizeof(sAudioMix[0]));
    return sceAudioOutputBlocking(sAudioOutputChannel, PSP_AUDIO_VOLUME_MAX, mix);
}

static s32 OotPspAudioBackend_OutputHardwareSrc(const s16* samples, u32 frames, s32 writeback) {
    if (writeback) {
        OotPspAudioBackend_WritebackRange(samples, frames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    }

    return sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, (void*)samples);
}

static void OotPspAudioBackend_PrepareSilenceFrames(s16* mix, u32 frames) {
    memset(mix, 0, frames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
}

static void OotPspAudioBackend_PrepareSilence(s16* mix) {
    OotPspAudioBackend_PrepareSilenceFrames(mix, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES);
}

static void OotPspAudioBackend_FadeChunkToSilenceState(s16** outPtr, u32 frames, s16 left, s16 right,
                                                       s16* lastLeft, s16* lastRight, u32* resampleFrac) {
    s16* out = *outPtr;
    u32 i;

    for (i = 0; i < frames; i++) {
        s32 fade = (s32)(frames - i);

        *out++ = (s16)(((s32)left * fade) / (s32)(frames + 1));
        *out++ = (s16)(((s32)right * fade) / (s32)(frames + 1));
    }

    *outPtr = out;
    *lastLeft = 0;
    *lastRight = 0;
    *resampleFrac = 0;
}

static s32 OotPspAudioBackend_CanRunUpdate(void) {
    u32 reserveFrames = gAudioCtx.audioBufferParameters.maxAiBufferLength;

    if ((reserveFrames == 0) || (reserveFrames >= OOT_PSP_AUDIO_RING_FRAMES)) {
        reserveFrames = OotPspAudioBackend_SourceChunkFrames();
    }

    return OotPspAudioBackend_FreeFrames() >= reserveFrames;
}

static s32 OotPspAudioBackend_TryRunUpdate(void) {
    if (!OotPspAudioBackend_CanRunUpdate()) {
        AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_RING_FULL);
        AUDIO_DIAG_INCREMENT(sAudioDiagnosticRingFull);
        return false;
    }

    if ((OotPspAudioBackend_TotalBufferedFrames() >= OotPspAudioBackend_IoBackoffFrames()) &&
        OotPsp_AssetReadHasForegroundPressure()) {
        AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_IO_BACKOFF);
        AUDIO_DIAG_INCREMENT(sAudioDiagnosticIoBackoffs);
        sceKernelDelayThread(OOT_PSP_AUDIO_PRODUCER_IO_BACKOFF_USEC);
        return false;
    }

    AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_UPDATE);
    AudioThread_Update();
    AUDIO_DIAG_INCREMENT(sAudioDiagnosticUpdates);
    sceKernelRotateThreadReadyQueue(OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY);
    return true;
}

static void OotPspAudioBackend_RunUpdates(u32 maxUpdates) {
    u32 updates = 0;
    u32 buffered = OotPspAudioBackend_TotalBufferedFrames();
    u32 targetFrames = OotPspAudioBackend_TargetBufferFrames();

    while ((updates < maxUpdates) && (buffered < targetFrames)) {
        if (!OotPspAudioBackend_TryRunUpdate()) {
            break;
        }

        updates++;
        buffered = OotPspAudioBackend_TotalBufferedFrames();
    }
}

static u32 OotPspAudioBackend_RenderOutputChunkState(u32 buffered, u32* readPosPtr, u32* resampleFracPtr,
                                                     u32 resampleStep, s16* lastLeftPtr, s16* lastRightPtr,
                                                     u32* flagsPtr, s16* mix) {
    u32 readPos = *readPosPtr;
    u32 resampleFrac = *resampleFracPtr;
    s16 lastLeft = *lastLeftPtr;
    s16 lastRight = *lastRightPtr;
    u32 sourceFrames = 0;
    u32 flags = 0;
    s16* out = mix;
    u32 i;

    if ((resampleStep == OOT_PSP_AUDIO_RESAMPLE_STEP_2X) && (resampleFrac == 0)) {
        u32 maxPairs = OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES / 2;
        u32 renderedPairs;
        const u32* packedRing = (const u32*)sAudioRing;
        u32* packedOut = (u32*)out;
        u32 current;

        if (buffered <= 1) {
            return 0;
        }

        renderedPairs = buffered - 1;
        if (renderedPairs > maxPairs) {
            renderedPairs = maxPairs;
        }

        current = packedRing[readPos];
        for (i = 0; i < renderedPairs; i++) {
            u32 next;
            s16 left = (s16)(u16)current;
            s16 right = (s16)(current >> 16);
            s16 halfLeft;
            s16 halfRight;

            readPos++;
            if (readPos == OOT_PSP_AUDIO_RING_FRAMES) {
                readPos = 0;
            }
            next = packedRing[readPos];
            halfLeft = OotPspAudioBackend_LerpHalfSample(left, (s16)(u16)next);
            halfRight = OotPspAudioBackend_LerpHalfSample(right, (s16)(next >> 16));

            *packedOut++ = current;
            *packedOut++ = (u16)halfLeft | ((u32)(u16)halfRight << 16);
            lastLeft = halfLeft;
            lastRight = halfRight;
            current = next;
        }
        out = (s16*)packedOut;

        resampleFrac = 0;

        if (renderedPairs < maxPairs) {
            OotPspAudioBackend_FadeChunkToSilenceState(&out,
                                                       OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES - (renderedPairs * 2),
                                                       lastLeft, lastRight, &lastLeft, &lastRight, &resampleFrac);
            flags |= OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN;
        }

        *readPosPtr = readPos;
        *resampleFracPtr = resampleFrac;
        *lastLeftPtr = lastLeft;
        *lastRightPtr = lastRight;
        *flagsPtr = flags;
        return renderedPairs;
    }

    if (buffered <= 1) {
        return 0;
    }

    for (i = 0; i < OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES; i++) {
        u32 ringOffset;
        u32 nextRingOffset;
        u32 advance;
        s16 left;
        s16 right;

        if (buffered <= 1) {
            break;
        }

        ringOffset = readPos * OOT_PSP_AUDIO_CHANNELS;
        nextRingOffset = ((readPos + 1) & OOT_PSP_AUDIO_RING_MASK) * OOT_PSP_AUDIO_CHANNELS;
        left = OotPspAudioBackend_LerpSample(sAudioRing[ringOffset + 0], sAudioRing[nextRingOffset + 0],
                                             resampleFrac);
        right = OotPspAudioBackend_LerpSample(sAudioRing[ringOffset + 1], sAudioRing[nextRingOffset + 1],
                                              resampleFrac);

        *out++ = left;
        *out++ = right;
        lastLeft = left;
        lastRight = right;

        resampleFrac += resampleStep;
        advance = resampleFrac >> OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS;
        resampleFrac &= OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK;

        if (advance > (buffered - 1)) {
            advance = buffered - 1;
        }

        readPos = (readPos + advance) & OOT_PSP_AUDIO_RING_MASK;
        sourceFrames += advance;
        buffered -= advance;
    }

    if (i < OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES) {
        OotPspAudioBackend_FadeChunkToSilenceState(&out, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES - i, lastLeft,
                                                   lastRight, &lastLeft, &lastRight, &resampleFrac);
        flags |= OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN;
    }

    *readPosPtr = readPos;
    *resampleFracPtr = resampleFrac;
    *lastLeftPtr = lastLeft;
    *lastRightPtr = lastRight;
    *flagsPtr = flags;
    return sourceFrames;
}

static u32 OotPspAudioBackend_RenderOutputChunkCpu(u32 buffered, s16* mix) {
    u32 readPos = sAudioReadPos;
    u32 resampleFrac = sAudioResampleFrac;
    s16 lastLeft = sAudioLastLeft;
    s16 lastRight = sAudioLastRight;
    u32 flags = 0;
    u32 sourceFrames;

    sourceFrames = OotPspAudioBackend_RenderOutputChunkState(buffered, &readPos, &resampleFrac, sAudioResampleStep,
                                                            &lastLeft, &lastRight, &flags, mix);
    if (sourceFrames == 0) {
        return 0;
    }

    sAudioReadPos = readPos;
    sAudioResampleFrac = resampleFrac;
    sAudioLastLeft = lastLeft;
    sAudioLastRight = lastRight;
    if (flags & OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN) {
        sAudioPlaybackPrimed = false;
        AUDIO_DIAG_INCREMENT(sAudioDiagnosticUnderruns);
    }

    return sourceFrames;
}

static void OotPspAudioBackend_MeQueueBuffer(s32 invalidateSource) {
    const s16* samples = (const s16*)(uintptr_t)sAudioMeQueueSrc;
    u32 frames = sAudioMeQueueFrames;
    u32 writePos = sAudioMeQueueWritePos & OOT_PSP_AUDIO_RING_MASK;

    if (invalidateSource) {
        OotPspAudioBackend_MeInvalidateRange(samples, frames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    }

    while (frames != 0) {
        u32 todo = frames;
        u32 untilWrap = OOT_PSP_AUDIO_RING_FRAMES - writePos;
        s16* dst;
        u32 bytes;

        if (todo > untilWrap) {
            todo = untilWrap;
        }

        dst = &sAudioRing[writePos * OOT_PSP_AUDIO_CHANNELS];
        bytes = todo * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
        OotPspAudioBackend_MeInvalidateRange(dst, bytes);
        memcpy(dst, samples, bytes);
        OotPspAudioBackend_MeWritebackRange(dst, bytes);

        samples += todo * OOT_PSP_AUDIO_CHANNELS;
        frames -= todo;
        writePos = (writePos + todo) & OOT_PSP_AUDIO_RING_MASK;
    }

    sAudioMeQueueResultWritePos = writePos;
}

static u32 OotPspAudioBackend_RenderOutputChunk(s16* mix) {
    u32 buffered = OotPspAudioBackend_BufferedFrames();

    /* The default 22.05 kHz source rate uses the small exact-2x path. Keeping
     * output conversion on Allegrex leaves the ME exclusively to the mixer. */
    return OotPspAudioBackend_RenderOutputChunkCpu(buffered, mix);
}

static void OotPspAudioBackend_CopyHardwareSrcChunk(u32 readPos, s16* mix) {
    u32 frames = OotPspAudioBackend_SourceChunkFrames();
    u32 firstFrames = OOT_PSP_AUDIO_RING_FRAMES - readPos;

    if (firstFrames > frames) {
        firstFrames = frames;
    }

    memcpy(mix, &sAudioRing[readPos * OOT_PSP_AUDIO_CHANNELS],
           firstFrames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    if (firstFrames < frames) {
        memcpy(mix + (firstFrames * OOT_PSP_AUDIO_CHANNELS), sAudioRing,
               (frames - firstFrames) * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    }
}

static void OotPspAudioBackend_RunHardwareSrcOutput(void) {
    u32 heldFrames = 0;
    u32 mixIndex = 0;

    AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_STARTING);

    /*
     * A direct ring buffer remains owned by the audio driver until the next
     * blocking submission returns. Keeping heldFrames behind sAudioReadPos
     * prevents the producer from recycling those samples in the meantime.
     */
    while (sAudioOutputThreadRunning) {
        u32 readPos = sAudioReadPos;
        u32 buffered = OotPspAudioBackend_BufferedFrames();
        u32 available = (heldFrames < buffered) ? buffered - heldFrames : 0;
        u32 sourceFrames = 0;
        u32 sourceReadPos = (readPos + heldFrames) & OOT_PSP_AUDIO_RING_MASK;
        s16* mix = sAudioMix[mixIndex];
        const s16* output = mix;
        s32 directRing = false;
        s32 usedMix = true;
        s32 ret;

        AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_PREPARE);
        if (!sAudioPlaybackPrimed && (available >= OotPspAudioBackend_TargetBufferFrames())) {
            sAudioPlaybackPrimed = true;
        }

        if (!sAudioPlaybackPrimed || (available < OotPspAudioBackend_SourceChunkFrames())) {
            if (sAudioPlaybackPrimed) {
                AUDIO_DIAG_INCREMENT(sAudioDiagnosticUnderruns);
            }
            sAudioPlaybackPrimed = false;
            AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_PRIMING);
            OotPspAudioBackend_PrepareSilenceFrames(mix, OotPspAudioBackend_SourceChunkFrames());
        } else {
            sourceFrames = OotPspAudioBackend_SourceChunkFrames();
            if (((sourceReadPos & ((OOT_PSP_AUDIO_CACHE_LINE_SIZE /
                                   (OOT_PSP_AUDIO_CHANNELS * sizeof(s16))) - 1)) == 0) &&
                (sourceReadPos + sourceFrames <= OOT_PSP_AUDIO_RING_FRAMES)) {
                /* Ring data was already written back by its producer. */
                output = &sAudioRing[sourceReadPos * OOT_PSP_AUDIO_CHANNELS];
                directRing = true;
                usedMix = false;
            } else {
                OotPspAudioBackend_CopyHardwareSrcChunk(sourceReadPos, mix);
            }
        }

        AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_WAIT_HARDWARE);
        ret = OotPspAudioBackend_OutputHardwareSrc(output, OotPspAudioBackend_SourceChunkFrames(), usedMix);
        if (ret >= 0) {
            readPos = (readPos + heldFrames) & OOT_PSP_AUDIO_RING_MASK;
            if (directRing) {
                heldFrames = sourceFrames;
                sAudioOutputFrames = 0;
            } else {
                readPos = (readPos + sourceFrames) & OOT_PSP_AUDIO_RING_MASK;
                heldFrames = 0;
                sAudioOutputFrames = sourceFrames;
                mixIndex ^= 1;
            }
            sAudioReadPos = readPos;
        } else {
            AUDIO_DIAG_INCREMENT(sAudioDiagnosticOutputErrors);
            AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_ERROR_BACKOFF);
            sAudioOutputFrames = 0;
            sAudioPlaybackPrimed = false;
            sceKernelDelayThread(OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC);
        }
    }

    AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_STOPPED);
}

static int OotPspAudioBackend_OutputThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 mixIndex = 0;

    AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_STARTING);
    if (sAudioHardwareSrc) {
        OotPspAudioBackend_RunHardwareSrcOutput();
        goto exit;
    }

    while (sAudioOutputThreadRunning) {
        u32 buffered;
        u32 sourceFrames;
        s16* mix = sAudioMix[mixIndex];
        s32 ret;

        AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_PREPARE);
        buffered = OotPspAudioBackend_BufferedFrames();

        if (!sAudioPlaybackPrimed && (buffered >= OotPspAudioBackend_TargetBufferFrames())) {
            sAudioPlaybackPrimed = true;
        }

        if (!sAudioPlaybackPrimed || (buffered < OotPspAudioBackend_SourceChunkFrames())) {
            if (sAudioPlaybackPrimed) {
                AUDIO_DIAG_INCREMENT(sAudioDiagnosticUnderruns);
            }
            sAudioPlaybackPrimed = false;
            AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_PRIMING);
            OotPspAudioBackend_PrepareSilence(mix);
            sourceFrames = 0;
        } else {
            sourceFrames = OotPspAudioBackend_RenderOutputChunk(mix);
            if (sourceFrames == 0) {
                sAudioPlaybackPrimed = false;
                OotPspAudioBackend_PrepareSilence(mix);
            }
        }

        /* The driver blocks this thread until the current hardware buffer can
         * be replaced. The other mix buffer remains untouched while playing. */
        sAudioPendingOutputFrames = sourceFrames;
        AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_WAIT_HARDWARE);
        ret = OotPspAudioBackend_OutputMix(mix);
        if (ret >= 0) {
            sAudioOutputFrames = sourceFrames;
            sAudioPendingOutputFrames = 0;
            mixIndex ^= 1;
        } else {
            AUDIO_DIAG_INCREMENT(sAudioDiagnosticOutputErrors);
            AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_ERROR_BACKOFF);
            sAudioOutputFrames = 0;
            sAudioPendingOutputFrames = 0;
            sAudioPlaybackPrimed = false;
            sceKernelDelayThread(OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC);
        }
    }

exit:
    AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_STOPPED);
    sAudioOutputThreadRunning = false;
    sAudioOutputThreadId = -1;

    sceKernelExitDeleteThread(0);
    return 0;
}

static int OotPspAudioBackend_ProducerThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 nextUpdateUsec;

    AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_PRIMING);
    OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PRIME);
    nextUpdateUsec = sceKernelGetSystemTimeLow();

    while (sAudioProducerThreadRunning) {
        s32 delayUsec;
        s32 isUrgent;
        u32 now;

        nextUpdateUsec += OOT_PSP_AUDIO_UPDATE_USEC;
        now = sceKernelGetSystemTimeLow();
        delayUsec = (s32)(nextUpdateUsec - now);
        if (delayUsec > 0) {
            AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_TIMER_WAIT);
            sceKernelDelayThread(delayUsec);
        } else if (delayUsec < 0) {
#if OOT_PSP_AUDIO_DIAGNOSTICS
            u32 lateUsec = (u32)-delayUsec;

            if (lateUsec >= OOT_PSP_AUDIO_DIAGNOSTIC_LATE_USEC) {
                sAudioDiagnosticProducerLate++;
                if (lateUsec > sAudioDiagnosticProducerLateMaxUsec) {
                    sAudioDiagnosticProducerLateMaxUsec = lateUsec;
                }
            }
#endif
            if (delayUsec < -(s32)OOT_PSP_AUDIO_UPDATE_USEC) {
                nextUpdateUsec = now;
            }
        }

        /* Keep generation at the game thread's priority. Raising it under
         * pressure fixes audio at the cost of visible frame-time spikes. */
        isUrgent = OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_UrgentBufferFrames();

        if (isUrgent) {
            AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_CATCHUP);
            AUDIO_DIAG_INCREMENT(sAudioDiagnosticCatchups);
            OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_CATCHUP);
        } else {
            AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_UPDATE);
            OotPspAudioBackend_TryRunUpdate();
        }
    }

    AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_STOPPED);
    sAudioProducerThreadRunning = false;
    sAudioProducerThreadId = -1;

    sceKernelExitDeleteThread(0);
    return 0;
}

static s32 OotPspAudioBackend_IsOutputInitialized(void) {
    return sAudioHardwareSrc || (sAudioOutputChannel >= 0);
}

#if OOT_PSP_AUDIO_HARDWARE_SRC
static s32 OotPspAudioBackend_IsHardwareSrcFrequency(u32 frequency) {
    switch (frequency) {
        case 8000:
        case 11050:
        case 12000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
            return true;

        default:
            return false;
    }
}
#endif

static s32 OotPspAudioBackend_TryReserveHardwareSrc(u32 frequency, u32 frames) {
#if OOT_PSP_AUDIO_HARDWARE_SRC
    if (OotPspAudioBackend_IsHardwareSrcFrequency(frequency) &&
        (sceAudioSRCChReserve(frames, frequency, OOT_PSP_AUDIO_CHANNELS) >= 0)) {
        sAudioHardwareSrc = true;
        return true;
    }
#else
    (void)frequency;
    (void)frames;
#endif

    sAudioHardwareSrc = false;
    return false;
}

s32 OotPspAudioBackend_Init(void) {
    if (OotPspAudioBackend_IsOutputInitialized()) {
        return 0;
    }

    memset(sAudioRing, 0, sizeof(sAudioRing));
    memset(sAudioMix, 0, sizeof(sAudioMix));
    sAudioReadPos = 0;
    sAudioWritePos = 0;
    sAudioOutputFrames = 0;
    sAudioPendingOutputFrames = 0;
    sAudioPlaybackPrimed = false;
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
    sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;
    sAudioSourceChunkFrames = OotPspAudioBackend_CalculateSourceChunkFrames(sAudioSourceFrequency);
    sAudioResampleStep = OotPspAudioBackend_CalculateResampleStep(sAudioSourceFrequency);

    sceAudioOutput2Release();
    sceAudioSRCChRelease();
    sAudioHardwareSrc = false;
    if (!OotPspAudioBackend_TryReserveHardwareSrc(sAudioSourceFrequency, sAudioSourceChunkFrames)) {
        sAudioOutputChannel =
            sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
        if (sAudioOutputChannel < 0) {
            return -1;
        }
    }

    return 0;
}

#if OOT_PSP_AUDIO_DIAGNOSTICS
typedef struct {
    s32 valid;
    s32 status;
    s32 priority;
    s32 waitType;
    u64 runClocks;
} OotPspAudioDiagnosticThreadSample;

typedef struct {
    u32 updates;
    u32 profileUpdates;
    u32 profileWaitUsec;
    u32 profilePrepareUsec;
    u32 profileSynthUsec;
    u32 profileSubmitUsec;
    u32 profileSequenceUsec;
    u32 profileCommandBuildUsec;
    u32 profileAbiCommands;
    u32 profileSampleDmas;
    u32 meSubmits;
    u32 cpuMixes;
    u32 meFallbacks;
    u32 meTimeouts;
    u32 meWaits;
    u32 meWaitTotalUsec;
    u32 meWaitMaxUsec;
    u32 meWaitLastUsec;
    u32 underruns;
    u32 outputErrors;
    u32 ioBackoffs;
    u32 ringFull;
    u32 catchups;
    u32 producerLate;
    u32 producerLateMaxUsec;
} OotPspAudioDiagnosticCounters;

typedef struct {
    u32 opcode;
    u32 ticks;
    u32 calls;
    u32 maxTicks;
} OotPspAudioOpcodeRank;

static const char* OotPspAudioBackend_OpcodeName(u32 opcode) {
    switch (opcode) {
        case A_SPNOOP:
            return "SPNOOP";
        case A_ADPCM:
            return "ADPCM";
        case A_CLEARBUFF:
            return "CLEAR";
        case A_UNK3:
            return "UNK3";
        case A_ADDMIXER:
            return "ADDMIX";
        case A_RESAMPLE:
            return "RESAMPLE";
        case A_RESAMPLE_ZOH:
            return "RESAMP_ZOH";
        case A_FILTER:
            return "FILTER";
        case A_SETBUFF:
            return "SETBUFF";
        case A_DUPLICATE:
            return "DUPLICATE";
        case A_DMEMMOVE:
            return "DMEMMOVE";
        case A_LOADADPCM:
            return "LOADADPCM";
        case A_MIXER:
            return "MIXER";
        case A_INTERLEAVE:
            return "INTERLEAVE";
        case A_HILOGAIN:
            return "HILOGAIN";
        case A_SETLOOP:
            return "SETLOOP";
        case OOT_PSP_A_COPYBLOCKS:
            return "COPYBLOCKS";
        case A_INTERL:
            return "INTERL";
        case A_ENVSETUP1:
            return "ENVSETUP1";
        case A_ENVMIXER:
            return "ENVMIXER";
        case A_LOADBUFF:
            return "LOADBUFF";
        case A_SAVEBUFF:
            return "SAVEBUFF";
        case A_ENVSETUP2:
            return "ENVSETUP2";
        case A_S8DEC:
            return "S8DEC";
        case OOT_PSP_A_REVERB_DOWNSAMPLE:
            return "RV_DOWN";
        case A_UNK19:
            return "UNK19";
        case OOT_PSP_A_REVERB_SAVE:
            return "RV_SAVE";
        case OOT_PSP_A_REVERB_LOAD:
            return "RV_LOAD";
        case OOT_PSP_A_LOAD_SAMPLE_CACHED:
            return "LD_SAMPLE";
        case OOT_PSP_A_LOAD_ADPCM_CACHED:
            return "LD_BOOK";
        case OOT_PSP_MIXER_PROFILE_OPCODE_IDLE:
            return "IDLE";
        default:
            return "UNKNOWN";
    }
}

static s32 OotPspAudioBackend_SnapshotMeOpcodeProfile(OotPspMixerOpcodeProfile* snapshot) {
    volatile OotPspMixerOpcodeProfile* source = sAudioMeOpcodeProfile;
    s32 attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        u32 sequenceBefore = source->sequence;
        u32 sequenceAfter;
        s32 opcode;

        if (sequenceBefore & 1) {
            continue;
        }

        __asm__ volatile("sync" ::: "memory");
        snapshot->sequence = sequenceBefore;
        snapshot->jobs = source->jobs;
        snapshot->commands = source->commands;
        snapshot->jobTicks = source->jobTicks;
        snapshot->jobMaxTicks = source->jobMaxTicks;
        snapshot->lastJobTicks = source->lastJobTicks;
        snapshot->lastJobCommands = source->lastJobCommands;
        snapshot->lastJobSlowOpcode = source->lastJobSlowOpcode;
        snapshot->lastJobSlowTicks = source->lastJobSlowTicks;
        snapshot->maxJobCommands = source->maxJobCommands;
        snapshot->maxJobSlowOpcode = source->maxJobSlowOpcode;
        snapshot->maxJobSlowTicks = source->maxJobSlowTicks;
        snapshot->currentOpcode = source->currentOpcode;
        snapshot->currentCommandIndex = source->currentCommandIndex;
        for (opcode = 0; opcode < OOT_PSP_MIXER_PROFILE_OPCODE_COUNT; opcode++) {
            snapshot->opcodeCalls[opcode] = source->opcodeCalls[opcode];
            snapshot->opcodeTicks[opcode] = source->opcodeTicks[opcode];
            snapshot->opcodeMaxTicks[opcode] = source->opcodeMaxTicks[opcode];
        }
        __asm__ volatile("sync" ::: "memory");
        sequenceAfter = source->sequence;
        if ((sequenceBefore == sequenceAfter) && !(sequenceAfter & 1)) {
            snapshot->sequence = sequenceAfter;
            return true;
        }
    }

    return false;
}

static void OotPspAudioBackend_InsertOpcodeRank(OotPspAudioOpcodeRank ranks[3], u32 opcode,
                                                u32 ticks, u32 calls, u32 maxTicks) {
    s32 rank;

    if ((ticks == 0) || (calls == 0)) {
        return;
    }

    for (rank = 0; rank < 3; rank++) {
        if (ticks > ranks[rank].ticks) {
            s32 move;

            for (move = 2; move > rank; move--) {
                ranks[move] = ranks[move - 1];
            }
            ranks[rank].opcode = opcode;
            ranks[rank].ticks = ticks;
            ranks[rank].calls = calls;
            ranks[rank].maxTicks = maxTicks;
            return;
        }
    }
}

static u32 OotPspAudioBackend_OpcodePercentTenths(u32 ticks, u64 totalTicks) {
    return totalTicks != 0 ? (u32)(((u64)ticks * 1000) / totalTicks) : 0;
}

static void OotPspAudioBackend_PrintMeOpcodeProfile(const OotPspMixerOpcodeProfile* current,
                                                    const OotPspMixerOpcodeProfile* previous) {
    OotPspAudioOpcodeRank ranks[3];
    u32 jobs = current->jobs - previous->jobs;
    u32 commands = current->commands - previous->commands;
    u32 jobTicks = current->jobTicks - previous->jobTicks;
    u64 totalOpcodeTicks = 0;
    s32 opcode;
    s32 rank;

    memset(ranks, 0, sizeof(ranks));
    for (rank = 0; rank < 3; rank++) {
        ranks[rank].opcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    }

    for (opcode = 0; opcode < OOT_PSP_MIXER_PROFILE_OPCODE_COUNT; opcode++) {
        u32 ticks = current->opcodeTicks[opcode] - previous->opcodeTicks[opcode];
        u32 calls = current->opcodeCalls[opcode] - previous->opcodeCalls[opcode];

        totalOpcodeTicks += ticks;
        OotPspAudioBackend_InsertOpcodeRank(ranks, opcode, ticks, calls,
                                            current->opcodeMaxTicks[opcode]);
    }

    printf("[audio] me-job 1s n=%lu cmd=%lu avg=%lutick last=%lutick/%lucmd "
           "max_all=%lutick/%lucmd last_hot=%s/%lutick max_hot=%s/%lutick\n",
           (unsigned long)jobs, (unsigned long)commands,
           (unsigned long)(jobs != 0 ? jobTicks / jobs : 0),
           (unsigned long)current->lastJobTicks, (unsigned long)current->lastJobCommands,
           (unsigned long)current->jobMaxTicks, (unsigned long)current->maxJobCommands,
           OotPspAudioBackend_OpcodeName(current->lastJobSlowOpcode),
           (unsigned long)current->lastJobSlowTicks,
           OotPspAudioBackend_OpcodeName(current->maxJobSlowOpcode),
           (unsigned long)current->maxJobSlowTicks);
    printf("[audio] me-op 1s total=%llutick top=%s %lu.%lu%% n=%lu avg=%lu max_all=%lu | "
           "%s %lu.%lu%% n=%lu avg=%lu max_all=%lu | %s %lu.%lu%% n=%lu avg=%lu max_all=%lu\n",
           (unsigned long long)totalOpcodeTicks,
           OotPspAudioBackend_OpcodeName(ranks[0].opcode),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[0].ticks, totalOpcodeTicks) / 10),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[0].ticks, totalOpcodeTicks) % 10),
           (unsigned long)ranks[0].calls,
           (unsigned long)(ranks[0].calls != 0 ? ranks[0].ticks / ranks[0].calls : 0),
           (unsigned long)ranks[0].maxTicks,
           OotPspAudioBackend_OpcodeName(ranks[1].opcode),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[1].ticks, totalOpcodeTicks) / 10),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[1].ticks, totalOpcodeTicks) % 10),
           (unsigned long)ranks[1].calls,
           (unsigned long)(ranks[1].calls != 0 ? ranks[1].ticks / ranks[1].calls : 0),
           (unsigned long)ranks[1].maxTicks,
           OotPspAudioBackend_OpcodeName(ranks[2].opcode),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[2].ticks, totalOpcodeTicks) / 10),
           (unsigned long)(OotPspAudioBackend_OpcodePercentTenths(ranks[2].ticks, totalOpcodeTicks) % 10),
           (unsigned long)ranks[2].calls,
           (unsigned long)(ranks[2].calls != 0 ? ranks[2].ticks / ranks[2].calls : 0),
           (unsigned long)ranks[2].maxTicks);
}

static const char* OotPspAudioBackend_MeStateName(u32 state) {
    switch (state) {
        case OOT_PSP_AUDIO_ME_STATE_BOOTING:
            return "BOOT";
        case OOT_PSP_AUDIO_ME_STATE_IDLE:
            return "IDLE";
        case OOT_PSP_AUDIO_ME_STATE_RUN:
            return "RUN";
        case OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER:
            return "QUEUE";
        case OOT_PSP_AUDIO_ME_STATE_STOP:
            return "STOP";
        case OOT_PSP_AUDIO_ME_STATE_HALTED:
            return "HALT";
        case OOT_PSP_AUDIO_ME_STATE_FAULT:
            return "FAULT";
        default:
            return "?";
    }
}

static const char* OotPspAudioBackend_ProducerStateName(u32 state) {
    switch (state) {
        case OOT_PSP_AUDIO_PRODUCER_STATE_STOPPED:
            return "STOP";
        case OOT_PSP_AUDIO_PRODUCER_STATE_STARTING:
            return "START";
        case OOT_PSP_AUDIO_PRODUCER_STATE_PRIMING:
            return "PRIME";
        case OOT_PSP_AUDIO_PRODUCER_STATE_TIMER_WAIT:
            return "TIMER";
        case OOT_PSP_AUDIO_PRODUCER_STATE_UPDATE:
            return "UPDATE";
        case OOT_PSP_AUDIO_PRODUCER_STATE_CATCHUP:
            return "CATCHUP";
        case OOT_PSP_AUDIO_PRODUCER_STATE_IO_BACKOFF:
            return "IO_BACKOFF";
        case OOT_PSP_AUDIO_PRODUCER_STATE_RING_FULL:
            return "RING_FULL";
        case OOT_PSP_AUDIO_PRODUCER_STATE_WAIT_ME:
            return "WAIT_ME";
        case OOT_PSP_AUDIO_PRODUCER_STATE_PREPARE:
            return "PREPARE";
        case OOT_PSP_AUDIO_PRODUCER_STATE_SYNTH:
            return "SYNTH";
        case OOT_PSP_AUDIO_PRODUCER_STATE_SUBMIT:
            return "SUBMIT";
        default:
            return "?";
    }
}

void OotPspAudioBackend_SetDiagnosticProducerState(OotPspAudioProducerState state) {
    sAudioDiagnosticProducerState = state;
}

static const char* OotPspAudioBackend_OutputStateName(u32 state) {
    switch (state) {
        case OOT_PSP_AUDIO_OUTPUT_STATE_STOPPED:
            return "STOP";
        case OOT_PSP_AUDIO_OUTPUT_STATE_STARTING:
            return "START";
        case OOT_PSP_AUDIO_OUTPUT_STATE_PRIMING:
            return "PRIME";
        case OOT_PSP_AUDIO_OUTPUT_STATE_PREPARE:
            return "PREP";
        case OOT_PSP_AUDIO_OUTPUT_STATE_WAIT_HARDWARE:
            return "WAIT_HW";
        case OOT_PSP_AUDIO_OUTPUT_STATE_ERROR_BACKOFF:
            return "ERROR";
        default:
            return "?";
    }
}

static const char* OotPspAudioBackend_KernelThreadStateName(s32 status) {
    if (status & PSP_THREAD_RUNNING) {
        return "RUN";
    }
    if (status & PSP_THREAD_READY) {
        return "READY";
    }
    if (status & PSP_THREAD_WAITING) {
        return "WAIT";
    }
    if (status & PSP_THREAD_SUSPEND) {
        return "SUSP";
    }
    if (status & PSP_THREAD_STOPPED) {
        return "STOP";
    }
    if (status & PSP_THREAD_KILLED) {
        return "KILLED";
    }
    return "?";
}

static void OotPspAudioBackend_SampleThread(SceUID threadId, OotPspAudioDiagnosticThreadSample* sample) {
    SceKernelThreadRunStatus status;

    memset(sample, 0, sizeof(*sample));
    sample->waitType = -1;
    if (!OotPspAudioBackend_IsValidUid(threadId)) {
        return;
    }

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(threadId, &status) < 0) {
        return;
    }

    sample->valid = true;
    sample->status = status.status;
    sample->priority = status.currentPriority;
    sample->waitType = status.waitType;
    sample->runClocks = ((u64)status.runClocks.hi << 32) | status.runClocks.low;
}

static void OotPspAudioBackend_SnapshotDiagnosticCounters(OotPspAudioDiagnosticCounters* counters) {
    counters->updates = sAudioDiagnosticUpdates;
    counters->profileUpdates = sAudioProfileUpdates;
    counters->profileWaitUsec = sAudioProfileWaitUsec;
    counters->profilePrepareUsec = sAudioProfilePrepareUsec;
    counters->profileSynthUsec = sAudioProfileSynthUsec;
    counters->profileSubmitUsec = sAudioProfileSubmitUsec;
    counters->profileSequenceUsec = sAudioProfileSequenceUsec;
    counters->profileCommandBuildUsec = sAudioProfileCommandBuildUsec;
    counters->profileAbiCommands = sAudioProfileAbiCommands;
    counters->profileSampleDmas = sAudioProfileSampleDmas;
    counters->meSubmits = sAudioDiagnosticMeSubmits;
    counters->cpuMixes = sAudioDiagnosticCpuMixes;
    counters->meFallbacks = sAudioDiagnosticMeFallbacks;
    counters->meTimeouts = sAudioDiagnosticMeTimeouts;
    counters->meWaits = sAudioDiagnosticMeWaits;
    counters->meWaitTotalUsec = sAudioDiagnosticMeWaitTotalUsec;
    counters->meWaitMaxUsec = sAudioDiagnosticMeWaitMaxUsec;
    counters->meWaitLastUsec = sAudioDiagnosticMeWaitLastUsec;
    counters->underruns = sAudioDiagnosticUnderruns;
    counters->outputErrors = sAudioDiagnosticOutputErrors;
    counters->ioBackoffs = sAudioDiagnosticIoBackoffs;
    counters->ringFull = sAudioDiagnosticRingFull;
    counters->catchups = sAudioDiagnosticCatchups;
    counters->producerLate = sAudioDiagnosticProducerLate;
    counters->producerLateMaxUsec = sAudioDiagnosticProducerLateMaxUsec;
}

static void OotPspAudioBackend_PrintDiagnosticEvent(const char* eventName, u32 amount, u32 total) {
    printf("[audio!] %s +%lu total=%lu buf=%lu ring=%lu meq=%lu me=%s/%lu op=%s#%lu "
           "pending=%ld wait=%ld\n",
           eventName, (unsigned long)amount, (unsigned long)total,
           (unsigned long)OotPspAudioBackend_TotalBufferedFrames(),
           (unsigned long)OotPspAudioBackend_BufferedFrames(),
           (unsigned long)OotPspAudioBackend_PendingMeQueueFrames(),
           OotPspAudioBackend_MeStateName(sAudioMeState), (unsigned long)sAudioMeProgress,
           OotPspAudioBackend_OpcodeName(sAudioMeOpcodeProfile->currentOpcode),
           (unsigned long)sAudioMeOpcodeProfile->currentCommandIndex,
           (long)sAudioMeCommandPending, (long)sAudioDiagnosticMeWaiting);
}

static int OotPspAudioBackend_DiagnosticThread(UNUSED SceSize args, UNUSED void* argp) {
    OotPspAudioDiagnosticCounters previousWindow;
    OotPspAudioDiagnosticCounters previousEvents;
    OotPspAudioDiagnosticCounters current;
    OotPspMixerOpcodeProfile previousOpcodeProfile;
    OotPspAudioDiagnosticThreadSample producerPrevious;
    OotPspAudioDiagnosticThreadSample outputPrevious;
    u32 previousSlowWaitMax;
    u32 lastReportUsec = sceKernelGetSystemTimeLow();

    OotPspAudioBackend_SnapshotDiagnosticCounters(&previousWindow);
    memset(&previousOpcodeProfile, 0, sizeof(previousOpcodeProfile));
    previousOpcodeProfile.currentOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    previousOpcodeProfile.lastJobSlowOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    previousOpcodeProfile.maxJobSlowOpcode = OOT_PSP_MIXER_PROFILE_OPCODE_IDLE;
    OotPspAudioBackend_SnapshotMeOpcodeProfile(&previousOpcodeProfile);
    previousEvents = previousWindow;
    previousSlowWaitMax = previousWindow.meWaitMaxUsec;
    OotPspAudioBackend_SampleThread(sAudioProducerThreadId, &producerPrevious);
    OotPspAudioBackend_SampleThread(sAudioOutputThreadId, &outputPrevious);
    printf("[audio] diagnostics enabled poll=%luus report=%luus producer_id=%d output_id=%d\n",
           (unsigned long)OOT_PSP_AUDIO_DIAGNOSTIC_POLL_USEC,
           (unsigned long)OOT_PSP_AUDIO_DIAGNOSTIC_REPORT_USEC, (int)sAudioProducerThreadId,
           (int)sAudioOutputThreadId);

    while (sAudioDiagnosticThreadRunning) {
        OotPspAudioDiagnosticThreadSample producer;
        OotPspAudioDiagnosticThreadSample output;
        OotPspMixerOpcodeProfile opcodeProfile;
        s32 opcodeProfileValid;
        u32 now;

        sceKernelDelayThread(OOT_PSP_AUDIO_DIAGNOSTIC_POLL_USEC);
        now = sceKernelGetSystemTimeLow();
        OotPspAudioBackend_SnapshotDiagnosticCounters(&current);

        if (current.underruns != previousEvents.underruns) {
            OotPspAudioBackend_PrintDiagnosticEvent("UNDERRUN", current.underruns - previousEvents.underruns,
                                                    current.underruns);
        }
        if (current.outputErrors != previousEvents.outputErrors) {
            OotPspAudioBackend_PrintDiagnosticEvent("OUTPUT_ERROR",
                                                    current.outputErrors - previousEvents.outputErrors,
                                                    current.outputErrors);
        }
        if (current.meFallbacks != previousEvents.meFallbacks) {
            OotPspAudioBackend_PrintDiagnosticEvent("ME_FALLBACK",
                                                    current.meFallbacks - previousEvents.meFallbacks,
                                                    current.meFallbacks);
        }
        if (current.meTimeouts != previousEvents.meTimeouts) {
            OotPspAudioBackend_PrintDiagnosticEvent("ME_TIMEOUT", current.meTimeouts - previousEvents.meTimeouts,
                                                    current.meTimeouts);
        }
        previousEvents = current;

        if ((u32)(now - lastReportUsec) < OOT_PSP_AUDIO_DIAGNOSTIC_REPORT_USEC) {
            continue;
        }

        OotPspAudioBackend_SampleThread(sAudioProducerThreadId, &producer);
        OotPspAudioBackend_SampleThread(sAudioOutputThreadId, &output);
        opcodeProfileValid = OotPspAudioBackend_SnapshotMeOpcodeProfile(&opcodeProfile);
        {
            u32 meWaitCount = current.meWaits - previousWindow.meWaits;
            u32 meWaitUsec = current.meWaitTotalUsec - previousWindow.meWaitTotalUsec;
            u32 meWaitAverageUsec = meWaitCount != 0 ? meWaitUsec / meWaitCount : 0;
            u32 meWaitActiveUsec =
                sAudioDiagnosticMeWaiting ? now - sAudioDiagnosticMeWaitStartUsec : 0;
            u32 ringFrames = OotPspAudioBackend_BufferedFrames();
            u32 meQueueFrames = OotPspAudioBackend_PendingMeQueueFrames();
            u32 driverFrames = OotPspAudioBackend_RestFrames() + sAudioPendingOutputFrames;
            u32 profileUpdates = current.profileUpdates - previousWindow.profileUpdates;
            u32 profileDivisor = profileUpdates != 0 ? profileUpdates : 1;
            u64 producerCpuUsec = producer.runClocks - producerPrevious.runClocks;
            u64 outputCpuUsec = output.runClocks - outputPrevious.runClocks;

            printf("[audio] t=%lu.%03lu prod=%s id=%d k=%s/w%d p=%d cpu=%lluus "
                   "out=%s id=%d k=%s/w%d p=%d cpu=%lluus me=%s/%lu op=%s#%lu active=%ld "
                   "pending=%ld wait=%ld waiter=%d age=%luus\n",
                   (unsigned long)(now / 1000000), (unsigned long)((now / 1000) % 1000),
                   OotPspAudioBackend_ProducerStateName(sAudioDiagnosticProducerState),
                   (int)sAudioProducerThreadId,
                   OotPspAudioBackend_KernelThreadStateName(producer.status), (int)producer.waitType,
                   (int)producer.priority, (unsigned long long)producerCpuUsec,
                   OotPspAudioBackend_OutputStateName(sAudioDiagnosticOutputState),
                   (int)sAudioOutputThreadId,
                   OotPspAudioBackend_KernelThreadStateName(output.status), (int)output.waitType,
                   (int)output.priority, (unsigned long long)outputCpuUsec,
                   OotPspAudioBackend_MeStateName(sAudioMeState), (unsigned long)sAudioMeProgress,
                   OotPspAudioBackend_OpcodeName(sAudioMeOpcodeProfile->currentOpcode),
                   (unsigned long)sAudioMeOpcodeProfile->currentCommandIndex,
                   (long)sAudioMeInitialized, (long)sAudioMeCommandPending,
                   (long)sAudioDiagnosticMeWaiting, (int)sAudioDiagnosticMeWaiterThreadId,
                   (unsigned long)meWaitActiveUsec);
            printf("[audio] buf=%lu/%lu ring=%lu meq=%lu driver=%lu free=%lu pos=%lu:%lu primed=%ld "
                   "src=%luHz/%lu hw=%ld audio=spec%u reset%u task%lu | 1s upd=%lu me=%lu cpu=%lu "
                   "wait=%lu avg=%luus last=%luus max_all=%luus underrun=%lu err=%lu io=%lu full=%lu "
                   "catchup=%lu late=%lu maxlate_all=%luus fallback=%lu timeout=%lu\n",
                   (unsigned long)OotPspAudioBackend_TotalBufferedFrames(),
                   (unsigned long)OotPspAudioBackend_TargetBufferFrames(), (unsigned long)ringFrames,
                   (unsigned long)meQueueFrames, (unsigned long)driverFrames,
                   (unsigned long)OotPspAudioBackend_FreeFrames(), (unsigned long)sAudioReadPos,
                   (unsigned long)sAudioWritePos, (long)sAudioPlaybackPrimed,
                   (unsigned long)sAudioSourceFrequency, (unsigned long)OotPspAudioBackend_SourceChunkFrames(),
                   (long)sAudioHardwareSrc, (unsigned int)gAudioCtx.specId,
                   (unsigned int)gAudioCtx.resetStatus, (unsigned long)gAudioCtx.totalTaskCount,
                   (unsigned long)(current.updates - previousWindow.updates),
                   (unsigned long)(current.meSubmits - previousWindow.meSubmits),
                   (unsigned long)(current.cpuMixes - previousWindow.cpuMixes),
                   (unsigned long)meWaitCount, (unsigned long)meWaitAverageUsec,
                   (unsigned long)current.meWaitLastUsec, (unsigned long)current.meWaitMaxUsec,
                   (unsigned long)(current.underruns - previousWindow.underruns),
                   (unsigned long)(current.outputErrors - previousWindow.outputErrors),
                   (unsigned long)(current.ioBackoffs - previousWindow.ioBackoffs),
                   (unsigned long)(current.ringFull - previousWindow.ringFull),
                   (unsigned long)(current.catchups - previousWindow.catchups),
                   (unsigned long)(current.producerLate - previousWindow.producerLate),
                   (unsigned long)current.producerLateMaxUsec,
                   (unsigned long)(current.meFallbacks - previousWindow.meFallbacks),
                   (unsigned long)(current.meTimeouts - previousWindow.meTimeouts));
            printf("[audio] phase 1s n=%lu wait_me=%luus prepare=%luus synth=%luus (seq=%luus cmd=%luus) "
                   "submit=%luus abi=%lu dma=%lu\n",
                   (unsigned long)profileUpdates,
                   (unsigned long)((current.profileWaitUsec - previousWindow.profileWaitUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profilePrepareUsec - previousWindow.profilePrepareUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profileSynthUsec - previousWindow.profileSynthUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profileSequenceUsec - previousWindow.profileSequenceUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profileCommandBuildUsec - previousWindow.profileCommandBuildUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profileSubmitUsec - previousWindow.profileSubmitUsec) /
                                   profileDivisor),
                   (unsigned long)((current.profileAbiCommands - previousWindow.profileAbiCommands) /
                                   profileDivisor),
                   (unsigned long)((current.profileSampleDmas - previousWindow.profileSampleDmas) /
                                   profileDivisor));
            if (opcodeProfileValid) {
                OotPspAudioBackend_PrintMeOpcodeProfile(&opcodeProfile, &previousOpcodeProfile);
            } else {
                printf("[audio] me-op snapshot=BUSY current=%s#%lu\n",
                       OotPspAudioBackend_OpcodeName(sAudioMeOpcodeProfile->currentOpcode),
                       (unsigned long)sAudioMeOpcodeProfile->currentCommandIndex);
            }
        }

        if ((current.meWaitMaxUsec > previousSlowWaitMax) &&
            (current.meWaitMaxUsec >= OOT_PSP_AUDIO_DIAGNOSTIC_SLOW_ME_WAIT_USEC)) {
            printf("[audio!] SLOW_ME_WAIT last=%luus max_all=%luus me=%s/%lu current_op=%s#%lu\n",
                   (unsigned long)current.meWaitLastUsec, (unsigned long)current.meWaitMaxUsec,
                   OotPspAudioBackend_MeStateName(sAudioMeState), (unsigned long)sAudioMeProgress,
                   OotPspAudioBackend_OpcodeName(sAudioMeOpcodeProfile->currentOpcode),
                   (unsigned long)sAudioMeOpcodeProfile->currentCommandIndex);
        }

        previousSlowWaitMax = current.meWaitMaxUsec;
        previousWindow = current;
        if (opcodeProfileValid) {
            previousOpcodeProfile = opcodeProfile;
        }
        producerPrevious = producer;
        outputPrevious = output;
        lastReportUsec = now;
    }

    sAudioDiagnosticThreadId = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

static s32 OotPspAudioBackend_StartDiagnosticThread(void) {
    SceUID threadId;
    s32 ret;

    if (OotPspAudioBackend_IsValidUid(sAudioDiagnosticThreadId) || sAudioDiagnosticThreadRunning) {
        return 0;
    }

    sAudioDiagnosticThreadRunning = true;
    threadId = sceKernelCreateThread("OOT PSP AudioDiag", OotPspAudioBackend_DiagnosticThread,
                                     OOT_PSP_AUDIO_DIAGNOSTIC_THREAD_PRIORITY, 0x8000,
                                     PSP_THREAD_ATTR_USER, NULL);
    if (!OotPspAudioBackend_IsValidUid(threadId)) {
        sAudioDiagnosticThreadRunning = false;
        printf("[audio!] diagnostic thread create failed err=%d\n", (int)threadId);
        return threadId;
    }

    ret = sceKernelStartThread(threadId, 0, NULL);
    if (ret < 0) {
        sceKernelDeleteThread(threadId);
        sAudioDiagnosticThreadRunning = false;
        printf("[audio!] diagnostic thread start failed err=%d\n", (int)ret);
        return ret;
    }

    sAudioDiagnosticThreadId = threadId;
    return 0;
}
#endif

static s32 OotPspAudioBackend_StartThreads(void) {
    SceUID threadId;
    s32 ret;

    if (!OotPspAudioBackend_IsOutputInitialized()) {
        return -1;
    }

    if ((sAudioOutputThreadId < 0) && !sAudioOutputThreadRunning) {
        sAudioOutputThreadRunning = true;
        AUDIO_DIAG_SET(sAudioDiagnosticOutputState, OOT_PSP_AUDIO_OUTPUT_STATE_STARTING);
        threadId = sceKernelCreateThread(
            "OOT PSP AudioOut",
            OotPspAudioBackend_OutputThread,
            OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY,
            0x10000,
            PSP_THREAD_ATTR_USER,
            NULL
        );
        if (threadId < 0) {
            sAudioOutputThreadRunning = false;
            return threadId;
        }

        ret = sceKernelStartThread(threadId, 0, NULL);
        if (ret < 0) {
            sceKernelDeleteThread(threadId);
            sAudioOutputThreadRunning = false;
            return ret;
        }
        sAudioOutputThreadId = threadId;
    }

    if ((sAudioProducerThreadId < 0) && !sAudioProducerThreadRunning) {
        sAudioProducerThreadRunning = true;
        AUDIO_DIAG_SET(sAudioDiagnosticProducerState, OOT_PSP_AUDIO_PRODUCER_STATE_STARTING);
        threadId = sceKernelCreateThread(
            "OOT PSP AudioGen",
            OotPspAudioBackend_ProducerThread,
            OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY,
            0x20000,
            PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,
            NULL
        );
        if (threadId < 0) {
            sAudioProducerThreadRunning = false;
            return threadId;
        }

        ret = sceKernelStartThread(threadId, 0, NULL);
        if (ret < 0) {
            sceKernelDeleteThread(threadId);
            sAudioProducerThreadRunning = false;
            return ret;
        }
        sAudioProducerThreadId = threadId;
    }

#if OOT_PSP_AUDIO_DIAGNOSTICS
    OotPspAudioBackend_StartDiagnosticThread();
#endif

    return 0;
}

static void OotPspAudioBackend_QueueCpuCopy(const s16* samples, u32 frames, u32 writePos) {
    u32 copied = 0;

    while (copied < frames) {
        u32 todo = frames - copied;
        u32 untilWrap = OOT_PSP_AUDIO_RING_FRAMES - writePos;

        if (todo > untilWrap) {
            todo = untilWrap;
        }

        {
            s16* dst = &sAudioRing[writePos * OOT_PSP_AUDIO_CHANNELS];
            u32 bytes = todo * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
            s32 usedDma = false;

            if ((bytes >= OOT_PSP_AUDIO_DMA_COPY_MIN_BYTES) &&
                ((((uintptr_t)dst | (uintptr_t)samples | bytes) & 0xF) == 0)) {
                /* Preserve adjacent partial cache lines, then discard stale
                 * destination lines before the DMA engine writes main RAM. */
                OotPspAudioBackend_WritebackRange(samples, bytes);
                OotPspAudioBackend_WritebackBoundaryRange(dst, bytes);
                OotPspAudioBackend_InvalidateRange(dst, bytes);
                usedDma = sceDmacTryMemcpy(dst, samples, bytes) == 0;
            }

            if (!usedDma) {
                memcpy(dst, samples, bytes);
                OotPspAudioBackend_WritebackRange(dst, bytes);
            }
        }

        samples += todo * OOT_PSP_AUDIO_CHANNELS;
        copied += todo;
        writePos = (writePos + todo) & OOT_PSP_AUDIO_RING_MASK;
    }

    sAudioWritePos = writePos;
}

s32 OotPspAudioBackend_Queue(const void* buf, u32 size) {
    const s16* samples = buf;
    u32 frames = size / (sizeof(s16) * OOT_PSP_AUDIO_CHANNELS);
    u32 freeFrames = OotPspAudioBackend_FreeFrames();
    u32 writePos = sAudioWritePos;

    if ((buf == NULL) || (frames == 0)) {
        return 0;
    }

    if (frames > freeFrames) {
        return -1;
    }

    OotPspAudioBackend_QueueCpuCopy(samples, frames, writePos);
    return 0;
}

s32 OotPspAudioBackend_SetFrequency(u32 frequency) {
    if (!OotPspAudioBackend_IsOutputInitialized() || (frequency == 0)) {
        return -1;
    }

    if (sAudioSourceFrequency != frequency) {
        /* The default PSP cap makes every OOT spec exactly 22.05 kHz. If a
         * custom build changes rates while running, retain the software path's
         * dynamic-rate behavior rather than feeding the SRC the wrong rate. */
        if (sAudioHardwareSrc) {
            return -1;
        }
        sAudioSourceFrequency = frequency;
        sAudioSourceChunkFrames = OotPspAudioBackend_CalculateSourceChunkFrames(frequency);
        sAudioResampleStep = OotPspAudioBackend_CalculateResampleStep(frequency);
        sAudioResampleFrac = 0;
    }

    return frequency;
}

u32 OotPspAudioBackend_GetLength(void) {
    return OotPspAudioBackend_ReportableFrames() * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
}

s32 OotPspAudioBackend_NeedsRefillUrgently(void) {
    return OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_UrgentBufferFrames();
}

s32 OotPspAudioBackend_NeedsRefillDuringIo(void) {
    return OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_IoBackoffFrames();
}

#if defined(OOTDEBUG)
static u64 OotPspAudioBackend_GetThreadRunClock(SceUID threadId) {
    SceKernelThreadRunStatus status;

    if (threadId < 0) {
        return 0;
    }

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(threadId, &status) < 0) {
        return 0;
    }

    return ((u64)status.runClocks.hi << 32) | status.runClocks.low;
}

void OotPspAudioBackend_GetThreadRunClocks(u64* producerClocks, u64* outputClocks) {
    if (producerClocks != NULL) {
        *producerClocks = OotPspAudioBackend_GetThreadRunClock(sAudioProducerThreadId);
    }
    if (outputClocks != NULL) {
        *outputClocks = OotPspAudioBackend_GetThreadRunClock(sAudioOutputThreadId);
    }
}

void OotPspAudioBackend_GetProfileCounters(OotPspAudioProfileCounters* counters) {
    if (counters == NULL) {
        return;
    }

    counters->updates = sAudioProfileUpdates;
    counters->waitUsec = sAudioProfileWaitUsec;
    counters->prepareUsec = sAudioProfilePrepareUsec;
    counters->synthUsec = sAudioProfileSynthUsec;
    counters->submitUsec = sAudioProfileSubmitUsec;
    counters->sequenceUsec = sAudioProfileSequenceUsec;
    counters->commandBuildUsec = sAudioProfileCommandBuildUsec;
    counters->abiCommands = sAudioProfileAbiCommands;
    counters->sampleDmas = sAudioProfileSampleDmas;
    counters->meSubmits = sAudioProfileMeSubmits;
    counters->cpuMixes = sAudioProfileCpuMixes;
    counters->meFailures = sAudioProfileMeFailures;
    counters->meActive = sAudioMeInitialized != false;
    counters->meState = sAudioMeState;
    counters->meProgress = sAudioMeProgress;
}
#endif

#if defined(OOTDEBUG) || OOT_PSP_AUDIO_DIAGNOSTICS
void OotPspAudioBackend_RecordUpdateProfile(u32 waitUsec, u32 prepareUsec, u32 synthUsec, u32 submitUsec,
                                            u32 abiCommands, u32 sampleDmas) {
    sAudioProfileUpdates++;
    sAudioProfileWaitUsec += waitUsec;
    sAudioProfilePrepareUsec += prepareUsec;
    sAudioProfileSynthUsec += synthUsec;
    sAudioProfileSubmitUsec += submitUsec;
    sAudioProfileAbiCommands += abiCommands;
    sAudioProfileSampleDmas += sampleDmas;
}

void OotPspAudioBackend_RecordSynthesisProfile(u32 sequenceUsec, u32 commandBuildUsec) {
    sAudioProfileSequenceUsec += sequenceUsec;
    sAudioProfileCommandBuildUsec += commandBuildUsec;
}
#endif

void OotPspAudio_Init(void) {
    if (sOotPspAudioInitialized) {
        OotPspAudioBackend_StartThreads();
        return;
    }

    if (OotPspAudioBackend_Init() < 0) {
        return;
    }

    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);

    /*
     * These must only happen once.
     * Audio_Init / Audio_InitSound may create internal timer threads.
     * Re-running them can leak kernel thread objects like oot-timer-00.
     */
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();
    OotPspAudioBackend_InitMe();

    sOotPspAudioInitialized = true;

    OotPspAudioBackend_StartThreads();
}

void OotPspAudio_Update(void) {
    if (!sAudioProducerThreadRunning) {
        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_NORMAL);
    }
}
