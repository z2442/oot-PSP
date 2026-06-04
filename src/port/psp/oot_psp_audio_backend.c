#include "oot_psp_audio_backend.h"

#include "attributes.h"
#include "audio.h"
#include "dma.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#define OOT_PSP_AUDIO_CHANNELS 2
#define OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY 32006
#define OOT_PSP_AUDIO_OUTPUT_FREQUENCY 44100
#define OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES 704
#define OOT_PSP_AUDIO_RING_FRAMES 32768
#define OOT_PSP_AUDIO_UPDATE_HZ 60
#define OOT_PSP_AUDIO_UPDATE_USEC (1000000 / OOT_PSP_AUDIO_UPDATE_HZ)
#define OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP 8
#define OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY 0x12
#define OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY 0x16
#define OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)

static s16 sAudioRing[OOT_PSP_AUDIO_RING_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 sAudioMix[OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static u8 sAudioExternalPool[OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE] __attribute__((aligned(64)));
static volatile u32 sAudioReadPos;
static volatile u32 sAudioWritePos;
static volatile u32 sAudioOutputFrames;
static volatile s32 sAudioThreadRunning;
static volatile s32 sAudioProducerThreadRunning;
static volatile s32 sAudioPlaybackPrimed;
static volatile u32 sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;
static s16 sAudioLastLeft;
static s16 sAudioLastRight;
static u64 sAudioResampleFrac;
static SceUID sAudioThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static s32 sAudioOutputChannel = -1;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);

static u32 OotPspAudioBackend_SourceChunkFrames(void) {
    u32 frequency = sAudioSourceFrequency;

    return (((OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES * frequency) + OOT_PSP_AUDIO_OUTPUT_FREQUENCY - 1) /
            OOT_PSP_AUDIO_OUTPUT_FREQUENCY);
}

static u32 OotPspAudioBackend_TargetBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * 6;
}

static u32 OotPspAudioBackend_RefillBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * 5;
}

static u32 OotPspAudioBackend_UrgentBufferFrames(void) {
    return OotPspAudioBackend_SourceChunkFrames() * 3;
}

static u64 OotPspAudioBackend_ResampleStep(void) {
    return ((u64)sAudioSourceFrequency << 32) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY;
}

static u32 OotPspAudioBackend_ViClock(void) {
    if (osTvType == OS_TV_PAL) {
        return VI_PAL_CLOCK;
    }

    if (osTvType == OS_TV_MPAL) {
        return VI_MPAL_CLOCK;
    }

    return VI_NTSC_CLOCK;
}

static s32 OotPspAudioBackend_CalculateAiFrequency(u32 frequency) {
    u32 clock;
    u32 dacRate;

    if (frequency == 0) {
        return -1;
    }

    clock = OotPspAudioBackend_ViClock();
    dacRate = (u32)(((u64)clock + (frequency / 2)) / frequency);
    if (dacRate < AI_MIN_DAC_RATE) {
        return -1;
    }

    return clock / (s32)dacRate;
}

