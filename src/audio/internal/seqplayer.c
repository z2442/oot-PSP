/**
 * @file audio_seqplayer.c
 * original name: track.c
 *
 * Manages audio sequence players, interprets and executes sequence instructions used to write .seq files
 *
 * Sequence Instructions:
 *   - A customized assembly language based on MIDI
 *   - All sequences are written using these instructions
 *   - There are 3 different sets of instructions
 *        1) Sequence Instructions
 *        2) Channel Instructions
 *        3) Layer Instruction
 *   - All three sets share a common pool of control flow instructions (>= 0xF2).
 *     Otherwise, each set of instructions has its own command interpreter
 */
#include "audio/aseq.h"
#include "array_count.h"
#include "assert.h"
#include "attributes.h"
#include "ultra64.h"
#include "audio.h"
#if defined(TARGET_PSP)
#include <string.h>
#endif

static_assert(MML_VERSION == MML_VERSION_OOT, "This file implements the OoT version of the MML");

#define PORTAMENTO_IS_SPECIAL(x) ((x).mode & 0x80)
#define PORTAMENTO_MODE(x) ((x).mode & ~0x80)

#define PROCESS_SCRIPT_END -1

#if defined(TARGET_PSP)
#define OOT_PSP_AUDIO_NATIVE_PTR_START 0x08000000U
#define OOT_PSP_AUDIO_NATIVE_PTR_END   0x0C000000U

static u8 sOotPspAudioSeqBadSampleLogged;
static u8 sOotPspAudioListBadLogged;
static u8 sOotPspAudioSeqBadChannelLogCount;
static u8 sOotPspAudioSeqBadPtrLogCount;
static SequenceChannel sOotPspAudioSequenceChannels[4][SEQ_NUM_CHANNELS] __attribute__((aligned(16)));

static s32 OotPspAudio_IsAlignedNativePtr(const void* ptr) {
    u32 addr = (u32)ptr;

    return (addr >= OOT_PSP_AUDIO_NATIVE_PTR_START) && (addr < OOT_PSP_AUDIO_NATIVE_PTR_END) && ((addr & 3) == 0);
}

static void OotPspAudio_LogBadChannel(const char* op, SequencePlayer* seqPlayer, s32 channelIndex,
                                      SequenceChannel* channel) {
    if (sOotPspAudioSeqBadChannelLogCount < 8) {
        s32 seqPlayerIsValid = OotPspAudio_IsAlignedNativePtr(seqPlayer);

        sOotPspAudioSeqBadChannelLogCount++;
        osSyncPrintf("oot-psp audio skipped bad channel op=%s seq=%p seqId=%u index=%d channel=%p\n", op, seqPlayer,
                     seqPlayerIsValid ? seqPlayer->seqId : 0xFF, channelIndex, channel);
    }
}

static s32 OotPspAudio_IsNoneSequenceChannel(SequenceChannel* channel) {
    return channel == &gAudioCtx.sequenceChannelNone;
}

static s32 OotPspAudio_IsSafeSequenceChannel(SequenceChannel* channel) {
    return (channel != NULL) && !OotPspAudio_IsNoneSequenceChannel(channel) && OotPspAudio_IsAlignedNativePtr(channel) &&
           IS_SEQUENCE_CHANNEL_VALID(channel);
}

static s32 OotPspAudio_IsOwnedSequenceChannel(SequencePlayer* seqPlayer, SequenceChannel* channel) {
    return OotPspAudio_IsSafeSequenceChannel(channel) && (channel->seqPlayer == seqPlayer);
}

static SequenceChannel* OotPspAudio_GetSafeSequenceChannel(SequencePlayer* seqPlayer, s32 channelIndex,
                                                           const char* op) {
    SequenceChannel* channel;

    if (!OotPspAudio_IsAlignedNativePtr(seqPlayer) || (channelIndex < 0) || (channelIndex >= SEQ_NUM_CHANNELS)) {
        OotPspAudio_LogBadChannel(op, seqPlayer, channelIndex, NULL);
        return NULL;
    }

    channel = seqPlayer->channels[channelIndex];
    if (OotPspAudio_IsNoneSequenceChannel(channel)) {
        return NULL;
    }

    if (!OotPspAudio_IsOwnedSequenceChannel(seqPlayer, channel)) {
        OotPspAudio_LogBadChannel(op, seqPlayer, channelIndex, channel);
        seqPlayer->channels[channelIndex] = &gAudioCtx.sequenceChannelNone;
        return NULL;
    }

    return channel;
}

static void OotPspAudio_LogBadList(const char* op, AudioListItem* list, AudioListItem* item) {
    if (!sOotPspAudioListBadLogged) {
        s32 listIsValid = OotPspAudio_IsAlignedNativePtr(list);

        sOotPspAudioListBadLogged = true;
        osSyncPrintf("oot-psp audio repaired bad %s list=%p prev=%p next=%p item=%p itemPrev=%p itemNext=%p\n", op,
                     list, listIsValid ? list->prev : NULL, listIsValid ? list->next : NULL, item,
                     OotPspAudio_IsAlignedNativePtr(item) ? item->prev : NULL,
                     OotPspAudio_IsAlignedNativePtr(item) ? item->next : NULL);
    }
}

static s32 OotPspAudio_RebuildAudioList(AudioListItem* list) {
    AudioListItem* prev = list;
    AudioListItem* cur;
    s32 count = 0;

    if (!OotPspAudio_IsAlignedNativePtr(list)) {
        return false;
    }

    if (!OotPspAudio_IsAlignedNativePtr(list->next)) {
        list->prev = list;
        list->next = list;
        list->u.count = 0;
        return false;
    }

    cur = list->next;
    while (cur != list) {
        if (!OotPspAudio_IsAlignedNativePtr(cur) || (cur->prev != prev) || (count >= 256)) {
            prev->next = list;
            list->prev = prev;
            list->u.count = count;
            return false;
        }

        prev = cur;
        cur = cur->next;
        count++;
    }

    list->prev = prev;
    list->u.count = count;
    return true;
}

static s32 OotPspAudio_PrepareAudioListTail(const char* op, AudioListItem* list) {
    AudioListItem* item;

    if (!OotPspAudio_IsAlignedNativePtr(list)) {
        OotPspAudio_LogBadList(op, list, NULL);
        return false;
    }

    item = list->prev;
    if ((item == NULL) || !OotPspAudio_IsAlignedNativePtr(item) ||
        ((item != list) && (!OotPspAudio_IsAlignedNativePtr(item->prev) || (item->next != list)))) {
        OotPspAudio_LogBadList(op, list, item);
        OotPspAudio_RebuildAudioList(list);
    }

    item = list->prev;
    return (item != NULL) && OotPspAudio_IsAlignedNativePtr(item) &&
           ((item == list) || (OotPspAudio_IsAlignedNativePtr(item->prev) && (item->next == list)));
}

static s32 OotPspAudio_IsSafeTunedSample(TunedSample* tunedSample) {
    Sample* sample;

    if (tunedSample == NULL) {
        return true;
    }

    if (!OotPspAudio_IsAlignedNativePtr(tunedSample)) {
        return false;
    }

    sample = tunedSample->sample;
    if (!OotPspAudio_IsAlignedNativePtr(sample) || !OotPspAudio_IsAlignedNativePtr(sample->loop) ||
        (sample->codec > CODEC_S16) || (sample->medium > MEDIUM_DISK_DRIVE)) {
        return false;
    }

    if (((sample->codec == CODEC_ADPCM) || (sample->codec == CODEC_SMALL_ADPCM)) &&
        !OotPspAudio_IsAlignedNativePtr(sample->book)) {
        return false;
    }

    return true;
}

static void OotPspAudio_DropBadSeqSample(SequenceLayer* layer, s32 instOrWave, u8 semitone) {
    TunedSample* tunedSample = layer->tunedSample;

    if (!sOotPspAudioSeqBadSampleLogged) {
        sOotPspAudioSeqBadSampleLogged = true;
        osSyncPrintf("oot-psp audio dropped bad seq sample font=%u inst=%d semitone=%u tuned=%p\n",
                     layer->channel != NULL ? layer->channel->fontId : 0xFF, instOrWave, semitone, tunedSample);
    }

    layer->muted = true;
    layer->tunedSample = NULL;
    layer->delay2 = layer->delay;
}
#endif

typedef enum PortamentoMode {
    /* 0 */ PORTAMENTO_MODE_OFF,
    /* 1 */ PORTAMENTO_MODE_1,
    /* 2 */ PORTAMENTO_MODE_2,
    /* 3 */ PORTAMENTO_MODE_3,
    /* 4 */ PORTAMENTO_MODE_4,
    /* 5 */ PORTAMENTO_MODE_5
} PortamentoMode;

u8 AudioSeq_ScriptReadU8(SeqScriptState* state);
s16 AudioSeq_ScriptReadS16(SeqScriptState* state);
u16 AudioSeq_ScriptReadCompressedU16(SeqScriptState* state);

void AudioSeq_SeqLayerProcessScriptStep1(SequenceLayer* layer);
s32 AudioSeq_SeqLayerProcessScriptStep2(SequenceLayer* layer);
s32 AudioSeq_SeqLayerProcessScriptStep3(SequenceLayer* layer, s32 cmd);
s32 AudioSeq_SeqLayerProcessScriptStep4(SequenceLayer* layer, s32 cmd);
s32 AudioSeq_SeqLayerProcessScriptStep5(SequenceLayer* layer, s32 sameTunedSample);

u8 AudioSeq_GetInstrument(SequenceChannel* channel, u8 instId, Instrument** instOut, AdsrSettings* adsr);

static u16 AudioSeq_ReadU16(const u8* ptr) {
    return ((u16)ptr[0] << 8) | ptr[1];
}

static s32 AudioSeq_CanIndexWithTR(s8 value) {
    return value != SEQ_IO_VAL_NONE;
}

#if defined(TARGET_PSP)
static u32 OotPspAudio_GetRealSequenceId(u32 seqId) {
    AudioTable* table = gAudioCtx.sequenceTable;

    if (!OotPspAudio_IsAlignedNativePtr(table) || (seqId >= (u32)table->header.numEntries)) {
        return seqId;
    }

    if (table->entries[seqId].size == 0) {
        seqId = table->entries[seqId].romAddr;
    }

    return seqId;
}

static s32 OotPspAudio_GetSequenceSpan(SequencePlayer* seqPlayer, u8** baseOut, u32* lenOut) {
    AudioTable* table = gAudioCtx.sequenceTable;
    void* cachedSeq;
    u32 seqId;
    u32 len;

    if (!OotPspAudio_IsAlignedNativePtr(seqPlayer) || !OotPspAudio_IsAlignedNativePtr(table) ||
        (seqPlayer->seqData == NULL)) {
        return false;
    }

    seqId = OotPspAudio_GetRealSequenceId(seqPlayer->seqId);
    if (seqId >= (u32)table->header.numEntries) {
        return false;
    }

    len = table->entries[seqId].size;
    if (len == 0) {
        return false;
    }

    cachedSeq = AudioHeap_SearchCaches(SEQUENCE_TABLE, CACHE_EITHER, seqId);
    if (cachedSeq != seqPlayer->seqData) {
        return false;
    }

    *baseOut = seqPlayer->seqData;
    *lenOut = len;
    return true;
}

static void OotPspAudio_LogBadSeqPtr(const char* op, SequencePlayer* seqPlayer, const void* ptr, u32 size) {
    u8* base = NULL;
    u32 len = 0;
    s32 seqPlayerIsValid = OotPspAudio_IsAlignedNativePtr(seqPlayer);

    if (sOotPspAudioSeqBadPtrLogCount >= 12) {
        return;
    }

    if (seqPlayerIsValid) {
        OotPspAudio_GetSequenceSpan(seqPlayer, &base, &len);
    }

    sOotPspAudioSeqBadPtrLogCount++;
    osSyncPrintf("oot-psp audio stopped bad seq ptr op=%s seq=%p seqId=%u ptr=%p size=%u base=%p len=%u\n", op,
                 seqPlayer, seqPlayerIsValid ? seqPlayer->seqId : 0xFF, ptr, size, base, len);
}

static s32 OotPspAudio_IsSeqRange(SequencePlayer* seqPlayer, const void* ptr, u32 size) {
    u8* base;
    u32 len;
    u32 addr;
    u32 baseAddr;

    if ((ptr == NULL) || !OotPspAudio_GetSequenceSpan(seqPlayer, &base, &len) || (size > len)) {
        return false;
    }

    addr = (u32)ptr;
    baseAddr = (u32)base;
    return (addr >= baseAddr) && (addr <= (baseAddr + len - size));
}

static u8* OotPspAudio_GetSeqPtr(SequencePlayer* seqPlayer, u32 offset, u32 size, const char* op) {
    u8* base;
    u32 len;

    if (!OotPspAudio_GetSequenceSpan(seqPlayer, &base, &len) || (size > len) || (offset > (len - size))) {
        OotPspAudio_LogBadSeqPtr(
            op, seqPlayer,
            OotPspAudio_IsAlignedNativePtr(seqPlayer) ? (void*)((u32)seqPlayer->seqData + offset) : NULL, size);
        return NULL;
    }

    return base + offset;
}

static s32 OotPspAudio_ValidateSeqPtr(SequencePlayer* seqPlayer, const void* ptr, u32 size, const char* op) {
    if (!OotPspAudio_IsSeqRange(seqPlayer, ptr, size)) {
        OotPspAudio_LogBadSeqPtr(op, seqPlayer, ptr, size);
        return false;
    }

    return true;
}

static s32 OotPspAudio_ValidateSeqCompressedU16(SequencePlayer* seqPlayer, SeqScriptState* state, const char* op) {
    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, op)) {
        return false;
    }

    if (((*state->pc & 0x80) != 0) && !OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 2, op)) {
        return false;
    }

    return true;
}

static s32 OotPspAudio_ReadSeqU16(SequencePlayer* seqPlayer, u32 offset, const char* op, u16* value) {
    u8* data = OotPspAudio_GetSeqPtr(seqPlayer, offset, 2, op);

    if (data == NULL) {
        return false;
    }

    *value = AudioSeq_ReadU16(data);
    return true;
}

static s32 OotPspAudio_SetChannelDynTable(SequencePlayer* seqPlayer, SequenceChannel* channel, u32 offset,
                                          const char* op) {
    u8* data = OotPspAudio_GetSeqPtr(seqPlayer, offset, 1, op);

    if (data == NULL) {
        channel->dynTable = NULL;
        return false;
    }

    channel->dynTable = (void*)data;
    return true;
}

