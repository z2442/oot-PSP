/**
 * Original Filename: system.c
 */

#include "alignment.h"
#include "array_count.h"
#include "attributes.h"
#include "buffers.h"
#include "segment_symbols.h"
#include "ultra64.h"
#include "versions.h"
#include "audio.h"
#if defined(TARGET_PSP)
#include "oot_psp_asset_loader.h"
#endif

#define MK_ASYNC_MSG(retData, tableType, id, loadStatus) \
    (((retData) << 24) | ((tableType) << 16) | ((id) << 8) | (loadStatus))
#define ASYNC_TBLTYPE(v) ((u8)(v >> 16))
#define ASYNC_ID(v) ((u8)(v >> 8))
#define ASYNC_LOAD_STATUS(v) ((u8)(v >> 0))
#if defined(TARGET_PSP)
#define AUDIOLOAD_SYNC_DMA_CHUNK_SIZE 0x4000
#else
#define AUDIOLOAD_SYNC_DMA_CHUNK_SIZE 0x400
#endif

typedef enum SlowLoadState {
    /* 0 */ SLOW_LOAD_STATE_WAITING,
    /* 1 */ SLOW_LOAD_STATE_START,
    /* 2 */ SLOW_LOAD_STATE_LOADING,
    /* 3 */ SLOW_LOAD_STATE_DONE
} SlowLoadState;

typedef struct SampleBankRelocInfo {
    /* 0x00 */ s32 sampleBankId1;
    /* 0x04 */ s32 sampleBankId2;
    /* 0x08 */ s32 baseAddr1;
    /* 0x0C */ s32 baseAddr2;
    /* 0x10 */ u32 medium1;
    /* 0x14 */ u32 medium2;
} SampleBankRelocInfo; // size = 0x18

// opaque type for soundfont data loaded into ram (should maybe get rid of this?)
typedef void SoundFontData;

/* forward declarations */
s32 AudioLoad_SyncInitSeqPlayerInternal(s32 playerIdx, s32 seqId, s32 arg2);
SoundFontData* AudioLoad_SyncLoadFont(u32 fontId);
Sample* AudioLoad_GetFontSample(s32 fontId, s32 instId);
void AudioLoad_ProcessAsyncLoads(s32 resetStatus);
void AudioLoad_ProcessAsyncLoadUnkMedium(AudioAsyncLoad* asyncLoad, s32 resetStatus);
void AudioLoad_ProcessAsyncLoad(AudioAsyncLoad* asyncLoad, s32 resetStatus);
void AudioLoad_RelocateFontAndPreloadSamples(s32 fontId, SoundFontData* fontData, SampleBankRelocInfo* sampleBankReloc,
                                             s32 isAsync);
void AudioLoad_RelocateSample(TunedSample* tunedSample, SoundFontData* fontData, SampleBankRelocInfo* sampleBankReloc);
void AudioLoad_DiscardFont(s32 fontId);
u32 AudioLoad_TrySyncLoadSampleBank(u32 sampleBankId, u32* outMedium, s32 noLoad);
void* AudioLoad_SyncLoad(u32 tableType, u32 id, s32* didAllocate);
u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 id);
void* AudioLoad_SearchCaches(s32 tableType, s32 id);
AudioTable* AudioLoad_GetLoadTable(s32 tableType);
void AudioLoad_SyncDma(u32 devAddr, u8* ramAddr, u32 size, s32 medium);
void AudioLoad_SyncDmaUnkMedium(u32 devAddr, u8* addr, u32 size, s32 unkMediumParam);
s32 AudioLoad_Dma(OSIoMesg* mesg, u32 priority, s32 direction, u32 devAddr, void* ramAddr, u32 size,
                  OSMesgQueue* reqQueue, s32 medium, const char* dmaFuncType);
void* AudioLoad_AsyncLoadInner(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue);
AudioAsyncLoad* AudioLoad_StartAsyncLoadUnkMedium(s32 unkMediumParam, u32 devAddr, void* ramAddr, s32 size, s32 medium,
                                                  s32 nChunks, OSMesgQueue* retQueue, s32 retMsg);
AudioAsyncLoad* AudioLoad_StartAsyncLoad(u32 devAddr, void* ramAddr, u32 size, s32 medium, s32 nChunks,
                                         OSMesgQueue* retQueue, s32 retMsg);
void AudioLoad_AsyncDma(AudioAsyncLoad* asyncLoad, u32 size);
void AudioLoad_AsyncDmaUnkMedium(u32 devAddr, void* ramAddr, u32 size, s16 arg3);
u8* AudioLoad_SyncLoadSeq(s32 seqId);
s32 AudioLoad_ProcessSamplePreloads(s32 resetStatus);
void AudioLoad_DmaSlowCopy(AudioSlowLoad* slowLoad, s32 size);
void AudioLoad_ProcessSlowLoads(s32 resetStatus);
void AudioLoad_DmaSlowCopyUnkMedium(s32 devAddr, u8* ramAddr, s32 size, s32 arg3);

OSMesgQueue sScriptLoadQueue;
OSMesg sScriptLoadMsgBuf[16];
s8* sScriptLoadDonePointers[0x10];
s32 sAudioLoadPad1[2]; // file padding

s32 D_8016B780;
s32 sAudioLoadPad2[4]; // double file padding?

DmaHandler sDmaHandler = osEPiStartDma;
void* sUnusedHandler = NULL;

s32 gAudioContextInitialized = false;

#if defined(TARGET_PSP)
#define OOT_PSP_AUDIO_NATIVE_PTR_START 0x08000000U
#define OOT_PSP_AUDIO_NATIVE_PTR_END   0x0C000000U
#define OOT_PSP_AUDIO_SWAP_CACHE_SIZE 4096

static void* sOotPspAudioSwapCache[OOT_PSP_AUDIO_SWAP_CACHE_SIZE];
static u32 sOotPspAudioSwapCacheCount;
static u8 sOotPspAudioBadSampleLookupLogged;

static s32 OotPspAudio_IsAlignedNativePtr(const void* ptr) {
    u32 addr = (u32)ptr;

    return (addr >= OOT_PSP_AUDIO_NATIVE_PTR_START) && (addr < OOT_PSP_AUDIO_NATIVE_PTR_END) && ((addr & 3) == 0);
}

static void* OotPspAudio_GetResidentSampleBank(u32 sampleBankId) {
    AudioTable* sampleBankTable = gAudioCtx.sampleBankTable;
    AudioTableEntry* entry;
    const void* ramAddr;

    if (!OotPspAudio_IsAlignedNativePtr(sampleBankTable) ||
        (sampleBankId >= (u32)sampleBankTable->header.numEntries)) {
        return NULL;
    }

    entry = &sampleBankTable->entries[sampleBankId];
    if (entry->size == 0) {
        return NULL;
    }

    ramAddr = OotPsp_GetCachedAssetPointer(entry->romAddr, entry->size);
    return (void*)ramAddr;
}

static void OotPspAudio_LogBadSampleLookup(s32 fontId, s32 instId) {
    if (!sOotPspAudioBadSampleLookupLogged) {
        sOotPspAudioBadSampleLookupLogged = true;
        osSyncPrintf("oot-psp audio skipped bad sample lookup font=%d inst=%d\n", fontId, instId);
    }
}

static void OotPspAudio_LogBadSamplePtr(s32 fontId, s32 instId, const Sample* sample) {
    if (!sOotPspAudioBadSampleLookupLogged) {
        sOotPspAudioBadSampleLookupLogged = true;
        osSyncPrintf("oot-psp audio skipped bad sample ptr font=%d inst=%d sample=%p\n", fontId, instId, sample);
    }
}

static s32 OotPspAudio_IsSafeSamplePtr(const Sample* sample) {
    return OotPspAudio_IsAlignedNativePtr(sample);
}

static s32 OotPspAudio_IsSafeSlowLoadSample(const Sample* sample) {
    if (!OotPspAudio_IsSafeSamplePtr(sample)) {
        return false;
    }

    if ((sample->codec > CODEC_S16) || (sample->medium > MEDIUM_DISK_DRIVE)) {
        return false;
    }

    if ((sample->medium != MEDIUM_RAM) && (sample->sampleAddr == NULL)) {
        return false;
    }

    return true;
}

static u16 OotPspAudio_Bswap16(u16 value) {
    return (value << 8) | (value >> 8);
}

static u32 OotPspAudio_Bswap32(u32 value) {
    return ((value & 0x000000FF) << 24) | ((value & 0x0000FF00) << 8) | ((value & 0x00FF0000) >> 8) |
           ((value & 0xFF000000) >> 24);
}

static s16 OotPspAudio_BswapS16(s16 value) {
    return (s16)OotPspAudio_Bswap16((u16)value);
}

static u32 OotPspAudio_ReadBE32(const void* ptr) {
    return OotPspAudio_Bswap32(*(u32*)ptr);
}

static f32 OotPspAudio_BswapF32(f32 value) {
    union {
        u32 u;
        f32 f;
    } bits;

    bits.f = value;
    bits.u = OotPspAudio_Bswap32(bits.u);
    return bits.f;
}

static void OotPspAudio_ResetSwapCache(void) {
    sOotPspAudioSwapCacheCount = 0;
}

static s32 OotPspAudio_MarkConverted(void* ptr) {
    u32 i;

    if (ptr == NULL) {
        return false;
    }

    for (i = 0; i < sOotPspAudioSwapCacheCount; i++) {
        if (sOotPspAudioSwapCache[i] == ptr) {
            return false;
        }
    }

    if (sOotPspAudioSwapCacheCount < OOT_PSP_AUDIO_SWAP_CACHE_SIZE) {
        sOotPspAudioSwapCache[sOotPspAudioSwapCacheCount++] = ptr;
    }
    return true;
}

static void OotPspAudio_SwapEnvelope(EnvelopePoint* envelope) {
    s32 i;

    if (!OotPspAudio_MarkConverted(envelope)) {
        return;
    }

    for (i = 0; i < 256; i++) {
        s16 delay = OotPspAudio_BswapS16(envelope[i].delay);

        envelope[i].delay = delay;
        envelope[i].arg = OotPspAudio_BswapS16(envelope[i].arg);

        if ((delay == ADSR_DISABLE) || (delay == ADSR_HANG)) {
            break;
        }
    }
}

static void OotPspAudio_SwapLoop(AdpcmLoop* loop) {
    s32 i;

    if (!OotPspAudio_MarkConverted(loop)) {
        return;
    }

    loop->header.start = OotPspAudio_Bswap32(loop->header.start);
    loop->header.end = OotPspAudio_Bswap32(loop->header.end);
    loop->header.count = OotPspAudio_Bswap32(loop->header.count);

    if (loop->header.count != 0) {
        for (i = 0; i < ARRAY_COUNT(loop->predictorState); i++) {
            loop->predictorState[i] = OotPspAudio_BswapS16(loop->predictorState[i]);
        }
    }
}

static void OotPspAudio_SwapBook(AdpcmBook* book) {
    s32 i;
    s32 count;

    if (!OotPspAudio_MarkConverted(book)) {
        return;
    }

    book->header.order = OotPspAudio_Bswap32(book->header.order);
    book->header.numPredictors = OotPspAudio_Bswap32(book->header.numPredictors);
    count = 8 * book->header.order * book->header.numPredictors;

    for (i = 0; i < count; i++) {
        book->book[i] = OotPspAudio_BswapS16(book->book[i]);
    }
}

static void OotPspAudio_SwapSample(Sample* sample) {
    u32 packed;

    if ((sample == NULL) || !OotPspAudio_MarkConverted(sample)) {
        return;
    }

    packed = OotPspAudio_ReadBE32(sample);
    sample->codec = (packed >> 28) & 0xF;
    sample->medium = (packed >> 26) & 3;
    sample->unk_bit26 = (packed >> 25) & 1;
    sample->isRelocated = (packed >> 24) & 1;
    sample->size = packed & 0xFFFFFF;
    sample->sampleAddr = (u8*)OotPspAudio_ReadBE32(&sample->sampleAddr);
    sample->loop = (AdpcmLoop*)OotPspAudio_ReadBE32(&sample->loop);
    sample->book = (AdpcmBook*)OotPspAudio_ReadBE32(&sample->book);
}

static void OotPspAudio_SwapTunedSample(TunedSample* tunedSample) {
    tunedSample->sample = (Sample*)OotPspAudio_ReadBE32(&tunedSample->sample);
    tunedSample->tuning = OotPspAudio_BswapF32(tunedSample->tuning);
}

static void OotPspAudio_SwapDrum(Drum* drum) {
    OotPspAudio_SwapTunedSample(&drum->tunedSample);
    drum->envelope = (EnvelopePoint*)OotPspAudio_ReadBE32(&drum->envelope);
}

