#ifndef OOT_PSP_AUDIO_BACKEND_H
#define OOT_PSP_AUDIO_BACKEND_H

#include "ultra64.h"

s32 OotPspAudioBackend_Init(void);
s32 OotPspAudioBackend_Queue(const void* buf, u32 size);
s32 OotPspAudioBackend_SetFrequency(u32 frequency);
u32 OotPspAudioBackend_GetLength(void);
s32 OotPspAudioBackend_NeedsRefillUrgently(void);

void OotPspAudio_Init(void);
void OotPspAudio_Update(void);

#endif
