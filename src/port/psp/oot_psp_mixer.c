#include "oot_psp_mixer.h"

#include "attributes.h"
#include "audio.h"
#include "oot_psp_audio_commands.h"

#ifndef OOT_PSP_AUDIO_MIXER_VME
#define OOT_PSP_AUDIO_MIXER_VME 1
#endif

#ifndef OOT_PSP_AUDIO_MIXER_FAST
#define OOT_PSP_AUDIO_MIXER_FAST 1
#endif

#ifndef OOT_PSP_AUDIO_MIXER_VERIFY
#define OOT_PSP_AUDIO_MIXER_VERIFY 0
#endif

#if defined(TARGET_PSP)
#if OOT_PSP_AUDIO_MIXER_VME
#include <me-core-mapper/me-core-mapper.h>
#include <me-core-mapper/me-lib.h>
#endif
#include <pspkernel.h>
#endif
#include <string.h>
#include <stddef.h>

#define OOT_PSP_DMEM_SIZE 0x2000
#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~15)
#define OOT_PSP_FILTER_TAP_COUNT 8
#define OOT_PSP_MIXER_VME_MAX_SAMPLES 1024
#define OOT_PSP_MIXER_VME_PROLOGUE 0x10
#define OOT_PSP_MIXER_VME_LANE_STRIDE 0x2000
#define OOT_PSP_ENVMIXER_VME_HALF_SAMPLES 8
#define OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES (2 * OOT_PSP_ENVMIXER_VME_HALF_SAMPLES)
#define OOT_PSP_MIXER_VME_MUL_OP 0x00204000
#define OOT_PSP_SAMPLE_CACHE_SLOTS 16
#define OOT_PSP_SAMPLE_CACHE_SLOT_SIZE OOT_PSP_AUDIO_SAMPLE_CACHE_PAGE_SIZE
#define OOT_PSP_BOOK_CACHE_SLOTS 8
#define OOT_PSP_BOOK_CACHE_MAX_SIZE (8 * 2 * 8 * sizeof(s16))
#define OOT_PSP_REVERB_CACHE_SLOTS 4
#define OOT_PSP_REVERB_CACHE_MAX_SAMPLES 0x2000

typedef struct {
    const void* source;
    u16 size;
    u16 age;
    u8 data[OOT_PSP_SAMPLE_CACHE_SLOT_SIZE];
} OotPspMixerSampleCacheEntry;

typedef struct {
    OotPspMixerSampleCacheEntry entries[OOT_PSP_SAMPLE_CACHE_SLOTS];
    u16 clock;
} OotPspMixerSampleCache;

typedef struct {
    const s16* source;
    u16 size;
    u16 age;
    s16 data[OOT_PSP_BOOK_CACHE_MAX_SIZE / sizeof(s16)];
} OotPspMixerBookCacheEntry;

typedef struct {
    OotPspMixerBookCacheEntry entries[OOT_PSP_BOOK_CACHE_SLOTS];
    u16 clock;
} OotPspMixerBookCache;

typedef struct {
    const s16* mainLeft;
    const s16* mainRight;
    u32 epoch;
    u16 samplesPerChan;
    s16 left[OOT_PSP_REVERB_CACHE_MAX_SAMPLES];
    s16 right[OOT_PSP_REVERB_CACHE_MAX_SAMPLES];
} OotPspMixerReverbCacheEntry;

typedef struct {
    OotPspMixerReverbCacheEntry entries[OOT_PSP_REVERB_CACHE_SLOTS];
} OotPspMixerReverbCache;

typedef struct {
    u16 in;
    u16 out;
    u16 nbytes;
    ADPCM_STATE* adpcmLoopState;
    s16 adpcmTable[8][2][8];
    s16 filterScratch[OOT_PSP_DMEM_SIZE / sizeof(s16)];
    s32 filter2Count;
    s16* filter2Lut;
    s32 filter2Valid;
    struct {
        s32 initialReverb;
        s32 rampReverb;
        s32 rampLeft;
        s32 rampRight;
        s32 volLeft;
        s32 volRight;
    } env;
    union {
        s16 s16[OOT_PSP_DMEM_SIZE / sizeof(s16)];
        u8 u8[OOT_PSP_DMEM_SIZE];
    } dmem;
    OotPspMixerSampleCache* sampleCache;
    OotPspMixerBookCache* bookCache;
    OotPspMixerReverbCache* reverbCache;
} OotPspMixerState;

typedef struct {
    OotPspMixerState mixer;
    OotPspMixerSampleCache sampleCache;
    OotPspMixerBookCache bookCache;
    OotPspMixerReverbCache reverbCache;
} OotPspMixerMeStorage;

static OotPspMixerState sCpuMixer __attribute__((aligned(64)));
static OotPspMixerMeStorage* sMeStorage;
static OotPspMixerState* sCurrentMixer = &sCpuMixer;
static s32 sExecutingOnMe;

static OotPspMixerState* OotPspMixer_GetState(void) {
    return sCurrentMixer;
}

#define OOT_PSP_MIXER_STATE() OotPspMixerState* mixer = OotPspMixer_GetState()
#define sMixer (*mixer)
#define DMEM_U8(addr) (&sMixer.dmem.u8[(u16)(addr)])
#define DMEM_S16(addr) (&sMixer.dmem.s16[(u16)(addr) / sizeof(s16)])

#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
static volatile s32 sMixerVmeReady;
static volatile u32 sMixerVmeRuns;
static volatile u32 sMixerVmeFallbacks;
static volatile u32 sMixerEnvVmeRuns;
static volatile u32 sMixerEnvVmeFallbacks;
#endif

#if (OOT_PSP_AUDIO_MIXER_FAST || OOT_PSP_AUDIO_MIXER_VME) && OOT_PSP_AUDIO_MIXER_VERIFY
static volatile u32 sMixerMixVerifyMismatches;
static volatile u32 sMixerEnvVerifyMismatches;
#endif

static s16 sResampleTable[64][4] = {
    { 0x0C39, 0x66AD, 0x0D46, 0xFFDF }, { 0x0B39, 0x6696, 0x0E5F, 0xFFD8 },
    { 0x0A44, 0x6669, 0x0F83, 0xFFD0 }, { 0x095A, 0x6626, 0x10B4, 0xFFC8 },
    { 0x087D, 0x65CD, 0x11F0, 0xFFBF }, { 0x07AB, 0x655E, 0x1338, 0xFFB6 },
    { 0x06E4, 0x64D9, 0x148C, 0xFFAC }, { 0x0628, 0x643F, 0x15EB, 0xFFA1 },
    { 0x0577, 0x638F, 0x1756, 0xFF96 }, { 0x04D1, 0x62CB, 0x18CB, 0xFF8A },
    { 0x0435, 0x61F3, 0x1A4C, 0xFF7E }, { 0x03A4, 0x6106, 0x1BD7, 0xFF71 },
    { 0x031C, 0x6007, 0x1D6C, 0xFF64 }, { 0x029F, 0x5EF5, 0x1F0B, 0xFF56 },
    { 0x022A, 0x5DD0, 0x20B3, 0xFF48 }, { 0x01BE, 0x5C9A, 0x2264, 0xFF3A },
    { 0x015B, 0x5B53, 0x241E, 0xFF2C }, { 0x0101, 0x59FC, 0x25E0, 0xFF1E },
    { 0x00AE, 0x5896, 0x27A9, 0xFF10 }, { 0x0063, 0x5720, 0x297A, 0xFF02 },
    { 0x001F, 0x559D, 0x2B50, 0xFEF4 }, { 0xFFE2, 0x540D, 0x2D2C, 0xFEE8 },
    { 0xFFAC, 0x5270, 0x2F0D, 0xFEDB }, { 0xFF7C, 0x50C7, 0x30F3, 0xFED0 },
    { 0xFF53, 0x4F14, 0x32DC, 0xFEC6 }, { 0xFF2E, 0x4D57, 0x34C8, 0xFEBD },
    { 0xFF0F, 0x4B91, 0x36B6, 0xFEB6 }, { 0xFEF5, 0x49C2, 0x38A5, 0xFEB0 },
    { 0xFEDF, 0x47ED, 0x3A95, 0xFEAC }, { 0xFECE, 0x4611, 0x3C85, 0xFEAB },
    { 0xFEC0, 0x4430, 0x3E74, 0xFEAC }, { 0xFEB6, 0x424A, 0x4060, 0xFEAF },
    { 0xFEAF, 0x4060, 0x424A, 0xFEB6 }, { 0xFEAC, 0x3E74, 0x4430, 0xFEC0 },
    { 0xFEAB, 0x3C85, 0x4611, 0xFECE }, { 0xFEAC, 0x3A95, 0x47ED, 0xFEDF },
    { 0xFEB0, 0x38A5, 0x49C2, 0xFEF5 }, { 0xFEB6, 0x36B6, 0x4B91, 0xFF0F },
    { 0xFEBD, 0x34C8, 0x4D57, 0xFF2E }, { 0xFEC6, 0x32DC, 0x4F14, 0xFF53 },
    { 0xFED0, 0x30F3, 0x50C7, 0xFF7C }, { 0xFEDB, 0x2F0D, 0x5270, 0xFFAC },
    { 0xFEE8, 0x2D2C, 0x540D, 0xFFE2 }, { 0xFEF4, 0x2B50, 0x559D, 0x001F },
    { 0xFF02, 0x297A, 0x5720, 0x0063 }, { 0xFF10, 0x27A9, 0x5896, 0x00AE },
    { 0xFF1E, 0x25E0, 0x59FC, 0x0101 }, { 0xFF2C, 0x241E, 0x5B53, 0x015B },
    { 0xFF3A, 0x2264, 0x5C9A, 0x01BE }, { 0xFF48, 0x20B3, 0x5DD0, 0x022A },
    { 0xFF56, 0x1F0B, 0x5EF5, 0x029F }, { 0xFF64, 0x1D6C, 0x6007, 0x031C },
    { 0xFF71, 0x1BD7, 0x6106, 0x03A4 }, { 0xFF7E, 0x1A4C, 0x61F3, 0x0435 },
    { 0xFF8A, 0x18CB, 0x62CB, 0x04D1 }, { 0xFF96, 0x1756, 0x638F, 0x0577 },
    { 0xFFA1, 0x15EB, 0x643F, 0x0628 }, { 0xFFAC, 0x148C, 0x64D9, 0x06E4 },
    { 0xFFB6, 0x1338, 0x655E, 0x07AB }, { 0xFFBF, 0x11F0, 0x65CD, 0x087D },
    { 0xFFC8, 0x10B4, 0x6626, 0x095A }, { 0xFFD0, 0x0F83, 0x6669, 0x0A44 },
    { 0xFFD8, 0x0E5F, 0x6696, 0x0B39 }, { 0xFFDF, 0x0D46, 0x66AD, 0x0C39 },
};

