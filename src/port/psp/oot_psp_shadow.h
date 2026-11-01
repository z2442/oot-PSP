#ifndef OOT_PSP_SHADOW_H
#define OOT_PSP_SHADOW_H

#include <stdint.h>

#define OOT_PSP_SHADOW_SIZE 64
#define OOT_PSP_SHADOW_MAX_STAMPS 256
#define OOT_PSP_SHADOW_TEXTURE_BYTES (64 * 64 * 4)
#define OOT_PSP_SHADOW_WRAP_ROWS 1
#define OOT_PSP_SHADOW_BORDER 2
#define OOT_PSP_SHADOW_GRADIENT 4

/* The original 4096-byte frame allocation now holds commands, not pixels.
 * Types 0..4 retain the game's body profiles; 5/6 are intact/torn cape stamps. */
typedef struct OotPspShadowStamp {
    int16_t x, y;
    uint16_t type;
} OotPspShadowStamp;

typedef struct OotPspShadowMask {
    uint16_t count, flags;
    int16_t gradientBase, gradientStep;
    OotPspShadowStamp stamps[OOT_PSP_SHADOW_MAX_STAMPS];
} OotPspShadowMask;

typedef char OotPspShadowMask_size_check[(sizeof(OotPspShadowMask) <= 4096) ? 1 : -1];

static inline void OotPspShadow_Begin(void* buffer, uint16_t flags) {
    OotPspShadowMask* mask = buffer;
    mask->count = 0;
    mask->flags = flags;
    mask->gradientBase = 255;
    mask->gradientStep = 0;
}

static inline void OotPspShadow_Add(void* buffer, int x, int y, unsigned int type) {
    OotPspShadowMask* mask = buffer;
    if (mask->count < OOT_PSP_SHADOW_MAX_STAMPS && type < 7) {
        mask->stamps[mask->count++] = (OotPspShadowStamp){x, y, type};
    }
}

void gfx_scegu_upload_shadow_mask(int tile, unsigned int textureId, const OotPspShadowMask* mask);

#endif