static void OotPspAudio_SwapSoundEffect(SoundEffect* soundEffect) {
    OotPspAudio_SwapTunedSample(&soundEffect->tunedSample);
}

static void OotPspAudio_SwapInstrument(Instrument* inst) {
    inst->envelope = (EnvelopePoint*)OotPspAudio_ReadBE32(&inst->envelope);
    OotPspAudio_SwapTunedSample(&inst->lowPitchTunedSample);
    OotPspAudio_SwapTunedSample(&inst->normalPitchTunedSample);
    OotPspAudio_SwapTunedSample(&inst->highPitchTunedSample);
}
#endif

/**
 * original name: Nas_WaveDmaFrameWork
 */
void AudioLoad_DecreaseSampleDmaTtls(void) {
    u32 i;

    for (i = 0; i < gAudioCtx.sampleDmaListSize1; i++) {
        SampleDma* dma = &gAudioCtx.sampleDmas[i];

        if (dma->ttl != 0) {
            dma->ttl--;
            if (dma->ttl == 0) {
                dma->reuseIndex = gAudioCtx.sampleDmaReuseQueue1WrPos;
                gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1WrPos] = i;
                gAudioCtx.sampleDmaReuseQueue1WrPos++;
            }
        }
    }

    for (i = gAudioCtx.sampleDmaListSize1; i < gAudioCtx.sampleDmaCount; i++) {
        SampleDma* dma = &gAudioCtx.sampleDmas[i];

        if (dma->ttl != 0) {
            dma->ttl--;
            if (dma->ttl == 0) {
                dma->reuseIndex = gAudioCtx.sampleDmaReuseQueue2WrPos;
                gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2WrPos] = i;
                gAudioCtx.sampleDmaReuseQueue2WrPos++;
            }
        }
    }

    gAudioCtx.unused2628 = 0;
}

/**
 * original name: Nas_WaveDmaCallBack
 */
void* AudioLoad_DmaSampleData(u32 devAddr, u32 size, s32 arg2, u8* dmaIndexRef, s32 medium) {
    s32 pad1;
    SampleDma* dma;
    s32 hasDma = false;
    u32 dmaDevAddr;
    u32 pad2;
    u32 dmaIndex;
    u32 transfer;
    s32 bufferPos;
    u32 i;

    if (arg2 != 0 || *dmaIndexRef >= gAudioCtx.sampleDmaListSize1) {
        for (i = gAudioCtx.sampleDmaListSize1; i < gAudioCtx.sampleDmaCount; i++) {
            dma = &gAudioCtx.sampleDmas[i];
            bufferPos = devAddr - dma->devAddr;
            if (0 <= bufferPos && (u32)bufferPos <= dma->size - size) {
                // We already have a DMA request for this memory range.
                if (dma->ttl == 0 && gAudioCtx.sampleDmaReuseQueue2RdPos != gAudioCtx.sampleDmaReuseQueue2WrPos) {
                    // Move the DMA out of the reuse queue, by swapping it with the
                    // read pos, and then incrementing the read pos.
                    if (dma->reuseIndex != gAudioCtx.sampleDmaReuseQueue2RdPos) {
                        gAudioCtx.sampleDmaReuseQueue2[dma->reuseIndex] =
                            gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos];
                        gAudioCtx.sampleDmas[gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos]]
                            .reuseIndex = dma->reuseIndex;
                    }
                    gAudioCtx.sampleDmaReuseQueue2RdPos++;
                }
                dma->ttl = 32;
                *dmaIndexRef = (u8)i;
                return &dma->ramAddr[devAddr - dma->devAddr];
            }
        }

        if (arg2 == 0) {
            goto search_short_lived;
        }

        if (gAudioCtx.sampleDmaReuseQueue2RdPos != gAudioCtx.sampleDmaReuseQueue2WrPos && arg2 != 0) {
            // Allocate a DMA from reuse queue 2, unless full.
            dmaIndex = gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos];
            gAudioCtx.sampleDmaReuseQueue2RdPos++;
            dma = gAudioCtx.sampleDmas + dmaIndex;
            hasDma = true;
        }
    } else {
    search_short_lived:
        dma = gAudioCtx.sampleDmas + *dmaIndexRef;
        i = 0;
    again:
        bufferPos = devAddr - dma->devAddr;
        if (0 <= bufferPos && (u32)bufferPos <= dma->size - size) {
            // We already have DMA for this memory range.
            if (dma->ttl == 0) {
                // Move the DMA out of the reuse queue, by swapping it with the
                // read pos, and then incrementing the read pos.
                if (dma->reuseIndex != gAudioCtx.sampleDmaReuseQueue1RdPos) {
                    gAudioCtx.sampleDmaReuseQueue1[dma->reuseIndex] =
                        gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos];
                    gAudioCtx.sampleDmas[gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos]]
                        .reuseIndex = dma->reuseIndex;
                }
                gAudioCtx.sampleDmaReuseQueue1RdPos++;
            }
            dma->ttl = 2;
            return dma->ramAddr + (devAddr - dma->devAddr);
        }
        dma = gAudioCtx.sampleDmas + i++;
        if (i <= gAudioCtx.sampleDmaListSize1) {
            goto again;
        }
    }

    if (!hasDma) {
        if (gAudioCtx.sampleDmaReuseQueue1RdPos == gAudioCtx.sampleDmaReuseQueue1WrPos) {
            return NULL;
        }
        // Allocate a DMA from reuse queue 1.
        dmaIndex = gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos++];
        dma = gAudioCtx.sampleDmas + dmaIndex;
        hasDma = true;
    }

    transfer = dma->size;
    dmaDevAddr = devAddr & ~0xF;
    dma->ttl = 3;
    dma->devAddr = dmaDevAddr;
    dma->sizeUnused = transfer;
    AudioLoad_Dma(&gAudioCtx.curAudioFrameDmaIoMsgBuf[gAudioCtx.curAudioFrameDmaCount++], OS_MESG_PRI_NORMAL, OS_READ,
                  dmaDevAddr, dma->ramAddr, transfer, &gAudioCtx.curAudioFrameDmaQueue, medium, "SUPERDMA");
    *dmaIndexRef = dmaIndex;
    return (devAddr - dmaDevAddr) + dma->ramAddr;
}

/**
 * original name: Nas_WaveDmaNew
 */
void AudioLoad_InitSampleDmaBuffers(s32 numNotes) {
    SampleDma* dma;
    s32 i;
    s32 t2;
    s32 j;

    gAudioCtx.sampleDmaBufSize = gAudioCtx.sampleDmaBufSize1;
    gAudioCtx.sampleDmas = AudioHeap_Alloc(&gAudioCtx.miscPool, 4 * gAudioCtx.numNotes * sizeof(SampleDma) *
                                                                    gAudioCtx.audioBufferParameters.specUnk4);
    t2 = 3 * gAudioCtx.numNotes * gAudioCtx.audioBufferParameters.specUnk4;
    for (i = 0; i < t2; i++) {
        dma = &gAudioCtx.sampleDmas[gAudioCtx.sampleDmaCount];
        dma->ramAddr = AudioHeap_AllocAttemptExternal(&gAudioCtx.miscPool, gAudioCtx.sampleDmaBufSize);
        if (dma->ramAddr == NULL) {
            break;
        } else {
            AudioHeap_WritebackDCache(dma->ramAddr, gAudioCtx.sampleDmaBufSize);
            dma->size = gAudioCtx.sampleDmaBufSize;
            dma->devAddr = 0;
            dma->sizeUnused = 0;
            dma->unused = 0;
            dma->ttl = 0;
            gAudioCtx.sampleDmaCount++;
        }
    }

    for (i = 0; (u32)i < gAudioCtx.sampleDmaCount; i++) {
        gAudioCtx.sampleDmaReuseQueue1[i] = i;
        gAudioCtx.sampleDmas[i].reuseIndex = i;
    }

    for (i = gAudioCtx.sampleDmaCount; i < 0x100; i++) {
        gAudioCtx.sampleDmaReuseQueue1[i] = 0;
    }

    gAudioCtx.sampleDmaReuseQueue1RdPos = 0;
    gAudioCtx.sampleDmaReuseQueue1WrPos = gAudioCtx.sampleDmaCount;
    gAudioCtx.sampleDmaListSize1 = gAudioCtx.sampleDmaCount;
    gAudioCtx.sampleDmaBufSize = gAudioCtx.sampleDmaBufSize2;

    for (j = 0; j < gAudioCtx.numNotes; j++) {
        dma = &gAudioCtx.sampleDmas[gAudioCtx.sampleDmaCount];
        dma->ramAddr = AudioHeap_AllocAttemptExternal(&gAudioCtx.miscPool, gAudioCtx.sampleDmaBufSize);
        if (dma->ramAddr == NULL) {
            break;
        } else {
            AudioHeap_WritebackDCache(dma->ramAddr, gAudioCtx.sampleDmaBufSize);
            dma->size = gAudioCtx.sampleDmaBufSize;
            dma->devAddr = 0U;
            dma->sizeUnused = 0;
            dma->unused = 0;
            dma->ttl = 0;
            gAudioCtx.sampleDmaCount++;
        }
    }

    for (i = gAudioCtx.sampleDmaListSize1; (u32)i < gAudioCtx.sampleDmaCount; i++) {
        gAudioCtx.sampleDmaReuseQueue2[i - gAudioCtx.sampleDmaListSize1] = i;
        gAudioCtx.sampleDmas[i].reuseIndex = i - gAudioCtx.sampleDmaListSize1;
    }

    for (i = gAudioCtx.sampleDmaCount; i < 0x100; i++) {
        gAudioCtx.sampleDmaReuseQueue2[i] = gAudioCtx.sampleDmaListSize1;
    }

    gAudioCtx.sampleDmaReuseQueue2RdPos = 0;
    gAudioCtx.sampleDmaReuseQueue2WrPos = gAudioCtx.sampleDmaCount - gAudioCtx.sampleDmaListSize1;
}

/**
 * original name: Nas_CheckIDbank
 */
s32 AudioLoad_IsFontLoadComplete(s32 fontId) {
    if (fontId == 0xFF) {
        return true;
#if defined(TARGET_PSP)
    } else if ((fontId < 0) || (fontId >= 0x30) || !OotPspAudio_IsAlignedNativePtr(gAudioCtx.soundFontTable) ||
               ((u32)fontId >= (u32)gAudioCtx.soundFontTable->header.numEntries)) {
        return false;
#endif
    } else if (gAudioCtx.fontLoadStatus[fontId] >= LOAD_STATUS_COMPLETE) {
        return true;
    } else if (gAudioCtx.fontLoadStatus[AudioLoad_GetRealTableIndex(FONT_TABLE, fontId)] >= LOAD_STATUS_COMPLETE) {
        return true;
    } else {
        return false;
    }
}

/**
 * original name: Nas_CheckIDseq
 */
s32 AudioLoad_IsSeqLoadComplete(s32 seqId) {
    if (seqId == 0xFF) {
        return true;
#if defined(TARGET_PSP)
    } else if ((seqId < 0) || (seqId >= 0x80) || !OotPspAudio_IsAlignedNativePtr(gAudioCtx.sequenceTable) ||
               ((u32)seqId >= (u32)gAudioCtx.sequenceTable->header.numEntries)) {
        return false;
#endif
    } else if (gAudioCtx.seqLoadStatus[seqId] >= LOAD_STATUS_COMPLETE) {
        return true;
    } else if (gAudioCtx.seqLoadStatus[AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId)] >= LOAD_STATUS_COMPLETE) {
        return true;
    } else {
        return false;
    }
}

/**
 * original name: Nas_CheckIDwave
 */
s32 AudioLoad_IsSampleLoadComplete(s32 sampleBankId) {
    if (sampleBankId == 0xFF) {
        return true;
#if defined(TARGET_PSP)
    } else if ((sampleBankId < 0) || (sampleBankId >= 0x30) ||
               !OotPspAudio_IsAlignedNativePtr(gAudioCtx.sampleBankTable) ||
               ((u32)sampleBankId >= (u32)gAudioCtx.sampleBankTable->header.numEntries)) {
        return false;
#endif
    } else if (gAudioCtx.sampleFontLoadStatus[sampleBankId] >= LOAD_STATUS_COMPLETE) {
        return true;
    } else if (gAudioCtx.sampleFontLoadStatus[AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankId)] >=
               LOAD_STATUS_COMPLETE) {
        return true;
    } else {
        return false;
    }
}

/**
 * original name: Nas_WriteIDbank
 */
void AudioLoad_SetFontLoadStatus(s32 fontId, s32 loadStatus) {
#if defined(TARGET_PSP)
    if ((fontId < 0) || (fontId >= 0x30)) {
        return;
    }
#endif
    if ((fontId != 0xFF) && (gAudioCtx.fontLoadStatus[fontId] != LOAD_STATUS_PERMANENTLY_LOADED)) {
        gAudioCtx.fontLoadStatus[fontId] = loadStatus;
    }
}