static s32 OotPspAudio_ReadDynTableU16(SequencePlayer* seqPlayer, SequenceChannel* channel, s8 index, const char* op,
                                       u16* value) {
    u8* data;
    s32 offset;

    if (channel->dynTable == NULL) {
        OotPspAudio_LogBadSeqPtr(op, seqPlayer, NULL, 2);
        return false;
    }

    offset = index * 2;
    data = (u8*)((u32)channel->dynTable + offset);
    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, data, 2, op)) {
        channel->dynTable = NULL;
        return false;
    }

    *value = AudioSeq_ReadU16(data);
    return true;
}

static s32 OotPspAudio_ReadDynTableU8(SequencePlayer* seqPlayer, SequenceChannel* channel, s8 index, const char* op,
                                      u8* value) {
    u8* data;

    if (channel->dynTable == NULL) {
        OotPspAudio_LogBadSeqPtr(op, seqPlayer, NULL, 1);
        return false;
    }

    data = (u8*)((u32)channel->dynTable + index);
    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, data, 1, op)) {
        channel->dynTable = NULL;
        return false;
    }

    *value = *data;
    return true;
}
#endif

/**
 * sSeqInstructionArgsTable is a table for each sequence instruction
 * that contains both how many arguments an instruction takes, as well
 * as the type of each argument
 *
 * sSeqInstructionArgsTable is bitpacked as follows:
 * abcUUUnn
 *
 * n - number of arguments that the sequence instruction takes
 *
 * a - bitFlag for the type of arg0 if it exists
 * b - bitFlag for the type of arg1 if it exists
 * c - bitFlag for the type of arg2 if it exists
 *
 * bitFlag on - argument is s16
 * bitFlag off - argument is u8
 *
 * U - Unused
 */

// CMD_ARGS_(NUMBER_OF_ARGS)
#define CMD_ARGS_0() 0
#define CMD_ARGS_1(arg0Type) (((sizeof(arg0Type) - 1) << 7) | 1)
#define CMD_ARGS_2(arg0Type, arg1Type) (((sizeof(arg0Type) - 1) << 7) | ((sizeof(arg1Type) - 1) << 6) | 2)
#define CMD_ARGS_3(arg0Type, arg1Type, arg2Type) \
    (((sizeof(arg0Type) - 1) << 7) | ((sizeof(arg1Type) - 1) << 6) | ((sizeof(arg2Type) - 1) << 5) | 3)

u8 sSeqInstructionArgsTable[] = {
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_LDFILTER
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_FREEFILTER
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_LDSEQTOPTR
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_FILTER
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_PTRTODYNTBL
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_DYNTBLTOPTR
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_DYNTBLV
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_RANDTOPTR
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_RAND
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_RANDVEL
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_RANDGATE
    CMD_ARGS_2(u8, s16),    // ASEQ_OP_CHAN_COMBFILTER
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_PTRADD
    CMD_ARGS_2(s16, s16),   // ASEQ_OP_CHAN_RANDPTR
    CMD_ARGS_0(),           // 0xBE
    CMD_ARGS_0(),           // 0xBF
    CMD_ARGS_0(),           // 0xC0
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_INSTR
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_DYNTBL
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_SHORT
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_NOSHORT
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_DYNTBLLOOKUP
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_FONT
    CMD_ARGS_2(u8, s16),    // ASEQ_OP_CHAN_STSEQ
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_SUB
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_AND
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_MUTEBHV
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_LDSEQ
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_LDI
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_STOPCHAN
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_LDPTR
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_STPTRTOSEQ
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_EFFECTS
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_NOTEALLOC
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_SUSTAIN
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_BEND
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_REVERB
    CMD_ARGS_1(u8),         // 0xD5
    CMD_ARGS_1(u8),         // 0xD6
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_VIBFREQ
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_VIBDEPTH
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_RELEASERATE
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_ENV
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_TRANSPOSE
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_PANWEIGHT
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_PAN
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_FREQSCALE
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_VOL
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_VOLEXP
    CMD_ARGS_3(u8, u8, u8), // ASEQ_OP_CHAN_VIBFREQGRAD
    CMD_ARGS_3(u8, u8, u8), // ASEQ_OP_CHAN_VIBDEPTHGRAD
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_VIBDELAY
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_DYNCALL
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_REVERBIDX
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_SAMPLEBOOK
    CMD_ARGS_1(s16),        // ASEQ_OP_CHAN_LDPARAMS
    CMD_ARGS_3(u8, u8, u8), // ASEQ_OP_CHAN_PARAMS
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_NOTEPRI
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_STOP
    CMD_ARGS_2(u8, u8),     // ASEQ_OP_CHAN_FONTINSTR
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_VIBRESET
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_GAIN
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_BENDFINE
    CMD_ARGS_2(s16, u8),    // 0xEF
    CMD_ARGS_0(),           // ASEQ_OP_CHAN_FREENOTELIST
    CMD_ARGS_1(u8),         // ASEQ_OP_CHAN_ALLOCNOTELIST
    // Control flow instructions (>= ASEQ_OP_CONTROL_FLOW_FIRST) can only have 0 or 1 args
    CMD_ARGS_1(u8),  // ASEQ_OP_RBLTZ
    CMD_ARGS_1(u8),  // ASEQ_OP_RBEQZ
    CMD_ARGS_1(u8),  // ASEQ_OP_RJUMP
    CMD_ARGS_1(s16), // ASEQ_OP_BGEZ
    CMD_ARGS_0(),    // ASEQ_OP_BREAK
    CMD_ARGS_0(),    // ASEQ_OP_LOOPEND
    CMD_ARGS_1(u8),  // ASEQ_OP_LOOP
    CMD_ARGS_1(s16), // ASEQ_OP_BLTZ
    CMD_ARGS_1(s16), // ASEQ_OP_BEQZ
    CMD_ARGS_1(s16), // ASEQ_OP_JUMP
    CMD_ARGS_1(s16), // ASEQ_OP_CALL
    CMD_ARGS_0(),    // ASEQ_OP_DELAY
    CMD_ARGS_0(),    // ASEQ_OP_DELAY1
    CMD_ARGS_0(),    // ASEQ_OP_END
};

#if defined(TARGET_PSP)
static u32 OotPspAudio_GetControlFlowArgSize(u8 cmd) {
    u8 highBits = sSeqInstructionArgsTable[cmd - 0xB0];

    if ((highBits & 3) == 0) {
        return 0;
    }

    return (highBits & 0x80) ? 2 : 1;
}

static s32 OotPspAudio_ValidateControlFlowArg(SequencePlayer* seqPlayer, SeqScriptState* state, u8 cmd,
                                              const char* op) {
    u32 size = OotPspAudio_GetControlFlowArgSize(cmd);

    return (size == 0) || OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, size, op);
}
#endif

/**
 * Read and return the argument from the sequence script for a control flow instruction.
 * Control flow instructions (>= ASEQ_OP_CONTROL_FLOW_FIRST) can only have 0 or 1 args.
 * @return the argument value for a control flow instruction, or 0 if there is no argument
 *
 * original name: Convert_Com
 */
u16 AudioSeq_GetScriptControlFlowArgument(SeqScriptState* state, u8 cmd) {
    u8 highBits = sSeqInstructionArgsTable[cmd - 0xB0];
    u8 lowBits = highBits & 3;
    u16 cmdArg = 0;

    // only 1 argument
    if (lowBits == 1) {
        if (!(highBits & 0x80)) {
            cmdArg = AudioSeq_ScriptReadU8(state);
        } else {
            cmdArg = AudioSeq_ScriptReadS16(state);
        }
    }

    return cmdArg;
}

/**
 * Read and execute the control flow sequence instructions
 * @return number of frames until next instruction. -1 signals termination
 *
 * original name: Common_Com
 */
s32 AudioSeq_HandleScriptFlowControl(SequencePlayer* seqPlayer, SeqScriptState* state, s32 cmd, s32 cmdArg) {
#if defined(TARGET_PSP)
    u8* data;
#endif

    switch (cmd) {
        case ASEQ_OP_END:
            if (state->depth == 0) {
                return PROCESS_SCRIPT_END;
            }
#if defined(TARGET_PSP)
            if ((state->depth > ARRAY_COUNT(state->stack)) ||
                !OotPspAudio_ValidateSeqPtr(seqPlayer, state->stack[state->depth - 1], 1, "flow-end")) {
                return PROCESS_SCRIPT_END;
            }
#endif
            state->pc = state->stack[--state->depth];
            break;

        case ASEQ_OP_DELAY:
#if defined(TARGET_PSP)
            if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "flow-delay")) {
                return PROCESS_SCRIPT_END;
            }
#endif
            return AudioSeq_ScriptReadCompressedU16(state);

        case ASEQ_OP_DELAY1:
            return 1;

        case ASEQ_OP_CALL:
#if defined(TARGET_PSP)
            if (state->depth >= ARRAY_COUNT(state->stack)) {
                return PROCESS_SCRIPT_END;
            }
            data = OotPspAudio_GetSeqPtr(seqPlayer, (u16)cmdArg, 1, "flow-call");
            if (data == NULL) {
                return PROCESS_SCRIPT_END;
            }
            state->stack[state->depth++] = state->pc;
            state->pc = data;
#else
            state->stack[state->depth++] = state->pc;
            state->pc = seqPlayer->seqData + (u16)cmdArg;
#endif
            break;

        case ASEQ_OP_LOOP:
#if defined(TARGET_PSP)
            if (state->depth >= ARRAY_COUNT(state->stack)) {
                return PROCESS_SCRIPT_END;
            }
#endif
            state->remLoopIters[state->depth] = cmdArg;
            state->stack[state->depth++] = state->pc;
            break;

        case ASEQ_OP_LOOPEND:
#if defined(TARGET_PSP)
            if ((state->depth == 0) || (state->depth > ARRAY_COUNT(state->stack)) ||
                !OotPspAudio_ValidateSeqPtr(seqPlayer, state->stack[state->depth - 1], 1, "flow-loopend")) {
                return PROCESS_SCRIPT_END;
            }
#endif
            state->remLoopIters[state->depth - 1]--;
            if (state->remLoopIters[state->depth - 1] != 0) {
                state->pc = state->stack[state->depth - 1];
            } else {
                state->depth--;
            }
            break;

        case ASEQ_OP_BREAK:
#if defined(TARGET_PSP)
            if (state->depth == 0) {
                return PROCESS_SCRIPT_END;
            }
#endif
            state->depth--;
            break;

        case ASEQ_OP_BGEZ:
        case ASEQ_OP_BLTZ:
        case ASEQ_OP_BEQZ:
        case ASEQ_OP_JUMP:
            if (cmd == ASEQ_OP_BEQZ && state->value != 0) {
                break;
            }
            if (cmd == ASEQ_OP_BLTZ && state->value >= 0) {
                break;
            }
            if (cmd == ASEQ_OP_BGEZ && state->value < 0) {
                break;
            }
#if defined(TARGET_PSP)
            data = OotPspAudio_GetSeqPtr(seqPlayer, (u16)cmdArg, 1, "flow-jump");
            if (data == NULL) {
                return PROCESS_SCRIPT_END;
            }
            state->pc = data;
#else
            state->pc = seqPlayer->seqData + (u16)cmdArg;
#endif
            break;

        case ASEQ_OP_RBLTZ:
        case ASEQ_OP_RBEQZ:
        case ASEQ_OP_RJUMP:
            if (cmd == ASEQ_OP_RBEQZ && state->value != 0) {
                break;
            }
            if (cmd == ASEQ_OP_RBLTZ && state->value >= 0) {
                break;
            }
#if defined(TARGET_PSP)
            data = state->pc + (s8)(cmdArg & 0xFF);
            if (!OotPspAudio_ValidateSeqPtr(seqPlayer, data, 1, "flow-rjump")) {
                return PROCESS_SCRIPT_END;
            }
            state->pc = data;
#else
            state->pc += (s8)(cmdArg & 0xFF);
#endif
            break;
    }

    return 0;
}

/**
 * original name: Nas_InitSubTrack
 */
void AudioSeq_InitSequenceChannel(SequenceChannel* channel) {
    s32 i;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(channel)) {
        OotPspAudio_LogBadChannel("init-channel", NULL, -1, channel);
        return;
    }
#endif

    if (channel == &gAudioCtx.sequenceChannelNone) {
        return;
    }

    channel->enabled = false;
    channel->finished = false;
    channel->stopScript = false;
    channel->muted = false;
    channel->hasInstrument = false;
    channel->stereoHeadsetEffects = false;
    channel->transposition = 0;
    channel->largeNotes = false;
    channel->bookOffset = 0;
    channel->stereo.asByte = 0;
    channel->changes.asByte = 0xFF;
    channel->scriptState.depth = 0;
    channel->scriptState.value = 0;
    channel->scriptState.pc = NULL;
    channel->dynTable = NULL;
    channel->unk_22 = 0;
    channel->newPan = 0x40;
    channel->panChannelWeight = 0x80;
    channel->velocityRandomVariance = 0;
    channel->gateTimeRandomVariance = 0;
    channel->noteUnused = NULL;
    channel->reverbIndex = 0;
    channel->targetReverbVol = 0;
    channel->gain = 0;
    channel->notePriority = 3;
    channel->someOtherPriority = 1;
    channel->delay = 0;
    channel->adsr.envelope = gDefaultEnvelope;
    channel->adsr.decayIndex = 0xF0;
    channel->adsr.sustain = 0;
#if defined(TARGET_PSP)
    channel->adsr.envelopeBigEndian = false;
#endif
    channel->vibratoRateTarget = 0x800;
    channel->vibratoRateStart = 0x800;
    channel->vibratoDepthTarget = 0;
    channel->vibratoDepthStart = 0;
    channel->vibratoRateChangeDelay = 0;
    channel->vibratoDepthChangeDelay = 0;
    channel->vibratoDelay = 0;
    channel->filter = NULL;
    channel->combFilterGain = 0;
    channel->combFilterSize = 0;
    channel->volume = 1.0f;
    channel->volumeScale = 1.0f;
    channel->freqScale = 1.0f;

    for (i = 0; i < ARRAY_COUNT(channel->seqScriptIO); i++) {
        channel->seqScriptIO[i] = SEQ_IO_VAL_NONE;
    }

    channel->unused = false;
    Audio_InitNoteLists(&channel->notePool);
}

/**
 * original name: Nas_EntryNoteTrack
 */
