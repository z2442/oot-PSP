#include "oot_psp_audio_backend.h"

#include "attributes.h"
#include "audio.h"
#include "dma.h"
#include "oot_psp_mixer.h"

#include <me-core-mapper/me-core.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#define OOT_PSP_AUDIO_CHANNELS 2
#define OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY 32000
#define OOT_PSP_AUDIO_OUTPUT_FREQUENCY 44100
#define OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES 768
#define OOT_PSP_AUDIO_RING_FRAMES 16384
#define OOT_PSP_AUDIO_RING_MASK (OOT_PSP_AUDIO_RING_FRAMES - 1)
#define OOT_PSP_AUDIO_CACHE_LINE_SIZE 64
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS 24
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK ((1U << OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS) - 1)
#define OOT_PSP_AUDIO_LERP_FRAC_BITS 15

static volatile s32 sOotPspAudioInitialized = false;

#define OOT_PSP_AUDIO_TARGET_CHUNKS 6
#define OOT_PSP_AUDIO_REFILL_CHUNKS 5
#define OOT_PSP_AUDIO_URGENT_CHUNKS 2

/* Output only resamples/submits blocks; synthesis stays at the main thread's priority. */
#define OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY 0x1F
#define OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY 0x20

#define OOT_PSP_AUDIO_MAX_UPDATES_NORMAL 8
/*
 * AudioThread_Update submits a buffer synthesized two updates earlier. The
 * first two submissions are only the 160-frame initialization buffers, so
 * prime those plus enough real buffers to reach the playback high-water mark.
 */
#define OOT_PSP_AUDIO_MAX_UPDATES_PRIME (OOT_PSP_AUDIO_TARGET_CHUNKS + 2)
#define OOT_PSP_AUDIO_UPDATE_USEC (1000000U / 60U)
#define OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)
#define OOT_PSP_AUDIO_ME_TIMEOUT_US 250000
#define OOT_PSP_AUDIO_OUTPUT_WAKE_GUARD_USEC 250
#define OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC 50
#define OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC \
    ((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * 1000000U) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY)

typedef enum {
    OOT_PSP_AUDIO_ME_STATE_BOOTING,
    OOT_PSP_AUDIO_ME_STATE_IDLE,
    OOT_PSP_AUDIO_ME_STATE_RUN,
    OOT_PSP_AUDIO_ME_STATE_STOP,
    OOT_PSP_AUDIO_ME_STATE_HALTED,
    OOT_PSP_AUDIO_ME_STATE_FAULT,
} OotPspAudioMeState;

enum {
    OOT_PSP_AUDIO_ME_SHARED_STATE,
    OOT_PSP_AUDIO_ME_SHARED_CMD_LIST,
    OOT_PSP_AUDIO_ME_SHARED_CMD_COUNT,
    OOT_PSP_AUDIO_ME_SHARED_PROGRESS,
    OOT_PSP_AUDIO_ME_SHARED_COUNT,
};

meLibMakeUncachedMem(sAudioMeSharedStorage, OOT_PSP_AUDIO_ME_SHARED_COUNT, u32Me);

#define sAudioMeShared \
    ((volatile u32Me*)(UNCACHED_USER_MASK | (u32Me)(uintptr_t)sAudioMeSharedStorage))
#define sAudioMeState    sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_STATE]
#define sAudioMeCmdList  sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_CMD_LIST]
#define sAudioMeCmdCount sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_CMD_COUNT]
#define sAudioMeProgress sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_PROGRESS]

#if (OOT_PSP_AUDIO_RING_FRAMES & (OOT_PSP_AUDIO_RING_FRAMES - 1)) != 0
#error OOT_PSP_AUDIO_RING_FRAMES must be a power of two
#endif

#if (OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES & 63) != 0
#error OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES must be a multiple of 64
#endif

static s16 sAudioRing[OOT_PSP_AUDIO_RING_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 sAudioMix[OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static u8 sAudioExternalPool[OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE] __attribute__((aligned(64)));

static volatile u32 sAudioReadPos;
static volatile u32 sAudioWritePos;
static volatile u32 sAudioOutputFrames;
static volatile s32 sAudioOutputThreadRunning;
static volatile s32 sAudioProducerThreadRunning;
static volatile s32 sAudioPlaybackPrimed;
static volatile s32 sAudioMeInitialized;
static volatile u32 sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;

static u32 sAudioResampleFrac;
static u32 sAudioResampleStep;
static s16 sAudioLastLeft;
static s16 sAudioLastRight;
static SceUID sAudioOutputThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static s32 sAudioOutputChannel = -1;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);

__attribute__((noinline, aligned(4))) void meLibOnException(void) {
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_FAULT;
    meLibSync();
    meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
    meLibExceptionHandlerInit(0);

    do {
        meLibDelayPipeline();
    } while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_BOOTING);

    while (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_STOP) {
        if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
            const Acmd* cmdList = (const Acmd*)(uintptr_t)sAudioMeCmdList;
            s32 cmdCount = (s32)sAudioMeCmdCount;

            sAudioMeProgress = 1;
            meLibDcacheWritebackInvalidateAll();
            sAudioMeProgress = 2;
            OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
            sAudioMeProgress = 3;
            meLibDcacheWritebackInvalidateAll();
            meLibSync();
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
        } else {
            meLibDelayPipeline();
        }
    }

    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_HALTED;
    meLibSync();
    meLibHalt();
}