/**
 * original name: Nas_WriteIDseq
 */
void AudioLoad_SetSeqLoadStatus(s32 seqId, s32 loadStatus) {
#if defined(TARGET_PSP)
    if ((seqId < 0) || (seqId >= 0x80)) {
        return;
    }
#endif
    if ((seqId != 0xFF) && (gAudioCtx.seqLoadStatus[seqId] != LOAD_STATUS_PERMANENTLY_LOADED)) {
        gAudioCtx.seqLoadStatus[seqId] = loadStatus;
    }
}

/**
 * original name: Nas_WriteIDwave
 */
void AudioLoad_SetSampleFontLoadStatusAndApplyCaches(s32 sampleBankId, s32 loadStatus) {
#if defined(TARGET_PSP)
    if ((sampleBankId < 0) || (sampleBankId >= 0x30)) {
        return;
    }
#endif
    if (sampleBankId != 0xFF) {
        if (gAudioCtx.sampleFontLoadStatus[sampleBankId] != LOAD_STATUS_PERMANENTLY_LOADED) {
            gAudioCtx.sampleFontLoadStatus[sampleBankId] = loadStatus;
        }

        if ((gAudioCtx.sampleFontLoadStatus[sampleBankId] == LOAD_STATUS_PERMANENTLY_LOADED) ||
            (gAudioCtx.sampleFontLoadStatus[sampleBankId] == LOAD_STATUS_COMPLETE)) {
            AudioHeap_ApplySampleBankCache(sampleBankId);
        }
    }
}

/**
 * original name: Nas_WriteIDwaveOnly
 */
void AudioLoad_SetSampleFontLoadStatus(s32 sampleBankId, s32 loadStatus) {
#if defined(TARGET_PSP)
    if ((sampleBankId < 0) || (sampleBankId >= 0x30)) {
        return;
    }
#endif
    if ((sampleBankId != 0xFF) && (gAudioCtx.sampleFontLoadStatus[sampleBankId] != LOAD_STATUS_PERMANENTLY_LOADED)) {
        gAudioCtx.sampleFontLoadStatus[sampleBankId] = loadStatus;
    }
}

/**
 * original name: Nas_BankHeaderInit
 */
void AudioLoad_InitTable(AudioTable* table, u32 romAddr, u16 unkMediumParam) {
    s32 i;

    table->header.unkMediumParam = unkMediumParam;
    table->header.romAddr = romAddr;

    for (i = 0; i < table->header.numEntries; i++) {
        if ((table->entries[i].size != 0) && (table->entries[i].medium == MEDIUM_CART)) {
            table->entries[i].romAddr += romAddr;
        }
    }
}

/**
 * original name: Nas_PreLoadBank
 */
SoundFontData* AudioLoad_SyncLoadSeqFonts(s32 seqId, u32* outDefaultFontId) {
    s32 pad[2];
    s32 index;
    SoundFontData* fontData;
    s32 numFonts;
    s32 fontId;
    s32 i;

    if (seqId >= gAudioCtx.numSequences) {
        return NULL;
    }

    fontId = 0xFF;
    index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    numFonts = gAudioCtx.sequenceFontTable[index++];

    while (numFonts > 0) {
        fontId = gAudioCtx.sequenceFontTable[index++];
        fontData = AudioLoad_SyncLoadFont(fontId);
        numFonts--;
    }

    *outDefaultFontId = fontId;
    return fontData;
}

/**
 * original name: Nas_PreLoadSeq
 */
void AudioLoad_SyncLoadSeqParts(s32 seqId, s32 arg1) {
    s32 pad;
    u32 defaultFontId;

    if (seqId < gAudioCtx.numSequences) {
        if (arg1 & 2) {
            AudioLoad_SyncLoadSeqFonts(seqId, &defaultFontId);
        }
        if (arg1 & 1) {
            AudioLoad_SyncLoadSeq(seqId);
        }
    }
}

/**
 * original name: __Nas_LoadVoice_Inner
 */
s32 AudioLoad_SyncLoadSample(Sample* sample, s32 fontId) {
    void* sampleAddr;

    if (sample == NULL) {
        return -1;
    }

    if (sample->isRelocated == true) {
        if (sample->medium != MEDIUM_RAM) {
            sampleAddr = AudioHeap_AllocSampleCache(sample->size, fontId, (void*)sample->sampleAddr, sample->medium,
                                                    CACHE_PERSISTENT);
            if (sampleAddr == NULL) {
                return -1;
            }

            if (sample->medium == MEDIUM_UNK) {
                AudioLoad_SyncDmaUnkMedium((u32)sample->sampleAddr, sampleAddr, sample->size,
                                           gAudioCtx.sampleBankTable->header.unkMediumParam);
            } else {
                AudioLoad_SyncDma((u32)sample->sampleAddr, sampleAddr, sample->size, sample->medium);
            }
            sample->medium = MEDIUM_RAM;
            sample->sampleAddr = sampleAddr;
        }
    }
    //! @bug Missing return, but the return value is never used so it's fine.
}

/**
 * original name: Nas_LoadVoice
 */
s32 AudioLoad_SyncLoadInstrument(s32 fontId, s32 instId, s32 drumId) {
    if (instId < 0x7F) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, instId);

        if (instrument == NULL) {
            return -1;
        }
        if (instrument->normalRangeLo != 0) {
            AudioLoad_SyncLoadSample(instrument->lowPitchTunedSample.sample, fontId);
        }
        AudioLoad_SyncLoadSample(instrument->normalPitchTunedSample.sample, fontId);
        if (instrument->normalRangeHi != 0x7F) {
            return AudioLoad_SyncLoadSample(instrument->highPitchTunedSample.sample, fontId);
        }
        //! @bug Missing return, but the return value is never used so it's fine.
    } else if (instId == 0x7F) {
        Drum* drum = Audio_GetDrum(fontId, drumId);

        if (drum == NULL) {
            return -1;
        }
        AudioLoad_SyncLoadSample(drum->tunedSample.sample, fontId);
        return 0;
    }
}

/**
 * original name: Nas_PreLoad_BG
 */
void AudioLoad_AsyncLoad(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue) {
    if (AudioLoad_AsyncLoadInner(tableType, id, nChunks, retData, retQueue) == NULL) {
        osSendMesg(retQueue, (OSMesg)0xFFFFFFFF, OS_MESG_NOBLOCK);
    }
}

/**
 * original name: Nas_PreLoadSeq_BG
 */
void AudioLoad_AsyncLoadSeq(s32 seqId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(SEQUENCE_TABLE, seqId, 0, retData, retQueue);
}

/**
 * original name: Nas_PreLoadWave_BG
 */
void AudioLoad_AsyncLoadSampleBank(s32 sampleBankId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(SAMPLE_TABLE, sampleBankId, 0, retData, retQueue);
}

/**
 * original name: Nas_PreLoadBank_BG
 */
void AudioLoad_AsyncLoadFont(s32 fontId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(FONT_TABLE, fontId, 0, retData, retQueue);
}

/**
 * original name: Nas_SeqToBank
 */
u8* AudioLoad_GetFontsForSequence(s32 seqId, u32* outNumFonts) {
    s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];

    *outNumFonts = gAudioCtx.sequenceFontTable[index++];
    if (*outNumFonts == 0) {
        return NULL;
    }
    return &gAudioCtx.sequenceFontTable[index];
}

/**
 * original name: Nas_FlushBank
 */
void AudioLoad_DiscardSeqFonts(s32 seqId) {
    s32 fontId;
    s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    s32 numFonts = gAudioCtx.sequenceFontTable[index++];

    while (numFonts > 0) {
        numFonts--;
        fontId = AudioLoad_GetRealTableIndex(FONT_TABLE, gAudioCtx.sequenceFontTable[index++]);
        if (AudioHeap_SearchPermanentCache(FONT_TABLE, fontId) == NULL) {
            AudioLoad_DiscardFont(fontId);
            AudioLoad_SetFontLoadStatus(fontId, LOAD_STATUS_NOT_LOADED);
        }
    }
}

/**
 * original name: __Kill_Bank
 */
void AudioLoad_DiscardFont(s32 fontId) {
    u32 i;
    AudioCache* pool = &gAudioCtx.fontCache;
    AudioPersistentCache* persistent;

    if (fontId == pool->temporary.entries[0].id) {
        pool->temporary.entries[0].id = -1;
    } else if (fontId == pool->temporary.entries[1].id) {
        pool->temporary.entries[1].id = -1;
    }

    persistent = &pool->persistent;
    for (i = 0; i < persistent->numEntries; i++) {
        if (fontId == persistent->entries[i].id) {
            persistent->entries[i].id = -1;
        }
    }

    AudioHeap_DiscardFont(fontId);
}

/**
 * original name: Nas_StartMySeq
 */
s32 AudioLoad_SyncInitSeqPlayer(s32 playerIdx, s32 seqId, s32 arg2) {
    if (gAudioCtx.resetTimer != 0) {
        return 0;
    }

    gAudioCtx.seqPlayers[playerIdx].skipTicks = 0;
    AudioLoad_SyncInitSeqPlayerInternal(playerIdx, seqId, arg2);
    //! @bug Missing return. Returning the result of the above function call
    //! matches but is UB because it too is missing a return, and using the
    //! result of a non-void function that has failed to return a value is UB.
    //! The callers of this function do not use the return value, so it's fine.
}

/**
 * original name: Nas_StartSeq_Skip
 */
s32 AudioLoad_SyncInitSeqPlayerSkipTicks(s32 playerIdx, s32 seqId, s32 skipTicks) {
    if (gAudioCtx.resetTimer != 0) {
        return 0;
    }

    gAudioCtx.seqPlayers[playerIdx].skipTicks = skipTicks;
    AudioLoad_SyncInitSeqPlayerInternal(playerIdx, seqId, 0);
    //! @bug Missing return, see comment in AudioLoad_SyncInitSeqPlayer above.
}

/**
 * original name: __Nas_StartSeq
 */
s32 AudioLoad_SyncInitSeqPlayerInternal(s32 playerIdx, s32 seqId, s32 arg2) {
    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[playerIdx];
    u8* seqData;
    s32 index;
    s32 numFonts;
    s32 fontId;

    if (seqId >= gAudioCtx.numSequences) {
        return 0;
    }

    AudioSeq_SequencePlayerDisable(seqPlayer);

    fontId = 0xFF;
    index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    numFonts = gAudioCtx.sequenceFontTable[index++];

    while (numFonts > 0) {
        fontId = gAudioCtx.sequenceFontTable[index++];
        AudioLoad_SyncLoadFont(fontId);
        numFonts--;
    }

    seqData = AudioLoad_SyncLoadSeq(seqId);
    if (seqData == NULL) {
        return 0;
    }

    AudioSeq_ResetSequencePlayer(seqPlayer);
    seqPlayer->seqId = seqId;
    seqPlayer->defaultFont = AudioLoad_GetRealTableIndex(FONT_TABLE, fontId);
    seqPlayer->seqData = seqData;
    seqPlayer->enabled = true;
    seqPlayer->scriptState.pc = seqData;
    seqPlayer->scriptState.depth = 0;
    seqPlayer->delay = 0;
    seqPlayer->finished = false;
    seqPlayer->playerIdx = playerIdx;
    AudioSeq_SkipForwardSequence(seqPlayer);
    //! @bug missing return (but the return value is not used so it's not UB)
}

/**
 * original name: __Load_Seq
 */
u8* AudioLoad_SyncLoadSeq(s32 seqId) {
    s32 pad;
    s32 didAllocate;

    if (gAudioCtx.seqLoadStatus[AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId)] == LOAD_STATUS_IN_PROGRESS) {
        return NULL;
    }

    return AudioLoad_SyncLoad(SEQUENCE_TABLE, seqId, &didAllocate);
}

/**
 * original name: __Load_Wave_Check
 */
u32 AudioLoad_GetSampleBank(u32 sampleBankId, u32* outMedium) {
    return AudioLoad_TrySyncLoadSampleBank(sampleBankId, outMedium, true);
}

/**
 * original name: __Load_Wave
 */
u32 AudioLoad_TrySyncLoadSampleBank(u32 sampleBankId, u32* outMedium, s32 noLoad) {
    void* ramAddr;
    AudioTable* sampleBankTable;
    u32 realTableId = AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankId);
    s8 cachePolicy;

    sampleBankTable = AudioLoad_GetLoadTable(SAMPLE_TABLE);
