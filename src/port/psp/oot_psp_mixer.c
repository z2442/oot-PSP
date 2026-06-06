#include "oot_psp_mixer.h"

#include "attributes.h"

#include <string.h>

#define OOT_PSP_DMEM_SIZE 0x2000
#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~15)
#define OOT_PSP_FILTER_TAP_COUNT 8
#define OOT_PSP_A_COPYBLOCKS 16

#define DMEM_U8(addr) (&sMixer.dmem.u8[(u16)(addr)])
#define DMEM_S16(addr) (&sMixer.dmem.s16[(u16)(addr) / sizeof(s16)])

typedef struct {
    u16 in;
    u16 out;
    u16 nbytes;
    ADPCM_STATE* adpcmLoopState;
    s16 adpcmTable[8][2][8];
    s16 filterScratch[OOT_PSP_DMEM_SIZE / sizeof(s16)];
    s32 filter2Count;
    s16 filter2Lut[OOT_PSP_FILTER_TAP_COUNT];
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
} OotPspMixerState;

static OotPspMixerState sMixer;

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

static s16 OotPspMixer_Clamp16(s64 value) {
    if (value < -0x8000) {
        return -0x8000;
    }
    if (value > 0x7FFF) {
        return 0x7FFF;
    }
    return value;
}

static s16 OotPspMixer_Vmulf(s16 left, s16 right) {
    return OotPspMixer_Clamp16((((s64)left * right * 2) + 0x8000) >> 16);
}

static s16 OotPspMixer_Vadd(s16 left, s16 right) {
    return OotPspMixer_Clamp16((s32)left + right);
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
    s32 j;
    s32 k;

    for (j = 0; j < 8; j++) {
        s32 acc = (table[0][j] * prev2) + (table[1][j] * prev1) + (ins[j] << 11);

        for (k = 0; k < j; k++) {
            acc += table[1][(j - k) - 1] * ins[k];
        }

        acc >>= 11;
        *out++ = OotPspMixer_Clamp16(acc);
    }

    *outPtr = out;
}

void OotPspMixer_ClearBuffer(u16 dmem, s32 nbytes) {
    memset(DMEM_U8(dmem), 0, ROUND_UP_16(nbytes));
}

void OotPspMixer_LoadBuffer(const void* source, u16 dmemDest, u16 nbytes) {
    memcpy(DMEM_U8(dmemDest), source, ROUND_DOWN_16(nbytes));
}

void OotPspMixer_SaveBuffer(u16 dmemSrc, void* dest, u16 nbytes) {
    memcpy(dest, DMEM_U8(dmemSrc), ROUND_DOWN_16(nbytes));
}

void OotPspMixer_LoadADPCM(s32 numEntriesBytes, const s16* book) {
    memcpy(sMixer.adpcmTable, book, numEntriesBytes);
}

void OotPspMixer_SetBuffer(UNUSED u8 flags, u16 dmemIn, u16 dmemOut, u16 nbytes) {
    sMixer.in = dmemIn;
    sMixer.out = dmemOut;
    sMixer.nbytes = nbytes;
}

void OotPspMixer_Interleave(u16 dmemOut, u16 dmemLeft, u16 dmemRight, u16 count) {
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
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_8(count);
    s32 i;

    for (i = 0; i < samples; i++) {
        out[i] = in[i * 2];
    }
}

void OotPspMixer_DMEMMove(u16 dmemIn, u16 dmemOut, s32 nbytes) {
    u8 block[16];
    s32 count = ROUND_UP_16(nbytes);
    s32 offset;

    for (offset = 0; offset < count; offset += sizeof(block)) {
        memcpy(block, DMEM_U8(dmemIn + offset), sizeof(block));
        memcpy(DMEM_U8(dmemOut + offset), block, sizeof(block));
    }
}

void OotPspMixer_SetLoop(ADPCM_STATE* state) {
    sMixer.adpcmLoopState = state;
}