static u32 OotPspAudioBackend_BufferedFrames(void) {
    u32 readPos = sAudioReadPos;
    u32 writePos = sAudioWritePos;

    if (writePos >= readPos) {
        return writePos - readPos;
    }

    return (OOT_PSP_AUDIO_RING_FRAMES - readPos) + writePos;
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
    return (rest > 0) ? rest : 0;
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

static s16 OotPspAudioBackend_Clamp16(s32 value) {
    if (value < -0x8000) {
        return -0x8000;
    }
    if (value > 0x7FFF) {
        return 0x7FFF;
    }
    return value;
}

static s16 OotPspAudioBackend_LerpSample(s16 cur, s16 next, u32 frac) {
    s64 sample = (s64)(next - cur) * frac;

    if (sample >= 0) {
        sample += 0x80000000LL;
    } else {
        sample -= 0x80000000LL;
    }

    return OotPspAudioBackend_Clamp16(cur + (s32)(sample >> 32));
}

static void OotPspAudioBackend_OutputMix(u32 sourceFrames) {
    sceKernelDcacheWritebackInvalidateRange(sAudioMix, sizeof(sAudioMix));
    sAudioOutputFrames = sourceFrames;
    sceAudioOutputBlocking(sAudioOutputChannel, PSP_AUDIO_VOLUME_MAX, sAudioMix);
    sAudioOutputFrames = 0;
}

static void OotPspAudioBackend_OutputSilence(void) {
    memset(sAudioMix, 0, sizeof(sAudioMix));
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
    OotPspAudioBackend_OutputMix(0);
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

static void OotPspAudioBackend_RunUpdates(u32 maxUpdates, s32 forceRefill) {
    u32 updates = 0;
    u32 buffered = OotPspAudioBackend_TotalBufferedFrames();
    u32 refillFrames = OotPspAudioBackend_RefillBufferFrames();
    u32 targetFrames = OotPspAudioBackend_TargetBufferFrames();

    if (!forceRefill && (buffered >= refillFrames)) {
        return;
    }

    while ((updates < maxUpdates) && (buffered < targetFrames)) {
        AudioThread_Update();
        updates++;
        buffered = OotPspAudioBackend_TotalBufferedFrames();
    }
}

static int OotPspAudioBackend_Thread(UNUSED SceSize args, UNUSED void* argp) {
    while (sAudioThreadRunning) {
        u32 buffered = OotPspAudioBackend_BufferedFrames();
        u32 readPos = sAudioReadPos;
        u32 sourceFrames = 0;
        u64 resampleStep = OotPspAudioBackend_ResampleStep();
        s16* out = sAudioMix;
        u32 i;

        if (!sAudioPlaybackPrimed) {
            if (buffered < OotPspAudioBackend_TargetBufferFrames()) {
                OotPspAudioBackend_OutputSilence();
                continue;
            }
            sAudioPlaybackPrimed = true;
        }

        if (buffered <= 1) {
            sAudioPlaybackPrimed = false;
            OotPspAudioBackend_OutputSilence();
            continue;
        }

        for (i = 0; i < OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES; i++) {
            u32 ringOffset = readPos * OOT_PSP_AUDIO_CHANNELS;
            u32 nextReadPos = readPos + 1;
            u32 nextRingOffset;
            u32 frac;
            u32 advance;
            s16 left;
            s16 right;

            if (buffered <= 1) {
                break;
            }

            if (nextReadPos == OOT_PSP_AUDIO_RING_FRAMES) {
                nextReadPos = 0;
            }
            nextRingOffset = nextReadPos * OOT_PSP_AUDIO_CHANNELS;
            frac = (u32)sAudioResampleFrac;
            left = OotPspAudioBackend_LerpSample(sAudioRing[ringOffset + 0], sAudioRing[nextRingOffset + 0], frac);
            right = OotPspAudioBackend_LerpSample(sAudioRing[ringOffset + 1], sAudioRing[nextRingOffset + 1], frac);

            *out++ = left;
            *out++ = right;
            sAudioLastLeft = left;
            sAudioLastRight = right;

            sAudioResampleFrac += resampleStep;
            advance = (u32)(sAudioResampleFrac >> 32);
            sAudioResampleFrac &= 0xFFFFFFFFULL;
            while (advance != 0) {
                readPos++;
                sourceFrames++;
                buffered--;
                if (readPos == OOT_PSP_AUDIO_RING_FRAMES) {
                    readPos = 0;
                }
                advance--;
            }
        }

        sAudioReadPos = readPos;
        if (i < OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES) {
            OotPspAudioBackend_FadeChunkToSilence(&out, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES - i, sAudioLastLeft,
                                                  sAudioLastRight);
            sAudioPlaybackPrimed = false;
        }

        OotPspAudioBackend_OutputMix(sourceFrames);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static int OotPspAudioBackend_ProducerThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 nextUpdateUsec = sceKernelGetSystemTimeLow();

    while (sAudioProducerThreadRunning) {
        s32 delayUsec;
        u32 now;

        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP, false);

        nextUpdateUsec += OOT_PSP_AUDIO_UPDATE_USEC;
        now = sceKernelGetSystemTimeLow();
        delayUsec = (s32)(nextUpdateUsec - now);
        if (delayUsec > 0) {
            sceKernelDelayThread(delayUsec);
        } else {
            nextUpdateUsec = now;
            sceKernelDelayThread(1000);
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

s32 OotPspAudioBackend_Init(void) {
    if (sAudioThreadRunning) {
        return 0;
    }

    memset(sAudioRing, 0, sizeof(sAudioRing));
    sAudioReadPos = 0;
    sAudioWritePos = 0;
    sAudioOutputFrames = 0;
    sAudioPlaybackPrimed = false;
    sAudioLastLeft = 0;
    sAudioLastRight = 0;
    sAudioResampleFrac = 0;
    sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;

    sceAudioOutput2Release();
    sceAudioSRCChRelease();
    if (sAudioOutputChannel >= 0) {
        sceAudioChRelease(sAudioOutputChannel);
        sAudioOutputChannel = -1;
    }
    sAudioOutputChannel =
        sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (sAudioOutputChannel < 0) {
        return -1;
    }

    sAudioThreadRunning = true;
    sAudioThreadId = sceKernelCreateThread("OOT PSP Audio", OotPspAudioBackend_Thread,
                                           OOT_PSP_AUDIO_OUTPUT_THREAD_PRIORITY, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (sAudioThreadId < 0) {
        sAudioThreadRunning = false;
        sceAudioChRelease(sAudioOutputChannel);
        sAudioOutputChannel = -1;
        return -1;
    }

    sceKernelStartThread(sAudioThreadId, 0, NULL);
    return 0;
}

static s32 OotPspAudioBackend_StartProducer(void) {
    if (sAudioProducerThreadRunning) {
        return 0;
    }

    sAudioProducerThreadRunning = true;
    sAudioProducerThreadId =
        sceKernelCreateThread("OOT PSP AudioGen", OotPspAudioBackend_ProducerThread,
                              OOT_PSP_AUDIO_PRODUCER_THREAD_PRIORITY, 0x20000,
                              PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (sAudioProducerThreadId < 0) {
        sAudioProducerThreadRunning = false;
        return -1;
    }

    sceKernelStartThread(sAudioProducerThreadId, 0, NULL);
    return 0;
}

s32 OotPspAudioBackend_Queue(const void* buf, u32 size) {
    const s16* samples = buf;
    u32 frames = size / (sizeof(s16) * OOT_PSP_AUDIO_CHANNELS);
    u32 freeFrames = OotPspAudioBackend_FreeFrames();
    u32 writePos = sAudioWritePos;
    u32 i;

    if ((buf == NULL) || (frames == 0)) {
        return 0;
    }

    if (frames > freeFrames) {
        frames = freeFrames;
    }

    for (i = 0; i < frames; i++) {
        u32 ringOffset = writePos * OOT_PSP_AUDIO_CHANNELS;

        sAudioRing[ringOffset + 0] = samples[(i * OOT_PSP_AUDIO_CHANNELS) + 0];
        sAudioRing[ringOffset + 1] = samples[(i * OOT_PSP_AUDIO_CHANNELS) + 1];
        writePos++;
        if (writePos == OOT_PSP_AUDIO_RING_FRAMES) {
            writePos = 0;
        }
    }

    sAudioWritePos = writePos;

    return 0;
}

s32 OotPspAudioBackend_SetFrequency(u32 frequency) {
    s32 actualFrequency = OotPspAudioBackend_CalculateAiFrequency(frequency);

    if (actualFrequency < 0) {
        return -1;
    }

    sAudioSourceFrequency = actualFrequency;
    sAudioResampleFrac = 0;
    return actualFrequency;
}

u32 OotPspAudioBackend_GetLength(void) {
    return OotPspAudioBackend_ReportableFrames() * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
}

s32 OotPspAudioBackend_NeedsRefillUrgently(void) {
    return OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_UrgentBufferFrames();
}

void OotPspAudio_Init(void) {
    OotPspAudioBackend_Init();
    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();
    OotPspAudioBackend_StartProducer();
}

void OotPspAudio_Update(void) {
    if (!sAudioProducerThreadRunning) {
        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP, true);
    }
}