#if defined(TARGET_PSP)
    ramAddr = OotPspAudio_GetResidentSampleBank(realTableId);
    if (ramAddr != NULL) {
        AudioLoad_SetSampleFontLoadStatus(realTableId, LOAD_STATUS_COMPLETE);
        *outMedium = MEDIUM_RAM;
        return (u32)ramAddr;
    }
#endif
    ramAddr = AudioLoad_SearchCaches(SAMPLE_TABLE, realTableId);
    if (ramAddr != NULL) {
        if (gAudioCtx.sampleFontLoadStatus[realTableId] != LOAD_STATUS_IN_PROGRESS) {
            AudioLoad_SetSampleFontLoadStatus(realTableId, LOAD_STATUS_COMPLETE);
        }
        *outMedium = MEDIUM_RAM;
        return (u32)ramAddr;
    }

    cachePolicy = sampleBankTable->entries[sampleBankId].cachePolicy;
    if (cachePolicy == 4 || noLoad == true) {
        *outMedium = sampleBankTable->entries[sampleBankId].medium;
        return sampleBankTable->entries[realTableId].romAddr;
    }

    ramAddr = AudioLoad_SyncLoad(SAMPLE_TABLE, sampleBankId, &noLoad);
    if (ramAddr != NULL) {
        *outMedium = MEDIUM_RAM;
        return (u32)ramAddr;
    }

    *outMedium = sampleBankTable->entries[sampleBankId].medium;
    return sampleBankTable->entries[realTableId].romAddr;
}

/**
 * original name: __Load_Ctrl
 */
SoundFontData* AudioLoad_SyncLoadFont(u32 fontId) {
    SoundFontData* fontData;
    s32 sampleBankId1;
    s32 sampleBankId2;
    s32 didAllocate;
    SampleBankRelocInfo sampleBankReloc;
    s32 realFontId = AudioLoad_GetRealTableIndex(FONT_TABLE, fontId);

    if (gAudioCtx.fontLoadStatus[realFontId] == LOAD_STATUS_IN_PROGRESS) {
        return NULL;
    }
    sampleBankId1 = gAudioCtx.soundFontList[realFontId].sampleBankId1;
    sampleBankId2 = gAudioCtx.soundFontList[realFontId].sampleBankId2;

    sampleBankReloc.sampleBankId1 = sampleBankId1;
    sampleBankReloc.sampleBankId2 = sampleBankId2;
    if (sampleBankId1 != 0xFF) {
        sampleBankReloc.baseAddr1 = AudioLoad_TrySyncLoadSampleBank(sampleBankId1, &sampleBankReloc.medium1, false);
    } else {
        sampleBankReloc.baseAddr1 = 0;
    }

    if (sampleBankId2 != 0xFF) {
        sampleBankReloc.baseAddr2 = AudioLoad_TrySyncLoadSampleBank(sampleBankId2, &sampleBankReloc.medium2, false);
    } else {
        sampleBankReloc.baseAddr2 = 0;
    }

    fontData = AudioLoad_SyncLoad(FONT_TABLE, fontId, &didAllocate);
    if (fontData == NULL) {
        return NULL;
    }
    if (didAllocate == true) {
        AudioLoad_RelocateFontAndPreloadSamples(realFontId, fontData, &sampleBankReloc, false);
    }

    return fontData;
}

/**
 * original name: __Load_Bank
 */
void* AudioLoad_SyncLoad(u32 tableType, u32 id, s32* didAllocate) {
    u32 size;
    AudioTable* table;
    s32 pad;
    u32 medium;
    s32 loadStatus;
    u32 romAddr;
    s32 cachePolicy;
    void* ramAddr;
    u32 realId;

    realId = AudioLoad_GetRealTableIndex(tableType, id);
    ramAddr = AudioLoad_SearchCaches(tableType, realId);
    if (ramAddr != NULL) {
        *didAllocate = false;
        loadStatus = LOAD_STATUS_COMPLETE;
    } else {
        table = AudioLoad_GetLoadTable(tableType);
        size = table->entries[realId].size;
        size = ALIGN16(size);
        medium = table->entries[id].medium;
        cachePolicy = table->entries[id].cachePolicy;
        romAddr = table->entries[realId].romAddr;
        switch (cachePolicy) {
            case 0:
                ramAddr = AudioHeap_AllocPermanent(tableType, realId, size);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;

            case 1:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_PERSISTENT, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;

            case 2:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_TEMPORARY, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;

            case 3:
            case 4:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_EITHER, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;
        }

        *didAllocate = true;
        if (medium == MEDIUM_UNK) {
            AudioLoad_SyncDmaUnkMedium(romAddr, ramAddr, size, (s16)table->header.unkMediumParam);
        } else {
            AudioLoad_SyncDma(romAddr, ramAddr, size, medium);
        }

        loadStatus = (cachePolicy == 0) ? LOAD_STATUS_PERMANENTLY_LOADED : LOAD_STATUS_COMPLETE;
    }

    switch (tableType) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(realId, loadStatus);
            break;

        case FONT_TABLE:
            AudioLoad_SetFontLoadStatus(realId, loadStatus);
            break;

        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(realId, loadStatus);
            break;

        default:
            break;
    }

    return ramAddr;
}

/**
 * original name: __Link_BankNum
 */
u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 id) {
    AudioTable* table = AudioLoad_GetLoadTable(tableType);

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(table) || (id >= (u32)table->header.numEntries)) {
        return id;
    }
#endif

    if (table->entries[id].size == 0) {
        id = table->entries[id].romAddr;
    }

    return id;
}

/**
 * original name: __Check_Cache
 */
void* AudioLoad_SearchCaches(s32 tableType, s32 id) {
    void* ramAddr;

    ramAddr = AudioHeap_SearchPermanentCache(tableType, id);
    if (ramAddr != NULL) {
        return ramAddr;
    }

    ramAddr = AudioHeap_SearchCaches(tableType, CACHE_EITHER, id);
    if (ramAddr != NULL) {
        return ramAddr;
    }

    return NULL;
}

/**
 * Animal Crossing's equivalent to this function is __Get_ArcHeader.
 * This name must be new, because ARC files are GameCube speicifc.
 */
AudioTable* AudioLoad_GetLoadTable(s32 tableType) {
    AudioTable* table;

    switch (tableType) {
        case SEQUENCE_TABLE:
            table = gAudioCtx.sequenceTable;
            break;

        case FONT_TABLE:
            table = gAudioCtx.soundFontTable;
            break;

        default:
            table = NULL;
            break;

        case SAMPLE_TABLE:
            table = gAudioCtx.sampleBankTable;
            break;
    }
    return table;
}

/**
 * Read and extract information from soundFont binary loaded into ram.
 * Also relocate offsets into pointers within this loaded soundFont.
 *
 * original name: Nas_BankOfsToAddr_Inner
 *
 * @param fontId index of font being processed
 * @param fontDataStartAddr ram address of raw soundfont binary loaded into cache
 * @param sampleBankReloc information on the sampleBank containing raw audio samples
 */
void AudioLoad_RelocateFont(s32 fontId, SoundFontData* fontDataStartAddr, SampleBankRelocInfo* sampleBankReloc) {
    u32 soundOffset;     // Relative offset from the beginning of fontData directly to the tunedSample/envelope
    u32 soundListOffset; // Relative offset from the beginning of fontData to the list of soundOffsets/sfxs
    Instrument* inst;
    Drum* drum;
    SoundEffect* soundEffect;
    s32 i;
    s32 numDrums = gAudioCtx.soundFontList[fontId].numDrums;
    s32 numInstruments = gAudioCtx.soundFontList[fontId].numInstruments;
    s32 numSfx = gAudioCtx.soundFontList[fontId].numSfx;
    u32* fontData = (u32*)fontDataStartAddr;

#if defined(TARGET_PSP)
    OotPspAudio_ResetSwapCache();
#endif

    // Relocate an offset (relative to the start of the font data) to a pointer (a ram address)
#define RELOC_TO_RAM(offset) ((u32)(offset) + (u32)(fontDataStartAddr))

    // Drums relocation

    // The first u32 in fontData is an offset to a list of offsets to the drums
    soundListOffset =
#if defined(TARGET_PSP)
        OotPspAudio_Bswap32(fontData[0]);
#else
        fontData[0];
#endif

    // If the soundFont has drums
    if ((soundListOffset != 0) && (numDrums != 0)) {

        fontData[0] = RELOC_TO_RAM(soundListOffset);

        // Loop through the drum offsets
        for (i = 0; i < numDrums; i++) {
            // Get the i'th drum offset
            soundOffset =
#if defined(TARGET_PSP)
                OotPspAudio_ReadBE32(&((u32*)fontData[0])[i]);
#else
                (u32)((Drum**)fontData[0])[i];
#endif

            // Some drum data entries are empty, represented by an offset of 0 in the list of drum offsets
            if (soundOffset == 0) {
                continue;
            }

            soundOffset = RELOC_TO_RAM(soundOffset);
            ((Drum**)fontData[0])[i] = drum = (Drum*)soundOffset;

            // The drum may be in the list multiple times and already relocated
            if (drum->isRelocated) {
                continue;
            }

#if defined(TARGET_PSP)
            OotPspAudio_SwapDrum(drum);
#endif
            AudioLoad_RelocateSample(&drum->tunedSample, fontDataStartAddr, sampleBankReloc);

            soundOffset = (u32)drum->envelope;
            drum->envelope = (EnvelopePoint*)RELOC_TO_RAM(soundOffset);
#if defined(TARGET_PSP)
            OotPspAudio_SwapEnvelope(drum->envelope);
#endif

            drum->isRelocated = true;
        }
    }

    // Sound effects relocation

    // The second u32 in fontData is an offset to the first sound effect entry
    soundListOffset =
#if defined(TARGET_PSP)
        OotPspAudio_Bswap32(fontData[1]);
#else
        fontData[1];
#endif

    // If the soundFont has sound effects
    if ((soundListOffset != 0) && (numSfx != 0)) {

        fontData[1] = RELOC_TO_RAM(soundListOffset);

        // Loop through the sound effects
        for (i = 0; i < numSfx; i++) {
            // Get a pointer to the i'th sound effect
            soundOffset = (u32)(((SoundEffect*)fontData[1]) + i);
            soundEffect = (SoundEffect*)soundOffset;
#if defined(TARGET_PSP)
            OotPspAudio_SwapSoundEffect(soundEffect);
#endif

            // Check for NULL (note: the pointer is guaranteed to be in fontData and can never be NULL)
            if ((soundEffect == NULL) || ((u32)soundEffect->tunedSample.sample == 0)) {
                continue;
            }

            AudioLoad_RelocateSample(&soundEffect->tunedSample, fontDataStartAddr, sampleBankReloc);
        }
    }

    // Instruments relocation

    // Instrument Id 126 and above is reserved.
    // There can only be 126 instruments, indexed from 0 to 125
    if (numInstruments > 126) {
        numInstruments = 126;
    }

    // Starting from the 3rd u32 in fontData is the list of offsets to the instruments
    // Loop through the instruments
    for (i = 2; i <= 2 + numInstruments - 1; i++) {
        // Some instrument data entries are empty, represented by an offset of 0 in the list of instrument offsets
        soundOffset =
#if defined(TARGET_PSP)
            OotPspAudio_Bswap32(fontData[i]);
#else
            fontData[i];
#endif
        if (soundOffset != 0) {
            fontData[i] = RELOC_TO_RAM(soundOffset);
            inst = (Instrument*)fontData[i];

            // The instrument may be in the list multiple times and already relocated
            if (!inst->isRelocated) {
#if defined(TARGET_PSP)
                OotPspAudio_SwapInstrument(inst);
#endif
                // Some instruments have a different sample for low pitches
                if (inst->normalRangeLo != 0) {
                    AudioLoad_RelocateSample(&inst->lowPitchTunedSample, fontDataStartAddr, sampleBankReloc);
                }

                // Every instrument has a sample for the default range
                AudioLoad_RelocateSample(&inst->normalPitchTunedSample, fontDataStartAddr, sampleBankReloc);

                // Some instruments have a different sample for high pitches
                if (inst->normalRangeHi != 0x7F) {
                    AudioLoad_RelocateSample(&inst->highPitchTunedSample, fontDataStartAddr, sampleBankReloc);
                }

                soundOffset = (u32)inst->envelope;
                inst->envelope = (EnvelopePoint*)RELOC_TO_RAM(soundOffset);
#if defined(TARGET_PSP)
                OotPspAudio_SwapEnvelope(inst->envelope);
#endif

                inst->isRelocated = true;
            }
        }
    }

#undef FONT_DATA_RELOC

    // Store the relocated pointers
    gAudioCtx.soundFontList[fontId].drums = (Drum**)fontData[0];
    gAudioCtx.soundFontList[fontId].soundEffects = (SoundEffect*)fontData[1];
    gAudioCtx.soundFontList[fontId].instruments = (Instrument**)(fontData + 2);
}

