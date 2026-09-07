#include "oot_psp_message_tables.h"

#include "oot_psp_rom_profiles.h"

#include <stdio.h>
#include <stdlib.h>

#define OOT_PSP_MESSAGE_ENTRY_MAX 4096
#define OOT_PSP_MESSAGE_SEGMENT_MASK 0x0F000000U

typedef struct OotPspRuntimeMessageTable {
    OotPspMessageEntry* entries;
    size_t count;
} OotPspRuntimeMessageTable;

static OotPspRuntimeMessageTable sOotPspRuntimeMessageTables[OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT];

static u16 OotPspMessageTables_ReadBe16(const u8* data) {
    return ((u16)data[0] << 8) | data[1];
}

static u32 OotPspMessageTables_ReadBe32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
}

static void OotPspMessageTables_Clear(void) {
    size_t i;

    for (i = 0; i < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; i++) {
        free(sOotPspRuntimeMessageTables[i].entries);
        sOotPspRuntimeMessageTables[i].entries = NULL;
        sOotPspRuntimeMessageTables[i].count = 0;
    }
}

static s32 OotPspMessageTables_ReadCode(u32 offset, void* output, size_t size) {
    uintptr_t codeVrom = OotPsp_GetExternalAssetVromStart("code");
    size_t codeSize = OotPsp_GetExternalAssetSize("code");

    return (codeVrom != 0) && (offset <= codeSize) && (size <= codeSize - offset) &&
           (OotPsp_AssetRead(output, codeVrom + offset, size) == OOT_PSP_ASSET_READ_OK);
}

static s32 OotPspMessageTables_LoadComplete(size_t messageIndex, u32 codeOffset) {
    OotPspRuntimeMessageTable* table = &sOotPspRuntimeMessageTables[messageIndex];
    uintptr_t messageVrom = OotPsp_GetExternalMessageAssetVromStart(messageIndex);
    size_t messageSize = OotPsp_GetExternalMessageAssetSize(messageIndex);
    size_t codeSize = OotPsp_GetExternalAssetSize("code");
    size_t rawSize;
    u8* raw;
    size_t count;
    size_t i;

    if ((codeOffset == 0) || (messageVrom == 0) || (messageSize == 0) || (codeOffset >= codeSize)) {
        return false;
    }
    rawSize = codeSize - codeOffset;
    if (rawSize > (OOT_PSP_MESSAGE_ENTRY_MAX * 8)) {
        rawSize = OOT_PSP_MESSAGE_ENTRY_MAX * 8;
    }
    raw = malloc(rawSize);
    if ((raw == NULL) || !OotPspMessageTables_ReadCode(codeOffset, raw, rawSize)) {
        free(raw);
        return false;
    }
    for (count = 0; (count < OOT_PSP_MESSAGE_ENTRY_MAX) && (((count + 1) * 8) <= rawSize); count++) {
        if (OotPspMessageTables_ReadBe16(raw + (count * 8)) == 0xFFFF) {
            break;
        }
    }
    if ((count == 0) || (count == OOT_PSP_MESSAGE_ENTRY_MAX) || (((count + 1) * 8) > rawSize)) {
        free(raw);
        return false;
    }
    table->entries = malloc(count * sizeof(*table->entries));
    if (table->entries == NULL) {
        free(raw);
        return false;
    }
    table->count = count;
    for (i = 0; i < count; i++) {
        const u8* source = raw + (i * 8);
        const u8* next = source + 8;
        OotPspMessageEntry* entry = &table->entries[i];
        u16 nextTextId = OotPspMessageTables_ReadBe16(next);
        u32 start = OotPspMessageTables_ReadBe32(source + 4) & ~OOT_PSP_MESSAGE_SEGMENT_MASK;
        u32 end = nextTextId == 0xFFFF
                      ? messageSize
                      : (OotPspMessageTables_ReadBe32(next + 4) & ~OOT_PSP_MESSAGE_SEGMENT_MASK);

        entry->textId = OotPspMessageTables_ReadBe16(source);
        entry->typePos = source[2];
        entry->pad = 0;
        if (((i != 0) && (entry->textId <= table->entries[i - 1].textId)) ||
            (start > end) || (end > messageSize)) {
            free(raw);
            return false;
        }
        entry->vromStart = messageVrom + start;
        entry->vromEnd = messageVrom + end;
    }
    free(raw);
    return true;
}

