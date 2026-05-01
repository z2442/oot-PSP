#include "ultra64.h"
#include "libu64/debug.h"
#include "libu64/mtxuty-cvt.h"

void MtxConv_F2L(Mtx* m1, MtxF* m2) {
#if !PLATFORM_PSP
    s32 i;
    s32 j;
#endif

    LOG_UTILS_CHECK_NULL_POINTER("m1", m1, "../mtxuty-cvt.c", 31);
    LOG_UTILS_CHECK_NULL_POINTER("m2", m2, "../mtxuty-cvt.c", 32);

#if PLATFORM_PSP
    guMtxF2L(m2->mf, m1);
#else
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 value = (m2->mf[i][j] * 0x10000);

            m1->intPart[i][j] = value >> 16;
            m1->fracPart[i][j] = value;
        }
    }
#endif
}

void MtxConv_L2F(MtxF* m1, Mtx* m2) {
    LOG_UTILS_CHECK_NULL_POINTER("m1", m1, "../mtxuty-cvt.c", 55);
    LOG_UTILS_CHECK_NULL_POINTER("m2", m2, "../mtxuty-cvt.c", 56);
    guMtxL2F(m1->mf, m2);
}