s32 AudioSeq_SeqChannelSetLayer(SequenceChannel* channel, s32 layerIndex) {
    SequenceLayer* layer;
    s32 pad;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSequenceChannel(channel) || (layerIndex < 0) ||
        (layerIndex >= (s32)ARRAY_COUNT(channel->layers))) {
        OotPspAudio_LogBadChannel("set-layer", NULL, layerIndex, channel);
        return -1;
    }
#endif

    if (channel->layers[layerIndex] == NULL) {
        layer = AudioSeq_AudioListPopBack(&gAudioCtx.layerFreeList);
        channel->layers[layerIndex] = layer;
        if (layer == NULL) {
            channel->layers[layerIndex] = NULL;
            return -1;
        }
    } else {
        Audio_SeqLayerNoteDecay(channel->layers[layerIndex]);
    }

    layer = channel->layers[layerIndex];

    layer->channel = channel;
    layer->adsr = channel->adsr;
    layer->adsr.decayIndex = 0;
    layer->enabled = true;
    layer->finished = false;
    layer->muted = false;
    layer->continuousNotes = false;
    layer->bit3 = false;
    layer->ignoreDrumPan = false;
    layer->bit1 = false;
    layer->notePropertiesNeedInit = false;
    layer->stereo.asByte = 0;
    layer->portamento.mode = PORTAMENTO_MODE_OFF;
    layer->scriptState.depth = 0;
    layer->gateTime = 0x80;
    layer->pan = 0x40;
    layer->transposition = 0;
    layer->delay = 0;
    layer->gateDelay = 0;
    layer->delay2 = 0;
    layer->note = NULL;
    layer->instrument = NULL;
    layer->freqScale = 1.0f;
    layer->bend = 1.0f;
    layer->velocitySquare2 = 0.0f;
    layer->instOrWave = 0xFF;

    return 0;
}

/**
 * original name: Nas_ReleaseNoteTrack
 */
void AudioSeq_SeqLayerDisable(SequenceLayer* layer) {
#if defined(TARGET_PSP)
    if ((layer != NULL) && !OotPspAudio_IsAlignedNativePtr(layer)) {
        OotPspAudio_LogBadChannel("layer-disable", NULL, -1, NULL);
        return;
    }

    if ((layer != NULL) && (layer->channel != &gAudioCtx.sequenceChannelNone) &&
        !OotPspAudio_IsSafeSequenceChannel(layer->channel)) {
        OotPspAudio_LogBadChannel("layer-channel", NULL, -1, layer->channel);
        layer->channel = &gAudioCtx.sequenceChannelNone;
    }
#endif

    if (layer != NULL) {
        if (layer->channel != &gAudioCtx.sequenceChannelNone && layer->channel->seqPlayer->finished == 1) {
            Audio_SeqLayerNoteRelease(layer);
        } else {
            Audio_SeqLayerNoteDecay(layer);
        }
        layer->enabled = false;
        layer->finished = true;
    }
}

/**
 * original name: Nas_CloseNoteTrack
 */
void AudioSeq_SeqLayerFree(SequenceChannel* channel, s32 layerIndex) {
    SequenceLayer* layer;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSequenceChannel(channel) || (layerIndex < 0) ||
        (layerIndex >= (s32)ARRAY_COUNT(channel->layers))) {
        OotPspAudio_LogBadChannel("layer-free", NULL, layerIndex, channel);
        return;
    }
#endif
    layer = channel->layers[layerIndex];

    if (layer != NULL) {
        AudioSeq_AudioListPushBack(&gAudioCtx.layerFreeList, &layer->listItem);
        AudioSeq_SeqLayerDisable(layer);
        channel->layers[layerIndex] = NULL;
    }
}

/**
 * original name: Nas_ReleaseSubTrack
 */
void AudioSeq_SequenceChannelDisable(SequenceChannel* channel) {
    s32 i;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSequenceChannel(channel)) {
        OotPspAudio_LogBadChannel("disable", NULL, -1, channel);
        return;
    }
#endif

    for (i = 0; i < 4; i++) {
        AudioSeq_SeqLayerFree(channel, i);
    }

    Audio_NotePoolClear(&channel->notePool);
    channel->enabled = false;
    channel->finished = true;
}

/**
 * original name: Nas_AllocSub
 */
void AudioSeq_SequencePlayerSetupChannels(SequencePlayer* seqPlayer, u16 channelBits) {
    SequenceChannel* channel;
    s32 i;

    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
        if (channelBits & 1) {
#if defined(TARGET_PSP)
            channel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, i, "setup-channel");
            if (channel != NULL) {
                channel->fontId = seqPlayer->defaultFont;
                channel->muteBehavior = seqPlayer->muteBehavior;
                channel->noteAllocPolicy = seqPlayer->noteAllocPolicy;
            }
#else
            channel = seqPlayer->channels[i];
            channel->fontId = seqPlayer->defaultFont;
            channel->muteBehavior = seqPlayer->muteBehavior;
            channel->noteAllocPolicy = seqPlayer->noteAllocPolicy;
#endif
        }
        channelBits = channelBits >> 1;
    }
}

/**
 * original name: Nas_DeAllocSub
 */
void AudioSeq_SequencePlayerDisableChannels(SequencePlayer* seqPlayer, u16 channelBitsUnused) {
    SequenceChannel* channel;
    s32 i;

    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
#if defined(TARGET_PSP)
        channel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, i, "disable-channel");
        if (channel != NULL) {
            AudioSeq_SequenceChannelDisable(channel);
        }
#else
        channel = seqPlayer->channels[i];
        if (IS_SEQUENCE_CHANNEL_VALID(channel) == 1) {
            AudioSeq_SequenceChannelDisable(channel);
        }
#endif
    }
}

/**
 * original name: Nas_OpenSub
 */
void AudioSeq_SequenceChannelEnable(SequencePlayer* seqPlayer, u8 channelIndex, void* script) {
#if defined(TARGET_PSP)
    SequenceChannel* channel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, channelIndex, "enable");
#else
    SequenceChannel* channel = seqPlayer->channels[channelIndex];
#endif
    s32 i;

#if defined(TARGET_PSP)
    if (channel == NULL) {
        return;
    }
#endif

    channel->enabled = true;
    channel->finished = false;
    channel->scriptState.depth = 0;
    channel->scriptState.value = 0;
    channel->scriptState.pc = script;
    channel->dynTable = NULL;
    channel->unk_22 = 0;
    channel->delay = 0;

    for (i = 0; i < ARRAY_COUNT(channel->layers); i++) {
        if (channel->layers[i] != NULL) {
            AudioSeq_SeqLayerFree(channel, i);
        }
    }
}

/**
 * original name: Nas_ReleaseGroup_Force
 */
void AudioSeq_SequencePlayerDisableAsFinished(SequencePlayer* seqPlayer) {
    seqPlayer->finished = true;
    AudioSeq_SequencePlayerDisable(seqPlayer);
}

/**
 * original name: Nas_ReleaseGroup
 */
void AudioSeq_SequencePlayerDisable(SequencePlayer* seqPlayer) {
    s32 finished = 0;

#if !(OOT_VERSION < NTSC_1_1 || !PLATFORM_N64)
    if (seqPlayer->finished == 1) {
        finished = 1;
    }
#endif

    AudioSeq_SequencePlayerDisableChannels(seqPlayer, 0xFFFF);
    Audio_NotePoolClear(&seqPlayer->notePool);
    if (!seqPlayer->enabled) {
        return;
    }

    seqPlayer->enabled = false;
    seqPlayer->finished = true;

    if (AudioLoad_IsSeqLoadComplete(seqPlayer->seqId)) {
        AudioLoad_SetSeqLoadStatus(seqPlayer->seqId, LOAD_STATUS_DISCARDABLE);
    }

    if (AudioLoad_IsFontLoadComplete(seqPlayer->defaultFont)) {
#if !(OOT_VERSION < NTSC_1_1 || !PLATFORM_N64)
        if (finished == 1) {
            AudioHeap_ReleaseNotesForFont(seqPlayer->defaultFont);
        }
#endif
        AudioLoad_SetFontLoadStatus(seqPlayer->defaultFont, LOAD_STATUS_MAYBE_DISCARDABLE);
    }

    if (seqPlayer->defaultFont == gAudioCtx.fontCache.temporary.entries[0].id) {
        gAudioCtx.fontCache.temporary.nextSide = 0;
    } else if (seqPlayer->defaultFont == gAudioCtx.fontCache.temporary.entries[1].id) {
        gAudioCtx.fontCache.temporary.nextSide = 1;
    }
}

/**
 * original name: Nas_AddList
 */
void AudioSeq_AudioListPushBack(AudioListItem* list, AudioListItem* item) {
#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(item)) {
        OotPspAudio_LogBadList("push-item", list, item);
        return;
    }
#endif

    if (item->prev == NULL) {
#if defined(TARGET_PSP)
        if (!OotPspAudio_PrepareAudioListTail("push", list)) {
            return;
        }
#endif
        list->prev->next = item;
        item->prev = list->prev;
        item->next = list;
        list->prev = item;
        list->u.count++;
        item->pool = list->pool;
    }
}

/**
 * original name: Nas_GetList
 */
void* AudioSeq_AudioListPopBack(AudioListItem* list) {
    AudioListItem* item;

#if defined(TARGET_PSP)
    if (!OotPspAudio_PrepareAudioListTail("pop", list)) {
        return NULL;
    }
#endif
    item = list->prev;

    if (item == list) {
        return NULL;
    }

    item->prev->next = list;
    list->prev = item->prev;
    item->prev = NULL;
    list->u.count--;

    return item->u.value;
}

/**
 * original name: Nas_InitNoteList
 */
void AudioSeq_InitLayerFreelist(void) {
    s32 i;

    gAudioCtx.layerFreeList.prev = &gAudioCtx.layerFreeList;
    gAudioCtx.layerFreeList.next = &gAudioCtx.layerFreeList;
    gAudioCtx.layerFreeList.u.count = 0;
    gAudioCtx.layerFreeList.pool = NULL;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.sequenceLayers); i++) {
        gAudioCtx.sequenceLayers[i].listItem.u.value = &gAudioCtx.sequenceLayers[i];
        gAudioCtx.sequenceLayers[i].listItem.prev = NULL;
        AudioSeq_AudioListPushBack(&gAudioCtx.layerFreeList, &gAudioCtx.sequenceLayers[i].listItem);
    }
}

/**
 * original name: Nas_ReadByteData
 */
u8 AudioSeq_ScriptReadU8(SeqScriptState* state) {
    return *(state->pc++);
}

/**
 * original name: Nas_ReadWordData
 */
s16 AudioSeq_ScriptReadS16(SeqScriptState* state) {
    s16 ret = AudioSeq_ReadU16(state->pc);

    state->pc += 2;
    return ret;
}

/**
 * original name: Nas_ReadLengthData
 */
u16 AudioSeq_ScriptReadCompressedU16(SeqScriptState* state) {
    u16 ret = *(state->pc++);

    if (ret & 0x80) {
        ret = (ret << 8) & 0x7F00;
        ret = *(state->pc++) | ret;
    }
    return ret;
}

/**
 * original name: Nas_NoteSeq
 */
void AudioSeq_SeqLayerProcessScript(SequenceLayer* layer) {
    s32 cmd;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(layer)) {
        OotPspAudio_LogBadChannel("layer-script", NULL, -1, NULL);
        return;
    }
#endif

    if (!layer->enabled) {
        return;
    }

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSequenceChannel(layer->channel) ||
        !OotPspAudio_ValidateSeqPtr(layer->channel->seqPlayer, layer->scriptState.pc, 1, "layer-start")) {
        AudioSeq_SeqLayerDisable(layer);
        return;
    }
#endif

    if (layer->delay > 1) {
        layer->delay--;
        if (!layer->muted && (layer->delay <= layer->gateDelay)) {
            Audio_SeqLayerNoteDecay(layer);
            layer->muted = true;
        }
        return;
    }

    AudioSeq_SeqLayerProcessScriptStep1(layer);

    cmd = AudioSeq_SeqLayerProcessScriptStep2(layer);
    if (cmd == PROCESS_SCRIPT_END) {
        return;
    }

    cmd = AudioSeq_SeqLayerProcessScriptStep3(layer, cmd);

    if (cmd != PROCESS_SCRIPT_END) {
        // returns `sameSound` instead of a command
        cmd = AudioSeq_SeqLayerProcessScriptStep4(layer, cmd);
    }

    if (cmd != PROCESS_SCRIPT_END) {
        AudioSeq_SeqLayerProcessScriptStep5(layer, cmd);
    }

    if (layer->muted == true) {
        if ((layer->note != NULL) || layer->continuousNotes) {
            Audio_SeqLayerNoteDecay(layer);
        }
    }
}

/**
 * original name: __Stop_Note
 */
void AudioSeq_SeqLayerProcessScriptStep1(SequenceLayer* layer) {
    if (!layer->continuousNotes) {
        Audio_SeqLayerNoteDecay(layer);
    } else if (layer->note != NULL && layer->note->playbackState.wantedParentLayer == layer) {
        Audio_SeqLayerNoteDecay(layer);
    }

    if (PORTAMENTO_MODE(layer->portamento) == PORTAMENTO_MODE_1 ||
        PORTAMENTO_MODE(layer->portamento) == PORTAMENTO_MODE_2) {
        layer->portamento.mode = PORTAMENTO_MODE_OFF;
    }
    layer->notePropertiesNeedInit = true;
}

/**
 * original name: __SetChannel
 */
s32 AudioSeq_SeqLayerProcessScriptStep5(SequenceLayer* layer, s32 sameTunedSample) {
    Note* note;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeTunedSample(layer->tunedSample)) {
        OotPspAudio_DropBadSeqSample(layer, -1, layer->semitone);
        return PROCESS_SCRIPT_END;
    }
#endif

    if (!layer->muted && (layer->tunedSample != NULL) && (layer->tunedSample->sample->codec == CODEC_S16_INMEMORY) &&
        (layer->tunedSample->sample->medium != MEDIUM_RAM)) {
        layer->muted = true;
        return PROCESS_SCRIPT_END;
    }

    if (layer->continuousNotes == true && layer->bit1 == 1) {
        return 0;
    }

    if (layer->continuousNotes == true && layer->note != NULL && layer->bit3 && sameTunedSample == true &&
        layer->note->playbackState.parentLayer == layer) {
        if (layer->tunedSample == NULL) {
            Audio_InitSyntheticWave(layer->note, layer);
        }
    } else {
        if (!sameTunedSample) {
            Audio_SeqLayerNoteDecay(layer);
        }

        layer->note = Audio_AllocNote(layer);
        if (layer->note != NULL && layer->note->playbackState.parentLayer == layer) {
            Audio_NoteVibratoInit(layer->note);
        }
    }

    if (layer->note != NULL && layer->note->playbackState.parentLayer == layer) {
        note = layer->note;

        Audio_NotePortamentoInit(note);
    }

    return 0;
}