static s32 OotPspMessageTables_LoadChild(size_t messageIndex, u32 codeOffset, size_t parentIndex) {
    OotPspRuntimeMessageTable* table = &sOotPspRuntimeMessageTables[messageIndex];
    const OotPspRuntimeMessageTable* parent = &sOotPspRuntimeMessageTables[parentIndex];
    uintptr_t messageVrom = OotPsp_GetExternalMessageAssetVromStart(messageIndex);
    size_t messageSize = OotPsp_GetExternalMessageAssetSize(messageIndex);
    size_t count = 0;
    size_t rawCount;
    u8* raw;
    size_t parentEntry;
    size_t rawIndex = 0;

    if ((codeOffset == 0) || (messageVrom == 0) || (messageSize == 0) ||
        (parent->entries == NULL) || (parent->count == 0)) {
        return false;
    }
    for (parentEntry = 0; parentEntry < parent->count; parentEntry++) {
        count += parent->entries[parentEntry].textId != 0xFFFC;
    }
    rawCount = count + 1;
    raw = malloc(rawCount * 4);
    table->entries = malloc(count * sizeof(*table->entries));
    if ((raw == NULL) || (table->entries == NULL) ||
        !OotPspMessageTables_ReadCode(codeOffset, raw, rawCount * 4)) {
        free(raw);
        return false;
    }
    table->count = count;
    for (parentEntry = 0; parentEntry < parent->count; parentEntry++) {
        const OotPspMessageEntry* parentMessage = &parent->entries[parentEntry];
        OotPspMessageEntry* entry;
        u32 start;
        u32 end;

        if (parentMessage->textId == 0xFFFC) {
            continue;
        }
        entry = &table->entries[rawIndex];
        start = OotPspMessageTables_ReadBe32(raw + (rawIndex * 4)) & ~OOT_PSP_MESSAGE_SEGMENT_MASK;
        end = parentMessage->textId == 0xFFFD
                  ? messageSize
                  : (OotPspMessageTables_ReadBe32(raw + ((rawIndex + 1) * 4)) &
                     ~OOT_PSP_MESSAGE_SEGMENT_MASK);
        if ((start > end) || (end > messageSize)) {
            free(raw);
            return false;
        }
        entry->textId = parentMessage->textId;
        entry->typePos = parentMessage->typePos;
        entry->pad = 0;
        entry->vromStart = messageVrom + start;
        entry->vromEnd = messageVrom + end;
        rawIndex++;
    }
    free(raw);
    return rawIndex == count;
}

s32 OotPspMessageTables_LoadActive(void) {
    const OotPspRomProfile* profile = OotPspRomProfiles_GetActive();
    s32 ok = false;

    OotPspMessageTables_Clear();
    if (profile == NULL) {
        return false;
    }
    if (profile->textLanguage == OOT_PSP_ROM_TEXT_PAL) {
        ok = OotPspMessageTables_LoadComplete(1, profile->messageTableOffsets[1]) &&
             OotPspMessageTables_LoadChild(2, profile->messageTableOffsets[2], 1) &&
             OotPspMessageTables_LoadChild(3, profile->messageTableOffsets[3], 1) &&
             OotPspMessageTables_LoadComplete(4, profile->messageTableOffsets[4]);
    } else {
        ok = OotPspMessageTables_LoadComplete(0, profile->messageTableOffsets[0]) &&
             OotPspMessageTables_LoadComplete(1, profile->messageTableOffsets[1]) &&
             OotPspMessageTables_LoadComplete(4, profile->messageTableOffsets[4]);
    }
    if (!ok) {
        printf("oot-psp could not load %s message metadata\n", profile->name);
        OotPspMessageTables_Clear();
    }
    return ok;
}

void OotPspMessageTables_Resolve(const OotPspMessageEntry** entries, size_t* count) {
    size_t index;

    if ((entries == NULL) || (count == NULL)) {
        return;
    }
    if (*entries == gOotPspJpnMessageEntries) {
        index = 0;
    } else if (*entries == gOotPspNesMessageEntries) {
        index = 1;
    } else if (*entries == gOotPspGerMessageEntries) {
        index = 2;
    } else if (*entries == gOotPspFraMessageEntries) {
        index = 3;
    } else if (*entries == gOotPspStaffMessageEntries) {
        index = 4;
    } else {
        return;
    }
    *entries = sOotPspRuntimeMessageTables[index].entries;
    *count = sOotPspRuntimeMessageTables[index].count;
}
