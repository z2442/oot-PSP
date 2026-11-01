#ifndef GFX_PSP_LIGHTING_H
#define GFX_PSP_LIGHTING_H

#include <stdint.h>

#define GFX_PSP_MAX_LIGHTS 4

struct GfxPspDirectionalLight {
    float direction[3];
    uint32_t color;
};

struct GfxPspLighting {
    uint32_t count;
    uint32_t ambient;
    struct GfxPspDirectionalLight lights[GFX_PSP_MAX_LIGHTS];
};

/* NULL selects vertex colors without hardware lighting. */
void gfx_scegu_set_lighting(const struct GfxPspLighting* lighting);

#endif