/**
 * original name: __Command_Seq
 */
s32 AudioSeq_SeqLayerProcessScriptStep2(SequenceLayer* layer) {
    SequenceChannel* channel = layer->channel;
    SeqScriptState* state = &layer->scriptState;
    SequencePlayer* seqPlayer = channel->seqPlayer;
    u8 cmd;
    u8 cmdArg8;
    u16 cmdArg16;
    u16 velocity;

    while (true) {
#if defined(TARGET_PSP)
        if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-cmd")) {
            AudioSeq_SeqLayerDisable(layer);
            return PROCESS_SCRIPT_END;
        }
#endif
        cmd = AudioSeq_ScriptReadU8(state);

        // To be processed in AudioSeq_SeqLayerProcessScriptStep3
        if (cmd <= 0xC0) {
            return cmd;
        }

        // Control Flow Commands
        if (cmd >= ASEQ_OP_CONTROL_FLOW_FIRST) {
#if defined(TARGET_PSP)
            if (!OotPspAudio_ValidateControlFlowArg(seqPlayer, state, cmd, "layer-flow-arg")) {
                AudioSeq_SeqLayerDisable(layer);
                return PROCESS_SCRIPT_END;
            }
#endif
            cmdArg16 = AudioSeq_GetScriptControlFlowArgument(state, cmd);

            if (AudioSeq_HandleScriptFlowControl(seqPlayer, state, cmd, cmdArg16) == 0) {
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-flow-target")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                continue;
            }
            AudioSeq_SeqLayerDisable(layer);
            return PROCESS_SCRIPT_END;
        }

        switch (cmd) {
            case ASEQ_OP_LAYER_SHORTVEL: // layer_setshortnotevelocity
            case ASEQ_OP_LAYER_NOTEPAN:  // layer_setpan
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-u8")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg8 = *(state->pc++);
                if (cmd == ASEQ_OP_LAYER_SHORTVEL) {
                    layer->velocitySquare = SQ(cmdArg8) / SQ(127.0f);
                } else {
                    layer->pan = cmdArg8;
                }
                break;

            case ASEQ_OP_LAYER_SHORTGATE: // layer_setshortnotegatetime
            case ASEQ_OP_LAYER_TRANSPOSE: // layer_transpose; set transposition in semitones
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-u8")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg8 = *(state->pc++);
                if (cmd == ASEQ_OP_LAYER_SHORTGATE) {
                    layer->gateTime = cmdArg8;
                } else {
                    layer->transposition = cmdArg8;
                }
                break;

            case ASEQ_OP_LAYER_LEGATO:   // layer_continuousnoteson
            case ASEQ_OP_LAYER_NOLEGATO: // layer_continuousnotesoff
                if (cmd == ASEQ_OP_LAYER_LEGATO) {
                    layer->continuousNotes = true;
                } else {
                    layer->continuousNotes = false;
                }
                layer->bit1 = false;
                Audio_SeqLayerNoteDecay(layer);
                break;

            case ASEQ_OP_LAYER_SHORTDELAY: // layer_setshortnotedefaultdelay
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-shortdelay")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg16 = AudioSeq_ScriptReadCompressedU16(state);
                layer->shortNoteDefaultDelay = cmdArg16;
                break;

            case ASEQ_OP_LAYER_INSTR: // layer_setinstr
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-instr")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmd = AudioSeq_ScriptReadU8(state);
                if (cmd >= 0x7E) {
                    if (cmd == 0x7E) {
                        // Sfxs
                        layer->instOrWave = 1;
                    } else if (cmd == 0x7F) {
                        // Drums
                        layer->instOrWave = 0;
                    } else {
                        // Synthetic Wave
                        layer->instOrWave = cmd;
                        layer->instrument = NULL;
                    }

                    if (cmd == 0xFF) {
                        layer->adsr.decayIndex = 0;
                    }
                } else {
                    // Instrument
                    if ((layer->instOrWave = AudioSeq_GetInstrument(channel, cmd, &layer->instrument, &layer->adsr)) ==
                        0) {
                        layer->instOrWave = 0xFF;
                    }
                }
                break;

            case ASEQ_OP_LAYER_PORTAMENTO: // layer_portamento
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 2, "layer-portamento")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                layer->portamento.mode = AudioSeq_ScriptReadU8(state);

                cmd = AudioSeq_ScriptReadU8(state);
                cmd += channel->transposition;
                cmd += layer->transposition;
                cmd += seqPlayer->transposition;

                if (cmd >= 0x80) {
                    cmd = 0;
                }

                layer->portamentoTargetNote = cmd;

                // If special, the next param is u8 instead of var
                if (PORTAMENTO_IS_SPECIAL(layer->portamento)) {
#if defined(TARGET_PSP)
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-portamento-time")) {
                        AudioSeq_SeqLayerDisable(layer);
                        return PROCESS_SCRIPT_END;
                    }
#endif
                    layer->portamentoTime = *(state->pc++);
                    break;
                }

#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-portamento-time")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg16 = AudioSeq_ScriptReadCompressedU16(state);
                layer->portamentoTime = cmdArg16;
                break;

            case ASEQ_OP_LAYER_NOPORTAMENTO: // layer_disableportamento
                layer->portamento.mode = PORTAMENTO_MODE_OFF;
                break;

            case ASEQ_OP_LAYER_ENV:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 3, "layer-env")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg16 = AudioSeq_ScriptReadS16(state);
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqPlayer->seqData + cmdArg16, 1, "layer-env-target")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                layer->adsr.envelope = (EnvelopePoint*)(seqPlayer->seqData + cmdArg16);
#if defined(TARGET_PSP)
                layer->adsr.envelopeBigEndian = true;
#endif
                FALLTHROUGH;
            case ASEQ_OP_LAYER_RELEASERATE:
#if defined(TARGET_PSP)
                if ((cmd == ASEQ_OP_LAYER_RELEASERATE) &&
                    !OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-releaserate")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                layer->adsr.decayIndex = AudioSeq_ScriptReadU8(state);
                break;

            case ASEQ_OP_LAYER_NODRUMPAN:
                layer->ignoreDrumPan = true;
                break;

            case ASEQ_OP_LAYER_STEREO:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-stereo")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                layer->stereo.asByte = AudioSeq_ScriptReadU8(state);
                break;

            case ASEQ_OP_LAYER_BENDFINE:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-bendfine")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                cmdArg8 = AudioSeq_ScriptReadU8(state);
                layer->bend = gBendPitchTwoSemitonesFrequencies[(u8)(cmdArg8 + 0x80)];
                break;

            default:
                switch (cmd & 0xF0) {
                    case ASEQ_OP_LAYER_LDSHORTVEL: // layer_setshortnotevelocityfromtable
                        velocity = seqPlayer->shortNoteVelocityTable[cmd & 0xF];
                        layer->velocitySquare = SQ(velocity) / SQ(127.0f);
                        break;

                    case ASEQ_OP_LAYER_LDSHORTGATE: // layer_setshortnotegatetimefromtable
                        layer->gateTime = seqPlayer->shortNoteGateTimeTable[cmd & 0xF];
                        break;
                }
        }
    }
}

/**
 * original name: __SetVoice
 */
s32 AudioSeq_SeqLayerProcessScriptStep4(SequenceLayer* layer, s32 cmd) {
    s32 sameTunedSample = true;
    s32 instOrWave;
    s32 speed;
    f32 temp_f14;
    f32 temp_f2;
    Portamento* portamento;
    f32 freqScale;
    f32 freqScale2;
    TunedSample* tunedSample;
    Instrument* instrument;
    Drum* drum;
    SoundEffect* soundEffect;
    SequenceChannel* channel;
    SequencePlayer* seqPlayer;
    u8 semitone = cmd;
    u16 sfxId;
    s32 semitone2;
    s32 vel;
    f32 time;
    f32 tuning;
    s32 speed2;

    instOrWave = layer->instOrWave;
    channel = layer->channel;
    seqPlayer = channel->seqPlayer;

    if (instOrWave == 0xFF) {
        if (!channel->hasInstrument) {
            return PROCESS_SCRIPT_END;
        }
        instOrWave = channel->instOrWave;
    }

    switch (instOrWave) {
        case 0:
            // Drums
            semitone += channel->transposition + layer->transposition;
            layer->semitone = semitone;

            drum = Audio_GetDrum(channel->fontId, semitone);
            if (drum == NULL) {
                layer->muted = true;
                layer->delay2 = layer->delay;
                return PROCESS_SCRIPT_END;
            }

            tunedSample = &drum->tunedSample;
            layer->adsr.envelope = drum->envelope;
            layer->adsr.decayIndex = drum->adsrDecayIndex;
#if defined(TARGET_PSP)
            layer->adsr.envelopeBigEndian = false;
#endif
            if (!layer->ignoreDrumPan) {
                layer->pan = drum->pan;
            }
            layer->tunedSample = tunedSample;
            layer->freqScale = tunedSample->tuning;
            break;

        case 1:
            // Sfxs
            layer->semitone = semitone;
            sfxId = (layer->transposition << 6) + semitone;

            soundEffect = Audio_GetSoundEffect(channel->fontId, sfxId);
            if (soundEffect == NULL) {
                layer->muted = true;
                layer->delay2 = layer->delay + 1;
                return PROCESS_SCRIPT_END;
            }

            tunedSample = &soundEffect->tunedSample;
            layer->tunedSample = tunedSample;
            layer->freqScale = tunedSample->tuning;
            break;

        default:
            semitone += seqPlayer->transposition + channel->transposition + layer->transposition;
            semitone2 = semitone;

            layer->semitone = semitone;
            if (semitone >= 0x80) {
                layer->muted = true;
                return PROCESS_SCRIPT_END;
            }

            if (layer->instOrWave == 0xFF) {
                instrument = channel->instrument;
            } else {
                instrument = layer->instrument;
            }

            if (layer->portamento.mode != PORTAMENTO_MODE_OFF) {
                portamento = &layer->portamento;
                vel = (semitone > layer->portamentoTargetNote) ? semitone : layer->portamentoTargetNote;

                if (instrument != NULL) {
                    tunedSample = Audio_GetInstrumentTunedSample(instrument, vel);
                    sameTunedSample = (layer->tunedSample == tunedSample);
                    layer->tunedSample = tunedSample;
                    tuning = tunedSample->tuning;
                } else {
                    layer->tunedSample = NULL;
                    tuning = 1.0f;
                    if (instOrWave >= 0xC0) {
                        layer->tunedSample = &gAudioCtx.synthesisReverbs[instOrWave - 0xC0].tunedSample;
                    }
                }

                temp_f2 = gPitchFrequencies[semitone2] * tuning;
                temp_f14 = gPitchFrequencies[layer->portamentoTargetNote] * tuning;

                switch (PORTAMENTO_MODE(*portamento)) {
                    case PORTAMENTO_MODE_1:
                    case PORTAMENTO_MODE_3:
                    case PORTAMENTO_MODE_5:
                        freqScale2 = temp_f2;
                        freqScale = temp_f14;
                        break;

                    case PORTAMENTO_MODE_2:
                    case PORTAMENTO_MODE_4:
                        freqScale = temp_f2;
                        freqScale2 = temp_f14;
                        break;

                    default:
                        freqScale = temp_f2;
                        freqScale2 = temp_f2;
                        break;
                }

                portamento->extent = (freqScale2 / freqScale) - 1.0f;

                if (PORTAMENTO_IS_SPECIAL(*portamento)) {
                    speed = seqPlayer->tempo * 0x8000 / gAudioCtx.maxTempo;
                    if (layer->delay != 0) {
                        speed = speed * 0x100 / (layer->delay * layer->portamentoTime);
                    }
                } else {
                    speed = 0x20000 / (layer->portamentoTime * gAudioCtx.audioBufferParameters.ticksPerUpdate);
                }

                if (speed >= 0x7FFF) {
                    speed = 0x7FFF;
                } else if (speed < 1) {
                    speed = 1;
                }

                portamento->speed = speed;
                portamento->cur = 0;
                layer->freqScale = freqScale;
                if (PORTAMENTO_MODE(*portamento) == PORTAMENTO_MODE_5) {
                    layer->portamentoTargetNote = semitone;
                }
                break;
            }

            if (instrument != NULL) {
                tunedSample = Audio_GetInstrumentTunedSample(instrument, semitone);
                sameTunedSample = (tunedSample == layer->tunedSample);
                layer->tunedSample = tunedSample;
                layer->freqScale = gPitchFrequencies[semitone2] * tunedSample->tuning;
            } else {
                layer->tunedSample = NULL;
                layer->freqScale = gPitchFrequencies[semitone2];
                if (instOrWave >= 0xC0) {
                    layer->tunedSample = &gAudioCtx.synthesisReverbs[instOrWave - 0xC0].tunedSample;
                }
            }
            break;
    }

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeTunedSample(layer->tunedSample)) {
        OotPspAudio_DropBadSeqSample(layer, instOrWave, semitone);
        return PROCESS_SCRIPT_END;
    }
#endif

    layer->delay2 = layer->delay;
    layer->freqScale *= layer->bend;

    if (layer->delay == 0) {
        if (layer->tunedSample != NULL) {
            time = layer->tunedSample->sample->loop->header.end;
        } else {
            time = 0.0f;
        }
        time *= seqPlayer->tempo;
        time *= gAudioCtx.unk_2870;
        time /= layer->freqScale;
        if (time > 0x7FFE) {
            time = 0x7FFE;
        }

        layer->gateDelay = 0;
        layer->delay = (u16)(s32)time + 1;

        if (layer->portamento.mode != PORTAMENTO_MODE_OFF) {
            // (It's a bit unclear if 'portamento' has actually always been
            // set when this is reached...)
            if (PORTAMENTO_IS_SPECIAL(*portamento)) {
                speed2 = seqPlayer->tempo * 0x8000 / gAudioCtx.maxTempo;
                speed2 = speed2 * 0x100 / (layer->delay * layer->portamentoTime);
                if (speed2 >= 0x7FFF) {
                    speed2 = 0x7FFF;
                } else if (speed2 < 1) {
                    speed2 = 1;
                }
                portamento->speed = speed2;
            }
        }
    }
    return sameTunedSample;
}

