#include "file_select.h"

#include "ultra64.h"
#include "gfx.h"
#include "versions.h"
#if PLATFORM_PSP
#include "oot_psp_asset_loader.h"
#include "oot_psp_rom_profiles.h"
#endif

#if PLATFORM_PSP
extern u8 _ovl_file_chooseSegmentRomStart[];
#endif

Vtx gNameEntryVtx[] = {
#include "assets/overlays/ovl_file_choose/gNameEntryVtx.inc.c"
};

#if OOT_NTSC
s16 gCharPageHira[] = {
#include "assets/overlays/ovl_file_choose/gCharPageHira.inc.c"
};

s16 gCharPageKata[] = {
#include "assets/overlays/ovl_file_choose/gCharPageKata.inc.c"
};

s16 gCharPageEng[] = {
#include "assets/overlays/ovl_file_choose/gCharPageEng.inc.c"
};

s16 gNextCharPage[] = {
#include "assets/overlays/ovl_file_choose/gNextCharPage.inc.c"
};
#endif

#if PLATFORM_PSP && OOT_PAL_N64
Vtx gOptionsMenuHeadersVtx[24];
#else
Vtx gOptionsMenuHeadersVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuHeadersVtx.inc.c"
};
#endif

#if OOT_PAL && (PLATFORM_GC || (PLATFORM_PSP && !OOT_PAL_N64))
#if PLATFORM_PSP
Vtx gOptionsMenuHeadersGERVtx[16];
#else
Vtx gOptionsMenuHeadersGERVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuHeadersGERVtx.inc.c"
};
#endif
#endif

Vtx gOptionsMenuSettingsVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuSettingsVtx.inc.c"
};

#if OOT_PAL && (PLATFORM_GC || (PLATFORM_PSP && !OOT_PAL_N64))
#if PLATFORM_PSP
Vtx gOptionsMenuSettingsGERVtx[32];
#else
Vtx gOptionsMenuSettingsGERVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuSettingsGERVtx.inc.c"
};
#endif
#endif

#if OOT_PAL_N64
#if PLATFORM_PSP
Vtx gOptionsMenuBrightnessVtx[8];
Vtx gOptionsMenuLanguageVtx[12];
#else
Vtx gOptionsMenuBrightnessVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuBrightnessVtx.inc.c"
};

Vtx gOptionsMenuLanguageVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsMenuLanguageVtx.inc.c"
};
#endif

u8 ovl_file_choose_zeros_000640[320] = { 0 };
#endif

Vtx gOptionsDividerSoundVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsDividerSoundVtx.inc.c"
};

Vtx gOptionsDividerZTargetVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsDividerZTargetVtx.inc.c"
};

Vtx gOptionsDividerBrightnessVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsDividerBrightnessVtx.inc.c"
};

#if OOT_PAL_N64
#if PLATFORM_PSP
Vtx gOptionsDividerLanguageVtx[4];
#else
Vtx gOptionsDividerLanguageVtx[] = {
#include "assets/overlays/ovl_file_choose/gOptionsDividerLanguageVtx.inc.c"
};
#endif
#endif

#if OOT_PAL
s16 gCharPageEng[] = {
#include "assets/overlays/ovl_file_choose/gCharPageEng.inc.c"
};
#endif

#if PLATFORM_PSP
static s32 OotPspFileSelect_Read(void* output, size_t size, u32 resourceOffset) {
    const OotPspRomProfile* profile = OotPspRomProfiles_GetActive();

    return (profile != NULL) &&
           (OotPsp_AssetRead(output,
                             (uintptr_t)_ovl_file_chooseSegmentRomStart + profile->fileChooseDataOffset +
                                 resourceOffset,
                             size) == OOT_PSP_ASSET_READ_OK);
}

