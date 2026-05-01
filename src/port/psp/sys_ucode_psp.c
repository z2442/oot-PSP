#include "sys_ucode.h"

static u64 sPspDummyUCodeBoot[1] __attribute__((aligned(16)));
static u64 sPspDummyUCodeText[1] __attribute__((aligned(16)));
static u64 sPspDummyUCodeData[1] __attribute__((aligned(16)));

u64 gspS2DEX2d_fifoTextStart[1] __attribute__((aligned(16)));
u64 gspS2DEX2d_fifoTextEnd[1] __attribute__((aligned(16)));
u64 gspS2DEX2d_fifoDataStart[1] __attribute__((aligned(16)));
u64 gspS2DEX2d_fifoDataEnd[1] __attribute__((aligned(16)));

u64* SysUcode_GetUCodeBoot(void) {
    return sPspDummyUCodeBoot;
}

size_t SysUcode_GetUCodeBootSize(void) {
    return sizeof(sPspDummyUCodeBoot);
}

u64* SysUcode_GetUCode(void) {
    return sPspDummyUCodeText;
}

u64* SysUcode_GetUCodeData(void) {
    return sPspDummyUCodeData;
}
