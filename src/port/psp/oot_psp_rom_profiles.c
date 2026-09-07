#include "oot_psp_rom_profiles.h"
#include "region.h"

#include <string.h>

static const OotPspRomProfile* sOotPspActiveRomProfile;
static s32 sOotPspActiveRegion = REGION_US;

const OotPspRomProfile* OotPspRomProfiles_FindDigest(const u8 digest[16]) {
    size_t i;

    if (digest == NULL) {
        return NULL;
    }
    for (i = 0; i < gOotPspRomDigestCount; i++) {
        if ((gOotPspRomDigests[i].profileIndex < gOotPspRomProfileCount) &&
            (memcmp(gOotPspRomDigests[i].digest, digest, 16) == 0)) {
            return &gOotPspRomProfiles[gOotPspRomDigests[i].profileIndex];
        }
    }
    return NULL;
}

const OotPspRomProfile* OotPspRomProfiles_FindName(const char* name) {
    size_t i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < gOotPspRomProfileCount; i++) {
        if (strcmp(gOotPspRomProfiles[i].name, name) == 0) {
            return &gOotPspRomProfiles[i];
        }
    }
    return NULL;
}

const OotPspRomProfile* OotPspRomProfiles_GetDefault(void) {
    const OotPspRomProfile* profile = OotPspRomProfiles_FindName("ntsc-1.0");

    return profile != NULL ? profile : (gOotPspRomProfileCount != 0 ? &gOotPspRomProfiles[0] : NULL);
}

const OotPspRomProfile* OotPspRomProfiles_GetActive(void) {
    return sOotPspActiveRomProfile != NULL ? sOotPspActiveRomProfile : OotPspRomProfiles_GetDefault();
}

size_t OotPspRomProfiles_GetActiveIndex(void) {
    const OotPspRomProfile* active = OotPspRomProfiles_GetActive();
    size_t i;

    for (i = 0; i < gOotPspRomProfileCount; i++) {
        if (active == &gOotPspRomProfiles[i]) {
            return i;
        }
    }
    return 0;
}

void OotPspRomProfiles_SetActive(const OotPspRomProfile* profile, const u8 digest[16]) {
    size_t i;

    sOotPspActiveRomProfile = profile;
    sOotPspActiveRegion = REGION_US;
    for (i = 0; digest != NULL && i < gOotPspRomDigestCount; i++) {
        if (memcmp(gOotPspRomDigests[i].digest, digest, 16) == 0) {
            sOotPspActiveRegion = gOotPspRomDigests[i].region;
            break;
        }
    }
}

s32 OotPspRomProfiles_GetActiveRegion(void) {
    return sOotPspActiveRegion;
}

/* Translate a format-indexed resource (such as a font glyph) whose source
 * ordering differs from the shared PSP asset layout. */
u32 OotPspRomProfiles_MapResourceOffset(size_t assetIndex, u32 sourceOffset, size_t size) {
    const OotPspRomProfile* profile = OotPspRomProfiles_GetActive();
    size_t i;

    for (i = 0; profile != NULL && i < profile->assetSpliceCount; i++) {
        const OotPspRomAssetSplice* splice = &profile->assetSplices[i];
        u32 copySize = splice->copySize & OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;

        if ((splice->assetIndex == assetIndex) && (sourceOffset >= splice->sourceOffset) &&
            (sourceOffset - splice->sourceOffset < copySize) &&
            (size <= copySize - (sourceOffset - splice->sourceOffset))) {
            return splice->targetOffset + sourceOffset - splice->sourceOffset;
        }
    }
    return sourceOffset;
}
