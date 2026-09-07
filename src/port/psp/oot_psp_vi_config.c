#include "idle.h"
#include "vi_mode.h"
#include "versions.h"

s8 D_80009430 = 1;
vu8 gViConfigBlack = true;
u8 gViConfigAdditionalScanLines = 0;
u32 gViConfigFeatures = OS_VI_DITHER_FILTER_ON | OS_VI_GAMMA_OFF;
f32 gViConfigXScale = 1.0f;
f32 gViConfigYScale = 1.0f;
OSViMode gViConfigMode;
#if OOT_PAL_N64
u8 gViConfigModeType = OS_VI_PAL_LPN1;
#else
u8 gViConfigModeType = OS_VI_NTSC_LPN1;
#endif