static s16 OotPspMixer_Clamp16(s32 value) {
    if (value < -0x8000) {
        return -0x8000;
    }
    if (value > 0x7FFF) {
        return 0x7FFF;
    }
    return value;
}

static s16 OotPspMixer_Vmulf(s16 left, s16 right) {
    s32 product = (s32)left * right;

    return OotPspMixer_Clamp16((product + 0x4000) >> 15);
}

static s16 OotPspMixer_Vadd(s16 left, s16 right) {
    return OotPspMixer_Clamp16((s32)left + right);
}

void OotPspMixer_InitVme(void) {
#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
    if (meLibGetCpuId() == 1) {
        OotPspMixerMeStorage* storage = meCoreEDRAMAlloc(ROUND_UP_64(sizeof(OotPspMixerMeStorage)));

        if (storage != NULL) {
            memset(storage, 0, sizeof(*storage));
            storage->mixer.sampleCache = &storage->sampleCache;
            storage->mixer.bookCache = &storage->bookCache;
            storage->mixer.reverbCache = &storage->reverbCache;
            sMeStorage = storage;
        }
        vmeLibEnable();
        vmeLibWipe();
        meLibSync();
        sMixerVmeReady = 1;
    }
#endif
}

void OotPspMixer_ShutdownVme(void) {
#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
    if ((meLibGetCpuId() == 1) && sMixerVmeReady) {
        sMixerVmeReady = 0;
        meLibSync();
        vmeLibDisable();
        if (sMeStorage != NULL) {
            meCoreEDRAMFree(sMeStorage);
            sMeStorage = NULL;
        }
    }
#endif
}

static s16 OotPspMixer_SignExtendShift2(u8 value, s32 shift) {
    return (s16)((((s32)(value & 3) << 30) >> 30) << shift);
}

static s16 OotPspMixer_SignExtendShift4(u8 value, s32 shift) {
    return (s16)((((s32)(value & 0xF) << 28) >> 28) << shift);
}

static void OotPspMixer_DecodeAdpcmHalf(s16** outPtr, const s16 table[2][8], const s16* ins) {
    s16* out = *outPtr;
    s16 prev1 = out[-1];
    s16 prev2 = out[-2];
    const s16* table0 = table[0];
    const s16* table1 = table[1];
    s32 acc;

    /* This triangular convolution is the hottest scalar part of ADPCM
     * synthesis. Keep it explicit: GCC otherwise emits a branch ladder for
     * the eight fixed iterations, along with repeated index calculations. */
    acc = (table0[0] * prev2) + (table1[0] * prev1) + (ins[0] << 11);
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[1] * prev2) + (table1[1] * prev1) + (ins[1] << 11);
    acc += table1[0] * ins[0];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[2] * prev2) + (table1[2] * prev1) + (ins[2] << 11);
    acc += table1[1] * ins[0];
    acc += table1[0] * ins[1];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[3] * prev2) + (table1[3] * prev1) + (ins[3] << 11);
    acc += table1[2] * ins[0];
    acc += table1[1] * ins[1];
    acc += table1[0] * ins[2];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[4] * prev2) + (table1[4] * prev1) + (ins[4] << 11);
    acc += table1[3] * ins[0];
    acc += table1[2] * ins[1];
    acc += table1[1] * ins[2];
    acc += table1[0] * ins[3];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[5] * prev2) + (table1[5] * prev1) + (ins[5] << 11);
    acc += table1[4] * ins[0];
    acc += table1[3] * ins[1];
    acc += table1[2] * ins[2];
    acc += table1[1] * ins[3];
    acc += table1[0] * ins[4];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[6] * prev2) + (table1[6] * prev1) + (ins[6] << 11);
    acc += table1[5] * ins[0];
    acc += table1[4] * ins[1];
    acc += table1[3] * ins[2];
    acc += table1[2] * ins[3];
    acc += table1[1] * ins[4];
    acc += table1[0] * ins[5];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    acc = (table0[7] * prev2) + (table1[7] * prev1) + (ins[7] << 11);
    acc += table1[6] * ins[0];
    acc += table1[5] * ins[1];
    acc += table1[4] * ins[2];
    acc += table1[3] * ins[3];
    acc += table1[2] * ins[4];
    acc += table1[1] * ins[5];
    acc += table1[0] * ins[6];
    *out++ = OotPspMixer_Clamp16(acc >> 11);

    *outPtr = out;
}

void OotPspMixer_ClearBuffer(u16 dmem, s32 nbytes) {
    OOT_PSP_MIXER_STATE();
    memset(DMEM_U8(dmem), 0, ROUND_UP_16(nbytes));
}

void OotPspMixer_LoadBuffer(const void* source, u16 dmemDest, u16 nbytes) {
    OOT_PSP_MIXER_STATE();
    memcpy(DMEM_U8(dmemDest), source, ROUND_DOWN_16(nbytes));
}

static u16 OotPspMixer_NextCacheAge(u16* clock, void* entries, size_t entrySize, s32 entryCount,
                                    size_t ageOffset) {
    u16 age = ++*clock;
    s32 i;

    if (age != 0) {
        return age;
    }

    age = *clock = 1;
    for (i = 0; i < entryCount; i++) {
        *(u16*)((u8*)entries + (i * entrySize) + ageOffset) = 0;
    }
    return age;
}

static OotPspMixerSampleCacheEntry* OotPspMixer_GetSampleCachePage(OotPspMixerState* mixer,
                                                                   const u8* pageSource) {
    OotPspMixerSampleCache* cache = mixer->sampleCache;
    OotPspMixerSampleCacheEntry* victim = NULL;
    s32 i;

    if (cache == NULL) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_SAMPLE_CACHE_SLOTS; i++) {
        OotPspMixerSampleCacheEntry* entry = &cache->entries[i];

        if (entry->source == pageSource) {
            entry->age = OotPspMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                                  OOT_PSP_SAMPLE_CACHE_SLOTS,
                                                  offsetof(OotPspMixerSampleCacheEntry, age));
            return entry;
        }
        if ((victim == NULL) || (entry->source == NULL) || (entry->age < victim->age)) {
            victim = entry;
            if (entry->source == NULL) {
                break;
            }
        }
    }

    memcpy(victim->data, pageSource, OOT_PSP_SAMPLE_CACHE_SLOT_SIZE);
    victim->source = pageSource;
    victim->size = OOT_PSP_SAMPLE_CACHE_SLOT_SIZE;
    victim->age = OotPspMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                           OOT_PSP_SAMPLE_CACHE_SLOTS,
                                           offsetof(OotPspMixerSampleCacheEntry, age));
    return victim;
}

static void OotPspMixer_LoadSampleCached(const void* source, u16 dmemDest, u16 nbytes) {
    OOT_PSP_MIXER_STATE();
    const u8* src = source;
    u8* dst = DMEM_U8(dmemDest);
    u16 remaining = ROUND_DOWN_16(nbytes);

    if (sMixer.sampleCache == NULL) {
        memcpy(dst, src, remaining);
        return;
    }

    while (remaining != 0) {
        const u8* pageSource = (const u8*)((uintptr_t)src & ~(OOT_PSP_SAMPLE_CACHE_SLOT_SIZE - 1));
        u16 pageOffset = src - pageSource;
        u16 todo = OOT_PSP_SAMPLE_CACHE_SLOT_SIZE - pageOffset;
        OotPspMixerSampleCacheEntry* entry;

        if (todo > remaining) {
            todo = remaining;
        }
        entry = OotPspMixer_GetSampleCachePage(mixer, pageSource);
        if (entry == NULL) {
            memcpy(dst, src, todo);
        } else {
            memcpy(dst, entry->data + pageOffset, todo);
        }
        src += todo;
        dst += todo;
        remaining -= todo;
    }
}

