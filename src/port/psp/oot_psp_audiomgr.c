#include "audiomgr.h"
#include "array_count.h"
#include "attributes.h"
#include "sfx.h"

void AudioMgr_Init(UNUSED AudioMgr* audioMgr, UNUSED void* stack, UNUSED OSPri pri, UNUSED OSId id,
                   UNUSED Scheduler* sched, UNUSED IrqMgr* irqMgr) {
}

void AudioMgr_WaitForInit(UNUSED AudioMgr* audioMgr) {
}

void AudioMgr_StopAllSfx(void) {
    static u8 sSfxBankIds[] = {
        BANK_PLAYER, BANK_ITEM, BANK_ENV, BANK_ENEMY, BANK_SYSTEM, BANK_OCARINA, BANK_VOICE,
    };
    u8 i;

    for (i = 0; i < ARRAY_COUNT(sSfxBankIds); i++) {
        Audio_StopSfxByBank(sSfxBankIds[i]);
    }
}
