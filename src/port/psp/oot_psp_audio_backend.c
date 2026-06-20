#include "oot_psp_audio_backend.h"

#include "attributes.h"
#include "audio.h"
#include "dma.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_commands.h"
#include "oot_psp_mixer.h"

#include <me-core-mapper/me-core.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#define OOT_PSP_AUDIO_CHANNELS 2
#ifndef OOT_PSP_AUDIO_SOURCE_FREQUENCY
#define OOT_PSP_AUDIO_SOURCE_FREQUENCY 22050
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
#define OOT_PSP_AUDIO_REFILL_CHUNKS 8
#define OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS 8
#define OOT_PSP_AUDIO_URGENT_CHUNKS 2

#if OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS > OOT_PSP_AUDIO_TARGET_CHUNKS
#error OOT_PSP_AUDIO_IO_BACKOFF_CHUNKS must not exceed OOT_PSP_AUDIO_TARGET_CHUNKS
#endif

/* PSP priorities are lower-is-higher. Only the output submitter needs to outrank the game thread. */
#define OOT_PSP_AUDIO_GAME_THREAD_PRIORITY     0x20
#define OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY   (OOT_PSP_AUDIO_GAME_THREAD_PRIORITY - 2)
#define OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY OOT_PSP_AUDIO_GAME_THREAD_PRIORITY
#define OOT_PSP_AUDIO_PRODUCER_BOOST_PRIORITY  (OOT_PSP_AUDIO_GAME_THREAD_PRIORITY - 1)

#define OOT_PSP_AUDIO_MAX_UPDATES_NORMAL 8
#define OOT_PSP_AUDIO_MAX_UPDATES_CATCHUP 4
/*
 * AudioThread_Update submits a buffer synthesized two updates earlier. The
 * first two submissions are only the 160-frame initialization buffers, so
 * prime those plus enough real buffers to reach the playback high-water mark.
 */
#define OOT_PSP_AUDIO_MAX_UPDATES_PRIME (OOT_PSP_AUDIO_TARGET_CHUNKS + 2)
#define OOT_PSP_AUDIO_UPDATE_USEC (1000000U / 60U)
#define OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)
#define OOT_PSP_AUDIO_ME_TIMEOUT_US 250000
#define OOT_PSP_AUDIO_OUTPUT_WAKE_GUARD_USEC 0
#define OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC 50
#define OOT_PSP_AUDIO_ME_POLL_USEC 100
#define OOT_PSP_AUDIO_PRODUCER_IO_BACKOFF_USEC 1000
#define OOT_PSP_AUDIO_OUTPUT_ERROR_RETRY_USEC \
    ((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * 1000000U) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY)
#define OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN 1

typedef enum {
    OOT_PSP_AUDIO_ME_STATE_BOOTING,
    OOT_PSP_AUDIO_ME_STATE_IDLE,
    OOT_PSP_AUDIO_ME_STATE_RUN,
    OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT,
    OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER,
    OOT_PSP_AUDIO_ME_STATE_STOP,
    OOT_PSP_AUDIO_ME_STATE_HALTED,
    OOT_PSP_AUDIO_ME_STATE_FAULT,
} OotPspAudioMeState;

