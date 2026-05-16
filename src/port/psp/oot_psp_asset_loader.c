#include "oot_psp_asset_loader.h"
#include "segment_symbols.h"

#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS 0x08800000U
#define OOT_PSP_RELOCATION_BIAS_MIN         0x08000000U
#define OOT_PSP_RELOCATION_BIAS_MAX         0x10000000U
#define OOT_PSP_NATIVE_ADDR_START           0x08800000U
#define OOT_PSP_NATIVE_ADDR_END             0x0C000000U
#define OOT_PSP_LOADED_ASSET_RANGE_COUNT    4096
#define OOT_PSP_ASSET_READ_CHUNK_SIZE       0x4000
#define OOT_PSP_ASSET_CACHE_SIZE            (4 * 1024 * 1024)
#define OOT_PSP_HOT_ASSET_CACHE_COUNT       4
#define OOT_PSP_HOT_READ_TRACKER_COUNT      16
#define OOT_PSP_HOT_READ_PROMOTE_COUNT      4
#define OOT_PSP_HOT_READ_MAX_SIZE           OOT_PSP_ASSET_CACHE_SIZE
#define OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT 8
#define OOT_PSP_KANJI_ASSET_PATH            "data/segments/kanji.bin"
#define OOT_PSP_LINK_ANIMATION_ASSET_PATH   "data/segments/link_animetion.bin"
#define OOT_PSP_ASSET_READ_ZERO_RETRY_COUNT 16
#define OOT_PSP_ASSET_READ_ZERO_RETRY_USEC  1000

typedef struct OotPspLoadedAssetRange {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    uintptr_t assetOffsetStart;
    u32 flags;
    u32 serial;
} OotPspLoadedAssetRange;

typedef struct OotPspAssetCache {
    const char* path;
    const OotPspExternalAsset* asset;
    u8* data;
    size_t capacity;
    size_t offset;
    size_t dataSize;
    u32 lastUsed;
    s32 failed;
} OotPspAssetCache;

typedef struct OotPspHotReadTracker {
    const OotPspExternalAsset* asset;
    u32 lastUsed;
    u8 hits;
} OotPspHotReadTracker;

static char sOotPspAssetRoot[256];
static uintptr_t sOotPspPrxRelocationBias = OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS;
static s32 sOotPspPrxRelocationBiasInitialized = false;
static OotPspLoadedAssetRange sOotPspLoadedAssetRanges[OOT_PSP_LOADED_ASSET_RANGE_COUNT];
static const OotPspLoadedAssetRange* sOotPspNativeTextureRangeCache[OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT];
static size_t sOotPspNativeTextureRangeCacheNext;
static size_t sOotPspLoadedAssetRangeNext;
static u32 sOotPspLoadedAssetSerial = 1;
static u32 sOotPspAssetCacheClock;
static OotPspAssetCache sOotPspPinnedAssetCaches[] = {
    { OOT_PSP_KANJI_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
    { OOT_PSP_LINK_ANIMATION_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
};
static OotPspAssetCache sOotPspHotAssetCaches[OOT_PSP_HOT_ASSET_CACHE_COUNT];
static OotPspHotReadTracker sOotPspHotReadTrackers[OOT_PSP_HOT_READ_TRACKER_COUNT];

#define OOT_PSP_PINNED_ASSET_CACHE_COUNT (sizeof(sOotPspPinnedAssetCaches) / sizeof(sOotPspPinnedAssetCaches[0]))

static void OotPsp_PreloadPersistentAssets(void);
static void OotPsp_ForgetNativeTextureRangeCache(const OotPspLoadedAssetRange* range);

void OotPsp_AssetInit(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t length;

    sOotPspAssetRoot[0] = '\0';

    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        printf("oot-psp asset root missing argv0\n");
        return;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    if (slash == NULL) {
        printf("oot-psp asset root no directory argv0=%s\n", executablePath);
        return;
    }

    length = (size_t)(slash - executablePath) + 1;
    if (length >= sizeof(sOotPspAssetRoot)) {
        length = sizeof(sOotPspAssetRoot) - 1;
    }

    memcpy(sOotPspAssetRoot, executablePath, length);
    sOotPspAssetRoot[length] = '\0';
    printf("oot-psp asset root=%s\n", sOotPspAssetRoot);
    OotPsp_PreloadPersistentAssets();
}

static s32 OotPsp_IsAbsolutePath(const char* path) {
    const char* slash;
    const char* colon;

    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }

    if ((path[0] == '/') || (path[0] == '\\')) {
        return true;
    }

    colon = strchr(path, ':');
    slash = strpbrk(path, "/\\");
    return (colon != NULL) && ((slash == NULL) || (colon < slash));
}

static const char* OotPsp_ResolveAssetPath(const char* path, char* buffer, size_t bufferSize) {
    int written;

    if (OotPsp_IsAbsolutePath(path) || (sOotPspAssetRoot[0] == '\0')) {
        return path;
    }

    written = snprintf(buffer, bufferSize, "%s%s", sOotPspAssetRoot, path);
    if ((written < 0) || ((size_t)written >= bufferSize)) {
        printf("oot-psp asset path too long root=%s path=%s\n", sOotPspAssetRoot, path);
        return path;
    }

    return buffer;
}

static int OotPsp_ReadAssetChunk(SceUID fd, u8* out, int chunk, const char* path, size_t readOffset) {
    int zeroReads = 0;

    while (true) {
        int read = sceIoRead(fd, out, chunk);

        if (read != 0) {
            return read;
        }

        if (zeroReads >= OOT_PSP_ASSET_READ_ZERO_RETRY_COUNT) {
            printf("oot-psp asset read zero path=%s off=%lu size=%d retries=%d\n", path,
                   (unsigned long)readOffset, chunk, zeroReads);
            return read;
        }

        zeroReads++;
        sceKernelDelayThread(OOT_PSP_ASSET_READ_ZERO_RETRY_USEC);
    }
}

static u32 OotPsp_NextAssetCacheClock(void) {
    sOotPspAssetCacheClock++;
    if (sOotPspAssetCacheClock == 0) {
        sOotPspAssetCacheClock = 1;
    }
    return sOotPspAssetCacheClock;
}

static OotPspAssetCache* OotPsp_FindPinnedAssetCache(const OotPspExternalAsset* asset) {
    size_t i;

    if (asset == NULL) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_PINNED_ASSET_CACHE_COUNT; i++) {
        OotPspAssetCache* cache = &sOotPspPinnedAssetCaches[i];

        if (strcmp(asset->path, cache->path) == 0) {
            cache->asset = asset;
            return cache;
        }
    }

    return NULL;
}

