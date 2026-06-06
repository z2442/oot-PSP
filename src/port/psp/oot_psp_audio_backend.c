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
#define OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES 512
#define OOT_PSP_AUDIO_RING_FRAMES 16384
#define OOT_PSP_AUDIO_RING_MASK (OOT_PSP_AUDIO_RING_FRAMES - 1)
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS 24
#define OOT_PSP_AUDIO_RESAMPLE_FRAC_MASK ((1U << OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS) - 1)
#define OOT_PSP_AUDIO_LERP_FRAC_BITS 15

/*
 * One-thread backend:
 *   - The same PSP thread generates N64 audio and submits PSP output.
 *   - No separate producer timer, so audio threads cannot fight each other.
 *   - The thread only generates a tiny bounded amount of audio before each
 *     output block. This prevents the old 8-update catch-up burst stutter.
 */
#define OOT_PSP_AUDIO_TARGET_CHUNKS 3
#define OOT_PSP_AUDIO_REFILL_CHUNKS 2
#define OOT_PSP_AUDIO_URGENT_CHUNKS 1

/* PSP priorities: lower number == higher priority. */
#define OOT_PSP_AUDIO_THREAD_PRIORITY 0x20

#define OOT_PSP_AUDIO_MAX_UPDATES_NORMAL 1
#define OOT_PSP_AUDIO_MAX_UPDATES_URGENT 2
#define OOT_PSP_AUDIO_MAX_UPDATES_PRIME 3
#define OOT_PSP_AUDIO_EXTERNAL_POOL_SIZE (2 * 1024 * 1024)

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
static volatile s32 sAudioThreadRunning;
static volatile s32 sAudioPlaybackPrimed;
static volatile u32 sAudioSourceFrequency = OOT_PSP_AUDIO_DEFAULT_SOURCE_FREQUENCY;

/* Q24 source-frame phase and step per 44100 Hz PSP output frame. */
static u32 sAudioResampleFrac;
static u32 sAudioResampleStep;
static s16 sAudioLastLeft;
static s16 sAudioLastRight;
static SceUID sAudioThreadId = -1;
static s32 sAudioOutputChannel = -1;

void AudioThread_InitExternalPool(void* ramAddr, u32 size);

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
    return (u32)(((u64)frequency << OOT_PSP_AUDIO_RESAMPLE_FRAC_BITS) / OOT_PSP_AUDIO_OUTPUT_FREQUENCY);
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

static void OotPspAudioBackend_OutputMix(u32 sourceFrames) {
    sceKernelDcacheWritebackRange(sAudioMix, sizeof(sAudioMix));
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

        /*
         * Linear interpolation avoids the imaging from zero-order hold. Q24
         * phase keeps N64 AI pacing accurate, while Q15 interpolation needs
         * only one 32-bit multiply per channel on Allegrex.
         */
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

static void OotPspAudioBackend_PumpBeforeOutput(void) {
    u32 buffered = OotPspAudioBackend_TotalBufferedFrames();
    u32 maxUpdates = OOT_PSP_AUDIO_MAX_UPDATES_NORMAL;

    if (buffered < OotPspAudioBackend_UrgentBufferFrames()) {
        maxUpdates = OOT_PSP_AUDIO_MAX_UPDATES_URGENT;
    }

    if (buffered < OotPspAudioBackend_TargetBufferFrames()) {
        OotPspAudioBackend_RunUpdates(maxUpdates, true);
    }
}

static int OotPspAudioBackend_Thread(UNUSED SceSize args, UNUSED void* argp) {
    /* Prime once after Audio_Init/Audio_InitSound have completed. */
    OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PRIME, true);

    while (sAudioThreadRunning) {
        u32 buffered;
        u32 sourceFrames;

        OotPspAudioBackend_PumpBeforeOutput();
        buffered = OotPspAudioBackend_BufferedFrames();

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

        sourceFrames = OotPspAudioBackend_RenderOutputChunk();
        if (sourceFrames == 0) {
            sAudioPlaybackPrimed = false;
            OotPspAudioBackend_OutputSilence();
            continue;
        }

        OotPspAudioBackend_OutputMix(sourceFrames);
    }

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
    if (sAudioOutputChannel >= 0) {
        sceAudioChRelease(sAudioOutputChannel);
        sAudioOutputChannel = -1;
    }

    sAudioOutputChannel =
        sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (sAudioOutputChannel < 0) {
        return -1;
    }

    return 0;
}

static s32 OotPspAudioBackend_StartThread(void) {
    if (sAudioThreadRunning) {
        return 0;
    }

    if (sAudioOutputChannel < 0) {
        return -1;
    }

    sAudioThreadRunning = true;
    sAudioThreadId = sceKernelCreateThread("OOT PSP AudioPump", OotPspAudioBackend_Thread,
                                           OOT_PSP_AUDIO_THREAD_PRIORITY, 0x20000,
                                           PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (sAudioThreadId < 0) {
        sAudioThreadRunning = false;
        return -1;
    }

    sceKernelStartThread(sAudioThreadId, 0, NULL);
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
    s32 actualFrequency = OotPspAudioBackend_CalculateAiFrequency(frequency);

    if (actualFrequency < 0) {
        return -1;
    }

    if (sAudioSourceFrequency != (u32)actualFrequency) {
        sAudioSourceFrequency = (u32)actualFrequency;
        sAudioResampleStep = OotPspAudioBackend_CalculateResampleStep(sAudioSourceFrequency);
        sAudioResampleFrac = 0;
    }

    return actualFrequency;
}

u32 OotPspAudioBackend_GetLength(void) {
    return OotPspAudioBackend_ReportableFrames() * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
}

s32 OotPspAudioBackend_NeedsRefillUrgently(void) {
    return OotPspAudioBackend_TotalBufferedFrames() < OotPspAudioBackend_UrgentBufferFrames();
}

void OotPspAudio_Init(void) {
    if (OotPspAudioBackend_Init() < 0) {
        return;
    }

    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();

    OotPspAudioBackend_StartThread();
}

void OotPspAudio_Update(void) {
    /*
     * The merged audio pump owns AudioThread_Update(). Keep this free so the
     * render/game thread does not pay for audio work.
     */
    if (!sAudioThreadRunning) {
        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_NORMAL, true);
    }
}