enum {
    OOT_PSP_AUDIO_ME_SHARED_STATE,
    OOT_PSP_AUDIO_ME_SHARED_CMD_LIST,
    OOT_PSP_AUDIO_ME_SHARED_CMD_COUNT,
    OOT_PSP_AUDIO_ME_SHARED_PROGRESS,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_READ_POS,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_BUFFERED,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_INPUT_FRAMES,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESAMPLE_FRAC,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESAMPLE_STEP,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_LAST,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_READ_POS,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_SOURCE_FRAMES,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_RESAMPLE_FRAC,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_LAST,
    OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_FLAGS,
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
#define sAudioMeRenderReadPos              sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_READ_POS]
#define sAudioMeRenderBuffered             sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_BUFFERED]
#define sAudioMeRenderInputFrames          sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_INPUT_FRAMES]
#define sAudioMeRenderResampleFrac         sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESAMPLE_FRAC]
#define sAudioMeRenderResampleStep         sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESAMPLE_STEP]
#define sAudioMeRenderLast                 sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_LAST]
#define sAudioMeRenderResultReadPos        sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_READ_POS]
#define sAudioMeRenderResultSourceFrames   sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_SOURCE_FRAMES]
#define sAudioMeRenderResultResampleFrac   sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_RESAMPLE_FRAC]
#define sAudioMeRenderResultLast           sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_LAST]
#define sAudioMeRenderResultFlags          sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_RENDER_RESULT_FLAGS]
#define sAudioMeQueueSrc                   sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_SRC]
#define sAudioMeQueueFrames                sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_FRAMES]
#define sAudioMeQueueWritePos              sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_WRITE_POS]
#define sAudioMeQueueResultWritePos        sAudioMeShared[OOT_PSP_AUDIO_ME_SHARED_QUEUE_RESULT_WRITE_POS]

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
static volatile s32 sAudioMeCommandPending;
static volatile u32 sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;

static const Acmd* sAudioMePendingCmdList;
static s32 sAudioMePendingCmdCount;
static u32 sAudioMePendingStartTime;
static u32 sAudioResampleFrac;
static u32 sAudioResampleStep;
static s16 sAudioLastLeft;
static s16 sAudioLastRight;
static SceUID sAudioOutputThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static SceUID sAudioOutputWakeSema = -1;
static SceUID sAudioMeLockSema = -1;
static s32 sAudioOutputChannel = -1;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);
static s32 OotPspAudioBackend_EnsureMeLockSema(void);
static void OotPspAudioBackend_MeInvalidateInputs(const Acmd* cmdList, s32 cmdCount);
static void OotPspAudioBackend_MeWritebackOutputs(const Acmd* cmdList, s32 cmdCount);
static void OotPspAudioBackend_MeRenderOutputChunk(void);
static void OotPspAudioBackend_MeQueueBuffer(void);

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

    OotPspMixer_InitVme();

    while (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_STOP) {
        if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
            const Acmd* cmdList = (const Acmd*)(uintptr_t)sAudioMeCmdList;
            s32 cmdCount = (s32)sAudioMeCmdCount;

            sAudioMeProgress = 1;
            OotPspAudioBackend_MeInvalidateInputs(cmdList, cmdCount);
            sAudioMeProgress = 2;
            OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
            sAudioMeProgress = 3;
            OotPspAudioBackend_MeWritebackOutputs(cmdList, cmdCount);
            meLibSync();
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
        } else if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT) {
            sAudioMeProgress = 1;
            OotPspAudioBackend_MeRenderOutputChunk();
            sAudioMeProgress = 2;
            meLibSync();
            sAudioMeState = OOT_PSP_AUDIO_ME_STATE_IDLE;
        } else if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER) {
            sAudioMeProgress = 1;
            OotPspAudioBackend_MeQueueBuffer();
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

static s32 OotPspAudioBackend_InitMe(void) {
    s32 ret;

    if (sAudioMeInitialized) {
        return 0;
    }

    sAudioMeCmdList = 0;
    sAudioMeCmdCount = 0;
    sAudioMeProgress = 0;
    ret = OotPspAudioBackend_EnsureMeLockSema();
    if (ret < 0) {
        return ret;
    }
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

static void OotPspAudioBackend_WritebackReverbDownsampleBoundaries(
    const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_WritebackBoundaryRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_WritebackBoundaryRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_WritebackBoundaryRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_WritebackBoundaryRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_WritebackReverbRingInputs(const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_WritebackRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_WritebackRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_WritebackRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_WritebackRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_MeInvalidateReverbRingInputs(const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_MeInvalidateRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeInvalidateRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeInvalidateRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_MeInvalidateRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_MeInvalidateReverbDownsampleOutputs(
    const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_MeInvalidateRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeInvalidateRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeInvalidateRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_MeInvalidateRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_MeWritebackReverbDownsampleOutputs(
    const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_MeWritebackRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeWritebackRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_MeWritebackRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_MeWritebackRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_InvalidateReverbDownsampleOutputs(const OotPspAudioReverbDownsampleCmd* desc) {
    if (desc == NULL) {
        return;
    }

    OotPspAudioBackend_InvalidateRange(&desc->leftRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_InvalidateRange(&desc->rightRingBuf[desc->startPos], desc->lengthA);
    OotPspAudioBackend_InvalidateRange(desc->leftRingBuf, desc->lengthB);
    OotPspAudioBackend_InvalidateRange(desc->rightRingBuf, desc->lengthB);
}

static void OotPspAudioBackend_WritebackMeInputs(const Acmd* cmdList, s32 cmdCount) {
    void* filterLut = NULL;
    s32 i;

    OotPspAudioBackend_WritebackRange(cmdList, cmdCount * sizeof(Acmd));

    for (i = 0; i < cmdCount; i++) {
        u32 w0 = cmdList[i].words.w0;
        u32 w1 = cmdList[i].words.w1;

        switch (w0 >> 24) {
            case A_ADPCM:
            case A_RESAMPLE:
            case A_S8DEC:
                OotPspAudioBackend_WritebackRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case A_FILTER:
                if (((w0 >> 16) & 0xFF) > 1) {
                    filterLut = (void*)(uintptr_t)w1;
                    OotPspAudioBackend_WritebackRange(filterLut, 8 * sizeof(s16));
                } else {
                    OotPspAudioBackend_WritebackRange((void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                    if (filterLut != NULL) {
                        OotPspAudioBackend_WritebackRange(filterLut, 8 * sizeof(s16));
                    }
                }
                break;

            case A_LOADADPCM:
                OotPspAudioBackend_WritebackRange((const void*)(uintptr_t)w1, w0 & 0xFFFFFF);
                break;

            case A_LOADBUFF:
                OotPspAudioBackend_WritebackRange((const void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case A_SAVEBUFF:
                OotPspAudioBackend_WritebackBoundaryRange((void*)(uintptr_t)w1,
                                                          OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case A_SETLOOP:
                OotPspAudioBackend_WritebackRange((const void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
            case OOT_PSP_A_REVERB_SAVE:
                OotPspAudioBackend_WritebackRange((const void*)(uintptr_t)w1,
                                                  sizeof(OotPspAudioReverbDownsampleCmd));
                OotPspAudioBackend_WritebackReverbDownsampleBoundaries(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_REVERB_LOAD:
                OotPspAudioBackend_WritebackRange((const void*)(uintptr_t)w1,
                                                  sizeof(OotPspAudioReverbDownsampleCmd));
                OotPspAudioBackend_WritebackReverbRingInputs(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_MeInvalidateInputs(const Acmd* cmdList, s32 cmdCount) {
    void* filterLut = NULL;
    s32 i;

    OotPspAudioBackend_MeInvalidateRange(cmdList, cmdCount * sizeof(Acmd));

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
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, w0 & 0xFFFFFF);
                break;

            case A_LOADBUFF:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case A_SAVEBUFF:
                OotPspAudioBackend_MeInvalidateRange((void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case A_SETLOOP:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1, sizeof(ADPCM_STATE));
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
            case OOT_PSP_A_REVERB_SAVE:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1,
                                                     sizeof(OotPspAudioReverbDownsampleCmd));
                OotPspAudioBackend_MeInvalidateReverbDownsampleOutputs(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_REVERB_LOAD:
                OotPspAudioBackend_MeInvalidateRange((const void*)(uintptr_t)w1,
                                                     sizeof(OotPspAudioReverbDownsampleCmd));
                OotPspAudioBackend_MeInvalidateReverbRingInputs(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_MeWritebackOutputs(const Acmd* cmdList, s32 cmdCount) {
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
                OotPspAudioBackend_MeWritebackRange((void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
            case OOT_PSP_A_REVERB_SAVE:
                OotPspAudioBackend_MeWritebackReverbDownsampleOutputs(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            default:
                break;
        }
    }
}

static void OotPspAudioBackend_MeInvalidateRingRange(u32 readPos, u32 frames) {
    u32 firstFrames;

    if (frames == 0) {
        return;
    }

    if (frames > OOT_PSP_AUDIO_RING_FRAMES) {
        frames = OOT_PSP_AUDIO_RING_FRAMES;
    }

    firstFrames = OOT_PSP_AUDIO_RING_FRAMES - readPos;
    if (firstFrames > frames) {
        firstFrames = frames;
    }

    OotPspAudioBackend_MeInvalidateRange(&sAudioRing[readPos * OOT_PSP_AUDIO_CHANNELS],
                                         firstFrames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));

    frames -= firstFrames;
    if (frames != 0) {
        OotPspAudioBackend_MeInvalidateRange(sAudioRing, frames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));
    }
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
                OotPspAudioBackend_InvalidateRange((void*)(uintptr_t)w1, OotPspAudioBackend_CommandDmaSize(w0));
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
            case OOT_PSP_A_REVERB_SAVE:
                OotPspAudioBackend_InvalidateReverbDownsampleOutputs(
                    (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
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

static void OotPspAudioBackend_ClearPendingMeCommand(void) {
    sAudioMeCommandPending = false;
    sAudioMePendingCmdList = NULL;
    sAudioMePendingCmdCount = 0;
    sAudioMePendingStartTime = 0;
}

static s32 OotPspAudioBackend_IsValidUid(SceUID uid) {
    return uid > 0;
}

static s32 OotPspAudioBackend_EnsureOutputWakeSema(void) {
    SceUID sema;

    if (OotPspAudioBackend_IsValidUid(sAudioOutputWakeSema)) {
        return 0;
    }

    sema = sceKernelCreateSema("OOT PSP AudioWake", 0, 0, 1, NULL);
    if (!OotPspAudioBackend_IsValidUid(sema)) {
        sAudioOutputWakeSema = -1;
        return sema;
    }

    sAudioOutputWakeSema = sema;
    return 0;
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

static void OotPspAudioBackend_LockMe(void) {
    if (OotPspAudioBackend_IsValidUid(sAudioMeLockSema)) {
        sceKernelWaitSema(sAudioMeLockSema, 1, NULL);
    }
}

static s32 OotPspAudioBackend_TryLockMe(void) {
    return OotPspAudioBackend_IsValidUid(sAudioMeLockSema) &&
           (sceKernelPollSema(sAudioMeLockSema, 1) == 0);
}

static void OotPspAudioBackend_UnlockMe(void) {
    if (OotPspAudioBackend_IsValidUid(sAudioMeLockSema)) {
        sceKernelSignalSema(sAudioMeLockSema, 1);
    }
}

static void OotPspAudioBackend_SignalOutputThread(void) {
    if (OotPspAudioBackend_IsValidUid(sAudioOutputWakeSema)) {
        sceKernelSignalSema(sAudioOutputWakeSema, 1);
    }
}

static void OotPspAudioBackend_WaitOutputThread(u32 timeoutUsec) {
    if (OotPspAudioBackend_IsValidUid(sAudioOutputWakeSema)) {
        if (timeoutUsec != 0) {
            SceUInt timeout = timeoutUsec;

            sceKernelWaitSema(sAudioOutputWakeSema, 1, &timeout);
        } else {
            sceKernelWaitSema(sAudioOutputWakeSema, 1, NULL);
        }
        return;
    }

    sceKernelDelayThread((timeoutUsec != 0) ? timeoutUsec : 1000);
}

static void OotPspAudioBackend_WaitForCommandsLocked(void) {
    if (!sAudioMeCommandPending) {
        return;
    }

    while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
        if ((sceKernelGetSystemTimeLow() - sAudioMePendingStartTime) >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
            u32 interruptTime;

            meLibEmitSoftwareInterrupt();
            interruptTime = sceKernelGetSystemTimeLow();
            while ((sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) &&
                   ((sceKernelGetSystemTimeLow() - interruptTime) < 10000)) {
                sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
            }

            if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RUN) {
                OotPspAudioBackend_FallbackFromMe(sAudioMePendingCmdList, sAudioMePendingCmdCount);
                OotPspAudioBackend_ClearPendingMeCommand();
                return;
            }
            break;
        }
        sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
    }

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        OotPspAudioBackend_FallbackFromMe(sAudioMePendingCmdList, sAudioMePendingCmdCount);
        OotPspAudioBackend_ClearPendingMeCommand();
        return;
    }

    OotPspAudioBackend_InvalidateMeWrites(sAudioMePendingCmdList, sAudioMePendingCmdCount);
    OotPspAudioBackend_ClearPendingMeCommand();
}

void OotPspAudioBackend_WaitForCommands(void) {
    OotPspAudioBackend_LockMe();
    OotPspAudioBackend_WaitForCommandsLocked();
    OotPspAudioBackend_UnlockMe();
}

void OotPspAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount) {
    if ((cmdList == NULL) || (cmdCount <= 0)) {
        return;
    }

    OotPspAudioBackend_LockMe();
    OotPspAudioBackend_WaitForCommandsLocked();

    if (!sAudioMeInitialized) {
        OotPspMixer_ExecuteCommandList(cmdList, cmdCount);
        OotPspAudioBackend_UnlockMe();
        return;
    }

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        OotPspAudioBackend_FallbackFromMe(cmdList, cmdCount);
        OotPspAudioBackend_UnlockMe();
        return;
    }

    /*
     * The CPU only writes back command-owned memory. The ME invalidates its
     * cache before consuming commands, then writes all mixer state and PCM back
     * before publishing IDLE.
     */
    OotPspAudioBackend_WritebackMeInputs(cmdList, cmdCount);
    sAudioMeCmdList = (u32)(uintptr_t)cmdList;
    sAudioMeCmdCount = (u32)cmdCount;
    sAudioMeProgress = 0;
    sAudioMePendingCmdList = cmdList;
    sAudioMePendingCmdCount = cmdCount;
    sAudioMePendingStartTime = sceKernelGetSystemTimeLow();
    sAudioMeCommandPending = true;
    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_RUN;
    OotPspAudioBackend_UnlockMe();
}

void OotPspAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount) {
    OotPspAudioBackend_SubmitCommands(cmdList, cmdCount);
    OotPspAudioBackend_WaitForCommands();
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

static s16 OotPspAudioBackend_LerpHalfSample(s16 current, s16 next) {
    s32 delta = (s32)next - current;

    if (delta >= 0) {
        delta++;
    } else {
        delta--;
    }

    return current + (delta >> 1);
}

static u32 OotPspAudioBackend_PackStereo(s16 left, s16 right) {
    return (u32)(u16)left | ((u32)(u16)right << 16);
}

static s16 OotPspAudioBackend_UnpackLeft(u32 packed) {
    return (s16)(packed & 0xFFFF);
}

static s16 OotPspAudioBackend_UnpackRight(u32 packed) {
    return (s16)(packed >> 16);
}

static u32 OotPspAudioBackend_RenderInputFrames(u32 buffered) {
    u32 frames = OotPspAudioBackend_SourceChunkFrames() + 2;

    if (frames > buffered) {
        frames = buffered;
    }

    return frames;
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
    if (*delayUsec < OOT_PSP_AUDIO_OUTPUT_MIN_RETRY_USEC) {
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
        return false;
    }

    if ((OotPspAudioBackend_TotalBufferedFrames() >= OotPspAudioBackend_IoBackoffFrames()) &&
        OotPsp_AssetReadHasForegroundPressure()) {
        sceKernelDelayThread(OOT_PSP_AUDIO_PRODUCER_IO_BACKOFF_USEC);
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

static u32 OotPspAudioBackend_RenderOutputChunkState(u32 buffered, u32* readPosPtr, u32* resampleFracPtr,
                                                     u32 resampleStep, s16* lastLeftPtr, s16* lastRightPtr,
                                                     u32* flagsPtr) {
    u32 readPos = *readPosPtr;
    u32 resampleFrac = *resampleFracPtr;
    s16 lastLeft = *lastLeftPtr;
    s16 lastRight = *lastRightPtr;
    u32 sourceFrames = 0;
    u32 flags = 0;
    s16* out = sAudioMix;
    u32 i;

    if ((resampleStep == OOT_PSP_AUDIO_RESAMPLE_STEP_2X) && (resampleFrac == 0)) {
        u32 maxPairs = OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES / 2;
        u32 renderedPairs;

        if (buffered <= 1) {
            return 0;
        }

        renderedPairs = buffered - 1;
        if (renderedPairs > maxPairs) {
            renderedPairs = maxPairs;
        }

        for (i = 0; i < renderedPairs; i++) {
            u32 ringOffset = readPos * OOT_PSP_AUDIO_CHANNELS;
            u32 nextRingOffset = ((readPos + 1) & OOT_PSP_AUDIO_RING_MASK) * OOT_PSP_AUDIO_CHANNELS;
            s16 left = sAudioRing[ringOffset + 0];
            s16 right = sAudioRing[ringOffset + 1];
            s16 halfLeft = OotPspAudioBackend_LerpHalfSample(left, sAudioRing[nextRingOffset + 0]);
            s16 halfRight = OotPspAudioBackend_LerpHalfSample(right, sAudioRing[nextRingOffset + 1]);

            *out++ = left;
            *out++ = right;
            *out++ = halfLeft;
            *out++ = halfRight;
            lastLeft = halfLeft;
            lastRight = halfRight;

            readPos = (readPos + 1) & OOT_PSP_AUDIO_RING_MASK;
        }

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

static u32 OotPspAudioBackend_RenderOutputChunkCpu(u32 buffered) {
    u32 readPos = sAudioReadPos;
    u32 resampleFrac = sAudioResampleFrac;
    s16 lastLeft = sAudioLastLeft;
    s16 lastRight = sAudioLastRight;
    u32 flags = 0;
    u32 sourceFrames;

    sourceFrames = OotPspAudioBackend_RenderOutputChunkState(buffered, &readPos, &resampleFrac, sAudioResampleStep,
                                                            &lastLeft, &lastRight, &flags);
    if (sourceFrames == 0) {
        return 0;
    }

    sAudioReadPos = readPos;
    sAudioResampleFrac = resampleFrac;
    sAudioLastLeft = lastLeft;
    sAudioLastRight = lastRight;
    if (flags & OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN) {
        sAudioPlaybackPrimed = false;
    }

    return sourceFrames;
}

static void OotPspAudioBackend_MeRenderOutputChunk(void) {
    u32 readPos = sAudioMeRenderReadPos;
    u32 resampleFrac = sAudioMeRenderResampleFrac;
    s16 lastLeft = OotPspAudioBackend_UnpackLeft(sAudioMeRenderLast);
    s16 lastRight = OotPspAudioBackend_UnpackRight(sAudioMeRenderLast);
    u32 flags = 0;
    u32 sourceFrames;

    OotPspAudioBackend_MeInvalidateRingRange(readPos, sAudioMeRenderInputFrames);
    OotPspAudioBackend_MeInvalidateRange(sAudioMix, sizeof(sAudioMix));

    sourceFrames = OotPspAudioBackend_RenderOutputChunkState(sAudioMeRenderBuffered, &readPos, &resampleFrac,
                                                            sAudioMeRenderResampleStep, &lastLeft, &lastRight, &flags);

    OotPspAudioBackend_MeWritebackRange(sAudioMix, sizeof(sAudioMix));
    sAudioMeRenderResultReadPos = readPos;
    sAudioMeRenderResultSourceFrames = sourceFrames;
    sAudioMeRenderResultResampleFrac = resampleFrac;
    sAudioMeRenderResultLast = OotPspAudioBackend_PackStereo(lastLeft, lastRight);
    sAudioMeRenderResultFlags = flags;
}

static void OotPspAudioBackend_MeQueueBuffer(void) {
    const s16* samples = (const s16*)(uintptr_t)sAudioMeQueueSrc;
    u32 frames = sAudioMeQueueFrames;
    u32 writePos = sAudioMeQueueWritePos & OOT_PSP_AUDIO_RING_MASK;

    OotPspAudioBackend_MeInvalidateRange(samples, frames * OOT_PSP_AUDIO_CHANNELS * sizeof(s16));

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

static u32 OotPspAudioBackend_RenderOutputChunkMe(u32 buffered) {
    u32 startTime;
    u32 sourceFrames;
    u32 flags;

    if (!OotPspAudioBackend_TryLockMe()) {
        return OotPspAudioBackend_RenderOutputChunkCpu(buffered);
    }

    if (sAudioMeCommandPending || !sAudioMeInitialized || (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE)) {
        OotPspAudioBackend_UnlockMe();
        return OotPspAudioBackend_RenderOutputChunkCpu(buffered);
    }

    sAudioMeRenderReadPos = sAudioReadPos;
    sAudioMeRenderBuffered = buffered;
    sAudioMeRenderInputFrames = OotPspAudioBackend_RenderInputFrames(buffered);
    sAudioMeRenderResampleFrac = sAudioResampleFrac;
    sAudioMeRenderResampleStep = sAudioResampleStep;
    sAudioMeRenderLast = OotPspAudioBackend_PackStereo(sAudioLastLeft, sAudioLastRight);
    sAudioMeRenderResultSourceFrames = 0;
    sAudioMeRenderResultFlags = 0;
    sAudioMeProgress = 0;
    startTime = sceKernelGetSystemTimeLow();
    meLibSync();
    sAudioMeState = OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT;

    while (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT) {
        if ((sceKernelGetSystemTimeLow() - startTime) >= OOT_PSP_AUDIO_ME_TIMEOUT_US) {
            u32 interruptTime;

            meLibEmitSoftwareInterrupt();
            interruptTime = sceKernelGetSystemTimeLow();
            while ((sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT) &&
                   ((sceKernelGetSystemTimeLow() - interruptTime) < 10000)) {
                sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
            }

            if (sAudioMeState == OOT_PSP_AUDIO_ME_STATE_RENDER_OUTPUT) {
                sAudioMeInitialized = false;
                OotPspAudioBackend_UnlockMe();
                return OotPspAudioBackend_RenderOutputChunkCpu(buffered);
            }
            break;
        }
        sceKernelDelayThread(OOT_PSP_AUDIO_ME_POLL_USEC);
    }

    if (sAudioMeState != OOT_PSP_AUDIO_ME_STATE_IDLE) {
        sAudioMeInitialized = false;
        OotPspAudioBackend_UnlockMe();
        return OotPspAudioBackend_RenderOutputChunkCpu(buffered);
    }

    OotPspAudioBackend_InvalidateRange(sAudioMix, sizeof(sAudioMix));
    sourceFrames = sAudioMeRenderResultSourceFrames;
    flags = sAudioMeRenderResultFlags;
    if (sourceFrames != 0) {
        sAudioReadPos = sAudioMeRenderResultReadPos & OOT_PSP_AUDIO_RING_MASK;
        sAudioResampleFrac = sAudioMeRenderResultResampleFrac & OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK;
        sAudioLastLeft = OotPspAudioBackend_UnpackLeft(sAudioMeRenderResultLast);
        sAudioLastRight = OotPspAudioBackend_UnpackRight(sAudioMeRenderResultLast);
        if (flags & OOT_PSP_AUDIO_RENDER_FLAG_UNDERRUN) {
            sAudioPlaybackPrimed = false;
        }
    }

    OotPspAudioBackend_UnlockMe();
    return sourceFrames;
}

static u32 OotPspAudioBackend_RenderOutputChunk(void) {
    u32 buffered = OotPspAudioBackend_BufferedFrames();

    if (sAudioMeInitialized) {
        return OotPspAudioBackend_RenderOutputChunkMe(buffered);
    }

    return OotPspAudioBackend_RenderOutputChunkCpu(buffered);
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
                OotPspAudioBackend_WaitOutputThread(0);
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
    SceUID threadId = sceKernelGetThreadId();
    s32 currentPriority = OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY;
    u32 nextUpdateUsec;

    OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PRIME, true);
    nextUpdateUsec = sceKernelGetSystemTimeLow();

    while (sAudioProducerThreadRunning) {
        s32 delayUsec;
        s32 needsCatchup;
        s32 desiredPriority;
        u32 now;

        nextUpdateUsec += OOT_PSP_AUDIO_UPDATE_USEC;
        now = sceKernelGetSystemTimeLow();
        delayUsec = (s32)(nextUpdateUsec - now);
        if (delayUsec > 0) {
            sceKernelDelayThread(delayUsec);
        } else if (delayUsec < -(s32)OOT_PSP_AUDIO_UPDATE_USEC) {
            nextUpdateUsec = now;
        }

        needsCatchup = OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_RefillBufferFrames();
        desiredPriority =
            needsCatchup ? OOT_PSP_AUDIO_PRODUCER_BOOST_PRIORITY : OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY;
        if (currentPriority != desiredPriority) {
            sceKernelChangeThreadPriority(threadId, desiredPriority);
            currentPriority = desiredPriority;
        }

        if (needsCatchup) {
            OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_CATCHUP, true);
        } else {
            OotPspAudioBackend_TryRunUpdate();
        }
    }

    if (currentPriority != OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY) {
        sceKernelChangeThreadPriority(threadId, OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY);
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

    ret = OotPspAudioBackend_EnsureOutputWakeSema();
    if (ret < 0) {
        return ret;
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

            memcpy(dst, samples, bytes);
            OotPspAudioBackend_WritebackRange(dst, bytes);
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
    OotPspAudioBackend_SignalOutputThread();
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

s32 OotPspAudioBackend_NeedsRefillDuringIo(void) {
    return OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_IoBackoffFrames();
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