/**
 * original name: Nas_FastCopy
 */
void AudioLoad_SyncDma(u32 devAddr, u8* ramAddr, u32 size, s32 medium) {
    OSMesgQueue* msgQueue = &gAudioCtx.syncDmaQueue;
    OSIoMesg* ioMesg = &gAudioCtx.syncDmaIoMesg;
    size = ALIGN16(size);

    Audio_InvalDCache(ramAddr, size);

    while (true) {
        if (size < AUDIOLOAD_SYNC_DMA_CHUNK_SIZE) {
            break;
        }
        AudioLoad_Dma(ioMesg, OS_MESG_PRI_HIGH, OS_READ, devAddr, ramAddr, AUDIOLOAD_SYNC_DMA_CHUNK_SIZE, msgQueue,
                      medium, "FastCopy");
        osRecvMesg(msgQueue, NULL, OS_MESG_BLOCK);
        size -= AUDIOLOAD_SYNC_DMA_CHUNK_SIZE;
        devAddr += AUDIOLOAD_SYNC_DMA_CHUNK_SIZE;
        ramAddr += AUDIOLOAD_SYNC_DMA_CHUNK_SIZE;
    }

    if (size != 0) {
        AudioLoad_Dma(ioMesg, OS_MESG_PRI_HIGH, OS_READ, devAddr, ramAddr, size, msgQueue, medium, "FastCopy");
        osRecvMesg(msgQueue, NULL, OS_MESG_BLOCK);
    }
}

/**
 * original name: Nas_FastDiskCopy
 */
void AudioLoad_SyncDmaUnkMedium(u32 devAddr, u8* addr, u32 size, s32 unkMediumParam) {
}

/**
 * original name: Nas_StartDma
 */
s32 AudioLoad_Dma(OSIoMesg* mesg, u32 priority, s32 direction, u32 devAddr, void* ramAddr, u32 size,
                  OSMesgQueue* reqQueue, s32 medium, const char* dmaFuncType) {
    OSPiHandle* handle;

    if (gAudioCtx.resetTimer > 16) {
        return -1;
    }

    switch (medium) {
        case MEDIUM_CART:
            handle = gAudioCtx.cartHandle;
            break;

        case MEDIUM_DISK_DRIVE:
            // driveHandle is uninitialized and corresponds to stubbed-out disk drive support.
            // SM64 Shindou called osDriveRomInit here.
            handle = gAudioCtx.driveHandle;
            break;

        default:
            return 0;
    }

    if ((size % 0x10) != 0) {
        size = ALIGN16(size);
    }

    mesg->hdr.pri = priority;
    mesg->hdr.retQueue = reqQueue;
    mesg->dramAddr = ramAddr;
    mesg->devAddr = devAddr;
    mesg->size = size;
    handle->transferInfo.cmdType = 2;
    sDmaHandler(handle, mesg, direction);
    return 0;
}

/**
 * original name: __OfsToLbaOfs
 */
void AudioLoad_Unused1(void) {
}

/**
 * original name: EmemLoad
 */
void AudioLoad_SyncLoadSimple(u32 tableType, u32 fontId) {
    s32 didAllocate;

    AudioLoad_SyncLoad(tableType, fontId, &didAllocate);
}

/**
 * original name: __Load_Bank_BG
 */
void* AudioLoad_AsyncLoadInner(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue) {
    u32 size;
    AudioTable* table;
    void* ramAddr;
    s32 medium;
    s8 cachePolicy;
    u32 devAddr;
    s32 loadStatus;
    s32 pad;
    u32 realId = AudioLoad_GetRealTableIndex(tableType, id);

    switch (tableType) {
        case SEQUENCE_TABLE:
            if (gAudioCtx.seqLoadStatus[realId] == LOAD_STATUS_IN_PROGRESS) {
                return NULL;
            }
            break;

        case FONT_TABLE:
            if (gAudioCtx.fontLoadStatus[realId] == LOAD_STATUS_IN_PROGRESS) {
                return NULL;
            }
            break;

        case SAMPLE_TABLE:
            if (gAudioCtx.sampleFontLoadStatus[realId] == LOAD_STATUS_IN_PROGRESS) {
                return NULL;
            }
            break;
    }

#if defined(TARGET_PSP)
    if (tableType == SAMPLE_TABLE) {
        ramAddr = OotPspAudio_GetResidentSampleBank(realId);
        if (ramAddr != NULL) {
            loadStatus = LOAD_STATUS_COMPLETE;
            AudioLoad_SetSampleFontLoadStatus(realId, loadStatus);
            osSendMesg(retQueue, (OSMesg)MK_ASYNC_MSG(retData, 0, 0, LOAD_STATUS_NOT_LOADED), OS_MESG_NOBLOCK);
            return ramAddr;
        }
    }
#endif

    ramAddr = AudioLoad_SearchCaches(tableType, realId);
    if (ramAddr != NULL) {
        loadStatus = LOAD_STATUS_COMPLETE;
        osSendMesg(retQueue, (OSMesg)MK_ASYNC_MSG(retData, 0, 0, LOAD_STATUS_NOT_LOADED), OS_MESG_NOBLOCK);
    } else {
        table = AudioLoad_GetLoadTable(tableType);
        size = table->entries[realId].size;
        size = ALIGN16(size);
        medium = table->entries[id].medium;
        cachePolicy = table->entries[id].cachePolicy;
        devAddr = table->entries[realId].romAddr;
        loadStatus = LOAD_STATUS_COMPLETE;

        switch (cachePolicy) {
            case 0:
                ramAddr = AudioHeap_AllocPermanent(tableType, realId, size);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                loadStatus = LOAD_STATUS_PERMANENTLY_LOADED;
                break;

            case 1:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_PERSISTENT, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;

            case 2:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_TEMPORARY, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;

            case 3:
            case 4:
                ramAddr = AudioHeap_AllocCached(tableType, size, CACHE_EITHER, realId);
                if (ramAddr == NULL) {
                    return ramAddr;
                }
                break;
        }

        if (medium == MEDIUM_UNK) {
            AudioLoad_StartAsyncLoadUnkMedium((s16)table->header.unkMediumParam, devAddr, ramAddr, size, medium,
                                              nChunks, retQueue, MK_ASYNC_MSG(retData, tableType, id, loadStatus));
        } else {
            AudioLoad_StartAsyncLoad(devAddr, ramAddr, size, medium, nChunks, retQueue,
                                     MK_ASYNC_MSG(retData, tableType, realId, loadStatus));
        }
        loadStatus = LOAD_STATUS_IN_PROGRESS;
    }

    switch (tableType) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(realId, loadStatus);
            break;

        case FONT_TABLE:
            AudioLoad_SetFontLoadStatus(realId, loadStatus);
            break;

        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(realId, loadStatus);
            break;

        default:
            break;
    }

    return ramAddr;
}

/**
 * original name: Nas_BgDmaFrameWork
 */
void AudioLoad_ProcessLoads(s32 resetStatus) {
    AudioLoad_ProcessSlowLoads(resetStatus);
    AudioLoad_ProcessSamplePreloads(resetStatus);
    AudioLoad_ProcessAsyncLoads(resetStatus);
}

/**
 * original name: Nas_SetRomHandler
 */
void AudioLoad_SetDmaHandler(DmaHandler callback) {
    sDmaHandler = callback;
}

/**
 * original name: Nas_SetRomHandler
 */
void AudioLoad_SetUnusedHandler(void* callback) {
    sUnusedHandler = callback;
}

/**
 * original name: __SetVlute
 */
void AudioLoad_InitSoundFont(s32 fontId) {
    SoundFont* font = &gAudioCtx.soundFontList[fontId];
    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    font->sampleBankId1 = (entry->shortData1 >> 8) & 0xFF;
    font->sampleBankId2 = (entry->shortData1) & 0xFF;
    font->numInstruments = (entry->shortData2 >> 8) & 0xFF;
    font->numDrums = entry->shortData2 & 0xFF;
    font->numSfx = entry->shortData3;
}

/**
 * original name: Nas_InitAudio
 */
void AudioLoad_Init(void* heap, u32 heapSize) {
    s32 pad[18];
    s32 numFonts;
    void* ramAddr;
    s32 i;

    gAudioCustomUpdateFunction = NULL;
    gAudioCtx.resetTimer = 0;

    {
        s32 i;
        u8* audioContextPtr = (u8*)&gAudioCtx;

#ifndef AVOID_UB
        //! @bug This clearing loop sets one extra byte to 0 following gAudioCtx.
        //! In practice this is harmless as it would set the most significant byte in gAudioCustomUpdateFunction to 0,
        //! which was just reset to NULL above.
        for (i = sizeof(gAudioCtx); i >= 0; i--) {
            *audioContextPtr++ = 0;
        }
#else
        // Avoid out-of-bounds variable access
        for (i = sizeof(gAudioCtx); i > 0; i--) {
            *audioContextPtr++ = 0;
        }
#endif
    }

    // 1000 is a conversion from seconds to milliseconds
#if !OOT_PAL_N64
    switch (osTvType) {
        case OS_TV_PAL:
            gAudioCtx.maxTempoTvTypeFactors = 1000 * REFRESH_RATE_DEVIATION_PAL / REFRESH_RATE_PAL;
            gAudioCtx.refreshRate = REFRESH_RATE_PAL;
            break;

        case OS_TV_MPAL:
            gAudioCtx.maxTempoTvTypeFactors = 1000 * REFRESH_RATE_DEVIATION_MPAL / REFRESH_RATE_MPAL;
            gAudioCtx.refreshRate = REFRESH_RATE_MPAL;
            break;

        case OS_TV_NTSC:
        default:
            gAudioCtx.maxTempoTvTypeFactors = 1000 * REFRESH_RATE_DEVIATION_NTSC / REFRESH_RATE_NTSC;
            gAudioCtx.refreshRate = REFRESH_RATE_NTSC;
            break;
    }
#else
    switch (osTvType) {
        case OS_TV_PAL:
        default:
            gAudioCtx.maxTempoTvTypeFactors = 1000 * REFRESH_RATE_DEVIATION_PAL / REFRESH_RATE_PAL;
            gAudioCtx.refreshRate = REFRESH_RATE_PAL;
            break;
    }
#endif

    AudioThread_InitMesgQueues();

    for (i = 0; i < 3; i++) {
        gAudioCtx.aiBufLengths[i] = 0xA0;
    }

    gAudioCtx.totalTaskCount = 0;
    gAudioCtx.rspTaskIndex = 0;
    gAudioCtx.curAiBufIndex = 0;
    gAudioCtx.soundOutputMode = SOUND_OUTPUT_STEREO;
    gAudioCtx.curTask = NULL;
    gAudioCtx.rspTask[0].task.t.data_size = 0;
    gAudioCtx.rspTask[1].task.t.data_size = 0;
    osCreateMesgQueue(&gAudioCtx.syncDmaQueue, &gAudioCtx.syncDmaMesg, 1);
    osCreateMesgQueue(&gAudioCtx.curAudioFrameDmaQueue, gAudioCtx.curAudioFrameDmaMsgBuf,
                      ARRAY_COUNT(gAudioCtx.curAudioFrameDmaMsgBuf));
    osCreateMesgQueue(&gAudioCtx.externalLoadQueue, gAudioCtx.externalLoadMsgBuf,
                      ARRAY_COUNT(gAudioCtx.externalLoadMsgBuf));
    osCreateMesgQueue(&gAudioCtx.preloadSampleQueue, gAudioCtx.preloadSampleMsgBuf,
                      ARRAY_COUNT(gAudioCtx.preloadSampleMsgBuf));
    gAudioCtx.curAudioFrameDmaCount = 0;
    gAudioCtx.sampleDmaCount = 0;
    gAudioCtx.cartHandle = osCartRomInit();

    if (heap == NULL) {
        gAudioCtx.audioHeap = gAudioHeap;
        gAudioCtx.audioHeapSize = gAudioHeapInitSizes.heapSize;
    } else {
        gAudioCtx.audioHeap = heap;
        gAudioCtx.audioHeapSize = heapSize;
    }

    for (i = 0; i < (s32)gAudioCtx.audioHeapSize / 8; i++) {
        ((u64*)gAudioCtx.audioHeap)[i] = 0;
    }

    // Main Pool Split (split entirety of audio heap into initPool and sessionPool)
    AudioHeap_InitMainPools(gAudioHeapInitSizes.initPoolSize);

    // Initialize the audio interface buffers
    for (i = 0; i < ARRAY_COUNT(gAudioCtx.aiBuffers); i++) {
        gAudioCtx.aiBuffers[i] = AudioHeap_AllocZeroed(&gAudioCtx.initPool, AIBUF_SIZE);
    }

    // Set audio tables pointers
    gAudioCtx.sequenceTable = &gSequenceTable;
    gAudioCtx.soundFontTable = &gSoundFontTable;
    gAudioCtx.sampleBankTable = &gSampleBankTable;
    gAudioCtx.sequenceFontTable = gSequenceFontTable;

    gAudioCtx.numSequences = gAudioCtx.sequenceTable->header.numEntries;

    gAudioCtx.specId = 0;
    gAudioCtx.resetStatus = 1; // Set reset to immediately initialize the audio heap

    AudioHeap_ResetStep();

    // Initialize audio tables
    AudioLoad_InitTable(gAudioCtx.sequenceTable, (u32)_AudioseqSegmentRomStart, 0);
    AudioLoad_InitTable(gAudioCtx.soundFontTable, (u32)_AudiobankSegmentRomStart, 0);
    AudioLoad_InitTable(gAudioCtx.sampleBankTable, (u32)_AudiotableSegmentRomStart, 0);
    numFonts = gAudioCtx.soundFontTable->header.numEntries;
    gAudioCtx.soundFontList = AudioHeap_Alloc(&gAudioCtx.initPool, numFonts * sizeof(SoundFont));

    for (i = 0; i < numFonts; i++) {
        AudioLoad_InitSoundFont(i);
    }

    ramAddr = AudioHeap_Alloc(&gAudioCtx.initPool, gAudioHeapInitSizes.permanentPoolSize);
    if (ramAddr == NULL) {
        gAudioHeapInitSizes.permanentPoolSize = 0;
    }

    AudioHeap_InitPool(&gAudioCtx.permanentPool, ramAddr, gAudioHeapInitSizes.permanentPoolSize);
    gAudioContextInitialized = true;
    osSendMesg(gAudioCtx.taskStartQueueP, (OSMesg)gAudioCtx.totalTaskCount, OS_MESG_NOBLOCK);
}