/**
 * original name: __SetNote
 */
s32 AudioSeq_SeqLayerProcessScriptStep3(SequenceLayer* layer, s32 cmd) {
    SeqScriptState* state = &layer->scriptState;
    u16 delay;
    s32 velocity;
    SequenceChannel* channel = layer->channel;
    SequencePlayer* seqPlayer = channel->seqPlayer;
    s32 intDelta;
    f32 floatDelta;

    if (cmd == ASEQ_OP_LAYER_LDELAY) {
#if defined(TARGET_PSP)
        if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-ldelay")) {
            AudioSeq_SeqLayerDisable(layer);
            return PROCESS_SCRIPT_END;
        }
#endif
        layer->delay = AudioSeq_ScriptReadCompressedU16(state);
        layer->muted = true;
        layer->bit1 = false;
        return PROCESS_SCRIPT_END;
    }

    layer->muted = false;

    if (channel->largeNotes == true) {
        switch (cmd & 0xC0) {
            case ASEQ_OP_LAYER_NOTEDVG:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-notedvg-delay")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                delay = AudioSeq_ScriptReadCompressedU16(state);
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 2, "layer-notedvg-args")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                velocity = *(state->pc++);
                layer->gateTime = *(state->pc++);
                layer->lastDelay = delay;
                break;

            case ASEQ_OP_LAYER_NOTEDV:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-notedv-delay")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                delay = AudioSeq_ScriptReadCompressedU16(state);
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 1, "layer-notedv-velocity")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                velocity = *(state->pc++);
                layer->gateTime = 0;
                layer->lastDelay = delay;
                break;

            case ASEQ_OP_LAYER_NOTEVG:
                delay = layer->lastDelay;
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, state->pc, 2, "layer-notevg-args")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                velocity = *(state->pc++);
                layer->gateTime = *(state->pc++);
                break;
        }

        if (velocity > 0x7F || velocity < 0) {
            velocity = 0x7F;
        }
        layer->velocitySquare = SQ((f32)velocity) / SQ(127.0f);
        cmd -= (cmd & 0xC0);
    } else {
        switch (cmd & 0xC0) {
            case ASEQ_OP_LAYER_NOTEDVG:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqCompressedU16(seqPlayer, state, "layer-short-notedvg-delay")) {
                    AudioSeq_SeqLayerDisable(layer);
                    return PROCESS_SCRIPT_END;
                }
#endif
                delay = AudioSeq_ScriptReadCompressedU16(state);
                layer->lastDelay = delay;
                break;

            case ASEQ_OP_LAYER_NOTEDV:
                delay = layer->shortNoteDefaultDelay;
                break;

            case ASEQ_OP_LAYER_NOTEVG:
                delay = layer->lastDelay;
                break;
        }
        cmd -= (cmd & 0xC0);
    }

    if (channel->velocityRandomVariance != 0) {
        floatDelta = layer->velocitySquare * (gAudioCtx.audioRandom % channel->velocityRandomVariance) / 100.0f;
        if ((gAudioCtx.audioRandom & 0x8000) != 0) {
            floatDelta = -floatDelta;
        }

        layer->velocitySquare2 = layer->velocitySquare + floatDelta;

        if (layer->velocitySquare2 < 0.0f) {
            layer->velocitySquare2 = 0.0f;
        } else if (layer->velocitySquare2 > 1.0f) {
            layer->velocitySquare2 = 1.0f;
        }
    } else {
        layer->velocitySquare2 = layer->velocitySquare;
    }

    layer->delay = delay;
    layer->gateDelay = (layer->gateTime * delay) >> 8;

    if (channel->gateTimeRandomVariance != 0) {
        //! @bug should probably be gateTimeRandomVariance
        intDelta = (layer->gateDelay * (gAudioCtx.audioRandom % channel->velocityRandomVariance)) / 100;
        if ((gAudioCtx.audioRandom & 0x4000) != 0) {
            intDelta = -intDelta;
        }

        layer->gateDelay += intDelta;
        if (layer->gateDelay < 0) {
            layer->gateDelay = 0;
        } else if (layer->gateDelay > layer->delay) {
            layer->gateDelay = layer->delay;
        }
    }

    if ((seqPlayer->muted && (channel->muteBehavior & (MUTE_BEHAVIOR_STOP_NOTES | MUTE_BEHAVIOR_4))) ||
        channel->muted) {
        layer->muted = true;
        return PROCESS_SCRIPT_END;
    }

    if (seqPlayer->skipTicks != 0) {
        layer->muted = true;
        return PROCESS_SCRIPT_END;
    }

    return cmd;
}

/**
 * original name: Nas_PriorityChanger
 */
void AudioSeq_SetChannelPriorities(SequenceChannel* channel, u8 priority) {
    if ((priority & 0xF) != 0) {
        channel->notePriority = priority & 0xF;
    }

    priority = priority >> 4;
    if (priority != 0) {
        channel->someOtherPriority = priority;
    }
}

/**
 * original name: Nas_ProgramChanger
 */
u8 AudioSeq_GetInstrument(SequenceChannel* channel, u8 instId, Instrument** instOut, AdsrSettings* adsr) {
    Instrument* inst = Audio_GetInstrumentInner(channel->fontId, instId);

    if (inst == NULL) {
        *instOut = NULL;
        return 0;
    }

    adsr->envelope = inst->envelope;
    adsr->decayIndex = inst->adsrDecayIndex;
#if defined(TARGET_PSP)
    adsr->envelopeBigEndian = false;
#endif
    *instOut = inst;

    // temporarily offset instrument id by 2 so that instId 0, 1
    // can be reserved by drums and sfxs respectively.
    instId += 2;

    return instId;
}

/**
 * original name: Nas_SubVoiceSet
 */
void AudioSeq_SetInstrument(SequenceChannel* channel, u8 instId) {
    if (instId >= 0x80) {
        // Synthetic Waves
        channel->instOrWave = instId;
        channel->instrument = NULL;
    } else if (instId == 0x7F) {
        // Drums
        channel->instOrWave = 0;
        channel->instrument = (Instrument*)1; // invalid pointer, never dereferenced
    } else if (instId == 0x7E) {
        // Sfxs
        channel->instOrWave = 1;
        channel->instrument = (Instrument*)2; // invalid pointer, never dereferenced
    } else {
        // Instruments
        if ((channel->instOrWave = AudioSeq_GetInstrument(channel, instId, &channel->instrument, &channel->adsr)) ==
            0) {
            channel->hasInstrument = false;
            return;
        }
    }

    channel->hasInstrument = true;
}

/**
 * original name: Nas_SubVolumeSet
 */
void AudioSeq_SequenceChannelSetVolume(SequenceChannel* channel, u8 volume) {
    channel->volume = (s32)volume / 127.0f;
}

/**
 * original name: Nas_SubSeq
 */
void AudioSeq_SequenceChannelProcessScript(SequenceChannel* channel) {
    s32 i;
    u8* data;
    u8* seqData;
    SequencePlayer* seqPlayer;

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsSafeSequenceChannel(channel)) {
        OotPspAudio_LogBadChannel("chan-script", NULL, -1, channel);
        return;
    }
#endif

    if (channel->stopScript) {
        goto exit_loop;
    }

    seqPlayer = channel->seqPlayer;
#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(seqPlayer)) {
        OotPspAudio_LogBadChannel("chan-seqplayer", seqPlayer, -1, channel);
        AudioSeq_SequenceChannelDisable(channel);
        return;
    }
#endif
    if (seqPlayer->muted && (channel->muteBehavior & MUTE_BEHAVIOR_STOP_SCRIPT)) {
        return;
    }

    if (channel->delay >= 2) {
        channel->delay--;
        goto exit_loop;
    }

    while (true) {
        SeqScriptState* scriptState = &channel->scriptState;
        s32 param;
        s16 temp1;
        u16 cmdArgU16;
        u32 cmdArgs[3];
        s8 cmdArgS8;
        u8 cmd;
        u8 lowBits;
        u8 highBits;
        s32 delay;
        s32 temp2;
#if defined(TARGET_PSP)
        SequenceChannel* targetChannel;
        u8 dynValue;
        s32 ptrOffset;
#endif

#if defined(TARGET_PSP)
        if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 1, "chan-cmd")) {
            AudioSeq_SequenceChannelDisable(channel);
            return;
        }
#endif
        cmd = AudioSeq_ScriptReadU8(scriptState);
        if (cmd >= 0xB0) {
            highBits = sSeqInstructionArgsTable[cmd - 0xB0];
            lowBits = highBits & 3;

            // read in arguments for the instruction
            for (i = 0; i < lowBits; i++, highBits <<= 1) {
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, (highBits & 0x80) ? 2 : 1,
                                                "chan-arg")) {
                    AudioSeq_SequenceChannelDisable(channel);
                    return;
                }
#endif
                if (!(highBits & 0x80)) {
                    cmdArgs[i] = AudioSeq_ScriptReadU8(scriptState);
                } else {
                    cmdArgs[i] = AudioSeq_ScriptReadS16(scriptState);
                }
            }

            // Control Flow Commands
            if (cmd >= ASEQ_OP_CONTROL_FLOW_FIRST) {
                delay = AudioSeq_HandleScriptFlowControl(seqPlayer, scriptState, cmd, cmdArgs[0]);

                if (delay != 0) {
                    if (delay == PROCESS_SCRIPT_END) {
                        AudioSeq_SequenceChannelDisable(channel);
                    } else {
                        channel->delay = delay;
                    }
                    break;
                }
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 1, "chan-flow-target")) {
                    AudioSeq_SequenceChannelDisable(channel);
                    return;
                }
#endif
                continue;
            }

            switch (cmd) {
                case ASEQ_OP_CHAN_STOP:
                    channel->stopScript = true;
                    goto exit_loop;

                case ASEQ_OP_CHAN_ALLOCNOTELIST:
                    Audio_NotePoolClear(&channel->notePool);
                    cmd = (u8)cmdArgs[0];
                    Audio_NotePoolFill(&channel->notePool, cmd);
                    break;

                case ASEQ_OP_CHAN_FREENOTELIST:
                    Audio_NotePoolClear(&channel->notePool);
                    break;

                case ASEQ_OP_CHAN_DYNTBL:
                    cmdArgU16 = (u16)cmdArgs[0];
#if defined(TARGET_PSP)
                    if (!OotPspAudio_SetChannelDynTable(seqPlayer, channel, cmdArgU16, "dyntbl")) {
                        channel->stopScript = true;
                        goto exit_loop;
                    }
#else
                    channel->dynTable = (void*)&seqPlayer->seqData[cmdArgU16];
#endif
                    break;

                case ASEQ_OP_CHAN_DYNTBLLOOKUP:
                    if (AudioSeq_CanIndexWithTR(scriptState->value)) {
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ReadDynTableU16(seqPlayer, channel, scriptState->value, "dyntbllookup",
                                                         &cmdArgU16) ||
                            !OotPspAudio_SetChannelDynTable(seqPlayer, channel, cmdArgU16, "dyntbllookup-target")) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
#else
                        data = (*channel->dynTable)[scriptState->value];
                        cmdArgU16 = AudioSeq_ReadU16(data);

                        channel->dynTable = (void*)&seqPlayer->seqData[cmdArgU16];
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_FONTINSTR:
                    cmd = (u8)cmdArgs[0];

                    if (seqPlayer->defaultFont != 0xFF) {
                        cmdArgU16 = ((u16*)gAudioCtx.sequenceFontTable)[seqPlayer->seqId];
                        lowBits = gAudioCtx.sequenceFontTable[cmdArgU16];
                        cmd = gAudioCtx.sequenceFontTable[cmdArgU16 + lowBits - cmd];
                    }

                    if (AudioHeap_SearchCaches(FONT_TABLE, CACHE_EITHER, cmd)) {
                        channel->fontId = cmd;
                    }

                    cmdArgs[0] = cmdArgs[1];
                    FALLTHROUGH;
                case ASEQ_OP_CHAN_INSTR:
                    cmd = (u8)cmdArgs[0];
                    AudioSeq_SetInstrument(channel, cmd);
                    break;

                case ASEQ_OP_CHAN_SHORT:
                    channel->largeNotes = false;
                    break;

                case ASEQ_OP_CHAN_NOSHORT:
                    channel->largeNotes = true;
                    break;

                case ASEQ_OP_CHAN_VOL:
                    cmd = (u8)cmdArgs[0];
                    AudioSeq_SequenceChannelSetVolume(channel, cmd);
                    channel->changes.s.volume = true;
                    break;

                case ASEQ_OP_CHAN_VOLEXP:
                    cmd = (u8)cmdArgs[0];
                    channel->volumeScale = (s32)cmd / 128.0f;
                    channel->changes.s.volume = true;
                    break;

                case ASEQ_OP_CHAN_FREQSCALE:
                    cmdArgU16 = (u16)cmdArgs[0];
                    channel->freqScale = (s32)cmdArgU16 / 32768.0f;
                    channel->changes.s.freqScale = true;
                    break;

                case ASEQ_OP_CHAN_BEND:
                    cmd = (u8)cmdArgs[0];
                    cmd += 0x80;
                    channel->freqScale = gBendPitchOneOctaveFrequencies[cmd];
                    channel->changes.s.freqScale = true;
                    break;

                case ASEQ_OP_CHAN_BENDFINE:
                    cmd = (u8)cmdArgs[0];
                    cmd += 0x80;
                    channel->freqScale = gBendPitchTwoSemitonesFrequencies[cmd];
                    channel->changes.s.freqScale = true;
                    break;

                case ASEQ_OP_CHAN_PAN:
                    cmd = (u8)cmdArgs[0];
                    channel->newPan = cmd;
                    channel->changes.s.pan = true;
                    break;

                case ASEQ_OP_CHAN_PANWEIGHT:
                    cmd = (u8)cmdArgs[0];
                    channel->panChannelWeight = cmd;
                    channel->changes.s.pan = true;
                    break;

                case ASEQ_OP_CHAN_TRANSPOSE:
                    cmdArgS8 = (s8)cmdArgs[0];
                    channel->transposition = cmdArgS8;
                    break;

                case ASEQ_OP_CHAN_ENV:
                    cmdArgU16 = (u16)cmdArgs[0];
                    channel->adsr.envelope = (EnvelopePoint*)&seqPlayer->seqData[cmdArgU16];
#if defined(TARGET_PSP)
                    channel->adsr.envelopeBigEndian = true;
#endif
                    break;

                case ASEQ_OP_CHAN_RELEASERATE:
                    cmd = (u8)cmdArgs[0];
                    channel->adsr.decayIndex = cmd;
                    break;

                case ASEQ_OP_CHAN_VIBDEPTH:
                    cmd = (u8)cmdArgs[0];
                    channel->vibratoDepthTarget = cmd * 8;
                    channel->vibratoDepthStart = 0;
                    channel->vibratoDepthChangeDelay = 0;
                    break;

                case ASEQ_OP_CHAN_VIBFREQ:
                    cmd = (u8)cmdArgs[0];
                    channel->vibratoRateChangeDelay = 0;
                    channel->vibratoRateTarget = cmd * 32;
                    channel->vibratoRateStart = cmd * 32;
                    break;

                case ASEQ_OP_CHAN_VIBDEPTHGRAD:
                    cmd = (u8)cmdArgs[0];
                    channel->vibratoDepthStart = cmd * 8;
                    cmd = (u8)cmdArgs[1];
                    channel->vibratoDepthTarget = cmd * 8;
                    cmd = (u8)cmdArgs[2];
                    channel->vibratoDepthChangeDelay = cmd * 16;
                    break;

                case ASEQ_OP_CHAN_VIBFREQGRAD:
                    cmd = (u8)cmdArgs[0];
                    channel->vibratoRateStart = cmd * 32;
                    cmd = (u8)cmdArgs[1];
                    channel->vibratoRateTarget = cmd * 32;
                    cmd = (u8)cmdArgs[2];
                    channel->vibratoRateChangeDelay = cmd * 16;
                    break;

                case ASEQ_OP_CHAN_VIBDELAY:
                    cmd = (u8)cmdArgs[0];
                    channel->vibratoDelay = cmd * 16;
                    break;

                case ASEQ_OP_CHAN_REVERB:
                    cmd = (u8)cmdArgs[0];
                    channel->targetReverbVol = cmd;
                    break;

                case ASEQ_OP_CHAN_FONT:
                    cmd = (u8)cmdArgs[0];

                    if (seqPlayer->defaultFont != 0xFF) {
                        cmdArgU16 = ((u16*)gAudioCtx.sequenceFontTable)[seqPlayer->seqId];
                        lowBits = gAudioCtx.sequenceFontTable[cmdArgU16];
                        cmd = gAudioCtx.sequenceFontTable[cmdArgU16 + lowBits - cmd];
                    }

                    if (AudioHeap_SearchCaches(FONT_TABLE, CACHE_EITHER, cmd)) {
                        channel->fontId = cmd;
                    }
                    break;

                case ASEQ_OP_CHAN_STSEQ:
                    cmd = (u8)cmdArgs[0];
                    cmdArgU16 = (u16)cmdArgs[1];
                    seqData = &seqPlayer->seqData[cmdArgU16];
                    seqData[0] = (u8)scriptState->value + cmd;
                    break;

                case ASEQ_OP_CHAN_SUB:
                case ASEQ_OP_CHAN_LDI:
                case ASEQ_OP_CHAN_AND:
                    cmdArgS8 = (s8)cmdArgs[0];

                    if (cmd == ASEQ_OP_CHAN_SUB) {
                        scriptState->value -= cmdArgS8;
                    } else if (cmd == ASEQ_OP_CHAN_LDI) {
                        scriptState->value = cmdArgS8;
                    } else {
                        scriptState->value &= cmdArgS8;
                    }
                    break;

                case ASEQ_OP_CHAN_STOPCHAN:
                    cmd = (u8)cmdArgs[0];
#if defined(TARGET_PSP)
                    targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, cmd, "chan-stop");
                    if (targetChannel != NULL) {
                        AudioSeq_SequenceChannelDisable(targetChannel);
                    }
