#ifndef OOT_PSP_AUDIO_PRODUCER_H
#define OOT_PSP_AUDIO_PRODUCER_H

#include "audio.h"

/* me-core.h selects SP=0x80200000 or 0x80400000 before entering our ME
 * callback. User Allegrex stacks are in user RAM; kernel bridge stacks are
 * outside this range. Do not use privileged CP0 reads on Allegrex to detect
 * the CPU. Update this predicate if the me-core stack contract changes. */
static inline s32 OotPspAudioProducer_IsMe(void) {
    uintptr_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    return sp >= 0x80000000u && sp < 0x80400000u;
}

enum {
    OOT_AUDIO_RPC_COMMAND = 1,
    OOT_AUDIO_RPC_MAINTENANCE,
    OOT_AUDIO_RPC_SAMPLE_DMA,
    OOT_AUDIO_RPC_SLOW_SAMPLE,
    OOT_AUDIO_RPC_SLOW_SEQ,
    OOT_AUDIO_RPC_SCRIPT_LOAD,
    OOT_AUDIO_RPC_INIT_SEQ
};
s32 OotPspAudioProducer_Active(void);
AudioContext* OotPspAudioProducer_GameContext(void);
u32 OotPspAudioProducer_Request(u32 op, u32 a, u32 b, u32 c, u32 d, u32 e);
s32 OotPspAudioProducer_SubmitBatch(void);
void OotPspAudioProducer_Pump(void);
s32 OotPspAudioProducer_CountNotes(s32 flags);
s32 OotPspAudioProducer_SampleRemaining(s32 player, s32 channel, s32 layer);
void OotPspAudioProducer_WaitTick(void);
s32 AudioLoad_HasPendingLoads(void);

#endif