/**
 * original name: LpsInit
 */
void AudioLoad_InitSlowLoads(void) {
    gAudioCtx.slowLoads[0].state = SLOW_LOAD_STATE_WAITING;
    gAudioCtx.slowLoads[1].state = SLOW_LOAD_STATE_WAITING;
}

/**
 * original name: VoiceLoad
 */
s32 AudioLoad_SlowLoadSample(s32 fontId, s32 instId, s8* status) {
    Sample* sample;
    AudioSlowLoad* slowLoad;

#if defined(TARGET_PSP)
    if (instId < 0) {
        OotPspAudio_LogBadSampleLookup(fontId, instId);
        *status = 0;
        return -1;
    }
#endif

    sample = AudioLoad_GetFontSample(fontId, instId);
    if (sample == NULL) {
        *status = 0;
        return -1;
    }
#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSlowLoadSample(sample)) {
        OotPspAudio_LogBadSamplePtr(fontId, instId, sample);
        *status = 0;
        return -1;
    }
#endif

    if (sample->medium == MEDIUM_RAM) {
        *status = 2;
        return 0;
    }

    slowLoad = &gAudioCtx.slowLoads[gAudioCtx.slowLoadPos];
    if (slowLoad->state == SLOW_LOAD_STATE_DONE) {
        slowLoad->state = SLOW_LOAD_STATE_WAITING;
    }

    slowLoad->sample = *sample;
    slowLoad->status = status;
    slowLoad->curRamAddr =
        AudioHeap_AllocSampleCache(sample->size, fontId, sample->sampleAddr, sample->medium, CACHE_TEMPORARY);

    if (slowLoad->curRamAddr == NULL) {
        if (sample->medium == MEDIUM_UNK || sample->codec == CODEC_S16_INMEMORY) {
            *status = 0;
            return -1;
        } else {
            *status = 3;
            return -1;
        }
    }

    slowLoad->state = SLOW_LOAD_STATE_START;
    slowLoad->bytesRemaining = ALIGN16(sample->size);
    slowLoad->ramAddr = slowLoad->curRamAddr;
    slowLoad->curDevAddr = (u32)sample->sampleAddr;
    slowLoad->medium = sample->medium;
    slowLoad->seqOrFontId = fontId;
    slowLoad->instId = instId;
    if (slowLoad->medium == MEDIUM_UNK) {
        slowLoad->unkMediumParam = gAudioCtx.sampleBankTable->header.unkMediumParam;
    }

    gAudioCtx.slowLoadPos ^= 1;
    return 0;
}

/**
 * original name: __GetWaveTable
 */
Sample* AudioLoad_GetFontSample(s32 fontId, s32 instId) {
    Sample* sample;

#if defined(TARGET_PSP)
    if (instId < 0) {
        OotPspAudio_LogBadSampleLookup(fontId, instId);
        return NULL;
    }
#endif

    if (instId < 0x80) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, instId);

        if (instrument == NULL) {
            return NULL;
        }
        sample = instrument->normalPitchTunedSample.sample;
    } else if (instId < 0x100) {
        Drum* drum = Audio_GetDrum(fontId, instId - 0x80);

        if (drum == NULL) {
            return NULL;
        }
        sample = drum->tunedSample.sample;
    } else {
        SoundEffect* soundEffect = Audio_GetSoundEffect(fontId, instId - 0x100);

        if (soundEffect == NULL) {
            return NULL;
        }
        sample = soundEffect->tunedSample.sample;
    }
#if defined(TARGET_PSP)
    if ((sample != NULL) && !OotPspAudio_IsSafeSamplePtr(sample)) {
        OotPspAudio_LogBadSamplePtr(fontId, instId, sample);
        return NULL;
    }
#endif
    return sample;
}

void AudioLoad_Unused2(void) {
}

/**
 * original name: __SwapLoadLps
 */
void AudioLoad_FinishSlowLoad(AudioSlowLoad* slowLoad) {
    Sample* sample;

    if (slowLoad->sample.sampleAddr == NULL) {
        return;
    }

    sample = AudioLoad_GetFontSample(slowLoad->seqOrFontId, slowLoad->instId);
    if (sample == NULL) {
        return;
    }
#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSlowLoadSample(sample)) {
        OotPspAudio_LogBadSamplePtr(slowLoad->seqOrFontId, slowLoad->instId, sample);
        return;
    }
#endif

    slowLoad->sample = *sample;
    sample->sampleAddr = slowLoad->ramAddr;
    sample->medium = MEDIUM_RAM;
}

/**
 * original name: LpsDma
 */
void AudioLoad_ProcessSlowLoads(s32 resetStatus) {
    AudioSlowLoad* slowLoad;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.slowLoads); i++) {
        slowLoad = &gAudioCtx.slowLoads[i];
        switch (gAudioCtx.slowLoads[i].state) {
            case SLOW_LOAD_STATE_LOADING:
                if (slowLoad->medium != MEDIUM_UNK) {
                    osRecvMesg(&slowLoad->msgQueue, NULL, OS_MESG_BLOCK);
                }

                if (resetStatus != 0) {
                    slowLoad->state = SLOW_LOAD_STATE_DONE;
                    continue;
                }
                FALLTHROUGH;
            case SLOW_LOAD_STATE_START:
                slowLoad->state = SLOW_LOAD_STATE_LOADING;
                if (slowLoad->bytesRemaining == 0) {
                    AudioLoad_FinishSlowLoad(slowLoad);
                    slowLoad->state = SLOW_LOAD_STATE_DONE;
                    *slowLoad->status = 1;
                } else if (slowLoad->bytesRemaining < 0x400) {
                    if (slowLoad->medium == MEDIUM_UNK) {
                        u32 size = slowLoad->bytesRemaining;

                        AudioLoad_DmaSlowCopyUnkMedium(slowLoad->curDevAddr, slowLoad->curRamAddr, size,
                                                       slowLoad->unkMediumParam);
                    } else {
                        AudioLoad_DmaSlowCopy(slowLoad, slowLoad->bytesRemaining);
                    }
                    slowLoad->bytesRemaining = 0;
                } else {
                    if (slowLoad->medium == MEDIUM_UNK) {
                        AudioLoad_DmaSlowCopyUnkMedium(slowLoad->curDevAddr, slowLoad->curRamAddr, 0x400,
                                                       slowLoad->unkMediumParam);
                    } else {
                        AudioLoad_DmaSlowCopy(slowLoad, 0x400);
                    }
                    slowLoad->bytesRemaining -= 0x400;
                    slowLoad->curRamAddr += 0x400;
                    slowLoad->curDevAddr += 0x400;
                }
                break;
        }
    }
}

/**
 * original name: __Nas_SlowCopy
 */
void AudioLoad_DmaSlowCopy(AudioSlowLoad* slowLoad, s32 size) {
    Audio_InvalDCache(slowLoad->curRamAddr, size);
    osCreateMesgQueue(&slowLoad->msgQueue, &slowLoad->msg, 1);
    AudioLoad_Dma(&slowLoad->ioMesg, OS_MESG_PRI_NORMAL, OS_READ, slowLoad->curDevAddr, slowLoad->curRamAddr, size,
                  &slowLoad->msgQueue, slowLoad->medium, "SLOWCOPY");
}

/**
 * original name: __Nas_SlowDiskCopy
 */
void AudioLoad_DmaSlowCopyUnkMedium(s32 devAddr, u8* ramAddr, s32 size, s32 arg3) {
}

/**
 * original name: SeqLoad
 */
s32 AudioLoad_SlowLoadSeq(s32 seqId, u8* ramAddr, s8* status) {
    AudioSlowLoad* slowLoad;
    AudioTable* seqTable;
    u32 size;

    if (seqId >= gAudioCtx.numSequences) {
        *status = 0;
        return -1;
    }

    seqId = AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId);
    seqTable = AudioLoad_GetLoadTable(SEQUENCE_TABLE);
    slowLoad = &gAudioCtx.slowLoads[gAudioCtx.slowLoadPos];
    if (slowLoad->state == SLOW_LOAD_STATE_DONE) {
        slowLoad->state = SLOW_LOAD_STATE_WAITING;
    }

    slowLoad->sample.sampleAddr = NULL;
    slowLoad->status = status;
    size = seqTable->entries[seqId].size;
    size = ALIGN16(size);
    slowLoad->curRamAddr = ramAddr;
    slowLoad->state = SLOW_LOAD_STATE_START;
    slowLoad->bytesRemaining = size;
    slowLoad->ramAddr = ramAddr;
    slowLoad->curDevAddr = seqTable->entries[seqId].romAddr;
    slowLoad->medium = seqTable->entries[seqId].medium;
    slowLoad->seqOrFontId = seqId;

    if (slowLoad->medium == MEDIUM_UNK) {
        slowLoad->unkMediumParam = seqTable->header.unkMediumParam;
    }

    gAudioCtx.slowLoadPos ^= 1;
    return 0;
}

/**
 * original name: Nas_BgCopyInit
 */
void AudioLoad_InitAsyncLoads(void) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        gAudioCtx.asyncLoads[i].status = 0;
    }
}

/**
 * original name: Nas_BgCopyDisk
 */
AudioAsyncLoad* AudioLoad_StartAsyncLoadUnkMedium(s32 unkMediumParam, u32 devAddr, void* ramAddr, s32 size, s32 medium,
                                                  s32 nChunks, OSMesgQueue* retQueue, s32 retMsg) {
    AudioAsyncLoad* asyncLoad;

    asyncLoad = AudioLoad_StartAsyncLoad(devAddr, ramAddr, size, medium, nChunks, retQueue, retMsg);

    if (asyncLoad == NULL) {
        return NULL;
    }

    osSendMesg(&gAudioCtx.asyncLoadUnkMediumQueue, (OSMesg)asyncLoad, OS_MESG_NOBLOCK);
    asyncLoad->unkMediumParam = unkMediumParam;
    return asyncLoad;
}

/**
 * original name: Nas_BgCopyReq
 */
AudioAsyncLoad* AudioLoad_StartAsyncLoad(u32 devAddr, void* ramAddr, u32 size, s32 medium, s32 nChunks,
                                         OSMesgQueue* retQueue, s32 retMsg) {
    AudioAsyncLoad* asyncLoad;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        if (gAudioCtx.asyncLoads[i].status == 0) {
            asyncLoad = &gAudioCtx.asyncLoads[i];
            break;
        }
    }

    // no more available async loads
    if (i == ARRAY_COUNT(gAudioCtx.asyncLoads)) {
        return NULL;
    }

    asyncLoad->status = 1;
    asyncLoad->curDevAddr = devAddr;
    asyncLoad->ramAddr = ramAddr;
    asyncLoad->curRamAddr = ramAddr;
    asyncLoad->bytesRemaining = size;

    if (nChunks == 0) {
        asyncLoad->chunkSize = 0x1000;
    } else if (nChunks == 1) {
        asyncLoad->chunkSize = size;
    } else {
        asyncLoad->chunkSize = ALIGN256((s32)size / nChunks);
        if (asyncLoad->chunkSize < 0x100) {
            asyncLoad->chunkSize = 0x100;
        }
    }

    asyncLoad->retQueue = retQueue;
    asyncLoad->delay = 3;
    asyncLoad->medium = medium;
    asyncLoad->retMsg = retMsg;
    osCreateMesgQueue(&asyncLoad->msgQueue, &asyncLoad->msg, 1);
    return asyncLoad;
}

