#ifndef OOT_PORT_PSP_OOT_PSP_GFX_EXT_H
#define OOT_PORT_PSP_OOT_PSP_GFX_EXT_H

#include "ultra64/gbi.h"

#define OOT_PSP_HUD_ANCHOR_TAG 0x48554400U
#define OOT_PSP_VISMONO_TAG 0x4D4F4E4FU
#define OOT_PSP_SHADOW_CMD 0x534844U

typedef enum OotPspHudAnchor {
    OOT_PSP_HUD_ANCHOR_NONE,
    OOT_PSP_HUD_ANCHOR_LEFT,
    OOT_PSP_HUD_ANCHOR_CENTER,
    OOT_PSP_HUD_ANCHOR_RIGHT,
} OotPspHudAnchor;

#define gOotPspSetHudAnchor(pkt, anchor) gDPNoOpTag((pkt), OOT_PSP_HUD_ANCHOR_TAG | (anchor))
#define gOotPspApplyVisMono(pkt) gDPNoOpTag((pkt), OOT_PSP_VISMONO_TAG)

/* A pointer-bearing NOOP extension, followed by the ordinary I8 texture load. */
#define gOotPspShadowMask(pkt, mask)                 \
    do {                                           \
        Gfx* shadowPkt = (pkt);                     \
        shadowPkt->words.w0 = _SHIFTL(G_NOOP, 24, 8) | OOT_PSP_SHADOW_CMD; \
        shadowPkt->words.w1 = (uintptr_t)(mask);    \
    } while (0)

#endif