static s32 OotPspAudioBackend_InitMe(void) {
    s32 ret;

    if (sAudioMeInitialized) {
        return 0;
    }

    sAudioMeCmdList = 0;
    sAudioMeCmdCount = 0;
    sAudioMeProgress = 0;
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_BOOTING;
    sceKernelDcacheWritebackAll();

    ret = meLibDefaultInit();
    if (ret < 0) {
        return ret;
    }

    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
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

static void OotPspAudioBackend_InvalidateMeWrites(const Acmd* cmdList, s32 cmdCount) {
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
                OotPspAudioBackend_InvalidateRange((void*)(uintptr_t)w1, ((w0 >> 16) & 0xFF) << 4);
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_FallbackFromMe(const Acmd* cmdList, s32 cmdCount) {
    sAudioMeInitialized = false;
    OotPspMixer_InvalidateStateCache();
    OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
}

void OotPspAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount) {
    u32 startTime;

    if ((cmdList == NULL) || (cmdCount <= 0)) {
        return;
    }

    if (!sAudioMeInitialized) {
        OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
        return;
    }

    while (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        sceKernelDelayThread(10);
    }

    /*
     * The CPU produces command lists and sample data in its cache. The ME
     * invalidates its cache before consuming them, then writes all mixer state
     * and PCM back before publishing IDLE.
     */
    sceKernelDcacheWritebackAll();
    sAudioMeCmdList = (u32)(uintptr_t)cmdList;
    sAudioMeCmdCount = (u32)cmdCount;
    sAudioMeProgress = 0;
    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_RUN;

    startTime = sceKernelGetSystemTimeLow();
    while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
        if ((sceKernelGetSystemTimeLow() - startTime) >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
            meLibEmitSoftwareInterrupt();
            startTime = sceKernelGetSystemTimeLow();
            while ((sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) &&
                   ((sceKernelGetSystemTimeLow() - startTime) < 10000)) {
                sceKernelDelayThread(10);
            }
            OotPspAudioBackend_FallbackFromMe(cmdList, cmdCount);
            return;
        }
        sceKernelDelayThread(10);
    }

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        OotPspAudioBackend_FallbackFromMe(cmdList, cmdCount);
        return;
    }

    OotPspAudioBackend_InvalidateMeWrites(cmdList, cmdCount);
}

static u32 OotPspAudioBackend_SourceChunkFrames(void) {
    u32 frequency = sAudioSourceFrequency;

    return (((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * frequency) + OOT_PSP_AUDIO_OUTPUT_FREQUENCY - 1) /
            OOT_PSP_AUDIO_OUTPUT_FREQUENCY);
}

static u32 OotPspAudioBackend_TargetBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * OOT_PSP_AUDIO_TARGET_CHUNKS;
}

static u32 OotPspAudioBackend_RefillBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * OOT_PSP_AUDIO_REFILL_CHUNKS;
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

static u32 OotPspAudioBackend_FreeFrames(void) {
    return (OOT_PSP_AUDIO_RING_FRAMES - 1) - OotPspAudioBackend_BufferedFrames();
}

static u32 OotPspAudioBackend_RestFrames(void) {
    s32 rest;

    if (sAudioOutputChannel < 0) {
        return 0;
    }

    rest = sAudioOutputFrames;
    return (rest > 0) ? (u32)rest : 0;
}

static u32 OotPspAudioBackend_TotalBufferedFrames(void) {
    return OotPspAudioBackend_BufferedFrames() + OotPspAudioBackend_RestFrames();
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

    if (delta >= 0) {
        delta += 1 << (OOT_PSP_AUDIO_LERP_FRAC_BITS - 1);
    } else {
        delta -= 1 << (OOT_PSP_AUDIO_LERP_FRAC_BITS - 1);
    }

    return current + (delta >> OOT_PSP_AUDIO_LERP_FRAC_BITS);
}

