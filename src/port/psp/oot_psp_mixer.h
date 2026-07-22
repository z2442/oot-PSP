#ifndef OOT_PSP_MIXER_H
#define OOT_PSP_MIXER_H

#include "ultra64.h"

#define OOT_PSP_MIXER_PROFILE_OPCODE_COUNT 32
#define OOT_PSP_MIXER_PROFILE_OPCODE_IDLE 0xFFFFFFFFU

/*
 * Written by the Media Engine through an uncached pointer and sampled by the
 * Allegrex diagnostic thread. Counter values are raw ME CP0 Count ticks; they
 * are intended for relative cost comparisons rather than wall-clock timing.
 */
typedef struct OotPspMixerOpcodeProfile {
    u32 sequence;
    u32 jobs;
    u32 commands;
    u32 jobTicks;
    u32 jobMaxTicks;
    u32 lastJobTicks;
    u32 lastJobCommands;
    u32 lastJobSlowOpcode;
    u32 lastJobSlowTicks;
    u32 maxJobCommands;
    u32 maxJobSlowOpcode;
    u32 maxJobSlowTicks;
    u32 currentOpcode;
    u32 currentCommandIndex;
    u32 opcodeCalls[OOT_PSP_MIXER_PROFILE_OPCODE_COUNT];
    u32 opcodeTicks[OOT_PSP_MIXER_PROFILE_OPCODE_COUNT];
    u32 opcodeMaxTicks[OOT_PSP_MIXER_PROFILE_OPCODE_COUNT];
} OotPspMixerOpcodeProfile;

void OotPspMixer_ClearBuffer(u16 dmem, s32 nbytes);
void OotPspMixer_LoadBuffer(const void* source, u16 dmemDest, u16 nbytes);
void OotPspMixer_SaveBuffer(u16 dmemSrc, void* dest, u16 nbytes);
void OotPspMixer_LoadADPCM(s32 numEntriesBytes, const s16* book);
void OotPspMixer_SetBuffer(u8 flags, u16 dmemIn, u16 dmemOut, u16 nbytes);
void OotPspMixer_Interleave(u16 dmemOut, u16 dmemLeft, u16 dmemRight, u16 count);
void OotPspMixer_Interl(u16 dmemIn, u16 dmemOut, u16 count);
void OotPspMixer_DMEMMove(u16 dmemIn, u16 dmemOut, s32 nbytes);
void OotPspMixer_SetLoop(ADPCM_STATE* state);
void OotPspMixer_ADPCMdec(u8 flags, ADPCM_STATE state);
void OotPspMixer_S8Dec(u8 flags, ADPCM_STATE state);
void OotPspMixer_Resample(u8 flags, u16 pitch, RESAMPLE_STATE state);
void OotPspMixer_ResampleZoh(u16 pitch, u16 pitchAccu);
void OotPspMixer_EnvSetup1(s32 initialReverb, s32 rampReverb, s32 rampLeft, s32 rampRight);
void OotPspMixer_EnvSetup2(s32 volLeft, s32 volRight);
void OotPspMixer_EnvMixer(u16 dmemSrc, s32 aiBufLen, s32 swapLR, s32 x0, s32 x1, s32 x2, s32 x3, u32 dmemDests,
                          u32 bits);
void OotPspMixer_Mix(s32 countQuads, s16 gain, u16 dmemIn, u16 dmemOut);
void OotPspMixer_AddMixer(s32 nbytes, u16 dmemIn, u16 dmemOut, s16 gain);
void OotPspMixer_Duplicate(s32 numCopies, u16 dmemSrc, u16 dmemDest);
void OotPspMixer_CopyBlocks(s32 numBlocks, u16 dmemSrc, u16 dmemDest, s32 blockSize);
void OotPspMixer_Filter(u8 flags, s32 countOrBuf, void* state);
void OotPspMixer_HiLoGain(s32 gain, u16 dmemIn, u16 dmemOut, s32 nbytes);
void OotPspMixer_UnkCmd3(s32 arg1, s32 arg2, s32 size);
void OotPspMixer_UnkCmd19(s32 arg1, s32 arg2, s32 size, s32 arg4);
void OotPspMixer_InitVme(void);
void OotPspMixer_ShutdownVme(void);
void OotPspMixer_ExecuteCommandList(const Acmd* cmdList, s32 cmdCount);
void OotPspMixer_ExecuteCommandListMe(const Acmd* cmdList, s32 cmdCount,
                                      volatile OotPspMixerOpcodeProfile* profile);
