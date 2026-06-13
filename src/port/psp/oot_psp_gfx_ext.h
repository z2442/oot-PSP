#ifndef OOT_PORT_PSP_OOT_PSP_GFX_EXT_H
#define OOT_PORT_PSP_OOT_PSP_GFX_EXT_H

#include "ultra64/gbi.h"

#define OOT_PSP_HUD_ANCHOR_TAG 0x48554400U

typedef enum OotPspHudAnchor {
    OOT_PSP_HUD_ANCHOR_NONE,
    OOT_PSP_HUD_ANCHOR_LEFT,
    OOT_PSP_HUD_ANCHOR_CENTER,
    OOT_PSP_HUD_ANCHOR_RIGHT,
} OotPspHudAnchor;

#define gOotPspSetHudAnchor(pkt, anchor) gDPNoOpTag((pkt), OOT_PSP_HUD_ANCHOR_TAG | (anchor))

#endif
