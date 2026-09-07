#include "oot_psp_audio_tables.h"

#include "audio.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_rom_profiles.h"
#include <stdio.h>
#include <string.h>

#define OOT_PSP_SEQUENCE_COUNT       110
#define OOT_PSP_SOUND_FONT_COUNT     38
#define OOT_PSP_SAMPLE_BANK_COUNT    7
#define OOT_PSP_SEQUENCE_FONT_SIZE   0x1C0
#define OOT_PSP_AUDIO_TABLE_RAW_MAX  (0x10 + (OOT_PSP_SEQUENCE_COUNT * 0x10))

static u16 OotPspAudioTables_ReadBe16(const u8* data) {
    return ((u16)data[0] << 8) | data[1];
}

static u32 OotPspAudioTables_ReadBe32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
}

static s32 OotPspAudioTables_LoadTable(AudioTable* table, u32 codeOffset, size_t expectedEntries,
                                       size_t audioAssetIndex) {
    u8 raw[OOT_PSP_AUDIO_TABLE_RAW_MAX];
    size_t rawSize = 0x10 + (expectedEntries * 0x10);
    size_t audioSize;
    size_t i;
    uintptr_t codeVrom = OotPsp_GetExternalAssetVromStart("code");

    if ((table == NULL) || (codeVrom == 0) || (rawSize > sizeof(raw)) ||
        (OotPsp_AssetRead(raw, codeVrom + codeOffset, rawSize) !=
         OOT_PSP_ASSET_READ_OK)) {
        return false;
    }
    if (OotPspAudioTables_ReadBe16(raw) != expectedEntries) {
        return false;
    }
    audioSize = OotPsp_GetExternalAudioAssetSize(audioAssetIndex);
    if (audioSize == 0) {
        return false;
    }

    memset(table, 0, rawSize);
    table->header.numEntries = (s16)expectedEntries;
    table->header.unkMediumParam = (s16)OotPspAudioTables_ReadBe16(raw + 2);
    table->header.romAddr = OotPspAudioTables_ReadBe32(raw + 4);
    for (i = 0; i < expectedEntries; i++) {
        const u8* source = raw + 0x10 + (i * 0x10);
        AudioTableEntry* entry = &table->entries[i];

        entry->romAddr = OotPspAudioTables_ReadBe32(source);
        entry->size = OotPspAudioTables_ReadBe32(source + 4);
        entry->medium = source[8];
        entry->cachePolicy = source[9];
        entry->shortData1 = (s16)OotPspAudioTables_ReadBe16(source + 0xA);
        entry->shortData2 = (s16)OotPspAudioTables_ReadBe16(source + 0xC);
        entry->shortData3 = (s16)OotPspAudioTables_ReadBe16(source + 0xE);
        if ((entry->size != 0) &&
            ((entry->romAddr > audioSize) || (entry->size > audioSize - entry->romAddr))) {
            return false;
        }
    }
    return true;
}

s32 OotPspAudioTables_LoadActive(void) {
    const OotPspRomProfile* profile = OotPspRomProfiles_GetActive();
    u8 sequenceFonts[OOT_PSP_SEQUENCE_FONT_SIZE];
    size_t i;
    uintptr_t codeVrom = OotPsp_GetExternalAssetVromStart("code");

    if ((profile == NULL) || (codeVrom == 0) ||
        (profile->sequenceFontTableSize != sizeof(sequenceFonts)) ||
        (OotPsp_AssetRead(sequenceFonts,
                          codeVrom + profile->sequenceFontTableOffset,
                          sizeof(sequenceFonts)) != OOT_PSP_ASSET_READ_OK) ||
        !OotPspAudioTables_LoadTable(&gSequenceTable, profile->sequenceTableOffset,
                                     OOT_PSP_SEQUENCE_COUNT, 1) ||
        !OotPspAudioTables_LoadTable(&gSoundFontTable, profile->soundFontTableOffset,
                                     OOT_PSP_SOUND_FONT_COUNT, 0) ||
        !OotPspAudioTables_LoadTable(&gSampleBankTable, profile->sampleBankTableOffset,
                                     OOT_PSP_SAMPLE_BANK_COUNT, 2)) {
        printf("oot-psp could not load %s audio metadata\n", profile != NULL ? profile->name : "ROM");
        return false;
    }

    memcpy(gSequenceFontTable, sequenceFonts, sizeof(sequenceFonts));
    for (i = 0; i < OOT_PSP_SEQUENCE_COUNT; i++) {
        u8* entry = &gSequenceFontTable[i * 2];
        u16 offset = OotPspAudioTables_ReadBe16(entry);

        entry[0] = offset;
        entry[1] = offset >> 8;
    }
    return true;
}

u32 OotPspAudioTables_GetSequenceVromStart(void) {
    return OotPsp_GetExternalAudioAssetVromStart(1);
}

u32 OotPspAudioTables_GetSoundFontVromStart(void) {
    return OotPsp_GetExternalAudioAssetVromStart(0);
}

u32 OotPspAudioTables_GetSampleBankVromStart(void) {
    return OotPsp_GetExternalAudioAssetVromStart(2);
}
