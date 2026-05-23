#include "gameplay_keep_0x4D160.h"
#include "sun_textures.h"
#include "sun_evening_textures.h"
#include "gfx.h"

Gfx gKokiriDustMoteMaterialDL[9] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteMaterialDL.inc.c"
};

Gfx gKokiriDustMoteModelDL[3] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteModelDL.inc.c"
};

Gfx gSunDL[49] = {
#if PLATFORM_PSP
    gsSPMatrix(0x01000000, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW),
    gsDPPipeSync(),
    gsDPLoadTextureBlock_4b(gSun1Tex, G_IM_FMT_I, 64, 32, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                            G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSPVertex(&gSunVtx[0], 12, 0),
    gsSP2Triangles(0, 1, 2, 0, 2, 1, 3, 0),
    gsDPLoadTextureBlock_4b(gSun2Tex, G_IM_FMT_I, 64, 17, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                            G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSP2Triangles(4, 5, 6, 0, 6, 5, 7, 0),
    gsDPLoadTextureBlock_4b(gSun3Tex, G_IM_FMT_I, 64, 17, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                            G_TX_NOMIRROR | G_TX_CLAMP, 6, 5, G_TX_NOLOD, G_TX_NOLOD),
    gsSP2Triangles(8, 9, 10, 0, 10, 9, 11, 0),
    gsSPEndDisplayList(),
#else
#include "assets/objects/gameplay_keep/gSunDL.inc.c"
#endif
};

Vtx gSunVtx[] = {
#include "assets/objects/gameplay_keep/gSunVtx.inc.c"
};

Vtx gKokiriDustMoteModelVtx[] = {
#include "assets/objects/gameplay_keep/gKokiriDustMoteModelVtx.inc.c"
};