static OotPspAssetCache* OotPsp_FindLoadedHotAssetCache(const OotPspExternalAsset* asset) {
    size_t i;

    for (i = 0; i < OOT_PSP_HOT_ASSET_CACHE_COUNT; i++) {
        OotPspAssetCache* cache = &sOotPspHotAssetCaches[i];

        if ((cache->asset == asset) && (cache->data != NULL)) {
            return cache;
        }
    }

    return NULL;
}

static OotPspAssetCache* OotPsp_FindAssetCache(const OotPspExternalAsset* asset) {
    OotPspAssetCache* cache = OotPsp_FindPinnedAssetCache(asset);

    if (cache != NULL) {
        return cache;
    }

    return OotPsp_FindLoadedHotAssetCache(asset);
}

static s32 OotPsp_RecordHotAssetRead(const OotPspExternalAsset* asset) {
    OotPspHotReadTracker* best = NULL;
    size_t i;

    if (asset == NULL) {
        return false;
    }

    for (i = 0; i < OOT_PSP_HOT_READ_TRACKER_COUNT; i++) {
        OotPspHotReadTracker* tracker = &sOotPspHotReadTrackers[i];

        if (tracker->asset == asset) {
            if (tracker->hits < 0xFF) {
                tracker->hits++;
            }
            tracker->lastUsed = OotPsp_NextAssetCacheClock();
            return tracker->hits >= OOT_PSP_HOT_READ_PROMOTE_COUNT;
        }

        if (tracker->asset == NULL) {
            best = tracker;
        } else if ((best == NULL) || (tracker->lastUsed < best->lastUsed)) {
            best = tracker;
        }
    }

    if (best == NULL) {
        return false;
    }

    best->asset = asset;
    best->hits = 1;
    best->lastUsed = OotPsp_NextAssetCacheClock();
    return false;
}