#else
                    AudioSeq_SequenceChannelDisable(seqPlayer->channels[cmd]);
#endif
                    break;

                case ASEQ_OP_CHAN_MUTEBHV:
                    cmd = (u8)cmdArgs[0];
                    channel->muteBehavior = cmd;
                    channel->changes.s.volume = true;
                    break;

                case ASEQ_OP_CHAN_LDSEQ:
                    cmdArgU16 = (u16)cmdArgs[0];
                    scriptState->value = *(seqPlayer->seqData + (u32)(cmdArgU16 + scriptState->value));
                    break;

                case ASEQ_OP_CHAN_LDPTR:
                    cmdArgU16 = (u16)cmdArgs[0];
                    channel->unk_22 = cmdArgU16;
                    break;

                case ASEQ_OP_CHAN_STPTRTOSEQ:
                    cmdArgU16 = (u16)cmdArgs[0];
                    seqData = &seqPlayer->seqData[cmdArgU16];
                    seqData[0] = (channel->unk_22 >> 8) & 0xFF;
                    seqData[1] = channel->unk_22 & 0xFF;
                    break;

                case ASEQ_OP_CHAN_EFFECTS:
                    cmd = (u8)cmdArgs[0];
                    if (cmd & 0x80) {
                        channel->stereoHeadsetEffects = true;
                    } else {
                        channel->stereoHeadsetEffects = false;
                    }
                    channel->stereo.asByte = cmd & 0x7F;
                    break;

                case ASEQ_OP_CHAN_NOTEALLOC:
                    cmd = (u8)cmdArgs[0];
                    channel->noteAllocPolicy = cmd;
                    break;

                case ASEQ_OP_CHAN_SUSTAIN:
                    cmd = (u8)cmdArgs[0];
                    channel->adsr.sustain = cmd;
                    break;

                case ASEQ_OP_CHAN_REVERBIDX:
                    cmd = (u8)cmdArgs[0];
                    channel->reverbIndex = cmd;
                    break;

                case ASEQ_OP_CHAN_DYNCALL:
                    if (AudioSeq_CanIndexWithTR(scriptState->value) &&
                        (scriptState->depth < ARRAY_COUNT(scriptState->stack))) {
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ReadDynTableU16(seqPlayer, channel, scriptState->value, "dyncall",
                                                         &cmdArgU16)) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
                        data = OotPspAudio_GetSeqPtr(seqPlayer, cmdArgU16, 1, "dyncall-target");
                        if (data == NULL) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
                        scriptState->stack[scriptState->depth++] = scriptState->pc;
                        scriptState->pc = data;
#else
                        data = (*channel->dynTable)[scriptState->value];
                        scriptState->stack[scriptState->depth++] = scriptState->pc;
                        cmdArgU16 = AudioSeq_ReadU16(data);
                        scriptState->pc = seqPlayer->seqData + cmdArgU16;
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_SAMPLEBOOK:
                    cmd = (u8)cmdArgs[0];
                    channel->bookOffset = cmd;
                    break;

                case ASEQ_OP_CHAN_LDPARAMS:
                    cmdArgU16 = (u16)cmdArgs[0];
                    data = &seqPlayer->seqData[cmdArgU16];
                    channel->muteBehavior = *data++;
                    channel->noteAllocPolicy = *data++;
                    AudioSeq_SetChannelPriorities(channel, *data++);
                    channel->transposition = (s8)*data++;
                    channel->newPan = *data++;
                    channel->panChannelWeight = *data++;
                    channel->targetReverbVol = *data++;
                    channel->reverbIndex = *data++;
                    //! @bug: Not marking reverb state as changed
                    channel->changes.s.pan = true;
                    break;

                case ASEQ_OP_CHAN_PARAMS:
                    channel->muteBehavior = cmdArgs[0];
                    channel->noteAllocPolicy = cmdArgs[1];
                    cmd = (u8)cmdArgs[2];
                    AudioSeq_SetChannelPriorities(channel, cmd);
                    channel->transposition = (s8)AudioSeq_ScriptReadU8(scriptState);
                    channel->newPan = AudioSeq_ScriptReadU8(scriptState);
                    channel->panChannelWeight = AudioSeq_ScriptReadU8(scriptState);
                    channel->targetReverbVol = AudioSeq_ScriptReadU8(scriptState);
                    channel->reverbIndex = AudioSeq_ScriptReadU8(scriptState);
                    //! @bug: Not marking reverb state as changed
                    channel->changes.s.pan = true;
                    break;

                case ASEQ_OP_CHAN_VIBRESET:
                    channel->vibratoDepthTarget = 0;
                    channel->vibratoDepthStart = 0;
                    channel->vibratoDepthChangeDelay = 0;
                    channel->vibratoRateTarget = 0;
                    channel->vibratoRateStart = 0;
                    channel->vibratoRateChangeDelay = 0;
                    channel->filter = NULL;
                    channel->gain = 0;
                    channel->adsr.sustain = 0;
                    channel->velocityRandomVariance = 0;
                    channel->gateTimeRandomVariance = 0;
                    channel->combFilterSize = 0;
                    channel->combFilterGain = 0;
                    channel->bookOffset = 0;
                    channel->freqScale = 1.0f;
                    break;

                case ASEQ_OP_CHAN_NOTEPRI:
                    AudioSeq_SetChannelPriorities(channel, (u8)cmdArgs[0]);
                    break;

                case ASEQ_OP_CHAN_GAIN:
                    cmd = (u8)cmdArgs[0];
                    channel->gain = cmd;
                    break;

                case ASEQ_OP_CHAN_LDFILTER:
                    cmdArgU16 = (u16)cmdArgs[0];
                    data = seqPlayer->seqData + cmdArgU16;
                    channel->filter = (s16*)data;
                    break;

                case ASEQ_OP_CHAN_FREEFILTER:
                    channel->filter = NULL;
                    break;

                case ASEQ_OP_CHAN_FILTER:
                    cmd = cmdArgs[0];

                    if (channel->filter != NULL) {
                        lowBits = (cmd >> 4) & 0xF; // LowPassCutoff
                        cmd &= 0xF;                 // HighPassCutoff
                        AudioHeap_LoadFilter(channel->filter, lowBits, cmd);
                    }
                    break;

                case ASEQ_OP_CHAN_LDSEQTOPTR:
                    cmdArgU16 = (u16)cmdArgs[0];
                    if (AudioSeq_CanIndexWithTR(scriptState->value)) {
#if defined(TARGET_PSP)
                        ptrOffset = cmdArgU16 + (scriptState->value * 2);
                        if ((ptrOffset < 0) ||
                            !OotPspAudio_ReadSeqU16(seqPlayer, (u32)ptrOffset, "ldseqtoptr", &channel->unk_22)) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
#else
                        channel->unk_22 =
                            AudioSeq_ReadU16(seqPlayer->seqData + (u32)(cmdArgU16 + (scriptState->value * 2)));
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_PTRTODYNTBL:
#if defined(TARGET_PSP)
                    if (!OotPspAudio_SetChannelDynTable(seqPlayer, channel, channel->unk_22, "ptrtodyntbl")) {
                        channel->stopScript = true;
                        goto exit_loop;
                    }
#else
                    channel->dynTable = (void*)&seqPlayer->seqData[channel->unk_22];
#endif
                    break;

                case ASEQ_OP_CHAN_DYNTBLTOPTR:
                    if (AudioSeq_CanIndexWithTR(scriptState->value)) {
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ReadDynTableU16(seqPlayer, channel, scriptState->value, "dyntbltoptr",
                                                         &channel->unk_22)) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
#else
                        channel->unk_22 = AudioSeq_ReadU16((u8*)channel->dynTable + (scriptState->value * 2));
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_DYNTBLV:
                    if (AudioSeq_CanIndexWithTR(scriptState->value)) {
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ReadDynTableU8(seqPlayer, channel, scriptState->value, "dyntblv",
                                                        &dynValue)) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
                        scriptState->value = dynValue;
#else
                        scriptState->value = (*channel->dynTable)[0][scriptState->value];
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_RANDTOPTR:
                    channel->unk_22 =
                        (cmdArgs[0] == 0) ? gAudioCtx.audioRandom & 0xFFFF : gAudioCtx.audioRandom % cmdArgs[0];
                    break;

                case ASEQ_OP_CHAN_RAND:
                    scriptState->value =
                        (cmdArgs[0] == 0) ? gAudioCtx.audioRandom & 0xFFFF : gAudioCtx.audioRandom % cmdArgs[0];
                    break;

                case ASEQ_OP_CHAN_RANDPTR:
                    temp2 = AudioThread_NextRandom();
                    channel->unk_22 = (cmdArgs[0] == 0) ? (temp2 & 0xFFFF) : (temp2 % cmdArgs[0]);
                    channel->unk_22 += cmdArgs[1];
                    temp2 = (channel->unk_22 / 0x100) + 0x80;
                    param = channel->unk_22 % 0x100;
                    channel->unk_22 = (temp2 << 8) | param;
                    break;

                case ASEQ_OP_CHAN_RANDVEL:
                    channel->velocityRandomVariance = cmdArgs[0];
                    break;

                case ASEQ_OP_CHAN_RANDGATE:
                    channel->gateTimeRandomVariance = cmdArgs[0];
                    break;

                case ASEQ_OP_CHAN_COMBFILTER:
                    channel->combFilterSize = cmdArgs[0];
                    channel->combFilterGain = cmdArgs[1];
                    break;

                case ASEQ_OP_CHAN_PTRADD:
                    channel->unk_22 += cmdArgs[0];
                    break;
            }
            continue;
        }

        if (cmd >= 0x70) {
            lowBits = cmd & 0x7;

            if ((cmd & 0xF8) != ASEQ_OP_CHAN_STIO && lowBits >= 4) {
                lowBits = 0;
            }

            switch (cmd & 0xF8) {
                case ASEQ_OP_CHAN_TESTLAYER:
                    if (channel->layers[lowBits] != NULL) {
                        scriptState->value = channel->layers[lowBits]->finished;
                    } else {
                        scriptState->value = -1;
                    }
                    break;

                case ASEQ_OP_CHAN_LDLAYER:
#if defined(TARGET_PSP)
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 2, "ldlayer-arg")) {
                        AudioSeq_SequenceChannelDisable(channel);
                        return;
                    }
#endif
                    cmdArgU16 = AudioSeq_ScriptReadS16(scriptState);
#if defined(TARGET_PSP)
                    data = OotPspAudio_GetSeqPtr(seqPlayer, cmdArgU16, 1, "ldlayer");
                    if (data == NULL) {
                        channel->stopScript = true;
                        goto exit_loop;
                    }
                    if (!AudioSeq_SeqChannelSetLayer(channel, lowBits)) {
                        channel->layers[lowBits]->scriptState.pc = data;
                    }