/**
 * original name: Nas_BgCopyMain
 */
void AudioLoad_ProcessAsyncLoads(s32 resetStatus) {
    AudioAsyncLoad* asyncLoad;
    s32 i;

    if (gAudioCtx.resetTimer == 1) {
        return;
    }

    if (gAudioCtx.curUnkMediumLoad == NULL) {
        if (resetStatus != 0) {
            // Clear and ignore queue if resetting.
            do {
            } while (osRecvMesg(&gAudioCtx.asyncLoadUnkMediumQueue, (OSMesg*)&asyncLoad, OS_MESG_NOBLOCK) != -1);
        } else if (osRecvMesg(&gAudioCtx.asyncLoadUnkMediumQueue, (OSMesg*)&asyncLoad, OS_MESG_NOBLOCK) == -1) {
            gAudioCtx.curUnkMediumLoad = NULL;
        } else {
            gAudioCtx.curUnkMediumLoad = asyncLoad;
        }
    }

    if (gAudioCtx.curUnkMediumLoad != NULL) {
        AudioLoad_ProcessAsyncLoadUnkMedium(gAudioCtx.curUnkMediumLoad, resetStatus);
    }

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        if (gAudioCtx.asyncLoads[i].status == 1) {
            asyncLoad = &gAudioCtx.asyncLoads[i];
            if (asyncLoad->medium != MEDIUM_UNK) {
                AudioLoad_ProcessAsyncLoad(asyncLoad, resetStatus);
            }
        }
    }
}

/**
 * original name: __BgCopyDisk
 */
void AudioLoad_ProcessAsyncLoadUnkMedium(AudioAsyncLoad* asyncLoad, s32 resetStatus) {
}

/**
 * original name: __BgCopyFinishProcess
 */
void AudioLoad_FinishAsyncLoad(AudioAsyncLoad* asyncLoad) {
    u32 retMsg = asyncLoad->retMsg;
    u32 fontId;
    u32 pad;
    OSMesg doneMsg;
    u32 sampleBankId1;
    u32 sampleBankId2;
    SampleBankRelocInfo sampleBankReloc;

    if (1) {}
    switch (ASYNC_TBLTYPE(retMsg)) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(ASYNC_ID(retMsg), ASYNC_LOAD_STATUS(retMsg));
            break;

        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(ASYNC_ID(retMsg), ASYNC_LOAD_STATUS(retMsg));
            break;

        case FONT_TABLE:
            fontId = ASYNC_ID(retMsg);
            sampleBankId1 = gAudioCtx.soundFontList[fontId].sampleBankId1;
            sampleBankId2 = gAudioCtx.soundFontList[fontId].sampleBankId2;
            sampleBankReloc.sampleBankId1 = sampleBankId1;
            sampleBankReloc.sampleBankId2 = sampleBankId2;
            sampleBankReloc.baseAddr1 =
                sampleBankId1 != 0xFF ? AudioLoad_GetSampleBank(sampleBankId1, &sampleBankReloc.medium1) : 0;
            sampleBankReloc.baseAddr2 =
                sampleBankId2 != 0xFF ? AudioLoad_GetSampleBank(sampleBankId2, &sampleBankReloc.medium2) : 0;
            AudioLoad_SetFontLoadStatus(fontId, ASYNC_LOAD_STATUS(retMsg));
            AudioLoad_RelocateFontAndPreloadSamples(fontId, asyncLoad->ramAddr, &sampleBankReloc, true);
            break;
    }

    doneMsg = (OSMesg)asyncLoad->retMsg;
    if (1) {}
    asyncLoad->status = 0;
    osSendMesg(asyncLoad->retQueue, doneMsg, OS_MESG_NOBLOCK);
}

/**
 * original name: __BgCopySub
 */
void AudioLoad_ProcessAsyncLoad(AudioAsyncLoad* asyncLoad, s32 resetStatus) {
    AudioTable* sampleBankTable = gAudioCtx.sampleBankTable;

    if (asyncLoad->delay >= 2) {
        asyncLoad->delay--;
        return;
    }

    if (asyncLoad->delay == 1) {
        asyncLoad->delay = 0;
    } else if (resetStatus != 0) {
        // Await the previous DMA response synchronously, then return.
        osRecvMesg(&asyncLoad->msgQueue, NULL, OS_MESG_BLOCK);
        asyncLoad->status = 0;
        return;
    } else if (osRecvMesg(&asyncLoad->msgQueue, NULL, OS_MESG_NOBLOCK) == -1) {
        // If the previous DMA step isn't done, return.
        return;
    }

    if (asyncLoad->bytesRemaining == 0) {
        AudioLoad_FinishAsyncLoad(asyncLoad);
        return;
    }

    if (asyncLoad->bytesRemaining < asyncLoad->chunkSize) {
        if (asyncLoad->medium == MEDIUM_UNK) {
            AudioLoad_AsyncDmaUnkMedium(asyncLoad->curDevAddr, asyncLoad->curRamAddr, asyncLoad->bytesRemaining,
                                        sampleBankTable->header.unkMediumParam);
        } else {
            AudioLoad_AsyncDma(asyncLoad, asyncLoad->bytesRemaining);
        }
        asyncLoad->bytesRemaining = 0;
        return;
    }

    if (asyncLoad->medium == MEDIUM_UNK) {
        AudioLoad_AsyncDmaUnkMedium(asyncLoad->curDevAddr, asyncLoad->curRamAddr, asyncLoad->chunkSize,
                                    sampleBankTable->header.unkMediumParam);
    } else {
        AudioLoad_AsyncDma(asyncLoad, asyncLoad->chunkSize);
    }

    asyncLoad->bytesRemaining -= asyncLoad->chunkSize;
    asyncLoad->curDevAddr += asyncLoad->chunkSize;
    asyncLoad->curRamAddr += asyncLoad->chunkSize;
}

/**
 * original name: __Nas_BgCopy
 */
void AudioLoad_AsyncDma(AudioAsyncLoad* asyncLoad, u32 size) {
    size = ALIGN16(size);
    Audio_InvalDCache(asyncLoad->curRamAddr, size);
    osCreateMesgQueue(&asyncLoad->msgQueue, &asyncLoad->msg, 1);
    AudioLoad_Dma(&asyncLoad->ioMesg, OS_MESG_PRI_NORMAL, OS_READ, asyncLoad->curDevAddr, asyncLoad->curRamAddr, size,
                  &asyncLoad->msgQueue, asyncLoad->medium, "BGCOPY");
}

/**
 * original name: __Nas_BgDiskCopy
 */
void AudioLoad_AsyncDmaUnkMedium(u32 devAddr, void* ramAddr, u32 size, s16 arg3) {
}

/**
 * Read and extract information from TunedSample and its Sample
 * contained in the soundFont binary loaded into ram
 * TunedSample contains metadata on a sample used by a particular instrument/drum/sfx
 * Also relocate offsets into pointers within this loaded TunedSample
 *
 * original name: __WaveTouch
 *
 * @param fontId index of font being processed
 * @param fontData ram address of raw soundfont binary loaded into cache
 * @param sampleBankReloc information on the sampleBank containing raw audio samples
 */
void AudioLoad_RelocateSample(TunedSample* tunedSample, SoundFontData* fontData, SampleBankRelocInfo* sampleBankReloc) {
    Sample* sample;
    void* reloc;

    if ((tunedSample == NULL) || (tunedSample->sample == NULL)) {
        return;
    }

    // Relocate an offset (relative to data loaded in ram at `base`) to a pointer (a ram address)
#define AUDIO_RELOC(offset, base) (reloc = (void*)((u32)(offset) + (u32)(base)))

    // If this has not already been relocated
    if ((u32)tunedSample->sample <= AUDIO_RELOCATED_ADDRESS_START) {

        sample = tunedSample->sample = AUDIO_RELOC(tunedSample->sample, fontData);
#if defined(TARGET_PSP)
        OotPspAudio_SwapSample(sample);
#endif

        // If the sample exists and has not already been relocated
        // Note: this is important, as the same sample can be used by different drums, sound effects, instruments
        if ((sample->size != 0) && (sample->isRelocated != true)) {
            sample->loop = AUDIO_RELOC(sample->loop, fontData);
            sample->book = AUDIO_RELOC(sample->book, fontData);
#if defined(TARGET_PSP)
            OotPspAudio_SwapLoop(sample->loop);
            OotPspAudio_SwapBook(sample->book);
#endif

            // Resolve the sample medium 2-bit bitfield into a real value based on sampleBankReloc.
            // Then relocate the offset sample within the sampleBank (not the fontData) into absolute address.
            // sampleAddr can be either rom or ram depending on sampleBank cache policy
            // in practice, this is always in rom
            switch (sample->medium) {
                case 0:
                    sample->sampleAddr = AUDIO_RELOC(sample->sampleAddr, sampleBankReloc->baseAddr1);
                    sample->medium = sampleBankReloc->medium1;
                    break;

                case 1:
                    sample->sampleAddr = AUDIO_RELOC(sample->sampleAddr, sampleBankReloc->baseAddr2);
                    sample->medium = sampleBankReloc->medium2;
                    break;

                case 2:
                case 3:
                    // Invalid? This leaves sample->medium as MEDIUM_CART and MEDIUM_DISK_DRIVE
                    // respectively, and the sampleAddr unrelocated.
                    break;
            }

            sample->isRelocated = true;

            if (sample->unk_bit26 && (sample->medium != MEDIUM_RAM)) {
                gAudioCtx.usedSamples[gAudioCtx.numUsedSamples++] = sample;
            }
        }
    }

#undef AUDIO_RELOC
}

/**
 * original name: Nas_BankOfsToAddr
 *
 * @param fontId index of font being processed
 * @param fontData ram address of raw soundfont binary loaded into cache
 * @param sampleBankReloc information on the sampleBank containing raw audio samples
 * @param isAsync bool for whether this is an asynchronous load or not
 */
void AudioLoad_RelocateFontAndPreloadSamples(s32 fontId, SoundFontData* fontData, SampleBankRelocInfo* sampleBankReloc,
                                             s32 isAsync) {
    AudioPreloadReq* preload;
    AudioPreloadReq* topPreload;
    Sample* sample;
    s32 size;
    s32 nChunks;
    u8* sampleRamAddr;
    s32 preloadInProgress;
    s32 i;

    preloadInProgress = false;
    if (gAudioCtx.preloadSampleStackTop != 0) {
        preloadInProgress = true;
    } else {
        D_8016B780 = 0;
    }

    gAudioCtx.numUsedSamples = 0;
    AudioLoad_RelocateFont(fontId, fontData, sampleBankReloc);

    size = 0;
    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        size += ALIGN16(gAudioCtx.usedSamples[i]->size);
    }
    if (size && size) {}

    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        if (gAudioCtx.preloadSampleStackTop == 120) {
            break;
        }

        sample = gAudioCtx.usedSamples[i];
        sampleRamAddr = NULL;
        switch (isAsync) {
            case false:
                if (sample->medium == sampleBankReloc->medium1) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId1,
                                                               sample->sampleAddr, sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == sampleBankReloc->medium2) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId2,
                                                               sample->sampleAddr, sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == MEDIUM_DISK_DRIVE) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, 0xFE, sample->sampleAddr, sample->medium,
                                                               CACHE_PERSISTENT);
                }
                break;

            case true:
                if (sample->medium == sampleBankReloc->medium1) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId1,
                                                               sample->sampleAddr, sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == sampleBankReloc->medium2) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId2,
                                                               sample->sampleAddr, sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == MEDIUM_DISK_DRIVE) {
                    sampleRamAddr = AudioHeap_AllocSampleCache(sample->size, 0xFE, sample->sampleAddr, sample->medium,
                                                               CACHE_TEMPORARY);
                }
                break;
        }
        if (sampleRamAddr == NULL) {
            continue;
        }

        switch (isAsync) {
            case false:
                if (sample->medium == MEDIUM_UNK) {
                    AudioLoad_SyncDmaUnkMedium((u32)sample->sampleAddr, sampleRamAddr, sample->size,
                                               gAudioCtx.sampleBankTable->header.unkMediumParam);
                    sample->sampleAddr = sampleRamAddr;
                    sample->medium = MEDIUM_RAM;
                } else {
                    AudioLoad_SyncDma((u32)sample->sampleAddr, sampleRamAddr, sample->size, sample->medium);
                    sample->sampleAddr = sampleRamAddr;
                    sample->medium = MEDIUM_RAM;
                }
                if (sample->medium == MEDIUM_DISK_DRIVE) {}
                break;

            case true:
                preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop];
                preload->sample = sample;
                preload->ramAddr = sampleRamAddr;
                preload->encodedInfo = (gAudioCtx.preloadSampleStackTop << 24) | 0xFFFFFF;
                preload->isFree = false;
                preload->endAndMediumKey = (u32)sample->sampleAddr + sample->size + sample->medium;
                gAudioCtx.preloadSampleStackTop++;
                break;
        }
    }
    gAudioCtx.numUsedSamples = 0;

    if (gAudioCtx.preloadSampleStackTop != 0 && !preloadInProgress) {
        topPreload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
        sample = topPreload->sample;
        nChunks = (sample->size >> 12) + 1;
        AudioLoad_StartAsyncLoad((u32)sample->sampleAddr, topPreload->ramAddr, sample->size, sample->medium, nChunks,
                                 &gAudioCtx.preloadSampleQueue, topPreload->encodedInfo);
    }
}