static OotPspAssetCache* OotPsp_GetHotAssetCacheCandidate(const OotPspExternalAsset* asset) {
    OotPspAssetCache* best = NULL;
    size_t i;

    if (asset == NULL) {
        return NULL;
    }

    for (i = 0; i < OOT_PSP_HOT_ASSET_CACHE_COUNT; i++) {
        OotPspAssetCache* cache = &sOotPspHotAssetCaches[i];

        if (cache->asset == asset) {
            return cache;
        }

        if (cache->asset == NULL) {
            best = cache;
        } else if ((best == NULL) || (cache->lastUsed < best->lastUsed)) {
            best = cache;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    if ((best->asset != NULL) && (best->asset != asset)) {
        free(best->data);
        best->data = NULL;
        best->capacity = 0;
        best->offset = 0;
        best->dataSize = 0;
        best->failed = false;
    }

    best->path = asset->path;
    best->asset = asset;
    best->lastUsed = OotPsp_NextAssetCacheClock();
    return best;
}

static s32 OotPsp_ReadAssetFileRange(const OotPspExternalAsset* asset, size_t offset, u8* out, size_t size) {
    char pathBuffer[384];
    const char* path;
    SceUID fd;
    size_t remaining = size;
    size_t readOffset = offset;

    path = OotPsp_ResolveAssetPath(asset->path, pathBuffer, sizeof(pathBuffer));
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        printf("oot-psp asset open failed path=%s err=%d\n", path, (int)fd);
        return false;
    }

    if ((offset != 0) && (sceIoLseek32(fd, (int)offset, PSP_SEEK_SET) < 0)) {
        printf("oot-psp asset seek failed path=%s off=%lu\n", path, (unsigned long)offset);
        sceIoClose(fd);
        return false;
    }

    while (remaining != 0) {
        int chunk = remaining > OOT_PSP_ASSET_READ_CHUNK_SIZE ? OOT_PSP_ASSET_READ_CHUNK_SIZE : (int)remaining;
        int read = OotPsp_ReadAssetChunk(fd, out, chunk, path, readOffset);

        if (read <= 0) {
            printf("oot-psp asset read failed path=%s off=%lu size=%lu read=%d\n", path,
                   (unsigned long)readOffset, (unsigned long)remaining, read);
            sceIoClose(fd);
            return false;
        }

        out += read;
        readOffset += read;
        remaining -= read;
    }

    sceIoClose(fd);
    return true;
}

static s32 OotPsp_EnsureAssetCacheRange(OotPspAssetCache* cache, const OotPspExternalAsset* asset, size_t offset,
                                        size_t size) {
    u8* data;
    size_t fileSize;
    size_t cacheOffset;
    size_t windowStart;
    size_t windowSize;
    size_t capacity;

    if (cache == NULL) {
        return false;
    }

    cache->asset = asset;
    cache->lastUsed = OotPsp_NextAssetCacheClock();

    if (size > OOT_PSP_ASSET_CACHE_SIZE) {
        return false;
    }

    if ((cache->data != NULL) && (offset >= cache->offset)) {
        cacheOffset = offset - cache->offset;
        if ((cacheOffset <= cache->dataSize) && (size <= (cache->dataSize - cacheOffset))) {
            return true;
        }
    }

    if (cache->failed) {
        return false;
    }

    if (asset->vromEnd <= asset->vromStart) {
        cache->failed = true;
        return false;
    }

    fileSize = asset->vromEnd - asset->vromStart;
    if ((offset > fileSize) || (size > (fileSize - offset))) {
        return false;
    }

    if (cache->data == NULL) {
        capacity = fileSize < OOT_PSP_ASSET_CACHE_SIZE ? fileSize : OOT_PSP_ASSET_CACHE_SIZE;
        if (size > capacity) {
            return false;
        }

        data = malloc(capacity);
        if (data == NULL) {
            printf("oot-psp asset cache alloc failed path=%s size=%lu\n", asset->path,
                   (unsigned long)capacity);
            cache->failed = true;
            return false;
        }

        cache->data = data;
        cache->capacity = capacity;
    }

    if (fileSize <= cache->capacity) {
        windowStart = 0;
        windowSize = fileSize;
    } else {
        windowStart = (offset / cache->capacity) * cache->capacity;
        if (windowStart > (fileSize - cache->capacity)) {
            windowStart = fileSize - cache->capacity;
        }
        windowSize = cache->capacity;
    }

    if (!OotPsp_ReadAssetFileRange(asset, windowStart, cache->data, windowSize)) {
        free(cache->data);
        cache->data = NULL;
        cache->capacity = 0;
        cache->offset = 0;
        cache->dataSize = 0;
        cache->failed = true;
        return false;
    }

    cache->offset = windowStart;
    cache->dataSize = windowSize;
    printf("oot-psp cached %s off=%lu size=%lu cap=%lu\n", asset->path, (unsigned long)windowStart,
           (unsigned long)windowSize, (unsigned long)cache->capacity);
    return true;
}

static void OotPsp_PreloadPersistentAssets(void) {
    size_t i;

    for (i = 0; i < gOotPspExternalAssetCount; i++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[i];
        OotPspAssetCache* cache = OotPsp_FindPinnedAssetCache(asset);

        if (cache != NULL) {
            OotPsp_EnsureAssetCacheRange(cache, asset, 0, 1);
        }
    }
}

static const OotPspExternalAsset* OotPsp_FindContainingExternalAsset(uintptr_t vrom, size_t* index) {
    size_t i;

    for (i = 0; i < gOotPspExternalAssetCount; i++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[i];

        if ((vrom >= asset->vromStart) && (vrom < asset->vromEnd)) {
            if (index != NULL) {
                *index = i;
            }
            return asset;
        }
    }

    return NULL;
}

static s32 OotPsp_RangeContains(uintptr_t rangeStart, uintptr_t rangeEnd, uintptr_t vromStart, uintptr_t vromEnd) {
    if ((rangeEnd < rangeStart) || (vromEnd < vromStart)) {
        return false;
    }

    if (vromStart == vromEnd) {
        return (vromStart >= rangeStart) && (vromStart <= rangeEnd);
    }

    return (vromStart >= rangeStart) && (vromStart < rangeEnd) && (vromEnd <= rangeEnd);
}

static s32 OotPsp_RangesOverlap(uintptr_t firstStart, uintptr_t firstEnd, uintptr_t secondStart,
                                uintptr_t secondEnd) {
    if ((firstEnd <= firstStart) || (secondEnd <= secondStart)) {
        return false;
    }

    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

static s32 OotPsp_RamRangeFromPtr(const void* ptr, size_t size, uintptr_t* ramStart, uintptr_t* ramEnd) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t normalizedStart;

    if ((ptr == NULL) || (size == 0) || (start > (UINTPTR_MAX - size))) {
        return false;
    }

    normalizedStart = start & 0x0FFFFFFFU;
    if ((normalizedStart >= OOT_PSP_NATIVE_ADDR_START) && (normalizedStart < OOT_PSP_NATIVE_ADDR_END)) {
        start = normalizedStart;
    }

    if (start > (UINTPTR_MAX - size)) {
        return false;
    }

    *ramStart = start;
    *ramEnd = start + size;
    return true;
}

static void OotPsp_ClearLoadedAssetRange(void* ram, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (!OotPsp_RamRangeFromPtr(ram, size, &ramStart, &ramEnd)) {
        return;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if (OotPsp_RangesOverlap(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            OotPsp_ForgetNativeTextureRangeCache(range);
            range->ramStart = 0;
            range->ramEnd = 0;
            range->assetOffsetStart = 0;
            range->flags = 0;
            range->serial = 0;
        }
    }
}

static void OotPsp_StoreLoadedAssetRange(void* ram, size_t size, uintptr_t assetOffsetStart, u32 flags, u32 serial) {
    OotPspLoadedAssetRange* range;
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;
    size_t slot = OOT_PSP_LOADED_ASSET_RANGE_COUNT;

    if (!OotPsp_RamRangeFromPtr(ram, size, &ramStart, &ramEnd)) {
        return;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        if (sOotPspLoadedAssetRanges[i].ramStart == sOotPspLoadedAssetRanges[i].ramEnd) {
            slot = i;
            break;
        }
    }

    if (slot == OOT_PSP_LOADED_ASSET_RANGE_COUNT) {
        slot = sOotPspLoadedAssetRangeNext;
        sOotPspLoadedAssetRangeNext = (sOotPspLoadedAssetRangeNext + 1) % OOT_PSP_LOADED_ASSET_RANGE_COUNT;
    }

    range = &sOotPspLoadedAssetRanges[slot];
    OotPsp_ForgetNativeTextureRangeCache(range);
    range->ramStart = ramStart;
    range->ramEnd = ramEnd;
    range->assetOffsetStart = assetOffsetStart;
    range->flags = flags;
    range->serial = serial;
}

static void OotPsp_ForgetNativeTextureRangeCache(const OotPspLoadedAssetRange* range) {
    size_t i;

    for (i = 0; i < OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT; i++) {
        if (sOotPspNativeTextureRangeCache[i] == range) {
            sOotPspNativeTextureRangeCache[i] = NULL;
        }
    }
}

static s32 OotPsp_IsNativeLoadedAssetByteRange(const OotPspLoadedAssetRange* range, uintptr_t ram) {
    return (range != NULL) && (range->serial != 0) && ((range->flags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0) &&
           (ram >= range->ramStart) && (ram < range->ramEnd);
}

static void OotPsp_RememberNativeTextureRange(const OotPspLoadedAssetRange* range) {
    size_t i;

    if (range == NULL) {
        return;
    }

    for (i = 0; i < OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT; i++) {
        if (sOotPspNativeTextureRangeCache[i] == range) {
            return;
        }
    }

    sOotPspNativeTextureRangeCache[sOotPspNativeTextureRangeCacheNext] = range;
    sOotPspNativeTextureRangeCacheNext =
        (sOotPspNativeTextureRangeCacheNext + 1) % OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT;
}

static const OotPspLoadedAssetRange* OotPsp_FindCachedNativeTextureRange(uintptr_t ram) {
    size_t i;

    for (i = 0; i < OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = sOotPspNativeTextureRangeCache[i];

        if (OotPsp_IsNativeLoadedAssetByteRange(range, ram)) {
            return range;
        }
    }

    return NULL;
}

static s32 OotPsp_MapNativeExternalTextureByteInRange(const OotPspLoadedAssetRange* range, uintptr_t ram,
                                                      const void** mapped) {
    uintptr_t assetOffset;
    uintptr_t assetEnd;
    uintptr_t assetSpan;
    uintptr_t mappedAssetOffset;
    uintptr_t mappedRam;

    assetSpan = range->ramEnd - range->ramStart;
    if (range->assetOffsetStart > (UINTPTR_MAX - assetSpan)) {
        return false;
    }

    assetEnd = range->assetOffsetStart + assetSpan;
    assetOffset = range->assetOffsetStart + (ram - range->ramStart);
    mappedAssetOffset = assetOffset ^ 7U;

    if ((mappedAssetOffset < range->assetOffsetStart) || (mappedAssetOffset >= assetEnd)) {
        return false;
    }

    mappedRam = range->ramStart + (mappedAssetOffset - range->assetOffsetStart);
    *mapped = (const void*)mappedRam;
    return true;
}

static void OotPsp_RegisterLoadedAssetRanges(void* ram, size_t size, uintptr_t vromStart,
                                             const OotPspExternalAsset* asset, u32 serial) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    uintptr_t vromEnd;
    size_t i;

    if ((asset == NULL) || !OotPsp_RamRangeFromPtr(ram, size, &ramStart, &ramEnd)) {
        return;
    }

    OotPsp_ClearLoadedAssetRange(ram, size);
    OotPsp_StoreLoadedAssetRange((void*)ramStart, size, vromStart, asset->flags & ~OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS,
                                 serial);

    if ((asset->flags & (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) !=
        (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) {
        return;
    }

    if (vromStart > (UINTPTR_MAX - size)) {
        return;
    }

    vromEnd = vromStart + size;

    for (i = 0; i < gOotPspExternalAssetTextureRangeCount; i++) {
        const OotPspExternalAssetTextureRange* textureRange = &gOotPspExternalAssetTextureRanges[i];
        uintptr_t overlapStart;
        uintptr_t overlapEnd;

        if (textureRange->vromEnd <= vromStart) {
            continue;
        }

        if (textureRange->vromStart >= vromEnd) {
            break;
        }

        overlapStart = textureRange->vromStart > vromStart ? textureRange->vromStart : vromStart;
        overlapEnd = textureRange->vromEnd < vromEnd ? textureRange->vromEnd : vromEnd;

        if (overlapEnd <= overlapStart) {
            continue;
        }

        OotPsp_StoreLoadedAssetRange((void*)(ramStart + (overlapStart - vromStart)),
                                     (size_t)(overlapEnd - overlapStart), overlapStart, asset->flags, serial);
    }
}

static u32 OotPsp_NextLoadedAssetSerial(void) {
    u32 serial = sOotPspLoadedAssetSerial++;

    if (sOotPspLoadedAssetSerial == 0) {
        sOotPspLoadedAssetSerial = 1;
    }

    return serial;
}

static s32 OotPsp_TryReadAssetCache(const OotPspExternalAsset* asset, void* ram, uintptr_t vrom, size_t size) {
    OotPspAssetCache* cache;
    size_t offset;
    size_t cacheOffset;

    if ((asset == NULL) || (vrom < asset->vromStart)) {
        return false;
    }

    cache = OotPsp_FindAssetCache(asset);
    offset = vrom - asset->vromStart;
    if ((cache == NULL) && (size <= OOT_PSP_HOT_READ_MAX_SIZE) && OotPsp_RecordHotAssetRead(asset)) {
        cache = OotPsp_GetHotAssetCacheCandidate(asset);
    }

    if (!OotPsp_EnsureAssetCacheRange(cache, asset, offset, size)) {
        return false;
    }

    cacheOffset = offset - cache->offset;
    memcpy(ram, &cache->data[cacheOffset], size);
    OotPsp_RegisterLoadedAssetRanges(ram, size, vrom, asset, OotPsp_NextLoadedAssetSerial());
    return true;
}

static s32 OotPsp_TryTranslateAssetRange(const OotPspExternalAsset* asset, uintptr_t rangeStart, uintptr_t rangeEnd,
                                         uintptr_t vromStart, uintptr_t vromEnd, uintptr_t* normalizedStart,
                                         uintptr_t* normalizedEnd) {
    if (!OotPsp_RangeContains(rangeStart, rangeEnd, vromStart, vromEnd)) {
        return false;
    }

    *normalizedStart = asset->vromStart + (vromStart - rangeStart);
    *normalizedEnd = asset->vromStart + (vromEnd - rangeStart);
    return true;
}

static s32 OotPsp_TryNormalizeExternalRange(uintptr_t vromStart, uintptr_t vromEnd, uintptr_t* normalizedStart,
                                            uintptr_t* normalizedEnd) {
    size_t i;

    for (i = 0; i < gOotPspExternalAssetCount; i++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[i];

        if (OotPsp_RangeContains(asset->vromStart, asset->vromEnd, vromStart, vromEnd)) {
            *normalizedStart = vromStart;
            *normalizedEnd = vromEnd;
            return true;
        }

        if (OotPsp_TryTranslateAssetRange(asset, asset->originalVromStart, asset->originalVromEnd, vromStart, vromEnd,
                                          normalizedStart, normalizedEnd)) {
            return true;
        }
    }

    return false;
}

uintptr_t OotPsp_GetPrxRelocationBias(void) {
    if (!sOotPspPrxRelocationBiasInitialized) {
        sOotPspPrxRelocationBiasInitialized = true;

        if (gOotPspExternalAssetCount != 0) {
            uintptr_t relocatedStart = (uintptr_t)_AudiobankSegmentRomStart;
            uintptr_t expectedStart = gOotPspExternalAssets[0].vromStart;

            if (relocatedStart >= expectedStart) {
                uintptr_t bias = relocatedStart - expectedStart;

                if ((bias >= OOT_PSP_RELOCATION_BIAS_MIN) && (bias < OOT_PSP_RELOCATION_BIAS_MAX)) {
                    sOotPspPrxRelocationBias = bias;
                }
            }
        }

        if (sOotPspPrxRelocationBias != OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS) {
            printf("oot-psp asset relocation bias=%08lx\n", (unsigned long)sOotPspPrxRelocationBias);
        }
    }

    return sOotPspPrxRelocationBias;
}

static s32 OotPsp_TryNormalizePrxRelocatedAddressWithBias(uintptr_t addr, uintptr_t bias, uintptr_t* normalized) {
    if ((bias == 0) || (addr < bias)) {
        return false;
    }

    *normalized = addr - bias;
    return true;
}

s32 OotPsp_TryNormalizePrxRelocatedAddress(uintptr_t addr, uintptr_t* normalized) {
    uintptr_t detectedBias;

    if (normalized == NULL) {
        return false;
    }

    detectedBias = OotPsp_GetPrxRelocationBias();
    if (OotPsp_TryNormalizePrxRelocatedAddressWithBias(addr, detectedBias, normalized)) {
        return true;
    }

    if ((detectedBias != OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS) &&
        OotPsp_TryNormalizePrxRelocatedAddressWithBias(addr, OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS, normalized)) {
        return true;
    }

    return false;
}

static s32 OotPsp_TryNormalizeBiasedRange(uintptr_t vromStart, uintptr_t vromEnd, uintptr_t bias,
                                          uintptr_t* normalizedStart, uintptr_t* normalizedEnd) {
    if ((bias == 0) || (vromStart < bias) || (vromEnd < bias)) {
        return false;
    }

    return OotPsp_TryNormalizeExternalRange(vromStart - bias, vromEnd - bias, normalizedStart, normalizedEnd);
}

s32 OotPsp_NormalizeVromRange(uintptr_t vromStart, uintptr_t vromEnd, uintptr_t* normalizedStart,
                              uintptr_t* normalizedEnd) {
    uintptr_t detectedBias;

    if ((normalizedStart == NULL) || (normalizedEnd == NULL) || (vromEnd < vromStart)) {
        return false;
    }

    if (OotPsp_TryNormalizeExternalRange(vromStart, vromEnd, normalizedStart, normalizedEnd)) {
        return true;
    }

    detectedBias = OotPsp_GetPrxRelocationBias();
    if (OotPsp_TryNormalizeBiasedRange(vromStart, vromEnd, detectedBias, normalizedStart, normalizedEnd)) {
        return true;
    }
    if ((detectedBias != OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS) &&
        OotPsp_TryNormalizeBiasedRange(vromStart, vromEnd, OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS, normalizedStart,
                                       normalizedEnd)) {
        return true;
    }

    return false;
}

uintptr_t OotPsp_NormalizeVrom(uintptr_t vrom) {
    uintptr_t normalizedStart;
    uintptr_t normalizedEnd;

    if (OotPsp_NormalizeVromRange(vrom, vrom, &normalizedStart, &normalizedEnd)) {
        return normalizedStart;
    }

    return vrom;
}

void OotPsp_NormalizeRomFile(RomFile* file) {
    uintptr_t normalizedStart;
    uintptr_t normalizedEnd;

    if ((file == NULL) || (file->vromStart == 0)) {
        return;
    }

    if (OotPsp_NormalizeVromRange(file->vromStart, file->vromEnd, &normalizedStart, &normalizedEnd)) {
        file->vromStart = normalizedStart;
        file->vromEnd = normalizedEnd;
    }
}

const OotPspMessageEntry* OotPsp_FindMessageEntry(const OotPspMessageEntry* entries, size_t count, u16 textId) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (entries[i].textId == textId) {
            return &entries[i];
        }
    }

    return NULL;
}

s32 OotPsp_GetLoadedExternalAssetRangeFlags(const void* ptr, size_t size, u32* flags) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (flags != NULL) {
        *flags = 0;
    }

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return false;
    }

    {
        const OotPspLoadedAssetRange* cachedRange = OotPsp_FindCachedNativeTextureRange(ramStart);

        if (cachedRange != NULL) {
            if (flags != NULL) {
                *flags = cachedRange->flags;
            }
            return true;
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            if (flags != NULL) {
                *flags = range->flags;
            }
            if ((range->flags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0) {
                OotPsp_RememberNativeTextureRange(range);
            }
            return true;
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && (ramStart >= range->ramStart) && (ramStart < range->ramEnd)) {
            if (flags != NULL) {
                *flags = range->flags;
            }
            if ((range->flags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0) {
                OotPsp_RememberNativeTextureRange(range);
            }
            return true;
        }
    }

    return false;
}

s32 OotPsp_IsLoadedExternalAssetRange(const void* ptr, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return false;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            return true;
        }
    }

    return false;
}