void OotPspMixer_SaveBuffer(u16 dmemSrc, void* dest, u16 nbytes) {
    OOT_PSP_MIXER_STATE();
    memcpy(dest, DMEM_U8(dmemSrc), ROUND_DOWN_16(nbytes));
}

static OotPspMixerReverbCacheEntry* OotPspMixer_GetReverbCache(
    OotPspMixerState* mixer, const OotPspAudioReverbDownsampleCmd* desc) {
    OotPspMixerReverbCache* cache = mixer->reverbCache;
    OotPspMixerReverbCacheEntry* victim = NULL;
    s32 i;

    if ((cache == NULL) || (desc == NULL) || (desc->leftRingBuf == NULL) || (desc->rightRingBuf == NULL) ||
        (desc->bufSizePerChan == 0) || (desc->bufSizePerChan > OOT_PSP_REVERB_CACHE_MAX_SAMPLES)) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_REVERB_CACHE_SLOTS; i++) {
        OotPspMixerReverbCacheEntry* entry = &cache->entries[i];

        if ((entry->epoch == desc->cacheEpoch) && (entry->mainLeft == desc->leftRingBuf) &&
            (entry->mainRight == desc->rightRingBuf) && (entry->samplesPerChan == desc->bufSizePerChan)) {
            return entry;
        }
        if ((victim == NULL) && (entry->epoch != desc->cacheEpoch)) {
            victim = entry;
        }
    }

    if (victim == NULL) {
        uintptr_t key = ((uintptr_t)desc->leftRingBuf >> 6) ^ ((uintptr_t)desc->rightRingBuf >> 10);

        victim = &cache->entries[key & (OOT_PSP_REVERB_CACHE_SLOTS - 1)];
    }

#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
    {
        u32 bytes = desc->bufSizePerChan * sizeof(s16);
        uintptr_t leftStart = (uintptr_t)desc->leftRingBuf & ~63U;
        uintptr_t rightStart = (uintptr_t)desc->rightRingBuf & ~63U;
        uintptr_t leftEnd = ((uintptr_t)desc->leftRingBuf + bytes + 63) & ~63U;
        uintptr_t rightEnd = ((uintptr_t)desc->rightRingBuf + bytes + 63) & ~63U;

        meLibDcacheInvalidateRange((u32)leftStart, leftEnd - leftStart);
        meLibDcacheInvalidateRange((u32)rightStart, rightEnd - rightStart);
    }
#endif
    memcpy(victim->left, desc->leftRingBuf, desc->bufSizePerChan * sizeof(s16));
    memcpy(victim->right, desc->rightRingBuf, desc->bufSizePerChan * sizeof(s16));
    victim->mainLeft = desc->leftRingBuf;
    victim->mainRight = desc->rightRingBuf;
    victim->samplesPerChan = desc->bufSizePerChan;
    victim->epoch = desc->cacheEpoch;
    return victim;
}

static void OotPspMixer_ReverbDownsample(u16 dmemLeft, const OotPspAudioReverbDownsampleCmd* desc) {
    OOT_PSP_MIXER_STATE();
    OotPspMixerReverbCacheEntry* cacheEntry = OotPspMixer_GetReverbCache(mixer, desc);
    const s16* left = DMEM_S16(dmemLeft);
    const s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 step = desc->downsampleRate;
    s32 lengthASamples = desc->lengthA / (s32)sizeof(s16);
    s32 lengthBSamples = desc->lengthB / (s32)sizeof(s16);
    s32 i;
    s32 j;

    for (j = 0, i = 0; i < lengthASamples; i++, j += step) {
        leftRing[startPos + i] = left[j];
        rightRing[startPos + i] = right[j];
    }

    for (i = 0; i < lengthBSamples; i++, j += step) {
        leftRing[i] = left[j];
        rightRing[i] = right[j];
    }
}

static void OotPspMixer_ReverbSave(u16 dmemLeft, const OotPspAudioReverbDownsampleCmd* desc) {
    OOT_PSP_MIXER_STATE();
    OotPspMixerReverbCacheEntry* cacheEntry = OotPspMixer_GetReverbCache(mixer, desc);
    const s16* left = DMEM_S16(dmemLeft);
    const s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 lengthASamples = desc->lengthA / (s32)sizeof(s16);
    s32 lengthBSamples = desc->lengthB / (s32)sizeof(s16);
    s32 i;
    s32 j;

    for (j = 0, i = 0; i < lengthASamples; i++, j++) {
        leftRing[startPos + i] = left[j];
        rightRing[startPos + i] = right[j];
    }

    for (i = 0; i < lengthBSamples; i++, j++) {
        leftRing[i] = left[j];
        rightRing[i] = right[j];
    }
}

static void OotPspMixer_ReverbLoad(u16 dmemLeft, const OotPspAudioReverbDownsampleCmd* desc) {
    OOT_PSP_MIXER_STATE();
    OotPspMixerReverbCacheEntry* cacheEntry = OotPspMixer_GetReverbCache(mixer, desc);
    s16* left = DMEM_S16(dmemLeft);
    s16* right = DMEM_S16(dmemLeft + DMEM_1CH_SIZE);
    const s16* leftRing = (cacheEntry != NULL) ? cacheEntry->left : desc->leftRingBuf;
    const s16* rightRing = (cacheEntry != NULL) ? cacheEntry->right : desc->rightRingBuf;
    s32 startPos = desc->startPos;
    s32 lengthASamples = desc->lengthA / (s32)sizeof(s16);
    s32 lengthBSamples = desc->lengthB / (s32)sizeof(s16);
    s32 i;
    s32 j;

    for (j = 0, i = 0; i < lengthASamples; i++, j++) {
        left[j] = leftRing[startPos + i];
        right[j] = rightRing[startPos + i];
    }

    for (i = 0; i < lengthBSamples; i++, j++) {
        left[j] = leftRing[i];
        right[j] = rightRing[i];
    }
}

void OotPspMixer_LoadADPCM(s32 numEntriesBytes, const s16* book) {
    OOT_PSP_MIXER_STATE();
    if ((book != NULL) && (numEntriesBytes > 0) && (numEntriesBytes <= (s32)sizeof(sMixer.adpcmTable))) {
        memcpy(sMixer.adpcmTable, book, numEntriesBytes);
    }
}

static void OotPspMixer_LoadADPCMCached(s32 numEntriesBytes, const s16* book) {
    OOT_PSP_MIXER_STATE();
    OotPspMixerBookCache* cache = sMixer.bookCache;
    OotPspMixerBookCacheEntry* victim = NULL;
    s32 i;

    if ((book == NULL) || (numEntriesBytes <= 0) || (numEntriesBytes > (s32)sizeof(sMixer.adpcmTable))) {
        return;
    }
    if (cache == NULL) {
        memcpy(sMixer.adpcmTable, book, numEntriesBytes);
        return;
    }

    for (i = 0; i < OOT_PSP_BOOK_CACHE_SLOTS; i++) {
        OotPspMixerBookCacheEntry* entry = &cache->entries[i];

        if ((entry->source == book) && (entry->size == numEntriesBytes)) {
            entry->age = OotPspMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                                  OOT_PSP_BOOK_CACHE_SLOTS,
                                                  offsetof(OotPspMixerBookCacheEntry, age));
            memcpy(sMixer.adpcmTable, entry->data, numEntriesBytes);
            return;
        }
        if ((victim == NULL) || (entry->source == NULL) || (entry->age < victim->age)) {
            victim = entry;
            if (entry->source == NULL) {
                break;
            }
        }
    }

    memcpy(victim->data, book, numEntriesBytes);
    victim->source = book;
    victim->size = numEntriesBytes;
    victim->age = OotPspMixer_NextCacheAge(&cache->clock, cache->entries, sizeof(cache->entries[0]),
                                           OOT_PSP_BOOK_CACHE_SLOTS,
                                           offsetof(OotPspMixerBookCacheEntry, age));
    memcpy(sMixer.adpcmTable, victim->data, numEntriesBytes);
}

void OotPspMixer_SetBuffer(UNUSED u8 flags, u16 dmemIn, u16 dmemOut, u16 nbytes) {
    OOT_PSP_MIXER_STATE();
    sMixer.in = dmemIn;
    sMixer.out = dmemOut;
    sMixer.nbytes = nbytes;
}

void OotPspMixer_Interleave(u16 dmemOut, u16 dmemLeft, u16 dmemRight, u16 count) {
    OOT_PSP_MIXER_STATE();
    s16* out = DMEM_S16(dmemOut);
    s16* left = DMEM_S16(dmemLeft);
    s16* right = DMEM_S16(dmemRight);
    s32 frames = ROUND_DOWN_16(count) / 2;
    s32 i;

    for (i = 0; i < frames; i++) {
        out[(i * 2) + 0] = left[i];
        out[(i * 2) + 1] = right[i];
    }
}