static s32 OotPspAudioBackend_OutputReady(s32* outputActive, u32* delayUsec) {
    s32 rest;

    if (!*outputActive) {
        return true;
    }

    rest = sceAudioGetChannelRestLength(sAudioOutputChannel);

    if (rest == 0) {
        sAudioOutputFrames = 0;
        *outputActive = false;
        return true;
    }

    if (rest < 0) {
        sAudioOutputFrames = 0;
        sAudioPlaybackPrimed = false;
        *outputActive = false;
        *delayUsec = OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC;
        return false;
    }

    *delayUsec = ((u32)rest * 1000000U) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY;
    if (*delayUsec > OOT_PSP_AUDIO_OUTPUT_WAKE_GUARD_USEC) {
        *delayUsec -= OOT_PSP_AUDIO_OUTPUT_WAKE_GUARD_USEC;
    } else {
        *delayUsec = OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC;
    }
    return false;
}

static s32 OotPspAudioBackend_OutputMix(u32 sourceFrames) {
    s32 ret;

    sceKernelDcacheWritebackRange(sAudioMix, sizeof(sAudioMix));
    ret = sceAudioOutput(sAudioOutputChannel, PSP_AUDIO_VOLUME_MAX, sAudioMix);

    if (ret >= 0) {
        sAudioOutputFrames = sourceFrames;
    } else if (ret != (s32)SCE_AUDIO_ERROR_OUTPUT_BUSY) {
        sAudioPlaybackPrimed = false;
    }

    return ret;
}

static void OotPspAudioBackend_PrepareSilence(void) {
    memset(sAudioMix, 0, sizeof(sAudioMix));
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
}

static void OotPspAudioBackend_FadeChunkToSilence(s16** outPtr, u32 frames, s16 left, s16 right) {
    s16* out = *outPtr;
    u32 i;

    for (i = 0; i < frames; i++) {
        s32 fade = (s32)(frames - i);

        *out++ = (s16)(((s32)left * fade) / (s32)(frames + 1));
        *out++ = (s16)(((s32)right * fade) / (s32)(frames + 1));
    }

    *outPtr = out;
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
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
        return false;
    }

    AudioThread_Update();
    sceKernelRotateThreadReadyQueue(OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY);
    return true;
}

static void OotPspAudioBackend_RunUpdates(u32 maxUpdates, s32 forceRefill) {
    u32 updates = 0;
    u32 buffered = OotPspAudioBackend_TotalBufferedFrames();
    u32 refillFrames = OotPspAudioBackend_RefillBufferFrames();
    u32 targetFrames = OotPspAudioBackend_TargetBufferFrames();

    if (!forceRefill && (buffered >= refillFrames)) {
        return;
    }

    while ((updates < maxUpdates) && (buffered < targetFrames)) {
        if (!OotPspAudioBackend_TryRunUpdate()) {
            break;
        }

        updates++;
        buffered = OotPspAudioBackend_TotalBufferedFrames();
    }
}

static u32 OotPspAudioBackend_RenderOutputChunk(void) {
    u32 buffered = OotPspAudioBackend_BufferedFrames();
    u32 readPos = sAudioReadPos;
    u32 sourceFrames = 0;
    s16* out = sAudioMix;
    u32 i;

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
                                             sAudioResampleFrac);
        right = OotPspAudioBackend_LerpSample(sAudioRing[ringOffset + 1], sAudioRing[nextRingOffset + 1],
                                              sAudioResampleFrac);

        *out++ = left;
        *out++ = right;
        sAudioLastLeft = left;
        sAudioLastRight = right;

        sAudioResampleFrac += sAudioResampleStep;
        advance = sAudioResampleFrac >> OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS;
        sAudioResampleFrac &= OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK;

        if (advance > (buffered - 1)) {
            advance = buffered - 1;
        }

        readPos = (readPos + advance) & OOT_PSP_AUDIO_RING_MASK;
        sourceFrames += advance;
        buffered -= advance;
    }

    sAudioReadPos = readPos;

    if (i < OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES) {
        OotPspAudioBackend_FadeChunkToSilence(&out, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES - i, sAudioLastLeft,
                                              sAudioLastRight);
        sAudioPlaybackPrimed = false;
    }

    return sourceFrames;
}

