#ifndef OOT_PSP_ROM_PROFILES_H
#define OOT_PSP_ROM_PROFILES_H

#include "ultra64.h"

#include <stddef.h>

#define OOT_PSP_ROM_PROFILE_DMA_MISSING 0xFFFFU
#define OOT_PSP_ROM_PROFILE_DEFLATE (1U << 0)
#define OOT_PSP_ROM_MESSAGE_ASSET_COUNT 5
#define OOT_PSP_ROM_TEXT_NTSC 0
#define OOT_PSP_ROM_TEXT_PAL 1
#define OOT_PSP_ROM_TEXT_CN 2
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK 0x0FFFFFFFU
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_MODE_MASK 0xF0000000U
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_EXACT 0x00000000U
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_DLIST 0x10000000U
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_SCENE_COMMANDS 0x20000000U
#define OOT_PSP_ROM_ASSET_SPLICE_COPY_CUTSCENE 0x30000000U

typedef struct OotPspRomAssetSplice {
    u16 assetIndex;
    u8 sourceSegmentId;
    u8 targetSegmentId;
    u32 targetOffset;
    u32 sourceOffset;
    u32 copySize;
} OotPspRomAssetSplice;

typedef struct OotPspRomProfile {
    const char* name;
    u32 dmadataOffset;
    u16 dmadataCount;
    u16 flags;
    u32 njpgTextOffset;
    u32 njpgTextSize;
    u32 njpgDataOffset;
    u32 njpgDataSize;
    u32 sequenceFontTableOffset;
    u32 sequenceFontTableSize;
    u32 sequenceTableOffset;
    u32 soundFontTableOffset;
    u32 sampleBankTableOffset;
    u16 textLanguage;
    u16 reserved;
    u32 messageTableOffsets[OOT_PSP_ROM_MESSAGE_ASSET_COUNT];
    u16 messageDmaIndices[OOT_PSP_ROM_MESSAGE_ASSET_COUNT];
    u16 messagePadding;
    u32 nintendoLogoDlistOffset;
    u32 fileChooseDataOffset;
    u16 assetSpliceCount;
    u16 splicePadding;
    const OotPspRomAssetSplice* assetSplices;
    const u16* assetDmaIndices;
} OotPspRomProfile;

typedef struct OotPspRomDigest {
    u8 digest[16];
    u16 profileIndex;
    u16 region;
} OotPspRomDigest;

extern const OotPspRomProfile gOotPspRomProfiles[];
extern const size_t gOotPspRomProfileCount;
extern const OotPspRomDigest gOotPspRomDigests[];
extern const size_t gOotPspRomDigestCount;

const OotPspRomProfile* OotPspRomProfiles_FindDigest(const u8 digest[16]);
const OotPspRomProfile* OotPspRomProfiles_FindName(const char* name);
const OotPspRomProfile* OotPspRomProfiles_GetDefault(void);
const OotPspRomProfile* OotPspRomProfiles_GetActive(void);
size_t OotPspRomProfiles_GetActiveIndex(void);
void OotPspRomProfiles_SetActive(const OotPspRomProfile* profile, const u8 digest[16]);
s32 OotPspRomProfiles_GetActiveRegion(void);
u32 OotPspRomProfiles_MapResourceOffset(size_t assetIndex, u32 sourceOffset, size_t size);

#endif
