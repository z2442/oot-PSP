#include "oot_psp_audio_backend.h"

#include "attributes.h"
#include "audio.h"
#include "dma.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#define OOT_PSP_AUDIO_CHANNELS 2
#define OOT_PSP_AUDIO_FREQUENCY 32000
#define OOT_PSP_AUDIO_CHUNK_FRAMES 544
#define OOT_PSP_AUDIO_TARGET_BUFFER_FRAMES (OOT_PSP_AUDIO_CHUNK_FRAMES * 6)
#define OOT_PSP_AUDIO_PREBUFFER_FRAMES OOT_PSP_AUDIO_TARGET_BUFFER_FRAMES
#define OOT_PSP_AUDIO_RING_FRAMES 32768
#define OOT_PSP_AUDIO_UPDATE_HZ 60
#define OOT_PSP_AUDIO_UPDATE_USEC (1000000 / OOT_PSP_AUDIO_UPDATE_HZ)
#define OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP 8

static s16 sAudioRing[OOT_PSP_AUDIO_RING_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static s16 sAudioMix[OOT_PSP_AUDIO_CHUNK_FRAMES * OOT_PSP_AUDIO_CHANNELS] __attribute__((aligned(64)));
static volatile u32 sAudioReadPos;
static volatile u32 sAudioWritePos;
static volatile u32 sAudioOutputFrames;
static volatile s32 sAudioThreadRunning;
static volatile s32 sAudioProducerThreadRunning;
static volatile s32 sAudioPlaybackPrimed;
static SceUID sAudioThreadId = -1;
static SceUID sAudioProducerThreadId = -1;
static s32 sAudioSrcReserved;

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

    if (!sAudioSrcReserved) {
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

    if (buffered <= OOT_PSP_AUDIO_TARGET_BUFFER_FRAMES) {
        return 0;
    }

    return buffered - OOT_PSP_AUDIO_TARGET_BUFFER_FRAMES;
}

static void OotPspAudioBackend_OutputSilence(void) {
    memset(sAudioMix, 0, sizeof(sAudioMix));
    sceKernelDcacheWritebackInvalidateRange(sAudioMix, sizeof(sAudioMix));
    sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, sAudioMix);
}

static void OotPspAudioBackend_RunUpdates(u32 maxUpdates) {
    u32 updates = 0;

    while ((updates < maxUpdates) &&
           (OotPspAudioBackend_TotalBufferedFrames() < OOT_PSP_AUDIO_TARGET_BUFFER_FRAMES)) {
        AudioThread_Update();
        updates++;
    }
}

static int OotPspAudioBackend_Thread(UNUSED SceSize args, UNUSED void* argp) {
    while (sAudioThreadRunning) {
        u32 buffered = OotPspAudioBackend_BufferedFrames();
        u32 readPos = sAudioReadPos;
        s16* out = sAudioMix;
        u32 i;

        if (!sAudioPlaybackPrimed) {
            if (buffered < OOT_PSP_AUDIO_PREBUFFER_FRAMES) {
                OotPspAudioBackend_OutputSilence();
                continue;
            }
            sAudioPlaybackPrimed = true;
        }

        if (buffered < OOT_PSP_AUDIO_CHUNK_FRAMES) {
            sAudioPlaybackPrimed = false;
            OotPspAudioBackend_OutputSilence();
            continue;
        }

        for (i = 0; i < OOT_PSP_AUDIO_CHUNK_FRAMES; i++) {
            u32 ringOffset = readPos * OOT_PSP_AUDIO_CHANNELS;

            *out++ = sAudioRing[ringOffset + 0];
            *out++ = sAudioRing[ringOffset + 1];
            readPos++;
            if (readPos == OOT_PSP_AUDIO_RING_FRAMES) {
                readPos = 0;
            }
        }

        sAudioReadPos = readPos;
        sceKernelDcacheWritebackInvalidateRange(sAudioMix, sizeof(sAudioMix));
        sAudioOutputFrames = OOT_PSP_AUDIO_CHUNK_FRAMES;
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, sAudioMix);
        sAudioOutputFrames = 0;
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static int OotPspAudioBackend_ProducerThread(UNUSED SceSize args, UNUSED void* argp) {
    u32 nextUpdateUsec = sceKernelGetSystemTimeLow();

    while (sAudioProducerThreadRunning) {
        s32 delayUsec;
        u32 now;

        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP);

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

    sceAudioOutput2Release();
    sceAudioSRCChRelease();
    if (sceAudioSRCChReserve(OOT_PSP_AUDIO_CHUNK_FRAMES, OOT_PSP_AUDIO_FREQUENCY, OOT_PSP_AUDIO_CHANNELS) < 0) {
        return -1;
    }
    sAudioSrcReserved = true;

    sAudioThreadRunning = true;
    sAudioThreadId =
        sceKernelCreateThread("OOT PSP Audio", OotPspAudioBackend_Thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (sAudioThreadId < 0) {
        sAudioThreadRunning = false;
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
    sAudioProducerThreadId = sceKernelCreateThread("OOT PSP AudioGen", OotPspAudioBackend_ProducerThread, 0x16,
                                                   0x20000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
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

s32 OotPspAudioBackend_SetFrequency(UNUSED u32 frequency) {
    return OOT_PSP_AUDIO_FREQUENCY;
}

u32 OotPspAudioBackend_GetLength(void) {
    return OotPspAudioBackend_ReportableFrames() * OOT_PSP_AUDIO_CHANNELS * sizeof(s16);
}

void OotPspAudio_Init(void) {
    OotPspAudioBackend_Init();
    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    Audio_Init();
    Audio_InitSound();
    OotPspAudioBackend_StartProducer();
}

void OotPspAudio_Update(void) {
    if (!sAudioProducerThreadRunning) {
        OotPspAudioBackend_RunUpdates(OOT_PSP_AUDIO_MAX_UPDATES_PER_PUMP);
    }
}