void OotPspMixer_InvalidateStateCache(void);

#if defined(OOT_PSP_MIXER_INLINE)
#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aResample
#undef aInterleave
#undef aInterl
#undef aSetVolume
#undef aSetVolume32
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aEnvSetup1
#undef aEnvSetup2
#undef aFilter
#undef aDuplicate
#undef aAddMixer
#undef aResampleZoh
#undef aS8Dec
#undef aHiLoGain
#undef aUnkCmd3
#undef aUnkCmd19
#undef aPoleFilter
#undef aPan

#define OOT_PSP_MIXER_EVAL(pkt) ((void)(pkt))

#define aSegment(pkt, s, b) OOT_PSP_MIXER_EVAL(pkt)
#define aClearBuffer(pkt, d, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_ClearBuffer(d, c))
#define aLoadBuffer(pkt, s, d, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_LoadBuffer(s, d, c))
#define aSaveBuffer(pkt, s, d, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_SaveBuffer(s, d, c))
#define aLoadADPCM(pkt, c, d) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_LoadADPCM(c, d))
#define aSetBuffer(pkt, f, i, o, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_SetBuffer(f, i, o, c))
#define aInterleave(pkt, o, l, r, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Interleave(o, l, r, c))
#define aInterl(pkt, i, o, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Interl(i, o, c))
#define aDMEMMove(pkt, i, o, c) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_DMEMMove(i, o, c))
#define aSetLoop(pkt, a) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_SetLoop((ADPCM_STATE*)(a)))
#define aADPCMdec(pkt, f, s) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_ADPCMdec(f, s))
#define aS8Dec(pkt, f, s) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_S8Dec(f, s))
#define aResample(pkt, f, p, s) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Resample(f, p, s))
#define aResampleZoh(pkt, p, a) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_ResampleZoh(p, a))
#define aEnvSetup1(pkt, a, b, c, d) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_EnvSetup1(a, b, c, d))
#define aEnvSetup2(pkt, l, r) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_EnvSetup2(l, r))
#define aEnvMixer(pkt, d, c, s, x0, x1, x2, x3, m, bits) \
    (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_EnvMixer(d, c, s, x0, x1, x2, x3, m, bits))
#define aMix(pkt, f, g, i, o) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Mix(f, g, i, o))
#define aAddMixer(pkt, c, i, o, g) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_AddMixer(c, i, o, g))
#define aDuplicate(pkt, n, s, d) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Duplicate(n, s, d))
#define aFilter(pkt, f, c, a) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_Filter(f, c, a))
#define aHiLoGain(pkt, gain, count, dmem, a4) \
    (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_HiLoGain(gain, dmem, a4, count))
#define aUnkCmd3(pkt, a1, a2, a3) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_UnkCmd3(a1, a2, a3))
#define aUnkCmd19(pkt, a1, a2, a3, a4) (OOT_PSP_MIXER_EVAL(pkt), OotPspMixer_UnkCmd19(a1, a2, a3, a4))
#define aSetVolume(pkt, f, v, t, r) OOT_PSP_MIXER_EVAL(pkt)
#define aSetVolume32(pkt, f, v, tr) OOT_PSP_MIXER_EVAL(pkt)
#define aPoleFilter(pkt, f, g, s) OOT_PSP_MIXER_EVAL(pkt)
#define aPan(pkt, f, d, s) OOT_PSP_MIXER_EVAL(pkt)
#endif

#endif