static void OotPspFileSelect_SwapVtx(Vtx* vertices, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        u8* bytes = (u8*)&vertices[i];
        size_t offset;

        for (offset = 0; offset < 12; offset += 2) {
            u8 value = bytes[offset];

            bytes[offset] = bytes[offset + 1];
            bytes[offset + 1] = value;
        }
    }
}

static void OotPspFileSelect_LoadVtx(Vtx* vertices, size_t count, u32 offset) {
    if (OotPspFileSelect_Read(vertices, count * sizeof(*vertices), offset)) {
        OotPspFileSelect_SwapVtx(vertices, count);
    }
}

static void OotPspFileSelect_LoadS16(s16* values, size_t count, u32 offset) {
    size_t i;

    if (!OotPspFileSelect_Read(values, count * sizeof(*values), offset)) {
        return;
    }
    for (i = 0; i < count; i++) {
        u8* bytes = (u8*)&values[i];
        u8 value = bytes[0];

        bytes[0] = bytes[1];
        bytes[1] = value;
    }
}

void OotPspFileSelect_LoadStaticData(void) {
    OotPspFileSelect_LoadVtx(gNameEntryVtx, 24, 0x000);
#if OOT_NTSC
    OotPspFileSelect_LoadS16(gCharPageHira, 65, 0x180);
    OotPspFileSelect_LoadS16(gCharPageKata, 65, 0x204);
    OotPspFileSelect_LoadS16(gCharPageEng, 65, 0x288);
    OotPspFileSelect_LoadS16(gNextCharPage, 9, 0x30C);
    OotPspFileSelect_LoadVtx(gOptionsMenuHeadersVtx, 16, 0x320);
    OotPspFileSelect_LoadVtx(gOptionsMenuSettingsVtx, 32, 0x420);
    OotPspFileSelect_LoadVtx(gOptionsDividerSoundVtx, 4, 0x620);
    OotPspFileSelect_LoadVtx(gOptionsDividerZTargetVtx, 4, 0x660);
    OotPspFileSelect_LoadVtx(gOptionsDividerBrightnessVtx, 4, 0x6A0);
#elif OOT_PAL_N64
    OotPspFileSelect_LoadVtx(gOptionsMenuHeadersVtx, 24, 0x180);
    OotPspFileSelect_LoadVtx(gOptionsMenuSettingsVtx, 32, 0x300);
    OotPspFileSelect_LoadVtx(gOptionsMenuBrightnessVtx, 8, 0x500);
    OotPspFileSelect_LoadVtx(gOptionsMenuLanguageVtx, 12, 0x580);
    OotPspFileSelect_LoadVtx(gOptionsDividerSoundVtx, 4, 0x780);
    OotPspFileSelect_LoadVtx(gOptionsDividerZTargetVtx, 4, 0x7C0);
    OotPspFileSelect_LoadVtx(gOptionsDividerBrightnessVtx, 4, 0x800);
    OotPspFileSelect_LoadVtx(gOptionsDividerLanguageVtx, 4, 0x840);
    OotPspFileSelect_LoadS16(gCharPageEng, 65, 0x880);
#else
    OotPspFileSelect_LoadVtx(gOptionsMenuHeadersVtx, 16, 0x180);
    OotPspFileSelect_LoadVtx(gOptionsMenuHeadersGERVtx, 16, 0x280);
    OotPspFileSelect_LoadVtx(gOptionsMenuSettingsVtx, 32, 0x380);
    OotPspFileSelect_LoadVtx(gOptionsMenuSettingsGERVtx, 32, 0x580);
    OotPspFileSelect_LoadVtx(gOptionsDividerSoundVtx, 4, 0x780);
    OotPspFileSelect_LoadVtx(gOptionsDividerZTargetVtx, 4, 0x7C0);
    OotPspFileSelect_LoadVtx(gOptionsDividerBrightnessVtx, 4, 0x800);
    OotPspFileSelect_LoadS16(gCharPageEng, 65, 0x840);
#endif
}
#endif