void OotPspMixer_ADPCMdec(u8 flags, ADPCM_STATE state) {
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
    sMixer.env.initialReverb = initialReverb;
    sMixer.env.rampReverb = rampReverb;
    sMixer.env.rampLeft = rampLeft;
    sMixer.env.rampRight = rampRight;
}

void OotPspMixer_EnvSetup2(s32 volLeft, s32 volRight) {
    sMixer.env.volLeft = volLeft;
    sMixer.env.volRight = volRight;
}

static s16 OotPspMixer_MulHighSignedUnsigned(s16 sample, u16 volume) {
    return (s16)(((s64)sample * volume) >> 16);
}

void OotPspMixer_EnvMixer(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3, u32 dmemDests,
                          UNUSED u32 bits) {
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
            s16 left = OotPspMixer_MulHighSignedUnsigned(sample, volLeft);
            s16 right = OotPspMixer_MulHighSignedUnsigned(sample, volRight);
            s16 wetL;
            s16 wetR;

            left ^= dryLeftMask;
            right ^= dryRightMask;

            *dryLeft = OotPspMixer_Clamp16(*dryLeft + left);
            dryLeft++;
            *dryRight = OotPspMixer_Clamp16(*dryRight + right);
            dryRight++;

            wetL = OotPspMixer_MulHighSignedUnsigned(left, reverb);
            wetR = OotPspMixer_MulHighSignedUnsigned(right, reverb);
            wetL ^= wetLeftMask;
            wetR ^= wetRightMask;

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

void OotPspMixer_Mix(s32 countQuads, s16 gain, u16 dmemIn, u16 dmemOut) {
    s16* in = DMEM_S16(dmemIn);
    s16* out = DMEM_S16(dmemOut);
    s32 samples = ROUND_UP_32((countQuads & 0xFF) << 4) / sizeof(s16);
    s32 i;

    for (i = 0; i < samples; i++) {
        s64 accumulator = ((s64)out[i] * 0x7FFF * 2) + 0x8000;

        accumulator += (s64)in[i] * gain * 2;
        out[i] = OotPspMixer_Clamp16(accumulator >> 16);
    }
}

void OotPspMixer_AddMixer(s32 nbytes, u16 dmemIn, u16 dmemOut, UNUSED s16 gain) {
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
    s32 i;

    for (i = 0; i < numCopies; i++) {
        memcpy(DMEM_U8(dmemDest + (i * 128)), DMEM_U8(dmemSrc), 128);
    }
}

void OotPspMixer_CopyBlocks(s32 numBlocks, u16 dmemSrc, u16 dmemDest, s32 blockSize) {
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
    s16* save = state;
    s16* input;
    s16* out = sMixer.filterScratch;
    s16 history[OOT_PSP_FILTER_TAP_COUNT];
    s16 taps[OOT_PSP_FILTER_TAP_COUNT];
    s32 count;
    s32 processedCount;
    s32 i;
    u8 lastStateLowByte;

    /* Zelda ABI2 FILTER is a two-command sequence: setup taps/count, then process DMEM with persistent state. */
    if (flags > 1) {
        sMixer.filter2Count = ROUND_UP_16(countOrBuf);
        if (state != NULL) {
            memcpy(sMixer.filter2Lut, state, sizeof(sMixer.filter2Lut));
            sMixer.filter2Valid = 1;
        } else {
            sMixer.filter2Valid = 0;
        }
        return;
    }

    if ((save == NULL) || !sMixer.filter2Valid || (sMixer.filter2Count <= 0)) {
        return;
    }

    /* aspMain writes 31 state bytes, preserving the final low byte in RAM. */
    lastStateLowByte = (u16)save[15] & 0xFF;
    if (flags & A_INIT) {
        memset(save, 0, 16 * sizeof(s16));
    }

    /* RSP vector halfword pairs are reversed relative to native s16 array order. */
    for (i = 0; i < OOT_PSP_FILTER_TAP_COUNT; i++) {
        s32 logicalIndex = i ^ 1;
        s16 avg = OotPspMixer_Clamp16(
            ((((s64)save[OOT_PSP_FILTER_TAP_COUNT + logicalIndex] + sMixer.filter2Lut[logicalIndex]) * 0x8000) +
             0x8000) >>
            16);

        history[i] = save[logicalIndex];
        taps[i] = avg;
        save[OOT_PSP_FILTER_TAP_COUNT + logicalIndex] = avg;
    }
    save[15] = (save[15] & 0xFF00) | lastStateLowByte;

    input = DMEM_S16((u16)countOrBuf);
    count = sMixer.filter2Count;
    if (count > (s32)sizeof(sMixer.filterScratch)) {
        count = sizeof(sMixer.filterScratch);
    }
    processedCount = count;

    while (count > 0) {
        s16 blockInput[OOT_PSP_FILTER_TAP_COUNT];
        s64 out1[OOT_PSP_FILTER_TAP_COUNT];

        for (i = 0; i < OOT_PSP_FILTER_TAP_COUNT; i++) {
            blockInput[i] = input[i ^ 1];
        }

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

        out[0] = OotPspMixer_Clamp16((out1[1] + 0x4000) >> 15);
        out[1] = OotPspMixer_Clamp16((out1[0] + 0x4000) >> 15);
        out[2] = OotPspMixer_Clamp16((out1[3] + 0x4000) >> 15);
        out[3] = OotPspMixer_Clamp16((out1[2] + 0x4000) >> 15);
        out[4] = OotPspMixer_Clamp16((out1[5] + 0x4000) >> 15);
        out[5] = OotPspMixer_Clamp16((out1[4] + 0x4000) >> 15);
        out[6] = OotPspMixer_Clamp16((out1[7] + 0x4000) >> 15);
        out[7] = OotPspMixer_Clamp16((out1[6] + 0x4000) >> 15);

        memcpy(history, blockInput, sizeof(history));
        input += OOT_PSP_FILTER_TAP_COUNT;
        out += OOT_PSP_FILTER_TAP_COUNT;
        count -= OOT_PSP_FILTER_TAP_COUNT * sizeof(s16);
    }

    memcpy(state, input - OOT_PSP_FILTER_TAP_COUNT, OOT_PSP_FILTER_TAP_COUNT * sizeof(s16));
    memcpy(DMEM_S16((u16)countOrBuf), sMixer.filterScratch, processedCount);
}

void OotPspMixer_HiLoGain(s32 gain, u16 dmemIn, UNUSED u16 dmemOut, s32 nbytes) {
    s16* out = DMEM_S16(dmemIn);
    s32 samples = ROUND_UP_32(nbytes) / sizeof(s16);
    s32 i;

    gain &= 0xFF;
    for (i = 0; i < samples; i++) {
        out[i] = OotPspMixer_Clamp16((out[i] * gain) >> 4);
    }
}

void OotPspMixer_UnkCmd3(s32 arg1, s32 arg2, s32 size) {
    s16* src = DMEM_S16(arg1);
    s16* dst = DMEM_S16(arg2);
    s32 samples = ROUND_UP_32(size) / sizeof(s16);
    s32 i;

    for (i = 0; i < samples; i += 16) {
        s32 lane;

        for (lane = 0; lane < 8; lane++) {
            s64 product = (u16)src[i + 8 + lane] * (s64)src[i + lane];
            s64 doubled = product * 2;
            u16 highResult;

            if (doubled < -0x80000000LL) {
                highResult = 0;
            } else if (doubled > 0x7FFFFFFFLL) {
                highResult = 0xFFFF;
            } else {
                highResult = (u16)doubled;
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

void OotPspMixer_ExecuteCommandList(const Acmd* cmdList, s32 cmdCount) {
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