void OotPspMixer_Interl(u16 dmemIn, u16 dmemOut, u16 count) {
    OOT_PSP_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_8(count);
    s32 i;

    for (i = 0; i < samples; i++) {
        out[i] = in[i * 2];
    }
}

void OotPspMixer_DMEMMove(u16 dmemIn, u16 dmemOut, s32 nbytes) {
    OOT_PSP_MIXER_STATE();
    u8 block[16];
    s32 count = ROUND_UP_16(nbytes);
    s32 offset;

    for (offset = 0; offset < count; offset += sizeof(block)) {
        memcpy(block, DMEM_U8(dmemIn + offset), sizeof(block));
        memcpy(DMEM_U8(dmemOut + offset), block, sizeof(block));
    }
}

void OotPspMixer_SetLoop(ADPCM_STATE* state) {
    OOT_PSP_MIXER_STATE();
    sMixer.adpcmLoopState = state;
}

void OotPspMixer_ADPCMdec(u8 flags, ADPCM_STATE state) {
    OOT_PSP_MIXER_STATE();
    u8* in = DMEM_U8(sMixer.in);
    s16* out = DMEM_S16(sMixer.out);
    s32 nbytes = ROUND_UP_32(sMixer.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if ((flags & A_LOOP) && (sMixer.adpcmLoopState != NULL)) {
        memcpy(out, sMixer.adpcmLoopState, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

    while (nbytes > 0) {
        s32 shift = *in >> 4;
        s32 tableIndex = *in++ & 0xF;
        s16(*tbl)[8] = sMixer.adpcmTable[tableIndex & 7];
        s32 half;

        if (flags & 4) {
            if (shift > 14) {
                shift = 14;
            }
        } else if (shift > 12) {
            shift = 12;
        }

        for (half = 0; half < 2; half++) {
            s16 ins[8];
            s32 j;

            if (flags & 4) {
                for (j = 0; j < 2; j++) {
                    u8 packed = *in++;

                    ins[(j * 4) + 0] = OotPspMixer_SignExtendShift2(packed >> 6, shift);
                    ins[(j * 4) + 1] = OotPspMixer_SignExtendShift2(packed >> 4, shift);
                    ins[(j * 4) + 2] = OotPspMixer_SignExtendShift2(packed >> 2, shift);
                    ins[(j * 4) + 3] = OotPspMixer_SignExtendShift2(packed, shift);
                }
            } else {
                for (j = 0; j < 4; j++) {
                    u8 packed = *in++;

                    ins[j * 2] = OotPspMixer_SignExtendShift4(packed >> 4, shift);
                    ins[(j * 2) + 1] = OotPspMixer_SignExtendShift4(packed, shift);
                }
            }

            OotPspMixer_DecodeAdpcmHalf(&out, tbl, ins);
        }

        nbytes -= 16 * sizeof(s16);
    }

    memcpy(state, out - 16, 16 * sizeof(s16));
}

void OotPspMixer_S8Dec(u8 flags, ADPCM_STATE state) {
    OOT_PSP_MIXER_STATE();
    s8* in = (s8*)DMEM_U8(sMixer.in);
    s16* out = DMEM_S16(sMixer.out);
    s32 samples = ROUND_UP_32(sMixer.nbytes) / sizeof(s16);
    s32 i;

    if (flags & A_INIT) {
        memset(out, 0, sizeof(ADPCM_STATE));
    } else if ((flags & A_LOOP) && (sMixer.adpcmLoopState != NULL)) {
        memcpy(out, sMixer.adpcmLoopState, sizeof(ADPCM_STATE));
    } else {
        memcpy(out, state, sizeof(ADPCM_STATE));
    }
    out += 16;

    for (i = 0; i < samples; i++) {
        out[i] = in[i] << 8;
    }

    memcpy(state, out + samples - 16, sizeof(ADPCM_STATE));
}

void OotPspMixer_Resample(u8 flags, u16 pitch, RESAMPLE_STATE state) {
    OOT_PSP_MIXER_STATE();
    s16 tmp[16];
    s16* inInitial = DMEM_S16(sMixer.in);
    s16* in = inInitial;
    s16* out = DMEM_S16(sMixer.out);
    s32 nbytes = ROUND_UP_16(sMixer.nbytes);
    u32 pitchAccumulator;
    s32 i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(s16));
    } else {
        memcpy(tmp, state, 16 * sizeof(s16));
    }

    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / sizeof(s16);
    }

    in -= 4;
    pitchAccumulator = (u16)tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

    while (nbytes > 0) {
        for (i = 0; i < 8; i++) {
            s16* tbl = sResampleTable[(pitchAccumulator * 64) >> 16];
            s16 product01 = OotPspMixer_Vadd(OotPspMixer_Vmulf(in[0], tbl[0]),
                                             OotPspMixer_Vmulf(in[1], tbl[1]));
            s16 product23 = OotPspMixer_Vadd(OotPspMixer_Vmulf(in[2], tbl[2]),
                                             OotPspMixer_Vmulf(in[3], tbl[3]));

            *out++ = OotPspMixer_Vadd(product01, product23);
            pitchAccumulator += pitch << 1;
            in += pitchAccumulator >> 16;
            pitchAccumulator &= 0xFFFF;
        }
        nbytes -= 8 * sizeof(s16);
    }

    state[4] = (s16)pitchAccumulator;
    memcpy(state, in, 4 * sizeof(s16));
    i = (in - inInitial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}

void OotPspMixer_ResampleZoh(u16 pitch, u16 pitchAccu) {
    OOT_PSP_MIXER_STATE();
    s16* out = DMEM_S16(sMixer.out);
    s32 samples = ROUND_UP_8(sMixer.nbytes) / sizeof(s16);
    u32 accumulator = ((u32)sMixer.in << 16) | pitchAccu;
    u32 step = (u32)pitch << 2;
    s32 i;

    for (i = 0; i < samples; i++) {
        out[i] = *DMEM_S16((accumulator >> 16) & 0xFFFE);
        accumulator += step;
    }
}

void OotPspMixer_EnvSetup1(s32 initialReverb, s32 rampReverb, s32 rampLeft, s32 rampRight) {
    OOT_PSP_MIXER_STATE();
    sMixer.env.initialReverb = initialReverb;
    sMixer.env.rampReverb = rampReverb;
    sMixer.env.rampLeft = rampLeft;
    sMixer.env.rampRight = rampRight;
}

void OotPspMixer_EnvSetup2(s32 volLeft, s32 volRight) {
    OOT_PSP_MIXER_STATE();
    sMixer.env.volLeft = volLeft;
    sMixer.env.volRight = volRight;
}

static s16 OotPspMixer_MulHighSignedUnsigned(s16 sample, u16 volume) {
    return (s16)(((s32)sample * volume) >> 16);
}

static void OotPspMixer_EnvMixerProducts(s16 sample, u16 volLeft, u16 volRight, u16 reverb, s16 dryLeftMask,
                                         s16 dryRightMask, s16 wetLeftMask, s16 wetRightMask, s16* leftOut,
                                         s16* rightOut, s16* wetLeftOut, s16* wetRightOut) {
    s16 left = OotPspMixer_MulHighSignedUnsigned(sample, volLeft);
    s16 right = OotPspMixer_MulHighSignedUnsigned(sample, volRight);
    s16 wetLeft;
    s16 wetRight;

    left ^= dryLeftMask;
    right ^= dryRightMask;

    wetLeft = OotPspMixer_MulHighSignedUnsigned(left, reverb);
    wetRight = OotPspMixer_MulHighSignedUnsigned(right, reverb);
    wetLeft ^= wetLeftMask;
    wetRight ^= wetRightMask;

    *leftOut = left;
    *rightOut = right;
    *wetLeftOut = wetLeft;
    *wetRightOut = wetRight;
}

#if !OOT_PSP_AUDIO_MIXER_FAST || OOT_PSP_AUDIO_MIXER_VERIFY
static s16 OotPspMixer_MixSampleReference(s16 in, s16 out, s16 gain) {
    s32 outProduct = (s32)out * 0x7FFF;
    s32 inProduct = (s32)in * gain;
    s32 accumulator;

    if ((inProduct > 0) && (outProduct > (0x7FFFFFFF - inProduct))) {
        return 0x7FFF;
    }
    if ((inProduct < 0) && (outProduct < ((-0x7FFFFFFF - 1) - inProduct))) {
        return -0x8000;
    }

    accumulator = outProduct + inProduct;
    if (accumulator > (0x7FFFFFFF - 0x4000)) {
        return 0x7FFF;
    }

    return OotPspMixer_Clamp16((accumulator + 0x4000) >> 15);
}
#endif

#if OOT_PSP_AUDIO_MIXER_FAST
static s16 OotPspMixer_MixSampleFast(s16 in, s16 out, s16 gain) {
    s32 accumulator = ((s32)out * 0x7FFF) + ((s32)in * gain);

    return OotPspMixer_Clamp16((accumulator + 0x4000) >> 15);
}
#endif

#if OOT_PSP_AUDIO_MIXER_FAST && OOT_PSP_AUDIO_MIXER_VERIFY
static s16 OotPspMixer_MixSampleFastVerified(s16 in, s16 out, s16 gain) {
    s16 mixed = OotPspMixer_MixSampleFast(in, out, gain);
    s16 reference = OotPspMixer_MixSampleReference(in, out, gain);

    if (mixed != reference) {
        sMixerMixVerifyMismatches++;
        return reference;
    }

    return mixed;
}
#endif

#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
static s32 OotPspMixer_IsVmeAvailable(void) {
    return sMixerVmeReady && sExecutingOnMe;
}

static volatile s32* OotPspMixer_VmeTopLane(s32 lane) {
    return (volatile s32*)(VME_TOP_BUFFERS + (lane * OOT_PSP_MIXER_VME_LANE_STRIDE));
}

static volatile s32* OotPspMixer_VmeBaseLane(s32 lane) {
    return (volatile s32*)(VME_BASE_BUFFERS + (lane * OOT_PSP_MIXER_VME_LANE_STRIDE));
}

static void OotPspMixer_VmeClearTail(volatile s32* lane, s32 count, s32 vmeCount) {
    s32 i;

    for (i = count; i < vmeCount; i++) {
        lane[i] = 0;
    }
}

static void OotPspMixer_VmeMul4(s32 count, s32 factor0, s32 factor1, s32 factor2, s32 factor3) {
    s32 vmeCount = count + OOT_PSP_MIXER_VME_PROLOGUE;

    meLibSync();
    vmeLibStart();

    vme_icn(FLOW, 0);
    vme_icn(ARCH, VME_DEF_MAPPER);

    vme_pe0(vme_fu(PRIMARY), vme_mux(TOP_0), OOT_PSP_MIXER_VME_MUL_OP, 0);
    vme_pe0(fu_reg(PRIMARY, B), (u32)factor0);
    vme_pe0(agu_top(MODE), VME_DEF_MODE);
    vme_pe0(agu_top(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe0(agu_base(MODE), VME_DEF_MODE);
    vme_pe0(agu_base(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe0(agu_write(MODE), VME_DEF_MODE, VME_CYCLE_6);
    vme_pe0(agu_write(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe0(agu_write(FORMAT_0), OOT_PSP_MIXER_VME_PROLOGUE);
    vme_pe0(agu_write(FORMAT_1), VME_END_TOKEN);

    vme_pe1(vme_fu(PRIMARY), vme_mux(TOP_1), OOT_PSP_MIXER_VME_MUL_OP, 0);
    vme_pe1(fu_reg(PRIMARY, B), (u32)factor1);
    vme_pe1(agu_top(MODE), VME_DEF_MODE);
    vme_pe1(agu_top(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe1(agu_base(MODE), VME_DEF_MODE);
    vme_pe1(agu_base(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe1(agu_write(MODE), VME_DEF_MODE, VME_CYCLE_6);
    vme_pe1(agu_write(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe1(agu_write(FORMAT_0), OOT_PSP_MIXER_VME_PROLOGUE);
    vme_pe1(agu_write(FORMAT_1), VME_END_TOKEN);

    vme_pe2(vme_fu(PRIMARY), vme_mux(TOP_2), OOT_PSP_MIXER_VME_MUL_OP, 0);
    vme_pe2(fu_reg(PRIMARY, B), (u32)factor2);
    vme_pe2(agu_top(MODE), VME_DEF_MODE);
    vme_pe2(agu_top(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe2(agu_base(MODE), VME_DEF_MODE);
    vme_pe2(agu_base(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe2(agu_write(MODE), VME_DEF_MODE, VME_CYCLE_6);
    vme_pe2(agu_write(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe2(agu_write(FORMAT_0), OOT_PSP_MIXER_VME_PROLOGUE);
    vme_pe2(agu_write(FORMAT_1), VME_END_TOKEN);

    vme_pe3(vme_fu(PRIMARY), vme_mux(TOP_3), OOT_PSP_MIXER_VME_MUL_OP, 0);
    vme_pe3(fu_reg(PRIMARY, B), (u32)factor3);
    vme_pe3(agu_top(MODE), VME_DEF_MODE);
    vme_pe3(agu_top(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe3(agu_base(MODE), VME_DEF_MODE);
    vme_pe3(agu_base(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe3(agu_write(MODE), VME_DEF_MODE, VME_CYCLE_6);
    vme_pe3(agu_write(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe3(agu_write(FORMAT_0), OOT_PSP_MIXER_VME_PROLOGUE);
    vme_pe3(agu_write(FORMAT_1), VME_END_TOKEN);

    vmeLibFinish();
    meLibSync();
}

static s32 OotPspMixer_MixVme(s16* in, s16* out, s16 gain, s32 samples) {
    volatile s32* top = (volatile s32*)VME_TOP_BUFFERS;
    volatile s32* products = (volatile s32*)VME_BASE_BUFFERS;
    s32 vmeCount = samples + OOT_PSP_MIXER_VME_PROLOGUE;
    s32 i;

    if (!OotPspMixer_IsVmeAvailable() || (samples <= 0) || (samples > OOT_PSP_MIXER_VME_MAX_SAMPLES)) {
        sMixerVmeFallbacks++;
        return 0;
    }

    for (i = 0; i < samples; i++) {
        top[i] = in[i];
    }

    for (; i < vmeCount; i++) {
        top[i] = 0;
    }

    meLibSync();
    vmeLibStart();

    vme_icn(FLOW, 0);
    vme_icn(ARCH, VME_DEF_MAPPER);

    vme_pe0(vme_fu(PRIMARY), vme_mux(TOP_0), OOT_PSP_MIXER_VME_MUL_OP, 0);
    vme_pe0(fu_reg(PRIMARY, B), (u32)(s32)gain);

    vme_pe0(agu_top(MODE), VME_DEF_MODE);
    vme_pe0(agu_top(COUNT), VME_DEF_STEP, vmeCount - 1);

    vme_pe0(agu_base(MODE), VME_DEF_MODE);
    vme_pe0(agu_base(COUNT), VME_DEF_STEP, vmeCount - 1);

    vme_pe0(agu_write(MODE), VME_DEF_MODE, VME_CYCLE_6);
    vme_pe0(agu_write(COUNT), VME_DEF_STEP, vmeCount - 1);
    vme_pe0(agu_write(FORMAT_0), OOT_PSP_MIXER_VME_PROLOGUE);
    vme_pe0(agu_write(FORMAT_1), VME_END_TOKEN);

    vmeLibFinish();
    meLibSync();

    products += OOT_PSP_MIXER_VME_PROLOGUE;
    for (i = 0; i < samples; i++) {
        s16 oldOut = out[i];
        s32 accumulator = ((s32)oldOut * 0x7FFF) + products[i];
        s16 mixed = OotPspMixer_Clamp16((accumulator + 0x4000) >> 15);

#if OOT_PSP_AUDIO_MIXER_VERIFY
        s16 reference = OotPspMixer_MixSampleReference(in[i], oldOut, gain);

        if (mixed != reference) {
            sMixerMixVerifyMismatches++;
            mixed = reference;
        }
#endif

        out[i] = mixed;
    }

    sMixerVmeRuns++;
    return 1;
}
#endif

static void OotPspMixer_EnvMixerCpu(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3,
                                    u32 dmemDests) {
    OOT_PSP_MIXER_STATE();
    s16* in = DMEM_S16(dmemSrc);
    s16* dryLeft = DMEM_S16(((dmemDests >> 24) & 0xFF) << 4);
    s16* dryRight = DMEM_S16(((dmemDests >> 16) & 0xFF) << 4);
    s16* wetLeft = DMEM_S16(((dmemDests >> 8) & 0xFF) << 4);
    s16* wetRight = DMEM_S16((dmemDests & 0xFF) << 4);
    u16 volLeft0 = (u16)sMixer.env.volLeft;
    u16 volRight0 = (u16)sMixer.env.volRight;
    u16 reverb0 = (u16)(sMixer.env.initialReverb << 8);
    u16 volLeft1 = (u16)(volLeft0 + (s16)sMixer.env.rampLeft);
    u16 volRight1 = (u16)(volRight0 + (s16)sMixer.env.rampRight);
    u16 reverb1 = (u16)(reverb0 + (s16)sMixer.env.rampReverb);
    s16 rampLeft = (s16)(sMixer.env.rampLeft * 2);
    s16 rampRight = (s16)(sMixer.env.rampRight * 2);
    s16 rampReverb = (s16)(sMixer.env.rampReverb * 2);
    s16 dryLeftMask = x2 ? -1 : 0;
    s16 dryRightMask = x3 ? -1 : 0;
    s16 wetLeftMask = x0 ? -4 : 0;
    s16 wetRightMask = x1 ? -2 : 0;
    s32 remaining = ROUND_UP_16(aiBufLen);

    while (remaining > 0) {
        s32 block;

        for (block = 0; block < 16; block++) {
            s16 sample = *in++;
            u16 volLeft = (block < 8) ? volLeft0 : volLeft1;
            u16 volRight = (block < 8) ? volRight0 : volRight1;
            u16 reverb = (block < 8) ? reverb0 : reverb1;
            s16 left;
            s16 right;
            s16 wetL;
            s16 wetR;

            OotPspMixer_EnvMixerProducts(sample, volLeft, volRight, reverb, dryLeftMask, dryRightMask, wetLeftMask,
                                         wetRightMask, &left, &right, &wetL, &wetR);

            *dryLeft = OotPspMixer_Clamp16(*dryLeft + left);
            dryLeft++;
            *dryRight = OotPspMixer_Clamp16(*dryRight + right);
            dryRight++;

            if (swapLR) {
                *wetLeft = OotPspMixer_Clamp16(*wetLeft + wetR);
                wetLeft++;
                *wetRight = OotPspMixer_Clamp16(*wetRight + wetL);
                wetRight++;
            } else {
                *wetLeft = OotPspMixer_Clamp16(*wetLeft + wetL);
                wetLeft++;
                *wetRight = OotPspMixer_Clamp16(*wetRight + wetR);
                wetRight++;
            }
        }

        volLeft0 = (u16)(volLeft0 + rampLeft);
        volLeft1 = (u16)(volLeft1 + rampLeft);
        volRight0 = (u16)(volRight0 + rampRight);
        volRight1 = (u16)(volRight1 + rampRight);
        reverb0 = (u16)(reverb0 + rampReverb);
        reverb1 = (u16)(reverb1 + rampReverb);
        remaining -= 16;
    }
}

#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
static s32 OotPspMixer_EnvMixerVme(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3,
                                   u32 dmemDests) {
    OOT_PSP_MIXER_STATE();
    s16* in = DMEM_S16(dmemSrc);
    s16* dryLeft = DMEM_S16(((dmemDests >> 24) & 0xFF) << 4);
    s16* dryRight = DMEM_S16(((dmemDests >> 16) & 0xFF) << 4);
    s16* wetLeft = DMEM_S16(((dmemDests >> 8) & 0xFF) << 4);
    s16* wetRight = DMEM_S16((dmemDests & 0xFF) << 4);
    u16 volLeft0 = (u16)sMixer.env.volLeft;
    u16 volRight0 = (u16)sMixer.env.volRight;
    u16 reverb0 = (u16)(sMixer.env.initialReverb << 8);
    u16 volLeft1 = (u16)(volLeft0 + (s16)sMixer.env.rampLeft);
    u16 volRight1 = (u16)(volRight0 + (s16)sMixer.env.rampRight);
    u16 reverb1 = (u16)(reverb0 + (s16)sMixer.env.rampReverb);
    s16 rampLeft = (s16)(sMixer.env.rampLeft * 2);
    s16 rampRight = (s16)(sMixer.env.rampRight * 2);
    s16 rampReverb = (s16)(sMixer.env.rampReverb * 2);
    s16 dryLeftMask = x2 ? -1 : 0;
    s16 dryRightMask = x3 ? -1 : 0;
    s16 wetLeftMask = x0 ? -4 : 0;
    s16 wetRightMask = x1 ? -2 : 0;
    volatile s32* top0 = OotPspMixer_VmeTopLane(0);
    volatile s32* top1 = OotPspMixer_VmeTopLane(1);
    volatile s32* top2 = OotPspMixer_VmeTopLane(2);
    volatile s32* top3 = OotPspMixer_VmeTopLane(3);
    volatile s32* product0 = OotPspMixer_VmeBaseLane(0) + OOT_PSP_MIXER_VME_PROLOGUE;
    volatile s32* product1 = OotPspMixer_VmeBaseLane(1) + OOT_PSP_MIXER_VME_PROLOGUE;
    volatile s32* product2 = OotPspMixer_VmeBaseLane(2) + OOT_PSP_MIXER_VME_PROLOGUE;
    volatile s32* product3 = OotPspMixer_VmeBaseLane(3) + OOT_PSP_MIXER_VME_PROLOGUE;
    s32 remaining = ROUND_UP_16(aiBufLen);
    s32 vmeCount = OOT_PSP_ENVMIXER_VME_HALF_SAMPLES + OOT_PSP_MIXER_VME_PROLOGUE;
    s16 samples[OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES];
    s16 left[OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES];
    s16 right[OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES];
    s16 wetL[OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES];
    s16 wetR[OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES];
    s32 i;

    if (!OotPspMixer_IsVmeAvailable()) {
        sMixerEnvVmeFallbacks++;
        return 0;
    }

    while (remaining > 0) {
        for (i = 0; i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES; i++) {
            samples[i] = in[i];
            samples[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] = in[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES];

            top0[i] = samples[i];
            top1[i] = samples[i];
            top2[i] = samples[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES];
            top3[i] = samples[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES];
        }
        OotPspMixer_VmeClearTail(top0, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top1, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top2, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top3, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);

        OotPspMixer_VmeMul4(OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, volLeft0, volRight0, volLeft1, volRight1);

        for (i = 0; i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES; i++) {
            left[i] = (s16)(product0[i] >> 16);
            right[i] = (s16)(product1[i] >> 16);
            left[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] = (s16)(product2[i] >> 16);
            right[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] = (s16)(product3[i] >> 16);

            left[i] ^= dryLeftMask;
            right[i] ^= dryRightMask;
            left[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] ^= dryLeftMask;
            right[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] ^= dryRightMask;

            top0[i] = left[i];
            top1[i] = right[i];
            top2[i] = left[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES];
            top3[i] = right[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES];
        }
        OotPspMixer_VmeClearTail(top0, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top1, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top2, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);
        OotPspMixer_VmeClearTail(top3, OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, vmeCount);

        OotPspMixer_VmeMul4(OOT_PSP_ENVMIXER_VME_HALF_SAMPLES, reverb0, reverb0, reverb1, reverb1);

        for (i = 0; i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES; i++) {
            wetL[i] = (s16)(product0[i] >> 16);
            wetR[i] = (s16)(product1[i] >> 16);
            wetL[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] = (s16)(product2[i] >> 16);
            wetR[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] = (s16)(product3[i] >> 16);

            wetL[i] ^= wetLeftMask;
            wetR[i] ^= wetRightMask;
            wetL[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] ^= wetLeftMask;
            wetR[i + OOT_PSP_ENVMIXER_VME_HALF_SAMPLES] ^= wetRightMask;
        }

        for (i = 0; i < OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES; i++) {
#if OOT_PSP_AUDIO_MIXER_VERIFY
            u16 volLeft = (i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES) ? volLeft0 : volLeft1;
            u16 volRight = (i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES) ? volRight0 : volRight1;
            u16 reverb = (i < OOT_PSP_ENVMIXER_VME_HALF_SAMPLES) ? reverb0 : reverb1;
            s16 refLeft;
            s16 refRight;
            s16 refWetL;
            s16 refWetR;

            OotPspMixer_EnvMixerProducts(samples[i], volLeft, volRight, reverb, dryLeftMask, dryRightMask,
                                         wetLeftMask, wetRightMask, &refLeft, &refRight, &refWetL, &refWetR);
            if ((left[i] != refLeft) || (right[i] != refRight) || (wetL[i] != refWetL) || (wetR[i] != refWetR)) {
                sMixerEnvVerifyMismatches++;
                left[i] = refLeft;
                right[i] = refRight;
                wetL[i] = refWetL;
                wetR[i] = refWetR;
            }
#endif

            *dryLeft = OotPspMixer_Clamp16(*dryLeft + left[i]);
            dryLeft++;
            *dryRight = OotPspMixer_Clamp16(*dryRight + right[i]);
            dryRight++;

            if (swapLR) {
                *wetLeft = OotPspMixer_Clamp16(*wetLeft + wetR[i]);
                wetLeft++;
                *wetRight = OotPspMixer_Clamp16(*wetRight + wetL[i]);
                wetRight++;
            } else {
                *wetLeft = OotPspMixer_Clamp16(*wetLeft + wetL[i]);
                wetLeft++;
                *wetRight = OotPspMixer_Clamp16(*wetRight + wetR[i]);
                wetRight++;
            }
        }

        in += OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES;
        volLeft0 = (u16)(volLeft0 + rampLeft);
        volLeft1 = (u16)(volLeft1 + rampLeft);
        volRight0 = (u16)(volRight0 + rampRight);
        volRight1 = (u16)(volRight1 + rampRight);
        reverb0 = (u16)(reverb0 + rampReverb);
        reverb1 = (u16)(reverb1 + rampReverb);
        remaining -= OOT_PSP_ENVMIXER_VME_BLOCK_SAMPLES;
    }

    sMixerEnvVmeRuns++;
    return 1;
}
#endif

void OotPspMixer_EnvMixer(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3, u32 dmemDests,
                          UNUSED u32 bits) {
#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
    if (OotPspMixer_EnvMixerVme(dmemSrc, aiBufLen, swapLR, x0, x1, x2, x3, dmemDests)) {
        return;
    }
#endif

    OotPspMixer_EnvMixerCpu(dmemSrc, aiBufLen, swapLR, x0, x1, x2, x3, dmemDests);
}

void OotPspMixer_Mix(s32 countQuads, s16 gain, u16 dmemIn, u16 dmemOut) {
    OOT_PSP_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_32((countQuads & 0xFF) << 4) / sizeof(s16);
    s32 i;

#if defined(TARGET_PSP) && OOT_PSP_AUDIO_MIXER_VME
    if (OotPspMixer_MixVme(in, out, gain, samples)) {
        return;
    }
#endif

#if OOT_PSP_AUDIO_MIXER_FAST
    s32 unrolledSamples = samples & ~7;

    for (i = 0; i < unrolledSamples; i += 8) {
#if OOT_PSP_AUDIO_MIXER_VERIFY
        out[i + 0] = OotPspMixer_MixSampleFastVerified(in[i + 0], out[i + 0], gain);
        out[i + 1] = OotPspMixer_MixSampleFastVerified(in[i + 1], out[i + 1], gain);
        out[i + 2] = OotPspMixer_MixSampleFastVerified(in[i + 2], out[i + 2], gain);
        out[i + 3] = OotPspMixer_MixSampleFastVerified(in[i + 3], out[i + 3], gain);
        out[i + 4] = OotPspMixer_MixSampleFastVerified(in[i + 4], out[i + 4], gain);
        out[i + 5] = OotPspMixer_MixSampleFastVerified(in[i + 5], out[i + 5], gain);
        out[i + 6] = OotPspMixer_MixSampleFastVerified(in[i + 6], out[i + 6], gain);
        out[i + 7] = OotPspMixer_MixSampleFastVerified(in[i + 7], out[i + 7], gain);
#else
        out[i + 0] = OotPspMixer_MixSampleFast(in[i + 0], out[i + 0], gain);
        out[i + 1] = OotPspMixer_MixSampleFast(in[i + 1], out[i + 1], gain);
        out[i + 2] = OotPspMixer_MixSampleFast(in[i + 2], out[i + 2], gain);
        out[i + 3] = OotPspMixer_MixSampleFast(in[i + 3], out[i + 3], gain);
        out[i + 4] = OotPspMixer_MixSampleFast(in[i + 4], out[i + 4], gain);
        out[i + 5] = OotPspMixer_MixSampleFast(in[i + 5], out[i + 5], gain);
        out[i + 6] = OotPspMixer_MixSampleFast(in[i + 6], out[i + 6], gain);
        out[i + 7] = OotPspMixer_MixSampleFast(in[i + 7], out[i + 7], gain);
#endif
    }

    for (; i < samples; i++) {
#if OOT_PSP_AUDIO_MIXER_VERIFY
        out[i] = OotPspMixer_MixSampleFastVerified(in[i], out[i], gain);
#else
        out[i] = OotPspMixer_MixSampleFast(in[i], out[i], gain);
#endif
    }
#else
    for (i = 0; i < samples; i++) {
        out[i] = OotPspMixer_MixSampleReference(in[i], out[i], gain);
    }
#endif
}

void OotPspMixer_AddMixer(s32 nbytes, u16 dmemIn, u16 dmemOut, UNUSED s16 gain) {
    OOT_PSP_MIXER_STATE();
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 encodedBytes = ROUND_DOWN_16(nbytes);
    s32 samples = ROUND_UP_64(encodedBytes) / sizeof(s16);
    s32 i;

    if (encodedBytes == 0) {
        return;
    }

    for (i = 0; i < samples; i++) {
        out[i] = OotPspMixer_Vadd(out[i], in[i]);
    }
}

void OotPspMixer_Duplicate(s32 numCopies, u16 dmemSrc, u16 dmemDest) {
    OOT_PSP_MIXER_STATE();
    s32 i;

    for (i = 0; i < numCopies; i++) {
        memcpy(DMEM_U8(dmemDest + (i * 128)), DMEM_U8(dmemSrc), 128);
    }
}

void OotPspMixer_CopyBlocks(s32 numBlocks, u16 dmemSrc, u16 dmemDest, s32 blockSize) {
    OOT_PSP_MIXER_STATE();
    u8 block[32];
    s32 blockBytes = ROUND_UP_32(blockSize);
    s32 blockIndex;
    s32 offset;

    for (blockIndex = 0; blockIndex < numBlocks; blockIndex++) {
        for (offset = 0; offset < blockBytes; offset += sizeof(block)) {
            memcpy(block, DMEM_U8(dmemSrc), sizeof(block));
            memcpy(DMEM_U8(dmemDest), block, sizeof(block));
            dmemSrc += sizeof(block);
            dmemDest += sizeof(block);
        }
    }
}

void OotPspMixer_Filter(u8 flags, s32 countOrBuf, void* state) {
    OOT_PSP_MIXER_STATE();
    s16* save = state;
    s16* input;
    s16* out = sMixer.filterScratch;
    s16 history[OOT_PSP_FILTER_TAP_COUNT];
    s16 taps[OOT_PSP_FILTER_TAP_COUNT];
    s32 count;
    s32 processedCount;
    s32 i;

    /* Zelda ABI2 FILTER is a two-command sequence: setup taps/count, then process DMEM with persistent state. */
    if (flags > 1) {
        sMixer.filter2Count = ROUND_UP_16(countOrBuf);
        sMixer.filter2Lut = state;
        sMixer.filter2Valid = state != NULL;
        return;
    }

    if ((save == NULL) || !sMixer.filter2Valid || (sMixer.filter2Count <= 0)) {
        return;
    }

    if (flags & A_INIT) {
        memset(save, 0, 16 * sizeof(s16));
    }

    for (i = 0; i < OOT_PSP_FILTER_TAP_COUNT; i++) {
        s16 avg = ((s32)save[OOT_PSP_FILTER_TAP_COUNT + i] + sMixer.filter2Lut[i]) >> 1;

        history[i] = save[i];
        taps[i] = avg;
        save[OOT_PSP_FILTER_TAP_COUNT + i] = avg;
        sMixer.filter2Lut[i] = avg;
    }

    input = DMEM_S16((u16)countOrBuf);
    count = sMixer.filter2Count;
    if (count > (s32)sizeof(sMixer.filterScratch)) {
        count = sizeof(sMixer.filterScratch);
    }
    processedCount = count;

    while (count > 0) {
        s16 blockInput[OOT_PSP_FILTER_TAP_COUNT];
        /* OoT's filter tables have an L1 gain below 1.0, so all eight products fit in s32. */
        s32 out1[OOT_PSP_FILTER_TAP_COUNT];

        memcpy(blockInput, input, sizeof(blockInput));

        out1[1] = history[0] * taps[6];
        out1[1] += history[3] * taps[7];
        out1[1] += history[2] * taps[4];
        out1[1] += history[5] * taps[5];
        out1[1] += history[4] * taps[2];
        out1[1] += history[7] * taps[3];
        out1[1] += history[6] * taps[0];
        out1[1] += blockInput[1] * taps[1];

        out1[0] = history[3] * taps[6];
        out1[0] += history[2] * taps[7];
        out1[0] += history[5] * taps[4];
        out1[0] += history[4] * taps[5];
        out1[0] += history[7] * taps[2];
        out1[0] += history[6] * taps[3];
        out1[0] += blockInput[1] * taps[0];
        out1[0] += blockInput[0] * taps[1];

        out1[3] = history[2] * taps[6];
        out1[3] += history[5] * taps[7];
        out1[3] += history[4] * taps[4];
        out1[3] += history[7] * taps[5];
        out1[3] += history[6] * taps[2];
        out1[3] += blockInput[1] * taps[3];
        out1[3] += blockInput[0] * taps[0];
        out1[3] += blockInput[3] * taps[1];

        out1[2] = history[5] * taps[6];
        out1[2] += history[4] * taps[7];
        out1[2] += history[7] * taps[4];
        out1[2] += history[6] * taps[5];
        out1[2] += blockInput[1] * taps[2];
        out1[2] += blockInput[0] * taps[3];
        out1[2] += blockInput[3] * taps[0];
        out1[2] += blockInput[2] * taps[1];

        out1[5] = history[4] * taps[6];
        out1[5] += history[7] * taps[7];
        out1[5] += history[6] * taps[4];
        out1[5] += blockInput[1] * taps[5];
        out1[5] += blockInput[0] * taps[2];
        out1[5] += blockInput[3] * taps[3];
        out1[5] += blockInput[2] * taps[0];
        out1[5] += blockInput[5] * taps[1];

        out1[4] = history[7] * taps[6];
        out1[4] += history[6] * taps[7];
        out1[4] += blockInput[1] * taps[4];
        out1[4] += blockInput[0] * taps[5];
        out1[4] += blockInput[3] * taps[2];
        out1[4] += blockInput[2] * taps[3];
        out1[4] += blockInput[5] * taps[0];
        out1[4] += blockInput[4] * taps[1];

        out1[7] = history[6] * taps[6];
        out1[7] += blockInput[1] * taps[7];
        out1[7] += blockInput[0] * taps[4];
        out1[7] += blockInput[3] * taps[5];
        out1[7] += blockInput[2] * taps[2];
        out1[7] += blockInput[5] * taps[3];
        out1[7] += blockInput[4] * taps[0];
        out1[7] += blockInput[7] * taps[1];

        out1[6] = blockInput[1] * taps[6];
        out1[6] += blockInput[0] * taps[7];
        out1[6] += blockInput[3] * taps[4];
        out1[6] += blockInput[2] * taps[5];
        out1[6] += blockInput[5] * taps[2];
        out1[6] += blockInput[4] * taps[3];
        out1[6] += blockInput[7] * taps[0];
        out1[6] += blockInput[6] * taps[1];

        out[1] = OotPspMixer_Clamp16((out1[1] + 0x4000) >> 15);
        out[0] = OotPspMixer_Clamp16((out1[0] + 0x4000) >> 15);
        out[3] = OotPspMixer_Clamp16((out1[3] + 0x4000) >> 15);
        out[2] = OotPspMixer_Clamp16((out1[2] + 0x4000) >> 15);
        out[5] = OotPspMixer_Clamp16((out1[5] + 0x4000) >> 15);
        out[4] = OotPspMixer_Clamp16((out1[4] + 0x4000) >> 15);
        out[7] = OotPspMixer_Clamp16((out1[7] + 0x4000) >> 15);
        out[6] = OotPspMixer_Clamp16((out1[6] + 0x4000) >> 15);

        memcpy(history, blockInput, sizeof(history));
        input += OOT_PSP_FILTER_TAP_COUNT;
        out += OOT_PSP_FILTER_TAP_COUNT;
        count -= OOT_PSP_FILTER_TAP_COUNT * sizeof(s16);
    }

    memcpy(state, input - OOT_PSP_FILTER_TAP_COUNT, OOT_PSP_FILTER_TAP_COUNT * sizeof(s16));
    memcpy(DMEM_S16((u16)countOrBuf), sMixer.filterScratch, processedCount);
}

void OotPspMixer_HiLoGain(s32 gain, u16 dmemIn, UNUSED u16 dmemOut, s32 nbytes) {
    OOT_PSP_MIXER_STATE();
    s16* out = DMEM_S16(dmemIn);
    s32 samples = ROUND_UP_32(nbytes) / sizeof(s16);
    s32 i;

    gain &= 0xFF;
    for (i = 0; i < samples; i++) {
        out[i] = OotPspMixer_Clamp16((out[i] * gain) >> 4);
    }
}

void OotPspMixer_UnkCmd3(s32 arg1, s32 arg2, s32 size) {
    OOT_PSP_MIXER_STATE();
    s16* src = DMEM_S16(arg1);
    s16* dst = DMEM_S16(arg2);
    s32 samples = ROUND_UP_32(size) / sizeof(s16);
    s32 i;

    for (i = 0; i < samples; i += 16) {
        s32 lane;

        for (lane = 0; lane < 8; lane++) {
            s32 product = (u16)src[i + 8 + lane] * (s32)src[i + lane];
            u16 highResult;

            if (product < -0x40000000) {
                highResult = 0;
            } else if (product > 0x3FFFFFFF) {
                highResult = 0xFFFF;
            } else {
                highResult = (u16)((u16)product << 1);
            }

            dst[i + lane] = (s16)highResult;
            dst[i + 8 + lane] = (s16)(u16)product;
        }
    }
}

void OotPspMixer_UnkCmd19(UNUSED s32 arg1, UNUSED s32 arg2, UNUSED s32 size, UNUSED s32 arg4) {
    /*
     * Opcode 25 is not present in OoT's aspMain jump table, and the stock
     * sequences never emit samplebook mode 3. There is no N64 operation to emulate.
     */
}

static void OotPspMixer_ExecuteCommandListInternal(const Acmd* cmdList, s32 cmdCount) {
    s32 i;

    for (i = 0; i < cmdCount; i++) {
        const Acmd* cmd = &cmdList[i];
        u32 w0 = cmd->words.w0;
        u32 w1 = cmd->words.w1;

        switch (w0 >> 24) {
            case A_SPNOOP:
                break;

            case A_ADPCM:
                OotPspMixer_ADPCMdec((w0 >> 16) & 0xFF, (s16*)(uintptr_t)w1);
                break;

            case A_CLEARBUFF:
                OotPspMixer_ClearBuffer(w0 & 0xFFFF, w1);
                break;

            case A_UNK3:
                OotPspMixer_UnkCmd3((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_ADDMIXER:
                OotPspMixer_AddMixer(((w0 >> 16) & 0xFF) << 4, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF, (s16)w0);
                break;

            case A_RESAMPLE:
                OotPspMixer_Resample((w0 >> 16) & 0xFF, w0 & 0xFFFF, (s16*)(uintptr_t)w1);
                break;

            case A_RESAMPLE_ZOH:
                OotPspMixer_ResampleZoh(w0 & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_FILTER:
                OotPspMixer_Filter((w0 >> 16) & 0xFF, w0 & 0xFFFF, (void*)(uintptr_t)w1);
                break;

            case A_SETBUFF:
                OotPspMixer_SetBuffer((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_DUPLICATE:
                OotPspMixer_Duplicate((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF);
                break;

            case A_DMEMMOVE:
                OotPspMixer_DMEMMove(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_LOADADPCM:
                OotPspMixer_LoadADPCM(w0 & 0xFFFFFF, (const s16*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_LOAD_ADPCM_CACHED:
                OotPspMixer_LoadADPCMCached(w0 & 0xFFFFFF, (const s16*)(uintptr_t)w1);
                break;

            case A_MIXER:
                OotPspMixer_Mix((w0 >> 16) & 0xFF, (s16)w0, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_INTERLEAVE:
                OotPspMixer_Interleave(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF,
                                       ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_HILOGAIN:
                OotPspMixer_HiLoGain((w0 >> 16) & 0xFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_SETLOOP:
                OotPspMixer_SetLoop((ADPCM_STATE*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_COPYBLOCKS:
                OotPspMixer_CopyBlocks((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case OOT_PSP_A_REVERB_DOWNSAMPLE:
                OotPspMixer_ReverbDownsample(w0 & 0xFFFF,
                                             (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_REVERB_SAVE:
                OotPspMixer_ReverbSave(w0 & 0xFFFF, (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case OOT_PSP_A_REVERB_LOAD:
                OotPspMixer_ReverbLoad(w0 & 0xFFFF, (const OotPspAudioReverbDownsampleCmd*)(uintptr_t)w1);
                break;

            case A_INTERL:
                OotPspMixer_Interl((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF);
                break;

            case A_ENVSETUP1:
                OotPspMixer_EnvSetup1((w0 >> 16) & 0xFF, (s16)w0, (s16)(w1 >> 16), (s16)w1);
                break;

            case A_ENVMIXER:
                OotPspMixer_EnvMixer(((w0 >> 16) & 0xFF) << 4, (w0 >> 8) & 0xFF, (w0 >> 4) & 1,
                                      (w0 >> 3) & 1, (w0 >> 2) & 1, (w0 >> 1) & 1, w0 & 1, w1,
                                      A_ENVMIXER << 24);
                break;

            case A_LOADBUFF:
                OotPspMixer_LoadBuffer((const void*)(uintptr_t)w1, w0 & 0xFFFF, ((w0 >> 16) & 0xFF) << 4);
                break;

            case OOT_PSP_A_LOAD_SAMPLE_CACHED:
                OotPspMixer_LoadSampleCached((const void*)(uintptr_t)w1, w0 & 0xFFFF,
                                             ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_SAVEBUFF:
                OotPspMixer_SaveBuffer(w0 & 0xFFFF, (void*)(uintptr_t)w1, ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_ENVSETUP2:
                OotPspMixer_EnvSetup2((u16)(w1 >> 16), (u16)w1);
                break;

            case A_S8DEC:
                OotPspMixer_S8Dec((w0 >> 16) & 0xFF, (s16*)(uintptr_t)w1);
                break;

            case A_UNK19:
                OotPspMixer_UnkCmd19((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, w0 & 0xFFFF, (w0 >> 16) & 0xFF);
                break;

            default:
                break;
        }
    }
}

void OotPspMixer_ExecuteCommandList(const Acmd* cmdList, s32 cmdCount) {
    sCurrentMixer = &sCpuMixer;
    sExecutingOnMe = false;
    OotPspMixer_ExecuteCommandListInternal(cmdList, cmdCount);
}

void OotPspMixer_ExecuteCommandListMe(const Acmd* cmdList, s32 cmdCount) {
    sCurrentMixer = (sMeStorage != NULL) ? &sMeStorage->mixer : &sCpuMixer;
    sExecutingOnMe = true;
    OotPspMixer_ExecuteCommandListInternal(cmdList, cmdCount);
}

void OotPspMixer_InvalidateStateCache(void) {
    /* The CPU fallback state is no longer shared with the ME-local mixer state. */
}
