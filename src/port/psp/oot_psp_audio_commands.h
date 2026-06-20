#ifndef OOT_PSP_AUDIO_COMMANDS_H
#define OOT_PSP_AUDIO_COMMANDS_H

#include "ultra64.h"

#define OOT_PSP_A_COPYBLOCKS          16
#define OOT_PSP_A_REVERB_DOWNSAMPLE  24
#define OOT_PSP_A_REVERB_SAVE        26
#define OOT_PSP_A_REVERB_LOAD        27

typedef struct {
    s16* leftRingBuf;
    s16* rightRingBuf;
    s32 startPos;
    s16 lengthA;
    s16 lengthB;
    u8 downsampleRate;
    u8 pad[3];
} OotPspAudioReverbDownsampleCmd;

#define aOotPspAudioReverbDownsample(pkt, dmemLeft, desc)                           \
    {                                                                               \
        Acmd* _a = (Acmd*)(pkt);                                                    \
        _a->words.w0 = _SHIFTL(OOT_PSP_A_REVERB_DOWNSAMPLE, 24, 8) |               \
                       _SHIFTL((dmemLeft), 0, 16);                                  \
        _a->words.w1 = (uintptr_t)(desc);                                           \
    }

#define aOotPspAudioReverbSave(pkt, dmemLeft, desc)                                 \
    {                                                                               \
        Acmd* _a = (Acmd*)(pkt);                                                    \
        _a->words.w0 = _SHIFTL(OOT_PSP_A_REVERB_SAVE, 24, 8) |                      \
                       _SHIFTL((dmemLeft), 0, 16);                                  \
        _a->words.w1 = (uintptr_t)(desc);                                           \
    }

#define aOotPspAudioReverbLoad(pkt, dmemLeft, desc)                                 \
    {                                                                               \
        Acmd* _a = (Acmd*)(pkt);                                                    \
        _a->words.w0 = _SHIFTL(OOT_PSP_A_REVERB_LOAD, 24, 8) |                      \
                       _SHIFTL((dmemLeft), 0, 16);                                  \
        _a->words.w1 = (uintptr_t)(desc);                                           \
    }

#endif
