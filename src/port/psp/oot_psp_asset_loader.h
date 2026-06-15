#ifndef OOT_PSP_ASSET_LOADER_H
#define OOT_PSP_ASSET_LOADER_H

#include "romfile.h"
#include "ultra64.h"

#include <stddef.h>
#include <stdint.h>

#define OOT_PSP_ASSET_READ_FAILED (-1)
#define OOT_PSP_ASSET_READ_OK 0
#define OOT_PSP_ASSET_READ_NOT_EXTERNAL 1
#define OOT_PSP_EXTERNAL_ASSET_NATIVE 1
#define OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS 2

typedef struct OotPspExternalAsset {
    uintptr_t vromStart;
    uintptr_t vromEnd;
    uintptr_t originalVromStart;
    uintptr_t originalVromEnd;
    u32 flags;
    uintptr_t fileOffset;
    const char* path;
} OotPspExternalAsset;

typedef struct OotPspExternalAssetTextureRange {
    uintptr_t vromStart;
    uintptr_t vromEnd;
} OotPspExternalAssetTextureRange;

typedef struct OotPspMessageEntry {
    u16 textId;
    u8 typePos;
    u8 pad;
    uintptr_t vromStart;
    uintptr_t vromEnd;
} OotPspMessageEntry;

extern const OotPspExternalAsset gOotPspExternalAssets[];
extern const size_t gOotPspExternalAssetCount;
extern const OotPspExternalAssetTextureRange gOotPspExternalAssetTextureRanges[];
extern const size_t gOotPspExternalAssetTextureRangeCount;
extern const OotPspMessageEntry gOotPspJpnMessageEntries[];
extern const size_t gOotPspJpnMessageEntriesCount;
extern const OotPspMessageEntry gOotPspNesMessageEntries[];
extern const size_t gOotPspNesMessageEntriesCount;
extern const OotPspMessageEntry gOotPspGerMessageEntries[];
extern const size_t gOotPspGerMessageEntriesCount;
extern const OotPspMessageEntry gOotPspFraMessageEntries[];
extern const size_t gOotPspFraMessageEntriesCount;
extern const OotPspMessageEntry gOotPspStaffMessageEntries[];
extern const size_t gOotPspStaffMessageEntriesCount;

void OotPsp_AssetInit(const char* executablePath);
const char* OotPsp_ResolveRootPath(const char* path, char* buffer, size_t bufferSize);
uintptr_t OotPsp_GetPrxRelocationBias(void);
s32 OotPsp_TryNormalizePrxRelocatedAddress(uintptr_t addr, uintptr_t* normalized);
uintptr_t OotPsp_NormalizeVrom(uintptr_t vrom);
s32 OotPsp_NormalizeVromRange(uintptr_t vromStart, uintptr_t vromEnd, uintptr_t* normalizedStart,
                              uintptr_t* normalizedEnd);
void OotPsp_NormalizeRomFile(RomFile* file);
s32 OotPsp_IsNativeExternalTextureRange(const void* ptr, size_t size);
s32 OotPsp_IsNativeExternalTextureByte(const void* ptr);
s32 OotPsp_IsLoadedExternalAssetRange(const void* ptr, size_t size);
s32 OotPsp_IsLoadedNativeExternalAssetRange(const void* ptr, size_t size);
s32 OotPsp_GetLoadedExternalAssetRangeFlags(const void* ptr, size_t size, u32* flags);
u32 OotPsp_GetExternalAssetRangeSerial(const void* ptr, size_t size);
s32 OotPsp_GetNativeExternalTextureRangeStart(const void* ptr, size_t size, uintptr_t* ramStart);
s32 OotPsp_MapNativeExternalTextureByte(const void* ptr, const void** mapped);
s32 OotPsp_AssetRead(void* ram, uintptr_t vrom, size_t size);
s32 OotPsp_AssetReadAudio(void* ram, uintptr_t vrom, size_t size);
s32 OotPsp_AssetReadAudioUrgent(void* ram, uintptr_t vrom, size_t size);
const OotPspMessageEntry* OotPsp_FindMessageEntry(const OotPspMessageEntry* entries, size_t count, u16 textId);

#endif