static int OotPspAudioBackend_OutputThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 pendingSourceFrames = 0;
    s32 outputActive = false;
    s32 outputPending = false;

    while (sAudioOutputThreadRunning) {
        u32 buffered;
        u32 delayUsec;
        u32 sourceFrames;
        s32 ret;

        if (!OotPspAudioBackend_OutputReady(&outputActive, &delayUsec)) {
            sceKernelDelayThread(delayUsec);
            continue;
        }

        if (outputPending) {
            ret = OotPspAudioBackend_OutputMix(pendingSourceFrames);
            if (ret >= 0) {
                outputActive = true;
                outputPending = false;
                sceKernelDelayThread(OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC);
            } else {
                if (ret != (s32)SCE_AUDIO_ERROR_OUTPUT_BUSY) {
                    outputPending = false;
                }
                sceKernelDelayThread(OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC);
            }
            continue;
        }

        buffered = OotPspAudioBackend_BufferedFrames();

        if (!sAudioPlaybackPrimed) {
            if (buffered < OotPspAudioBackend_TargetBufferFrames()) {
                OotPspAudioBackend_PrepareSilence();
                pendingSourceFrames = 0;
                outputPending = true;
                continue;
            }
            sAudioPlaybackPrimed = true;
        }

        if (buffered < OotPspAudioBackend_SourceChunkFrames()) {
            sAudioPlaybackPrimed = false;
            OotPspAudioBackend_PrepareSilence();
            pendingSourceFrames = 0;
            outputPending = true;
            continue;
        }

        sourceFrames = OotPspAudioBackend_RenderOutputChunk();
        if (sourceFrames == 0) {
            sAudioPlaybackPrimed = false;
            OotPspAudioBackend_PrepareSilence();
            pendingSourceFrames = 0;
            outputPending = true;
            continue;
        }

        pendingSourceFrames = sourceFrames;
        outputPending = true;
    }

    sAudioOutputThreadRunning = false;
    sAudioOutputThreadId = -1;

    sceKernelExitDeleteThread(0);
    return 0;
}

static int OotPspAudioBackend_ProducerThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 nextUpdateUsec;

    OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PRIME, true);
    nextUpdateUsec = sceKernelGetSystemTimeLow();

    while (sAudioProducerThreadRunning) {
        s32 delayUsec;
        u32 now;

        nextUpdateUsec += OOT_PSP_AUDIO_UPDATE_USEC;
        now = sceKernelGetSystemTimeLow();
        delayUsec = (s32)(nextUpdateUsec - now);
        if (delayUsec > 0) {
            sceKernelDelayThread(delayUsec);
        } else if (delayUsec < -(s32)OOT_PSP_AUDIO_UPDATE_USEC) {
            nextUpdateUsec = now;
        }

        OotPspAudioBackend_TryRunUpdate();
    }

    sAudioProducerThreadRunning = false;
    sAudioProducerThreadId = -1;

    sceKernelExitDeleteThread(0);
    return 0;
}

s32 OotPspAudioBackend_Init(void) {
    if (sAudioOutputChannel >= 0) {
        return 0;
    }

    memset(sAudioRing, 0, sizeof(sAudioRing));
    memset(sAudioMix, 0, sizeof(sAudioMix));
    sAudioReadPos = 0;
    sAudioWritePos = 0;
    sAudioOutputFrames = 0;
    sAudioPlaybackPrimed = false;
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
    sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;
    sAudioResampleStep = OotPspAudioBackend_CalculateResampleStep(sAudioSourceFrequency);

    sceAudioOutput2Release();
    sceAudioSRCChRelease();
    sAudioOutputChannel =
        sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (sAudioOutputChannel < 0) {
        return -1;
    }

    return 0;
}

static s32 OotPspAudioBackend_StartThreads(void) {
    SceUID threadId;
    s32 ret;

    if (sAudioOutputChannel < 0) {
        return -1;
    }

    if ((sAudioOutputThreadId < 0) && !sAudioOutputThreadRunning) {
        sAudioOutputThreadRunning = true;
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

    return 0;
}

s32 OotPspAudioBackend_Queue(const void* buf, u32 size) {
    const s16* samples = buf;
    u32 frames = size / (sizeof(s16) * OOT_PSP_AUDIO_CHANNELS);
    u32 freeFrames = OotPspAudioBackend_FreeFrames();
    u32 writePos = sAudioWritePos;
    u32 copied = 0;

    if ((buf == NULL) || (frames == 0)) {
        return 0;
    }

    if (frames > freeFrames) {
        return -1;
    }

    while (copied < frames) {
        u32 todo = frames - copied;
        u32 untilWrap = OOT_PSP_AUDIO_RING_FRAMES - writePos;

        if (todo > untilWrap) {
            todo = untilWrap;
        }

        memcpy(&sAudioRing[writePos * OOT_PSP_AUDIO_CHANNELS], samples,
               todo * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));

        samples += todo * OOT_PSP_AUDIO_CHANNELS;
        copied += todo;
        writePos = (writePos + todo) & OOT_PSP_AUDIO_RING_MASK;
    }

    sAudioWritePos = writePos;
    return 0;
}

s32 OotPspAudioBackend_SetFrequency(u32 frequency) {
    if ((sAudioOutputChannel < 0) || (frequency == 0)) {
        return -1;
    }

    if (sAudioSourceFrequency != frequency) {
        sAudioSourceFrequency = frequency;
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
        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_NORMAL, true);
    }
}
