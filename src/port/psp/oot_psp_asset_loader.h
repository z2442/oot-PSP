#ifndef OOT_PSP_ASSET_LOADER_H
#define OOT_PSP_ASSET_LOADER_H

#include "romfile.h"
#include "oot_psp_asset_identity.h"
#include "ultra64.h"

#include <stddef.h>
#include <stdint.h>

#define OOT_PSP_ASSET_READ_FAILED (-1)
#define OOT_PSP_ASSET_READ_OK 0
#define OOT_PSP_ASSET_READ_NOT_EXTERNAL 1
#define OOT_PSP_EXTERNAL_ASSET_NATIVE 1
#define OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS 2
#define OOT_PSP_EXTERNAL_ASSET_SOURCE_TEXTURES 4
#define OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT 3
#define OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT 5

typedef struct OotPspExternalAsset {
    uintptr_t vromStart;
    uintptr_t vromEnd;
    uintptr_t originalVromStart;
    uintptr_t originalVromEnd;
    u32 flags;
    uintptr_t fileOffset;
    const char* name;
} OotPspExternalAsset;

typedef struct OotPspExternalAssetTextureRange {
    uintptr_t vromStart;
    uintptr_t vromEnd;
} OotPspExternalAssetTextureRange;

typedef struct OotPspExternalAssetLegacyRange {
    uintptr_t legacyVromStart;
    uintptr_t legacyVromEnd;
    uintptr_t vromStart;
} OotPspExternalAssetLegacyRange;

typedef struct OotPspExternalAudioAssetLayout {
    uintptr_t fileOffset;
    size_t size;
} OotPspExternalAudioAssetLayout;

typedef struct OotPspExternalMessageAssetLayout {
    uintptr_t fileOffset;
    size_t size;
} OotPspExternalMessageAssetLayout;

typedef struct OotPspExternalSceneAssetLayout {
    u32 assetIndex;
    u32 fileOffset;
    u32 size;
    u32 sourceVromStart;
} OotPspExternalSceneAssetLayout;

s32 OotPsp_SetExternalSceneAssetLayout(const OotPspExternalSceneAssetLayout* layouts);

typedef struct OotPspMessageEntry {
    u16 textId;
    u8 typePos;
    u8 pad;
    uintptr_t vromStart;
    uintptr_t vromEnd;
} OotPspMessageEntry;

extern const OotPspExternalAsset gOotPspExternalAssets[];
extern const size_t gOotPspExternalAssetCount;
extern const OotPspExternalAssetLegacyRange gOotPspExternalAssetLegacyRanges[];
extern const size_t gOotPspExternalAssetLegacyRangeCount;
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

s32 OotPsp_AssetInit(const char* executablePath);
s32 OotPsp_SetAssetRoot(const char* executablePath);
/* The code segment is the final packed asset and may retain a source ROM's
 * revision-specific size without moving canonical asset addresses. */
void OotPsp_SetExternalCodeAssetSize(size_t size);
/* Audio segment lengths vary by ROM revision. They are exposed through
 * isolated logical ranges so a larger bank cannot overlap the next segment. */
s32 OotPsp_SetExternalAudioAssetLayout(
    const OotPspExternalAudioAssetLayout layouts[OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT]);
uintptr_t OotPsp_GetExternalAudioAssetVromStart(size_t index);
size_t OotPsp_GetExternalAudioAssetSize(size_t index);
s32 OotPsp_SetExternalMessageAssetLayout(
    const OotPspExternalMessageAssetLayout layouts[OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT]);
uintptr_t OotPsp_GetExternalMessageAssetVromStart(size_t index);
size_t OotPsp_GetExternalMessageAssetSize(size_t index);
uintptr_t OotPsp_GetExternalAssetVromStart(const char* name);
size_t OotPsp_GetExternalAssetSize(const char* name);
/* Callback-safe notification. The next asset transaction reopens the packed
 * file without discarding any allocated cache storage. */
void OotPsp_AssetNotifyResume(void);
const char* OotPsp_ResolveRootPath(const char* path, char* buffer, size_t bufferSize);
uintptr_t OotPsp_NormalizeVrom(uintptr_t vrom);
void OotPsp_NormalizeRomFile(RomFile* file);
s32 OotPsp_IsNativeExternalTextureRange(const void* ptr, size_t size);
s32 OotPsp_IsLoadedNativeExternalAssetRange(const void* ptr, size_t size);
s32 OotPsp_GetLoadedExternalAssetRangeFlags(const void* ptr, size_t size, u32* flags);
/* Runtime patches first read original asset bytes, then transform selected
 * ranges into the linked PSP representation. Record properties of that final
 * representation so renderers do not interpret it as raw N64 data. */
s32 OotPsp_MarkLoadedExternalAssetRangeFlags(const void* ptr, size_t size, u32 flags);
u32 OotPsp_GetExternalAssetRangeSerial(const void* ptr, size_t size);
s32 OotPsp_GetNativeExternalTextureMappingRange(const void* ptr, uintptr_t* ramStart, uintptr_t* ramEnd);
s32 OotPsp_GetNativeExternalTextureRangeStart(const void* ptr, size_t size, uintptr_t* ramStart);
s32 OotPsp_MapNativeExternalTextureByte(const void* ptr, const void** mapped);
s32 OotPsp_AssetRead(void* ram, uintptr_t vrom, size_t size);
s32 OotPsp_AssetReadAudio(void* ram, uintptr_t vrom, size_t size);
s32 OotPsp_AssetReadAudioUrgent(void* ram, uintptr_t vrom, size_t size);
s32 OotPsp_AssetReadHasForegroundPressure(void);
const void* OotPsp_GetCachedAssetPointer(uintptr_t vrom, size_t size);
const OotPspMessageEntry* OotPsp_FindMessageEntry(const OotPspMessageEntry* entries, size_t count, u16 textId);

#endif