s32 OotPsp_IsLoadedNativeExternalAssetRange(const void* ptr, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return false;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && ((range->flags & OOT_PSP_EXTERNAL_ASSET_NATIVE) != 0) &&
            OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            return true;
        }
    }

    return false;
}

s32 OotPsp_IsNativeExternalTextureRange(const void* ptr, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return false;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if (((range->flags & (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) ==
             (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) &&
            OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            OotPsp_RememberNativeTextureRange(range);
            return true;
        }
    }

    return false;
}

s32 OotPsp_IsNativeExternalTextureByte(const void* ptr) {
    const void* mapped;

    return OotPsp_MapNativeExternalTextureByte(ptr, &mapped);
}

u32 OotPsp_GetExternalAssetRangeSerial(const void* ptr, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return 0;
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            return range->serial;
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if ((range->serial != 0) && (ramStart >= range->ramStart) && (ramStart < range->ramEnd)) {
            return range->serial;
        }
    }

    return 0;
}

s32 OotPsp_MapNativeExternalTextureByte(const void* ptr, const void** mapped) {
    uintptr_t ram = (uintptr_t)ptr;
    uintptr_t normalizedRam;
    size_t i;

    if ((ptr == NULL) || (mapped == NULL)) {
        return false;
    }

    normalizedRam = ram & 0x0FFFFFFFU;
    if ((normalizedRam >= OOT_PSP_NATIVE_ADDR_START) && (normalizedRam < OOT_PSP_NATIVE_ADDR_END)) {
        ram = normalizedRam;
    }

    {
        const OotPspLoadedAssetRange* cachedRange = OotPsp_FindCachedNativeTextureRange(ram);

        if (cachedRange != NULL) {
            return OotPsp_MapNativeExternalTextureByteInRange(cachedRange, ram, mapped);
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        /*
         * Texture subranges are best-effort. Native object bins still store
         * compiled u64 texture words, so fall back to the full native asset
         * range when a specific texture subrange was not generated.
         */
        if (!OotPsp_IsNativeLoadedAssetByteRange(range, ram)) {
            continue;
        }

        OotPsp_RememberNativeTextureRange(range);
        return OotPsp_MapNativeExternalTextureByteInRange(range, ram, mapped);
    }

    return false;
}

s32 OotPsp_AssetRead(void* ram, uintptr_t vrom, size_t size) {
    uintptr_t normalizedVrom;
    uintptr_t normalizedEnd;
    uintptr_t cursor;
    size_t assetIndex;
    char pathBuffer[384];
    const char* path;
    SceUID fd;
    u8* out = ram;
    size_t remaining = size;

    if (size == 0) {
        return OOT_PSP_ASSET_READ_OK;
    }

    OotPsp_ClearLoadedAssetRange(ram, size);

    if ((vrom > (UINTPTR_MAX - size)) ||
        !OotPsp_NormalizeVromRange(vrom, vrom + size, &normalizedVrom, &normalizedEnd)) {
        return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
    }

    cursor = normalizedVrom;
    (void)normalizedEnd;

    if (OotPsp_FindContainingExternalAsset(normalizedVrom, &assetIndex) == NULL) {
        return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
    }

    if (OotPsp_TryReadAssetCache(&gOotPspExternalAssets[assetIndex], ram, normalizedVrom, size)) {
        return OOT_PSP_ASSET_READ_OK;
    }

    while (remaining != 0) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[assetIndex];
        uintptr_t offset;
        uintptr_t readOffset;
        uintptr_t chunkVromStart;
        size_t chunkRemaining;
        size_t chunkSize;
        u8* chunkOut;
        size_t chunkRequestedSize;
        u32 chunkSerial;

        if ((cursor < asset->vromStart) || (cursor >= asset->vromEnd)) {
            return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
        }

        offset = cursor - asset->vromStart;
        readOffset = offset;
        chunkVromStart = cursor;
        chunkRemaining = asset->vromEnd - cursor;
        chunkSize = remaining < chunkRemaining ? remaining : chunkRemaining;
        chunkOut = out;
        chunkRequestedSize = chunkSize;
        chunkSerial = OotPsp_NextLoadedAssetSerial();

        path = OotPsp_ResolveAssetPath(asset->path, pathBuffer, sizeof(pathBuffer));
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) {
            printf("oot-psp asset open failed path=%s err=%d\n", path, (int)fd);
            return OOT_PSP_ASSET_READ_FAILED;
        }

        if (sceIoLseek32(fd, (int)offset, PSP_SEEK_SET) < 0) {
            printf("oot-psp asset seek failed path=%s off=%lu\n", path, (unsigned long)offset);
            sceIoClose(fd);
            return OOT_PSP_ASSET_READ_FAILED;
        }

        while (chunkSize != 0) {
            int chunk = chunkSize > OOT_PSP_ASSET_READ_CHUNK_SIZE ? OOT_PSP_ASSET_READ_CHUNK_SIZE : (int)chunkSize;
            int read = OotPsp_ReadAssetChunk(fd, out, chunk, path, readOffset);

            if (read <= 0) {
                printf("oot-psp asset read failed path=%s off=%lu size=%lu read=%d\n", path,
                       (unsigned long)readOffset, (unsigned long)chunkSize, read);
                sceIoClose(fd);
                return OOT_PSP_ASSET_READ_FAILED;
            }

            out += read;
            cursor += read;
            readOffset += read;
            remaining -= read;
            chunkSize -= read;
        }

        sceIoClose(fd);
        OotPsp_RegisterLoadedAssetRanges(chunkOut, chunkRequestedSize, chunkVromStart, asset, chunkSerial);

        if (remaining != 0) {
            assetIndex++;
            if ((assetIndex >= gOotPspExternalAssetCount) ||
                (gOotPspExternalAssets[assetIndex].vromStart != cursor)) {
                printf("oot-psp asset span gap vrom=%08lx remaining=%lu\n", (unsigned long)cursor,
                       (unsigned long)remaining);
                return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
            }
        }
    }

    return OOT_PSP_ASSET_READ_OK;
}