#else
                    if (!AudioSeq_SeqChannelSetLayer(channel, lowBits)) {
                        channel->layers[lowBits]->scriptState.pc = &seqPlayer->seqData[cmdArgU16];
                    }
#endif
                    break;

                case ASEQ_OP_CHAN_DELLAYER:
                    AudioSeq_SeqLayerFree(channel, lowBits);
                    break;

                case ASEQ_OP_CHAN_DYNLDLAYER:
                    if (AudioSeq_CanIndexWithTR(scriptState->value)) {
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ReadDynTableU16(seqPlayer, channel, scriptState->value, "dynldlayer",
                                                         &cmdArgU16)) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
                        data = OotPspAudio_GetSeqPtr(seqPlayer, cmdArgU16, 1, "dynldlayer-target");
                        if (data == NULL) {
                            channel->stopScript = true;
                            goto exit_loop;
                        }
                        if (AudioSeq_SeqChannelSetLayer(channel, lowBits) != -1) {
                            channel->layers[lowBits]->scriptState.pc = data;
                        }
#else
                        if (AudioSeq_SeqChannelSetLayer(channel, lowBits) != -1) {
                            data = (*channel->dynTable)[scriptState->value];
                            cmdArgU16 = AudioSeq_ReadU16(data);
                            channel->layers[lowBits]->scriptState.pc = &seqPlayer->seqData[cmdArgU16];
                        }
#endif
                    }
                    break;

                case ASEQ_OP_CHAN_STIO:
                    channel->seqScriptIO[lowBits] = scriptState->value;
                    break;

                case ASEQ_OP_CHAN_RLDLAYER:
#if defined(TARGET_PSP)
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 2, "rldlayer-arg")) {
                        AudioSeq_SequenceChannelDisable(channel);
                        return;
                    }
#endif
                    temp1 = AudioSeq_ScriptReadS16(scriptState);
#if defined(TARGET_PSP)
                    data = &scriptState->pc[temp1];
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, data, 1, "rldlayer")) {
                        channel->stopScript = true;
                        goto exit_loop;
                    }
                    if (!AudioSeq_SeqChannelSetLayer(channel, lowBits)) {
                        channel->layers[lowBits]->scriptState.pc = data;
                    }
#else
                    if (!AudioSeq_SeqChannelSetLayer(channel, lowBits)) {
                        channel->layers[lowBits]->scriptState.pc = &scriptState->pc[temp1];
                    }
#endif
                    break;
            }
            continue;
        }

        lowBits = cmd & 0xF;

        switch (cmd & 0xF0) {
            case ASEQ_OP_CHAN_CDELAY:
                channel->delay = lowBits;
                goto exit_loop;

            case ASEQ_OP_CHAN_LDSAMPLE:
                if (lowBits < 8) {
                    channel->seqScriptIO[lowBits] = SEQ_IO_VAL_NONE;
                    if (AudioLoad_SlowLoadSample(channel->fontId, scriptState->value, &channel->seqScriptIO[lowBits]) ==
                        -1) {}
                } else {
                    lowBits -= 8;
                    channel->seqScriptIO[lowBits] = SEQ_IO_VAL_NONE;
                    if (AudioLoad_SlowLoadSample(channel->fontId, channel->unk_22 + 0x100,
                                                 &channel->seqScriptIO[lowBits]) == -1) {}
                }
                break;

            case ASEQ_OP_CHAN_LDIO:
                scriptState->value = channel->seqScriptIO[lowBits];
                if (lowBits < 2) {
                    channel->seqScriptIO[lowBits] = SEQ_IO_VAL_NONE;
                }
                break;

            case ASEQ_OP_CHAN_SUBIO:
                scriptState->value -= channel->seqScriptIO[lowBits];
                break;

            case ASEQ_OP_CHAN_LDCHAN:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 2, "chan-ldchan-arg")) {
                    AudioSeq_SequenceChannelDisable(channel);
                    return;
                }
#endif
                cmdArgU16 = AudioSeq_ScriptReadS16(scriptState);
#if defined(TARGET_PSP)
                data = OotPspAudio_GetSeqPtr(seqPlayer, cmdArgU16, 1, "chan-ldchan");
                if (data == NULL) {
                    channel->stopScript = true;
                    goto exit_loop;
                }
                AudioSeq_SequenceChannelEnable(seqPlayer, lowBits, data);
#else
                AudioSeq_SequenceChannelEnable(seqPlayer, lowBits, &seqPlayer->seqData[cmdArgU16]);
#endif
                break;

            case ASEQ_OP_CHAN_STCIO:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 1, "chan-stcio-arg")) {
                    AudioSeq_SequenceChannelDisable(channel);
                    return;
                }
#endif
                cmd = AudioSeq_ScriptReadU8(scriptState);
#if defined(TARGET_PSP)
                targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, lowBits, "chan-stcio");
                if ((targetChannel != NULL) && (cmd < ARRAY_COUNT(targetChannel->seqScriptIO))) {
                    targetChannel->seqScriptIO[cmd] = scriptState->value;
                }
#else
                seqPlayer->channels[lowBits]->seqScriptIO[cmd] = scriptState->value;
#endif
                break;

            case ASEQ_OP_CHAN_LDCIO:
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, scriptState->pc, 1, "chan-ldcio-arg")) {
                    AudioSeq_SequenceChannelDisable(channel);
                    return;
                }
#endif
                cmd = AudioSeq_ScriptReadU8(scriptState);
#if defined(TARGET_PSP)
                targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, lowBits, "chan-ldcio");
                if ((targetChannel != NULL) && (cmd < ARRAY_COUNT(targetChannel->seqScriptIO))) {
                    scriptState->value = targetChannel->seqScriptIO[cmd];
                } else {
                    scriptState->value = SEQ_IO_VAL_NONE;
                }
#else
                scriptState->value = seqPlayer->channels[lowBits]->seqScriptIO[cmd];
#endif
                break;
        }
    }
exit_loop:

    for (i = 0; i < ARRAY_COUNT(channel->layers); i++) {
        if (channel->layers[i] != NULL) {
            AudioSeq_SeqLayerProcessScript(channel->layers[i]);
        }
    }
}

/**
 * original name: Nas_GroupSeq
 */
void AudioSeq_SequencePlayerProcessSequence(SequencePlayer* seqPlayer) {
    u8 cmd;
    u8 cmdLowBits;
    SeqScriptState* seqScript = &seqPlayer->scriptState;
    s16 tempS;
    u16 temp;
    s32 i;
    s32 value;
    u8* data;
    u8* data2;
    u8* data3;
    s32 pad;
    s32 dummy;
    s32 delay;
#if defined(TARGET_PSP)
    SequenceChannel* targetChannel;
    s32 dynOffset;
#endif

#if defined(TARGET_PSP)
    if (!OotPspAudio_IsAlignedNativePtr(seqPlayer)) {
        OotPspAudio_LogBadChannel("seq-script", seqPlayer, -1, NULL);
        return;
    }
#endif

    if (!seqPlayer->enabled) {
        return;
    }

    if (!AudioLoad_IsSeqLoadComplete(seqPlayer->seqId) || !AudioLoad_IsFontLoadComplete(seqPlayer->defaultFont)) {
        AudioSeq_SequencePlayerDisable(seqPlayer);
        return;
    }

    AudioLoad_SetSeqLoadStatus(seqPlayer->seqId, LOAD_STATUS_COMPLETE);
    AudioLoad_SetFontLoadStatus(seqPlayer->defaultFont, LOAD_STATUS_COMPLETE);

    if (seqPlayer->muted && (seqPlayer->muteBehavior & MUTE_BEHAVIOR_STOP_SCRIPT)) {
        return;
    }

    seqPlayer->scriptCounter++;

    // Apply the tempo by controlling the number of updates run on the .seq script.
    // Processing the .seq script every possible update will result in a tempo = maxTempo
    // Processing the .seq script a fraction of the updates will result in a `tempo = fraction * maxTempo`
    // where `fraction = (tempo + tempoChange) / maxTempo`
    // This algorithm uses `tempoAcc` to discretize `(tempo + tempoChange) / maxTempo`
    seqPlayer->tempoAcc += seqPlayer->tempo;
    seqPlayer->tempoAcc += (s16)seqPlayer->tempoChange;
    if (seqPlayer->tempoAcc < gAudioCtx.maxTempo) {
        return;
    }
    seqPlayer->tempoAcc -= (u16)gAudioCtx.maxTempo;

    if (seqPlayer->stopScript == true) {
        return;
    }

    if (seqPlayer->delay > 1) {
        seqPlayer->delay--;
    } else {
        seqPlayer->recalculateVolume = true;

        while (true) {
#if defined(TARGET_PSP)
            if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqScript->pc, 1, "seq-cmd")) {
                AudioSeq_SequencePlayerDisable(seqPlayer);
                return;
            }
#endif
            cmd = AudioSeq_ScriptReadU8(seqScript);

            // 0xF2 and above are "flow control" commands, including termination.
            if (cmd >= ASEQ_OP_CONTROL_FLOW_FIRST) {
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateControlFlowArg(seqPlayer, seqScript, cmd, "seq-flow-arg")) {
                    AudioSeq_SequencePlayerDisable(seqPlayer);
                    return;
                }
#endif
                delay = AudioSeq_HandleScriptFlowControl(
                    seqPlayer, seqScript, cmd, AudioSeq_GetScriptControlFlowArgument(&seqPlayer->scriptState, cmd));

                if (delay != 0) {
                    if (delay == -1) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                    } else {
                        seqPlayer->delay = delay;
                    }
                    break;
                }
#if defined(TARGET_PSP)
                if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqScript->pc, 1, "seq-flow-target")) {
                    AudioSeq_SequencePlayerDisable(seqPlayer);
                    return;
                }
#endif
                continue;
            }

            if (cmd >= 0xC0) {
                switch (cmd) {
                    case ASEQ_OP_SEQ_ALLOCNOTELIST:
                        Audio_NotePoolClear(&seqPlayer->notePool);
                        cmd = AudioSeq_ScriptReadU8(seqScript);
                        Audio_NotePoolFill(&seqPlayer->notePool, cmd);
                        // Fake-match: the asm has two breaks in a row here,
                        // which the compiler normally optimizes out.
                        dummy = -1;
                        if (dummy < 0) {
                            dummy = 0;
                        }
                        if (dummy > 1) {
                            dummy = 1;
                        }
                        if (dummy) {}
                        break;

                    case ASEQ_OP_SEQ_FREENOTELIST:
                        Audio_NotePoolClear(&seqPlayer->notePool);
                        break;

                    case ASEQ_OP_SEQ_TRANSPOSE:
                        seqPlayer->transposition = 0;
                        FALLTHROUGH;
                    case ASEQ_OP_SEQ_RTRANSPOSE:
                        seqPlayer->transposition += (s8)AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_TEMPO:
                        seqPlayer->tempo = AudioSeq_ScriptReadU8(seqScript) * SEQTICKS_PER_BEAT;
                        if (seqPlayer->tempo > gAudioCtx.maxTempo) {
                            seqPlayer->tempo = (u16)gAudioCtx.maxTempo;
                        }

                        if ((s16)seqPlayer->tempo <= 0) {
                            seqPlayer->tempo = 1;
                        }
                        break;

                    case ASEQ_OP_SEQ_TEMPOCHG:
                        seqPlayer->tempoChange = (s8)AudioSeq_ScriptReadU8(seqScript) * SEQTICKS_PER_BEAT;
                        break;

                    case ASEQ_OP_SEQ_VOLMODE:
                        cmd = AudioSeq_ScriptReadU8(seqScript);
                        temp = AudioSeq_ScriptReadS16(seqScript);
                        switch (cmd) {
                            case 0:
                            case 1:
                                if (seqPlayer->state != 2) {
                                    seqPlayer->fadeTimerUnkEu = temp;
                                    seqPlayer->state = cmd;
                                }
                                break;

                            case 2:
                                seqPlayer->fadeTimer = temp;
                                seqPlayer->state = cmd;
                                seqPlayer->fadeVelocity = (0.0f - seqPlayer->fadeVolume) / (s32)seqPlayer->fadeTimer;
                                break;
                        }
                        break;

                    case ASEQ_OP_SEQ_VOL:
                        value = AudioSeq_ScriptReadU8(seqScript);
                        switch (seqPlayer->state) {
                            case 1:
                                seqPlayer->state = 0;
                                seqPlayer->fadeVolume = 0.0f;
                                FALLTHROUGH;
                            case 0:
                                seqPlayer->fadeTimer = seqPlayer->fadeTimerUnkEu;
                                if (seqPlayer->fadeTimerUnkEu != 0) {
                                    seqPlayer->fadeVelocity =
                                        ((value / 127.0f) - seqPlayer->fadeVolume) / (s32)seqPlayer->fadeTimer;
                                } else {
                                    seqPlayer->fadeVolume = value / 127.0f;
                                }
                                break;

                            case 2:
                                break;
                        }
                        break;

                    case ASEQ_OP_SEQ_VOLSCALE:
                        seqPlayer->fadeVolumeScale = (s8)AudioSeq_ScriptReadU8(seqScript) / 127.0f;
                        break;

                    case ASEQ_OP_SEQ_INITCHAN:
                        temp = AudioSeq_ScriptReadS16(seqScript);
                        AudioSeq_SequencePlayerSetupChannels(seqPlayer, temp);
                        break;

                    case ASEQ_OP_SEQ_FREECHAN:
                        AudioSeq_ScriptReadS16(seqScript);
                        break;

                    case ASEQ_OP_SEQ_MUTESCALE:
                        seqPlayer->muteVolumeScale = (s8)AudioSeq_ScriptReadU8(seqScript) / 127.0f;
                        break;

                    case ASEQ_OP_SEQ_MUTE:
                        seqPlayer->muted = true;
                        break;

                    case ASEQ_OP_SEQ_MUTEBHV:
                        seqPlayer->muteBehavior = AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_LDSHORTGATEARR:
                    case ASEQ_OP_SEQ_LDSHORTVELARR:
                        temp = AudioSeq_ScriptReadS16(seqScript);
#if defined(TARGET_PSP)
                        data3 = OotPspAudio_GetSeqPtr(seqPlayer, temp, 16, "seq-shorttbl");
                        if (data3 == NULL) {
                            AudioSeq_SequencePlayerDisable(seqPlayer);
                            return;
                        }
#else
                        data3 = &seqPlayer->seqData[temp];
#endif
                        if (cmd == ASEQ_OP_SEQ_LDSHORTVELARR) {
                            seqPlayer->shortNoteVelocityTable = data3;
                        } else {
                            seqPlayer->shortNoteGateTimeTable = data3;
                        }
                        break;

                    case ASEQ_OP_SEQ_NOTEALLOC:
                        seqPlayer->noteAllocPolicy = AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_RAND:
                        cmd = AudioSeq_ScriptReadU8(seqScript);
                        if (cmd == 0) {
                            seqScript->value = (gAudioCtx.audioRandom >> 2) & 0xFF;
                        } else {
                            seqScript->value = (gAudioCtx.audioRandom >> 2) % cmd;
                        }
                        break;

                    case ASEQ_OP_SEQ_DYNCALL:
#if defined(TARGET_PSP)
                        if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqScript->pc, 2, "seq-dyncall-arg")) {
                            AudioSeq_SequencePlayerDisable(seqPlayer);
                            return;
                        }