/**
 * original name: Nas_CheckBgWave
 */
s32 AudioLoad_ProcessSamplePreloads(s32 resetStatus) {
    Sample* sample;
    AudioPreloadReq* preload;
    u32 preloadIndex;
    u32 key;
    u32 nChunks;
    s32 pad;

    if (gAudioCtx.preloadSampleStackTop > 0) {
        if (resetStatus != 0) {
            // Clear result queue and preload stack and return.
            osRecvMesg(&gAudioCtx.preloadSampleQueue, (OSMesg*)&preloadIndex, OS_MESG_NOBLOCK);
            gAudioCtx.preloadSampleStackTop = 0;
            return false;
        }
        if (osRecvMesg(&gAudioCtx.preloadSampleQueue, (OSMesg*)&preloadIndex, OS_MESG_NOBLOCK) == -1) {
            // Previous preload is not done yet.
            return false;
        }

        preloadIndex >>= 24;
        preload = &gAudioCtx.preloadSampleStack[preloadIndex];

        if (!preload->isFree) {
            sample = preload->sample;
            key = (u32)sample->sampleAddr + sample->size + sample->medium;
            if (key == preload->endAndMediumKey) {
                // Change storage for sample to the preloaded version.
                sample->sampleAddr = preload->ramAddr;
                sample->medium = MEDIUM_RAM;
            }
            preload->isFree = true;
        }

        // Pop requests with isFree = true off the stack, as far as possible,
        // and dispatch the next DMA.
        while (true) {
            if (gAudioCtx.preloadSampleStackTop <= 0) {
                break;
            }
            preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
            if (preload->isFree == true) {
                gAudioCtx.preloadSampleStackTop--;
                continue;
            }

            sample = preload->sample;
            nChunks = (sample->size >> 12) + 1;
            key = (u32)sample->sampleAddr + sample->size + sample->medium;
            if (key != preload->endAndMediumKey) {
                preload->isFree = true;
                gAudioCtx.preloadSampleStackTop--;
            } else {
                AudioLoad_StartAsyncLoad((u32)sample->sampleAddr, preload->ramAddr, sample->size, sample->medium,
                                         nChunks, &gAudioCtx.preloadSampleQueue, preload->encodedInfo);
                break;
            }
        }
    }
    return true;
}

/**
 * original name: __AddList
 */
s32 AudioLoad_AddToSampleSet(Sample* sample, s32 numSamples, Sample** sampleSet) {
    s32 i;

    if (sample == NULL) {
        return numSamples;
    }

    for (i = 0; i < numSamples; i++) {
        if ((sampleSet[i] != NULL) && (sample->sampleAddr == sampleSet[i]->sampleAddr)) {
            break;
        }
    }

    if (i == numSamples) {
        sampleSet[numSamples] = sample;
        numSamples++;
    }

    return numSamples;
}

/**
 * original name: MakeWaveList
 */
s32 AudioLoad_GetSamplesForFont(s32 fontId, Sample** sampleSet) {
    s32 i;
    s32 numSamples = 0;
    s32 numDrums = gAudioCtx.soundFontList[fontId].numDrums;
    s32 numInstruments = gAudioCtx.soundFontList[fontId].numInstruments;

    for (i = 0; i < numDrums; i++) {
        Drum* drum = Audio_GetDrum(fontId, i);

        if (1) {}
        if (drum != NULL) {
            numSamples = AudioLoad_AddToSampleSet(drum->tunedSample.sample, numSamples, sampleSet);
        }
    }

    for (i = 0; i < numInstruments; i++) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, i);

        if (instrument != NULL) {
            if (instrument->normalRangeLo != 0) {
                numSamples = AudioLoad_AddToSampleSet(instrument->lowPitchTunedSample.sample, numSamples, sampleSet);
            }
            if (instrument->normalRangeHi != 0x7F) {
                numSamples = AudioLoad_AddToSampleSet(instrument->highPitchTunedSample.sample, numSamples, sampleSet);
            }
            numSamples = AudioLoad_AddToSampleSet(instrument->normalPitchTunedSample.sample, numSamples, sampleSet);
        }
    }

    // Should really also process sfx, but this method is never called.
    return numSamples;
}

/**
 * original name: __Reload
 */
void AudioLoad_AddUsedSample(TunedSample* tunedSample) {
    Sample* sample;

    if ((tunedSample == NULL) || (tunedSample->sample == NULL)) {
        return;
    }

    sample = tunedSample->sample;

    if ((sample->size != 0) && sample->unk_bit26 && (sample->medium != MEDIUM_RAM)) {
        gAudioCtx.usedSamples[gAudioCtx.numUsedSamples++] = sample;
    }
}

/**
 * original name: WaveReload
 */
void AudioLoad_PreloadSamplesForFont(s32 fontId, s32 async, SampleBankRelocInfo* sampleBankReloc) {
    s32 numDrums;
    s32 numInstruments;
    s32 numSfx;
    Drum* drum;
    Instrument* instrument;
    SoundEffect* soundEffect;
    AudioPreloadReq* preload;
    AudioPreloadReq* topPreload;
    u8* addr;
    s32 size;
    s32 i;
    Sample* sample;
    s32 preloadInProgress;
    s32 nChunks;

    preloadInProgress = false;
    if (gAudioCtx.preloadSampleStackTop != 0) {
        preloadInProgress = true;
    }

    gAudioCtx.numUsedSamples = 0;

    numDrums = gAudioCtx.soundFontList[fontId].numDrums;
    numInstruments = gAudioCtx.soundFontList[fontId].numInstruments;
    numSfx = gAudioCtx.soundFontList[fontId].numSfx;

    for (i = 0; i < numInstruments; i++) {
        instrument = Audio_GetInstrumentInner(fontId, i);
        if (instrument != NULL) {
            if (instrument->normalRangeLo != 0) {
                AudioLoad_AddUsedSample(&instrument->lowPitchTunedSample);
            }
            if (instrument->normalRangeHi != 0x7F) {
                AudioLoad_AddUsedSample(&instrument->highPitchTunedSample);
            }
            AudioLoad_AddUsedSample(&instrument->normalPitchTunedSample);
        }
    }

    for (i = 0; i < numDrums; i++) {
        drum = Audio_GetDrum(fontId, i);
        if (drum != NULL) {
            AudioLoad_AddUsedSample(&drum->tunedSample);
        }
    }

    for (i = 0; i < numSfx; i++) {
        soundEffect = Audio_GetSoundEffect(fontId, i);
        if (soundEffect != NULL) {
            AudioLoad_AddUsedSample(&soundEffect->tunedSample);
        }
    }

    if (gAudioCtx.numUsedSamples == 0) {
        return;
    }

    size = 0;
    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        size += ALIGN16(gAudioCtx.usedSamples[i]->size);
    }
    if (size) {}

    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        if (gAudioCtx.preloadSampleStackTop == 120) {
            break;
        }

        sample = gAudioCtx.usedSamples[i];
        if (sample->medium == MEDIUM_RAM) {
            continue;
        }

        switch (async) {
            case false:
                if (sample->medium == sampleBankReloc->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == sampleBankReloc->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                }
                break;

            case true:
                if (sample->medium == sampleBankReloc->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == sampleBankReloc->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, sampleBankReloc->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                }
                break;
        }
        if (addr == NULL) {
            continue;
        }

        switch (async) {
            case false:
                if (sample->medium == MEDIUM_UNK) {
                    AudioLoad_SyncDmaUnkMedium((u32)sample->sampleAddr, addr, sample->size,
                                               gAudioCtx.sampleBankTable->header.unkMediumParam);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                } else {
                    AudioLoad_SyncDma((u32)sample->sampleAddr, addr, sample->size, sample->medium);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                }
                break;

            case true:
                preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop];
                preload->sample = sample;
                preload->ramAddr = addr;
                preload->encodedInfo = (gAudioCtx.preloadSampleStackTop << 24) | 0xFFFFFF;
                preload->isFree = false;
                preload->endAndMediumKey = (u32)sample->sampleAddr + sample->size + sample->medium;
                gAudioCtx.preloadSampleStackTop++;
                break;
        }
    }
    gAudioCtx.numUsedSamples = 0;

    if (gAudioCtx.preloadSampleStackTop != 0 && !preloadInProgress) {
        topPreload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
        sample = topPreload->sample;
        nChunks = (sample->size >> 12) + 1;
        AudioLoad_StartAsyncLoad((u32)sample->sampleAddr, topPreload->ramAddr, sample->size, sample->medium, nChunks,
                                 &gAudioCtx.preloadSampleQueue, topPreload->encodedInfo);
    }
}

/**
 * original name: EmemReload
 */
void AudioLoad_LoadPermanentSamples(void) {
    s32 pad;
    u32 fontId;
    AudioTable* sampleBankTable;
    s32 pad2;
    s32 i;

    sampleBankTable = AudioLoad_GetLoadTable(SAMPLE_TABLE);
    for (i = 0; i < gAudioCtx.permanentPool.numEntries; i++) {
        SampleBankRelocInfo sampleBankReloc;

        if (gAudioCtx.permanentCache[i].tableType == FONT_TABLE) {
            fontId = AudioLoad_GetRealTableIndex(FONT_TABLE, gAudioCtx.permanentCache[i].id);
            sampleBankReloc.sampleBankId1 = gAudioCtx.soundFontList[fontId].sampleBankId1;
            sampleBankReloc.sampleBankId2 = gAudioCtx.soundFontList[fontId].sampleBankId2;

            if (sampleBankReloc.sampleBankId1 != 0xFF) {
                sampleBankReloc.sampleBankId1 =
                    AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankReloc.sampleBankId1);
                sampleBankReloc.medium1 = sampleBankTable->entries[sampleBankReloc.sampleBankId1].medium;
            }

            if (sampleBankReloc.sampleBankId2 != 0xFF) {
                sampleBankReloc.sampleBankId2 =
                    AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankReloc.sampleBankId2);
                sampleBankReloc.medium2 = sampleBankTable->entries[sampleBankReloc.sampleBankId2].medium;
            }
            AudioLoad_PreloadSamplesForFont(fontId, false, &sampleBankReloc);
        }
    }
}

/**
 * original name: __ExtDiskFinishCheck
 */
void AudioLoad_Unused3(void) {
}

/**
 * original name: __ExtDiskInit
 */
void AudioLoad_Unused4(void) {
}

/**
 * original name: __ExtDiskLoad
 */
void AudioLoad_Unused5(void) {
}

/**
 * original name: MK_load
 */
void AudioLoad_ScriptLoad(s32 tableType, s32 id, s8* status) {
    static u32 sLoadIndex = 0;

    sScriptLoadDonePointers[sLoadIndex] = status;
    AudioLoad_AsyncLoad(tableType, id, 0, sLoadIndex, &sScriptLoadQueue);
    sLoadIndex++;
    if (sLoadIndex == 0x10) {
        sLoadIndex = 0;
    }
}

/**
 * original name: MK_FrameWork
 */
void AudioLoad_ProcessScriptLoads(void) {
    u32 temp;
    u32 sp20;
    s8* status;

    if (osRecvMesg(&sScriptLoadQueue, (OSMesg*)&sp20, OS_MESG_NOBLOCK) != -1) {
        temp = sp20 >> 24;
        status = sScriptLoadDonePointers[temp];
        if (status != NULL) {
            *status = 0;
        }
    }
}

/**
 * original name: MK_Init
 */
void AudioLoad_InitScriptLoads(void) {
    osCreateMesgQueue(&sScriptLoadQueue, sScriptLoadMsgBuf, ARRAY_COUNT(sScriptLoadMsgBuf));
}