#endif
                        temp = AudioSeq_ScriptReadS16(seqScript);
                        if (AudioSeq_CanIndexWithTR(seqScript->value) &&
                            (seqScript->depth < ARRAY_COUNT(seqScript->stack))) {
#if defined(TARGET_PSP)
                            dynOffset = temp + (seqScript->value << 1);
                            if ((dynOffset < 0) ||
                                !OotPspAudio_ReadSeqU16(seqPlayer, (u32)dynOffset, "seq-dyncall", &temp)) {
                                AudioSeq_SequencePlayerDisable(seqPlayer);
                                return;
                            }
                            data = OotPspAudio_GetSeqPtr(seqPlayer, temp, 1, "seq-dyncall-target");
                            if (data == NULL) {
                                AudioSeq_SequencePlayerDisable(seqPlayer);
                                return;
                            }
                            seqScript->stack[seqScript->depth] = seqScript->pc;
                            seqScript->depth++;
                            seqScript->pc = data;
#else
                            data = seqPlayer->seqData + (u32)(temp + (seqScript->value << 1));
                            seqScript->stack[seqScript->depth] = seqScript->pc;
                            seqScript->depth++;

                            temp = AudioSeq_ReadU16(data);
                            seqScript->pc = &seqPlayer->seqData[temp];
#endif
                        }
                        break;

                    case ASEQ_OP_SEQ_LDI:
                        seqScript->value = AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_AND:
                        seqScript->value &= AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_SUB:
                        seqScript->value -= AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_STSEQ:
                        cmd = AudioSeq_ScriptReadU8(seqScript);
                        temp = AudioSeq_ScriptReadS16(seqScript);
#if defined(TARGET_PSP)
                        data2 = OotPspAudio_GetSeqPtr(seqPlayer, temp, 1, "seq-stseq");
                        if (data2 == NULL) {
                            AudioSeq_SequencePlayerDisable(seqPlayer);
                            return;
                        }
#else
                        data2 = &seqPlayer->seqData[temp];
#endif
                        *data2 = (u8)seqScript->value + cmd;
                        break;

                    case ASEQ_OP_SEQ_STOP:
                        seqPlayer->stopScript = true;
                        return;

                    case ASEQ_OP_SEQ_SCRIPTCTR:
                        seqPlayer->scriptCounter = (u16)AudioSeq_ScriptReadS16(seqScript);
                        break;

                    case ASEQ_OP_SEQ_EF:
                        AudioSeq_ScriptReadS16(seqScript);
                        AudioSeq_ScriptReadU8(seqScript);
                        break;

                    case ASEQ_OP_SEQ_RUNSEQ:
                        cmd = AudioSeq_ScriptReadU8(seqScript);
                        if (cmd == 0xFF) {
                            cmd = seqPlayer->playerIdx;
                        }
                        cmdLowBits = AudioSeq_ScriptReadU8(seqScript);
                        AudioLoad_SyncInitSeqPlayer(cmd, cmdLowBits, 0);
                        if (cmd == (u8)seqPlayer->playerIdx) {
                            return;
                        }
                        break;
                }
                continue;
            }

            cmdLowBits = cmd & 0x0F;

            switch (cmd & 0xF0) {
                case ASEQ_OP_SEQ_TESTCHAN:
#if defined(TARGET_PSP)
                    targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, cmdLowBits, "seq-testchan");
                    seqScript->value = (targetChannel != NULL) ? (targetChannel->enabled ^ 1) : 1;
#else
                    seqScript->value = seqPlayer->channels[cmdLowBits]->enabled ^ 1;
#endif
                    break;

                case ASEQ_OP_SEQ_SUBIO:
                    seqScript->value -= seqPlayer->seqScriptIO[cmdLowBits];
                    break;

                case ASEQ_OP_SEQ_STIO:
                    seqPlayer->seqScriptIO[cmdLowBits] = seqScript->value;
                    break;

                case ASEQ_OP_SEQ_LDIO:
                    seqScript->value = seqPlayer->seqScriptIO[cmdLowBits];
                    if (cmdLowBits < 2) {
                        seqPlayer->seqScriptIO[cmdLowBits] = SEQ_IO_VAL_NONE;
                    }
                    break;

                case ASEQ_OP_SEQ_STOPCHAN:
#if defined(TARGET_PSP)
                    targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, cmdLowBits, "seq-stopchan");
                    if (targetChannel != NULL) {
                        AudioSeq_SequenceChannelDisable(targetChannel);
                    }
#else
                    AudioSeq_SequenceChannelDisable(seqPlayer->channels[cmdLowBits]);
#endif
                    break;

                case ASEQ_OP_SEQ_LDCHAN:
#if defined(TARGET_PSP)
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqScript->pc, 2, "seq-ldchan-arg")) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                        return;
                    }
#endif
                    temp = AudioSeq_ScriptReadS16(seqScript);
#if defined(TARGET_PSP)
                    data = OotPspAudio_GetSeqPtr(seqPlayer, temp, 1, "seq-ldchan");
                    if (data == NULL) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                        return;
                    }
                    AudioSeq_SequenceChannelEnable(seqPlayer, cmdLowBits, data);
#else
                    AudioSeq_SequenceChannelEnable(seqPlayer, cmdLowBits, (void*)&seqPlayer->seqData[temp]);
#endif
                    break;

                case ASEQ_OP_SEQ_RLDCHAN:
#if defined(TARGET_PSP)
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, seqScript->pc, 2, "seq-rldchan-arg")) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                        return;
                    }
#endif
                    tempS = AudioSeq_ScriptReadS16(seqScript);
#if defined(TARGET_PSP)
                    data = &seqScript->pc[tempS];
                    if (!OotPspAudio_ValidateSeqPtr(seqPlayer, data, 1, "seq-rldchan")) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                        return;
                    }
                    AudioSeq_SequenceChannelEnable(seqPlayer, cmdLowBits, data);
#else
                    AudioSeq_SequenceChannelEnable(seqPlayer, cmdLowBits, (void*)&seqScript->pc[tempS]);
#endif
                    break;

                case ASEQ_OP_SEQ_LDSEQ:
                    cmd = AudioSeq_ScriptReadU8(seqScript);
                    temp = AudioSeq_ScriptReadS16(seqScript);
#if defined(TARGET_PSP)
                    data2 = OotPspAudio_GetSeqPtr(seqPlayer, temp, 1, "seq-ldseq-ret");
                    if (data2 == NULL) {
                        AudioSeq_SequencePlayerDisable(seqPlayer);
                        return;
                    }
#else
                    data2 = &seqPlayer->seqData[temp];
#endif
                    AudioLoad_SlowLoadSeq(cmd, data2, &seqPlayer->seqScriptIO[cmdLowBits]);
                    break;

                case ASEQ_OP_SEQ_LDRES:
                    cmd = AudioSeq_ScriptReadU8(seqScript);
                    value = cmd;
                    temp = AudioSeq_ScriptReadU8(seqScript);
                    AudioLoad_ScriptLoad(value, temp, &seqPlayer->seqScriptIO[cmdLowBits]);
                    break;
            }
        }
    }

    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
#if defined(TARGET_PSP)
        targetChannel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, i, "seq-process-channel");
        if ((targetChannel != NULL) && targetChannel->enabled) {
            AudioSeq_SequenceChannelProcessScript(targetChannel);
        }
#else
        if (seqPlayer->channels[i]->enabled) {
            AudioSeq_SequenceChannelProcessScript(seqPlayer->channels[i]);
        }
#endif
    }
}

/**
 * original name: Nas_MySeqMain
 */
void AudioSeq_ProcessSequences(s32 arg0) {
    SequencePlayer* seqPlayer;
    u32 i;

    gAudioCtx.noteSubEuOffset = (gAudioCtx.audioBufferParameters.ticksPerUpdate - arg0 - 1) * gAudioCtx.numNotes;

    for (i = 0; i < (u32)gAudioCtx.audioBufferParameters.numSequencePlayers; i++) {
        seqPlayer = &gAudioCtx.seqPlayers[i];
        if (seqPlayer->enabled == true) {
            AudioSeq_SequencePlayerProcessSequence(seqPlayer);
            Audio_SequencePlayerProcessSound(seqPlayer);
        }
    }

    Audio_ProcessNotes();
}

/**
 * original name: Nas_SeqSkip
 */
void AudioSeq_SkipForwardSequence(SequencePlayer* seqPlayer) {
    while (seqPlayer->skipTicks > 0) {
        AudioSeq_SequencePlayerProcessSequence(seqPlayer);
        Audio_SequencePlayerProcessSound(seqPlayer);
        seqPlayer->skipTicks--;
    }
}

/**
 * original name: Nas_InitMySeq
 */
void AudioSeq_ResetSequencePlayer(SequencePlayer* seqPlayer) {
    s32 i;
#if defined(TARGET_PSP)
    SequenceChannel* channel;

    if (!OotPspAudio_IsAlignedNativePtr(seqPlayer)) {
        OotPspAudio_LogBadChannel("reset-seq", seqPlayer, -1, NULL);
        return;
    }
#endif

    AudioSeq_SequencePlayerDisable(seqPlayer);
    seqPlayer->stopScript = false;
    seqPlayer->delay = 0;
    seqPlayer->state = 1;
    seqPlayer->fadeTimer = 0;
    seqPlayer->fadeTimerUnkEu = 0;
    seqPlayer->tempoAcc = 0;
    seqPlayer->tempo = 120 * SEQTICKS_PER_BEAT; // 120 BPM
    seqPlayer->tempoChange = 0;
    seqPlayer->transposition = 0;
    seqPlayer->noteAllocPolicy = 0;
    seqPlayer->shortNoteVelocityTable = gDefaultShortNoteVelocityTable;
    seqPlayer->shortNoteGateTimeTable = gDefaultShortNoteGateTimeTable;
    seqPlayer->scriptCounter = 0;
    seqPlayer->fadeVolume = 1.0f;
    seqPlayer->fadeVelocity = 0.0f;
    seqPlayer->volume = 0.0f;
    seqPlayer->muteVolumeScale = 0.5f;

    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
#if defined(TARGET_PSP)
        channel = OotPspAudio_GetSafeSequenceChannel(seqPlayer, i, "reset-channel");
        if (channel != NULL) {
            AudioSeq_InitSequenceChannel(channel);
        }
#else
        AudioSeq_InitSequenceChannel(seqPlayer->channels[i]);
#endif
    }
}

/**
 * original name: Nas_AssignSubTrack
 */
void AudioSeq_InitSequencePlayerChannels(s32 playerIdx) {
    SequenceChannel* channel;
    SequencePlayer* seqPlayer;
    s32 i;
    s32 j;

#if defined(TARGET_PSP)
    if ((playerIdx < 0) || (playerIdx >= (s32)ARRAY_COUNT(sOotPspAudioSequenceChannels))) {
        return;
    }
#endif

    seqPlayer = &gAudioCtx.seqPlayers[playerIdx];

#if defined(TARGET_PSP)
    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
        channel = &sOotPspAudioSequenceChannels[playerIdx][i];
        memset(channel, 0, sizeof(*channel));
        seqPlayer->channels[i] = channel;
        channel->seqPlayer = seqPlayer;
        channel->enabled = false;
        for (j = 0; j < ARRAY_COUNT(channel->layers); j++) {
            channel->layers[j] = NULL;
        }
        AudioSeq_InitSequenceChannel(channel);
    }
#else
    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
        seqPlayer->channels[i] = AudioHeap_AllocZeroed(&gAudioCtx.miscPool, sizeof(SequenceChannel));
        if (seqPlayer->channels[i] == NULL) {
            seqPlayer->channels[i] = &gAudioCtx.sequenceChannelNone;
        } else {
            channel = seqPlayer->channels[i];
            channel->seqPlayer = seqPlayer;
            channel->enabled = false;
            for (j = 0; j < ARRAY_COUNT(channel->layers); j++) {
                channel->layers[j] = NULL;
            }
        }
        AudioSeq_InitSequenceChannel(seqPlayer->channels[i]);
    }
#endif
}

/**
 * original name: __InitGroup
 */
void AudioSeq_InitSequencePlayer(SequencePlayer* seqPlayer) {
    s32 i;
    s32 j;

    for (i = 0; i < SEQ_NUM_CHANNELS; i++) {
        seqPlayer->channels[i] = &gAudioCtx.sequenceChannelNone;
    }

    seqPlayer->enabled = false;
    seqPlayer->muted = false;
    seqPlayer->fontDmaInProgress = false;
    seqPlayer->seqDmaInProgress = false;
    seqPlayer->applyBend = false;

    for (j = 0; j < ARRAY_COUNT(seqPlayer->seqScriptIO); j++) {
        seqPlayer->seqScriptIO[j] = SEQ_IO_VAL_NONE;
    }

    seqPlayer->muteBehavior = MUTE_BEHAVIOR_SOFTEN | MUTE_BEHAVIOR_STOP_NOTES;
    seqPlayer->fadeVolumeScale = 1.0f;
    seqPlayer->bend = 1.0f;
    Audio_InitNoteLists(&seqPlayer->notePool);
    AudioSeq_ResetSequencePlayer(seqPlayer);
}

/**
 * original name: Nas_InitPlayer
 */
void AudioSeq_InitSequencePlayers(void) {
    s32 i;

    AudioSeq_InitLayerFreelist();

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.sequenceLayers); i++) {
        gAudioCtx.sequenceLayers[i].channel = NULL;
        gAudioCtx.sequenceLayers[i].enabled = false;
    }

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.seqPlayers); i++) {
        AudioSeq_InitSequencePlayer(&gAudioCtx.seqPlayers[i]);
    }
}
