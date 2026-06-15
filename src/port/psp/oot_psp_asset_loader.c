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
#define OOT_PSP_ASSET_READ_CHUNK_SIZE       0x10000
#define OOT_PSP_ASSET_CACHE_SIZE            (2 * 1024 * 1024)
#define OOT_PSP_AUDIOTABLE_CACHE_SIZE       0x10000
#define OOT_PSP_HOT_ASSET_CACHE_COUNT       4
#define OOT_PSP_HOT_READ_TRACKER_COUNT      16
#define OOT_PSP_HOT_READ_PROMOTE_COUNT      4
#define OOT_PSP_HOT_READ_MAX_SIZE           OOT_PSP_ASSET_CACHE_SIZE
#define OOT_PSP_PACKED_CACHE_BLOCK_SIZE     0x10000
#define OOT_PSP_PACKED_CACHE_TARGET_SIZE    (8 * 1024 * 1024)
#define OOT_PSP_PACKED_CACHE_MIN_SIZE       (2 * 1024 * 1024)
#define OOT_PSP_PACKED_CACHE_ALLOC_STEP     (1024 * 1024)
#define OOT_PSP_PACKED_CACHE_MAX_BLOCK_COUNT \
    (OOT_PSP_PACKED_CACHE_TARGET_SIZE / OOT_PSP_PACKED_CACHE_BLOCK_SIZE)
#define OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT 8
#define OOT_PSP_ASSET_RANGE_SERIAL_CACHE_SET_COUNT 128
#define OOT_PSP_ASSET_RANGE_SERIAL_CACHE_WAYS      4
#define OOT_PSP_PACKED_ASSET_PATH           "data/segments/oot_psp_assets.bin"
#define OOT_PSP_AUDIOBANK_ASSET_PATH        "data/segments/Audiobank.bin"
#define OOT_PSP_AUDIOSEQ_ASSET_PATH         "data/segments/Audioseq.bin"
#define OOT_PSP_KANJI_ASSET_PATH            "data/segments/kanji.bin"
#define OOT_PSP_LINK_ANIMATION_ASSET_PATH   "data/segments/link_animetion.bin"
#define OOT_PSP_AUDIOTABLE_ASSET_PATH       "data/segments/Audiotable.bin"
#define OOT_PSP_ASSET_READ_ZERO_RETRY_COUNT 16
#define OOT_PSP_ASSET_READ_ZERO_RETRY_USEC  1000
#define OOT_PSP_AUDIO_READ_BACKOFF_USEC     1000
#define OOT_PSP_AUDIO_READ_BACKOFF_MAX_USEC 2000

typedef struct OotPspLoadedAssetRange {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    uintptr_t assetOffsetStart;
    u32 flags;
    u32 serial;
} OotPspLoadedAssetRange;

typedef struct OotPspLoadedAssetSerialRange {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    u32 serial;
} OotPspLoadedAssetSerialRange;

typedef struct OotPspAssetRangeSerialCacheEntry {
    uintptr_t ramStart;
    u32 serial;
    u32 generation;
} OotPspAssetRangeSerialCacheEntry;

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

typedef struct OotPspPackedAssetCacheBlock {
    size_t blockIndex;
    size_t dataSize;
    u32 lastUsed;
    s32 valid;
} OotPspPackedAssetCacheBlock;

static char sOotPspAssetRoot[256];
static uintptr_t sOotPspPrxRelocationBias = OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS;
static s32 sOotPspPrxRelocationBiasInitialized = false;
static s32 sOotPspOriginalRangesSorted = -1;
static SceUID sOotPspAssetSema = -1;
static SceUID sOotPspPackedAssetFd = -1;
static s32 sOotPspPackedAssetUnavailable = false;
static size_t sOotPspPackedAssetSize;
static s32 sOotPspPackedAssetSizeKnown = false;
static size_t sOotPspPackedAssetPosition;
static s32 sOotPspPackedAssetPositionKnown = false;
static u8* sOotPspPackedCacheData;
static size_t sOotPspPackedCacheBlockCount;
static s32 sOotPspPackedCacheInitTried = false;
static OotPspPackedAssetCacheBlock sOotPspPackedCacheBlocks[OOT_PSP_PACKED_CACHE_MAX_BLOCK_COUNT];
static OotPspLoadedAssetRange sOotPspLoadedAssetRanges[OOT_PSP_LOADED_ASSET_RANGE_COUNT];
static OotPspLoadedAssetSerialRange sOotPspLoadedAssetSerialRanges[OOT_PSP_LOADED_ASSET_RANGE_COUNT];
static const OotPspLoadedAssetRange* sOotPspNativeTextureRangeCache[OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT];
static const OotPspLoadedAssetRange* sOotPspLastNativeTextureRange;
static OotPspAssetRangeSerialCacheEntry sOotPspAssetRangeSerialCache[OOT_PSP_ASSET_RANGE_SERIAL_CACHE_SET_COUNT]
                                                                [OOT_PSP_ASSET_RANGE_SERIAL_CACHE_WAYS];
static u8 sOotPspAssetRangeSerialCacheNext[OOT_PSP_ASSET_RANGE_SERIAL_CACHE_SET_COUNT];
static u32 sOotPspAssetRangeSerialCacheGeneration = 1;
static size_t sOotPspLoadedAssetSerialRangeCount;
static s32 sOotPspLoadedAssetSerialRangeIndexComplete = true;
static size_t sOotPspNativeTextureRangeCacheNext;
static size_t sOotPspLoadedAssetRangeNext;
static u32 sOotPspLoadedAssetSerial = 1;
static u32 sOotPspAssetCacheClock;
static volatile s32 sOotPspForegroundAssetReadWaiters;
static OotPspAssetCache sOotPspPinnedAssetCaches[] = {
    { OOT_PSP_KANJI_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
    { OOT_PSP_LINK_ANIMATION_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
    { OOT_PSP_AUDIOTABLE_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
    { OOT_PSP_AUDIOBANK_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
    { OOT_PSP_AUDIOSEQ_ASSET_PATH, NULL, NULL, 0, 0, 0, 0, false },
};
static OotPspAssetCache sOotPspHotAssetCaches[OOT_PSP_HOT_ASSET_CACHE_COUNT];
static OotPspHotReadTracker sOotPspHotReadTrackers[OOT_PSP_HOT_READ_TRACKER_COUNT];

#define OOT_PSP_PINNED_ASSET_CACHE_COUNT (sizeof(sOotPspPinnedAssetCaches) / sizeof(sOotPspPinnedAssetCaches[0]))

static void OotPsp_ClearPackedAssetCache(void);
static void OotPsp_ClosePackedAssetFile(void);
static void OotPsp_PreloadPersistentAssets(void);
static void OotPsp_ForgetNativeTextureRangeCache(const OotPspLoadedAssetRange* range);

static void OotPsp_InitAssetSema(void) {
    if (sOotPspAssetSema >= 0) {
        return;
    }

    sOotPspAssetSema = sceKernelCreateSema("OOT PSP Asset", 0, 1, 1, NULL);
    if (sOotPspAssetSema < 0) {
        printf("oot-psp asset sema create failed err=%d\n", (int)sOotPspAssetSema);
    }
}

static void OotPsp_LockAssetLoader(void) {
    if (sOotPspAssetSema >= 0) {
        sceKernelWaitSema(sOotPspAssetSema, 1, NULL);
    }
}

static void OotPsp_UnlockAssetLoader(void) {
    if (sOotPspAssetSema >= 0) {
        sceKernelSignalSema(sOotPspAssetSema, 1);
    }
}

static size_t OotPsp_AssetRangeSerialCacheIndex(uintptr_t ramStart) {
    uintptr_t key = (ramStart >> 4) ^ (ramStart >> 12) ^ (ramStart >> 20);

    return (size_t)(key & (OOT_PSP_ASSET_RANGE_SERIAL_CACHE_SET_COUNT - 1));
}

static void OotPsp_ClearAssetRangeSerialCache(void) {
    sOotPspAssetRangeSerialCacheGeneration++;
    if (sOotPspAssetRangeSerialCacheGeneration == 0) {
        memset(sOotPspAssetRangeSerialCache, 0, sizeof(sOotPspAssetRangeSerialCache));
        sOotPspAssetRangeSerialCacheGeneration = 1;
    }
}

static s32 OotPsp_GetCachedAssetRangeSerial(uintptr_t ramStart, u32* serial) {
    const OotPspAssetRangeSerialCacheEntry* set =
        sOotPspAssetRangeSerialCache[OotPsp_AssetRangeSerialCacheIndex(ramStart)];
    size_t way;

    for (way = 0; way < OOT_PSP_ASSET_RANGE_SERIAL_CACHE_WAYS; way++) {
        const OotPspAssetRangeSerialCacheEntry* entry = &set[way];

        if ((entry->generation == sOotPspAssetRangeSerialCacheGeneration) && (entry->ramStart == ramStart)) {
            *serial = entry->serial;
            return true;
        }
    }

    return false;
}

static void OotPsp_RememberAssetRangeSerial(uintptr_t ramStart, u32 serial) {
    size_t setIndex = OotPsp_AssetRangeSerialCacheIndex(ramStart);
    size_t way = sOotPspAssetRangeSerialCacheNext[setIndex];
    OotPspAssetRangeSerialCacheEntry* entry = &sOotPspAssetRangeSerialCache[setIndex][way];

    entry->ramStart = ramStart;
    entry->serial = serial;
    entry->generation = sOotPspAssetRangeSerialCacheGeneration;
    sOotPspAssetRangeSerialCacheNext[setIndex] =
        (way + 1) & (OOT_PSP_ASSET_RANGE_SERIAL_CACHE_WAYS - 1);
}

void OotPsp_AssetInit(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t length;

    OotPsp_ClosePackedAssetFile();
    OotPsp_ClearPackedAssetCache();
    sOotPspPackedAssetUnavailable = false;
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
    OotPsp_InitAssetSema();
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

const char* OotPsp_ResolveRootPath(const char* path, char* buffer, size_t bufferSize) {
    int written;

    if (OotPsp_IsAbsolutePath(path) || (sOotPspAssetRoot[0] == '\0')) {
        return path;
    }

    written = snprintf(buffer, bufferSize, "%s%s", sOotPspAssetRoot, path);
    if ((written < 0) || ((size_t)written >= bufferSize)) {
        printf("oot-psp root path too long root=%s path=%s\n", sOotPspAssetRoot, path);
        return path;
    }

    return buffer;
}

static const char* OotPsp_ResolveAssetPath(const char* path, char* buffer, size_t bufferSize) {
    return OotPsp_ResolveRootPath(path, buffer, bufferSize);
}

static void OotPsp_ClearPackedAssetCache(void) {
    memset(sOotPspPackedCacheBlocks, 0, sizeof(sOotPspPackedCacheBlocks));
}

static void OotPsp_InitPackedAssetCache(void) {
    size_t cacheSize;

    if (sOotPspPackedCacheInitTried) {
        return;
    }

    sOotPspPackedCacheInitTried = true;
    for (cacheSize = OOT_PSP_PACKED_CACHE_TARGET_SIZE; cacheSize >= OOT_PSP_PACKED_CACHE_MIN_SIZE;) {
        sOotPspPackedCacheData = malloc(cacheSize);
        if (sOotPspPackedCacheData != NULL) {
            sOotPspPackedCacheBlockCount = cacheSize / OOT_PSP_PACKED_CACHE_BLOCK_SIZE;
            OotPsp_ClearPackedAssetCache();
            printf("oot-psp packed asset cache size=%lu blocks=%lu\n", (unsigned long)cacheSize,
                   (unsigned long)sOotPspPackedCacheBlockCount);
            return;
        }

        if (cacheSize == OOT_PSP_PACKED_CACHE_MIN_SIZE) {
            break;
        }

        if (cacheSize > (OOT_PSP_PACKED_CACHE_MIN_SIZE + OOT_PSP_PACKED_CACHE_ALLOC_STEP)) {
            cacheSize -= OOT_PSP_PACKED_CACHE_ALLOC_STEP;
        } else {
            cacheSize = OOT_PSP_PACKED_CACHE_MIN_SIZE;
        }
    }

    printf("oot-psp packed asset cache disabled alloc failed\n");
}

static void OotPsp_ClosePackedAssetFile(void) {
    if (sOotPspPackedAssetFd >= 0) {
        sceIoClose(sOotPspPackedAssetFd);
        sOotPspPackedAssetFd = -1;
    }

    sOotPspPackedAssetSize = 0;
    sOotPspPackedAssetSizeKnown = false;
    sOotPspPackedAssetPosition = 0;
    sOotPspPackedAssetPositionKnown = false;
}

static SceUID OotPsp_OpenPackedAssetFile(const char** resolvedPath, char* pathBuffer, size_t pathBufferSize) {
    *resolvedPath = OOT_PSP_PACKED_ASSET_PATH;

    if (sOotPspPackedAssetFd >= 0) {
        return sOotPspPackedAssetFd;
    }

    if (sOotPspPackedAssetUnavailable) {
        return -1;
    }

    *resolvedPath = OotPsp_ResolveAssetPath(OOT_PSP_PACKED_ASSET_PATH, pathBuffer, pathBufferSize);
    sOotPspPackedAssetFd = sceIoOpen(*resolvedPath, PSP_O_RDONLY, 0);
    if (sOotPspPackedAssetFd < 0) {
        printf("oot-psp packed asset open failed path=%s err=%d; falling back to loose assets\n", *resolvedPath,
               (int)sOotPspPackedAssetFd);
        sOotPspPackedAssetUnavailable = true;
        sOotPspPackedAssetFd = -1;
    } else if (!sOotPspPackedAssetSizeKnown) {
        SceOff end = sceIoLseek32(sOotPspPackedAssetFd, 0, PSP_SEEK_END);

        if (end >= 0) {
            sOotPspPackedAssetSize = (size_t)end;
            sOotPspPackedAssetSizeKnown = true;
            sceIoLseek32(sOotPspPackedAssetFd, 0, PSP_SEEK_SET);
            sOotPspPackedAssetPosition = 0;
            sOotPspPackedAssetPositionKnown = true;
        } else {
            printf("oot-psp packed asset size query failed path=%s err=%d\n", *resolvedPath, (int)end);
            sOotPspPackedAssetPositionKnown = false;
        }
    }

    return sOotPspPackedAssetFd;
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

static s32 OotPsp_IsAudioAsset(const OotPspExternalAsset* asset) {
    return (asset != NULL) &&
           ((strcmp(asset->path, OOT_PSP_AUDIOBANK_ASSET_PATH) == 0) ||
            (strcmp(asset->path, OOT_PSP_AUDIOSEQ_ASSET_PATH) == 0) ||
            (strcmp(asset->path, OOT_PSP_AUDIOTABLE_ASSET_PATH) == 0));
}

static s32 OotPsp_RecordHotAssetRead(const OotPspExternalAsset* asset) {
    OotPspHotReadTracker* best = NULL;
    size_t i;

    if ((asset == NULL) || OotPsp_IsAudioAsset(asset)) {
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

static s32 OotPsp_ReadOpenFileRangeTracked(SceUID fd, const char* path, size_t offset, u8* out, size_t size,
                                           size_t* currentOffset, s32* currentOffsetKnown) {
    size_t remaining = size;
    size_t readOffset = offset;

    if (offset > 0x7FFFFFFFUL) {
        printf("oot-psp asset seek offset too large path=%s off=%lu\n", path, (unsigned long)offset);
        return false;
    }

    if ((currentOffset == NULL) || (currentOffsetKnown == NULL) || !*currentOffsetKnown ||
        (*currentOffset != offset)) {
        if (((offset != 0) || (currentOffset != NULL)) && (sceIoLseek32(fd, (int)offset, PSP_SEEK_SET) < 0)) {
            if (currentOffsetKnown != NULL) {
                *currentOffsetKnown = false;
            }
            printf("oot-psp asset seek failed path=%s off=%lu\n", path, (unsigned long)offset);
            return false;
        }
        if ((currentOffset != NULL) && (currentOffsetKnown != NULL)) {
            *currentOffset = offset;
            *currentOffsetKnown = true;
        }
    }

    while (remaining != 0) {
        int chunk = remaining > OOT_PSP_ASSET_READ_CHUNK_SIZE ? OOT_PSP_ASSET_READ_CHUNK_SIZE : (int)remaining;
        int read = OotPsp_ReadAssetChunk(fd, out, chunk, path, readOffset);

        if (read <= 0) {
            if (currentOffsetKnown != NULL) {
                *currentOffsetKnown = false;
            }
            printf("oot-psp asset read failed path=%s off=%lu size=%lu read=%d\n", path,
                   (unsigned long)readOffset, (unsigned long)remaining, read);
            return false;
        }

        out += read;
        readOffset += read;
        remaining -= read;
        if ((currentOffset != NULL) && (currentOffsetKnown != NULL)) {
            *currentOffset += read;
            *currentOffsetKnown = true;
        }
    }

    return true;
}

static s32 OotPsp_ReadOpenFileRange(SceUID fd, const char* path, size_t offset, u8* out, size_t size) {
    return OotPsp_ReadOpenFileRangeTracked(fd, path, offset, out, size, NULL, NULL);
}

static s32 OotPsp_ReadPackedOpenFileRange(SceUID fd, const char* path, size_t offset, u8* out, size_t size) {
    return OotPsp_ReadOpenFileRangeTracked(fd, path, offset, out, size, &sOotPspPackedAssetPosition,
                                           &sOotPspPackedAssetPositionKnown);
}

static u8* OotPsp_PackedCacheBlockData(size_t slot) {
    return &sOotPspPackedCacheData[slot * OOT_PSP_PACKED_CACHE_BLOCK_SIZE];
}

static OotPspPackedAssetCacheBlock* OotPsp_FindPackedCacheBlock(size_t blockIndex) {
    size_t i;

    for (i = 0; i < sOotPspPackedCacheBlockCount; i++) {
        OotPspPackedAssetCacheBlock* block = &sOotPspPackedCacheBlocks[i];

        if (block->valid && (block->blockIndex == blockIndex)) {
            block->lastUsed = OotPsp_NextAssetCacheClock();
            return block;
        }
    }

    return NULL;
}

static OotPspPackedAssetCacheBlock* OotPsp_SelectPackedCacheBlock(void) {
    OotPspPackedAssetCacheBlock* best = NULL;
    size_t i;

    for (i = 0; i < sOotPspPackedCacheBlockCount; i++) {
        OotPspPackedAssetCacheBlock* block = &sOotPspPackedCacheBlocks[i];

        if (!block->valid) {
            return block;
        }

        if ((best == NULL) || (block->lastUsed < best->lastUsed)) {
            best = block;
        }
    }

    return best;
}

static OotPspPackedAssetCacheBlock* OotPsp_LoadPackedCacheBlock(SceUID fd, const char* path, size_t blockIndex) {
    OotPspPackedAssetCacheBlock* block;
    size_t blockStart = blockIndex * OOT_PSP_PACKED_CACHE_BLOCK_SIZE;
    size_t readSize = OOT_PSP_PACKED_CACHE_BLOCK_SIZE;
    size_t slot;

    if (sOotPspPackedCacheData == NULL) {
        OotPsp_InitPackedAssetCache();
        if (sOotPspPackedCacheData == NULL) {
            return NULL;
        }
    }

    block = OotPsp_FindPackedCacheBlock(blockIndex);
    if (block != NULL) {
        return block;
    }

    if (sOotPspPackedAssetSizeKnown) {
        if (blockStart >= sOotPspPackedAssetSize) {
            return NULL;
        }
        if (readSize > (sOotPspPackedAssetSize - blockStart)) {
            readSize = sOotPspPackedAssetSize - blockStart;
        }
    }

    block = OotPsp_SelectPackedCacheBlock();
    if (block == NULL) {
        return NULL;
    }

    slot = (size_t)(block - sOotPspPackedCacheBlocks);
    if (!OotPsp_ReadPackedOpenFileRange(fd, path, blockStart, OotPsp_PackedCacheBlockData(slot), readSize)) {
        block->valid = false;
        return NULL;
    }

    block->blockIndex = blockIndex;
    block->dataSize = readSize;
    block->lastUsed = OotPsp_NextAssetCacheClock();
    block->valid = true;
    return block;
}

static s32 OotPsp_ReadPackedAssetFileRangeCached(SceUID fd, const char* path, size_t offset, u8* out, size_t size) {
    size_t remaining = size;
    size_t cursor = offset;

    while (remaining != 0) {
        size_t blockIndex = cursor / OOT_PSP_PACKED_CACHE_BLOCK_SIZE;
        size_t blockOffset = cursor & (OOT_PSP_PACKED_CACHE_BLOCK_SIZE - 1);
        OotPspPackedAssetCacheBlock* block = OotPsp_LoadPackedCacheBlock(fd, path, blockIndex);
        size_t slot;
        size_t available;
        size_t copySize;

        if ((block == NULL) || (blockOffset >= block->dataSize)) {
            return false;
        }

        slot = (size_t)(block - sOotPspPackedCacheBlocks);
        available = block->dataSize - blockOffset;
        copySize = remaining < available ? remaining : available;
        memcpy(out, &OotPsp_PackedCacheBlockData(slot)[blockOffset], copySize);

        out += copySize;
        cursor += copySize;
        remaining -= copySize;
    }

    return true;
}

static s32 OotPsp_ReadPackedAssetFileRange(const OotPspExternalAsset* asset, size_t offset, u8* out, size_t size,
                                           s32 useBlockCache) {
    char pathBuffer[384];
    const char* path;
    SceUID fd;
    size_t packedOffset;

    if ((asset == NULL) || (asset->fileOffset > (UINTPTR_MAX - offset))) {
        return false;
    }

    fd = OotPsp_OpenPackedAssetFile(&path, pathBuffer, sizeof(pathBuffer));
    if (fd < 0) {
        return false;
    }

    packedOffset = asset->fileOffset + offset;
    if (useBlockCache && OotPsp_ReadPackedAssetFileRangeCached(fd, path, packedOffset, out, size)) {
        return true;
    }

    return OotPsp_ReadPackedOpenFileRange(fd, path, packedOffset, out, size);
}

static s32 OotPsp_ReadLooseAssetFileRange(const OotPspExternalAsset* asset, size_t offset, u8* out, size_t size) {
    char pathBuffer[384];
    const char* path;
    SceUID fd;
    s32 ok;

    path = OotPsp_ResolveAssetPath(asset->path, pathBuffer, sizeof(pathBuffer));
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        printf("oot-psp asset open failed path=%s err=%d\n", path, (int)fd);
        return false;
    }

    ok = OotPsp_ReadOpenFileRange(fd, path, offset, out, size);
    sceIoClose(fd);
    return ok;
}

static s32 OotPsp_ReadAssetFileRange(const OotPspExternalAsset* asset, size_t offset, u8* out, size_t size,
                                     s32 useBlockCache) {
    if (OotPsp_ReadPackedAssetFileRange(asset, offset, out, size, useBlockCache)) {
        return true;
    }

    return OotPsp_ReadLooseAssetFileRange(asset, offset, out, size);
}

static size_t OotPsp_GetAssetCacheSizeLimit(const OotPspExternalAsset* asset) {
    size_t i;

    if (asset == NULL) {
        return OOT_PSP_ASSET_CACHE_SIZE;
    }

    if (strcmp(asset->path, OOT_PSP_AUDIOTABLE_ASSET_PATH) == 0) {
        return OOT_PSP_AUDIOTABLE_CACHE_SIZE;
    }

    for (i = 0; i < OOT_PSP_PINNED_ASSET_CACHE_COUNT; i++) {
        if (strcmp(asset->path, sOotPspPinnedAssetCaches[i].path) == 0) {
            return asset->vromEnd - asset->vromStart;
        }
    }

    return OOT_PSP_ASSET_CACHE_SIZE;
}

static s32 OotPsp_EnsureAssetCacheRange(OotPspAssetCache* cache, const OotPspExternalAsset* asset, size_t offset,
                                        size_t size) {
    u8* data;
    size_t fileSize;
    size_t cacheOffset;
    size_t windowStart;
    size_t windowSize;
    size_t capacity;
    size_t cacheSizeLimit;

    if (cache == NULL) {
        return false;
    }

    cache->asset = asset;
    cache->lastUsed = OotPsp_NextAssetCacheClock();

    cacheSizeLimit = OotPsp_GetAssetCacheSizeLimit(asset);
    if (size > cacheSizeLimit) {
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
        capacity = fileSize < cacheSizeLimit ? fileSize : cacheSizeLimit;
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
        cacheOffset = offset - windowStart;
        if (size > (cache->capacity - cacheOffset)) {
            windowStart = (offset + size) - cache->capacity;
        }
        if (windowStart > (fileSize - cache->capacity)) {
            windowStart = fileSize - cache->capacity;
        }
        windowSize = cache->capacity;
    }

    if (!OotPsp_ReadAssetFileRange(asset, windowStart, cache->data, windowSize, false)) {
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
    return true;
}

static void OotPsp_PreloadPersistentAssets(void) {
    size_t cacheIndex;

    for (cacheIndex = 0; cacheIndex < OOT_PSP_PINNED_ASSET_CACHE_COUNT; cacheIndex++) {
        OotPspAssetCache* cache = &sOotPspPinnedAssetCaches[cacheIndex];
        size_t assetIndex;

        for (assetIndex = 0; assetIndex < gOotPspExternalAssetCount; assetIndex++) {
            const OotPspExternalAsset* asset = &gOotPspExternalAssets[assetIndex];
            size_t fileSize = asset->vromEnd - asset->vromStart;
            size_t cacheSizeLimit;

            if (strcmp(asset->path, cache->path) != 0) {
                continue;
            }

            cache->asset = asset;
            cacheSizeLimit = OotPsp_GetAssetCacheSizeLimit(asset);
            if (fileSize > cacheSizeLimit) {
                printf("oot-psp persistent asset deferred cache path=%s size=%lu window=%lu\n", asset->path,
                       (unsigned long)fileSize, (unsigned long)cacheSizeLimit);
                break;
            }

            if (!OotPsp_EnsureAssetCacheRange(cache, asset, 0, fileSize)) {
                printf("oot-psp persistent asset preload failed path=%s size=%lu\n", asset->path,
                       (unsigned long)fileSize);
            }
            break;
        }
    }
}

static const OotPspExternalAsset* OotPsp_FindContainingExternalAsset(uintptr_t vrom, size_t* index) {
    size_t left = 0;
    size_t right = gOotPspExternalAssetCount;

    while (left < right) {
        size_t mid = left + ((right - left) / 2);
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[mid];

        if (vrom < asset->vromStart) {
            right = mid;
        } else if (vrom >= asset->vromEnd) {
            left = mid + 1;
        } else {
            if (index != NULL) {
                *index = mid;
            }
            return asset;
        }
    }

    return NULL;
}

static s32 OotPsp_IsExternalAssetSpanContiguous(size_t index, uintptr_t vromStart, uintptr_t vromEnd) {
    uintptr_t cursor = vromStart;

    if (vromEnd < vromStart) {
        return false;
    }

    if (vromStart == vromEnd) {
        return true;
    }

    while (cursor < vromEnd) {
        const OotPspExternalAsset* asset;

        if (index >= gOotPspExternalAssetCount) {
            return false;
        }

        asset = &gOotPspExternalAssets[index];
        if ((cursor < asset->vromStart) || (cursor >= asset->vromEnd)) {
            return false;
        }

        cursor = asset->vromEnd;
        if ((cursor < vromEnd) &&
            (((index + 1) >= gOotPspExternalAssetCount) || (gOotPspExternalAssets[index + 1].vromStart != cursor))) {
            return false;
        }

        index++;
    }

    return true;
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

static s32 OotPsp_AreOriginalAssetRangesSorted(void) {
    size_t i;
    uintptr_t previousStart = 0;

    if (sOotPspOriginalRangesSorted >= 0) {
        return sOotPspOriginalRangesSorted;
    }

    sOotPspOriginalRangesSorted = true;
    for (i = 0; i < gOotPspExternalAssetCount; i++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[i];

        if (asset->originalVromEnd <= asset->originalVromStart) {
            continue;
        }

        if ((i != 0) && (asset->originalVromStart < previousStart)) {
            sOotPspOriginalRangesSorted = false;
            break;
        }

        previousStart = asset->originalVromStart;
    }

    return sOotPspOriginalRangesSorted;
}

static const OotPspExternalAsset* OotPsp_FindContainingOriginalExternalAssetRange(uintptr_t vromStart,
                                                                                  uintptr_t vromEnd) {
    size_t i;

    if (OotPsp_AreOriginalAssetRangesSorted()) {
        size_t left = 0;
        size_t right = gOotPspExternalAssetCount;

        while (left < right) {
            size_t mid = left + ((right - left) / 2);
            const OotPspExternalAsset* asset = &gOotPspExternalAssets[mid];

            if (vromStart < asset->originalVromStart) {
                right = mid;
            } else if ((vromStart > asset->originalVromEnd) ||
                       ((vromStart == asset->originalVromEnd) && (vromEnd != vromStart))) {
                left = mid + 1;
            } else if (OotPsp_RangeContains(asset->originalVromStart, asset->originalVromEnd, vromStart, vromEnd)) {
                return asset;
            } else {
                return NULL;
            }
        }

        return NULL;
    }

    for (i = 0; i < gOotPspExternalAssetCount; i++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[i];

        if (OotPsp_RangeContains(asset->originalVromStart, asset->originalVromEnd, vromStart, vromEnd)) {
            return asset;
        }
    }

    return NULL;
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

static s32 OotPsp_ClearLoadedAssetSerialRanges(uintptr_t ramStart, uintptr_t ramEnd) {
    size_t left = 0;
    size_t right = sOotPspLoadedAssetSerialRangeCount;
    size_t first;
    size_t last;

    while (left < right) {
        size_t mid = left + ((right - left) / 2);

        if (sOotPspLoadedAssetSerialRanges[mid].ramEnd <= ramStart) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    first = left;
    last = first;
    while ((last < sOotPspLoadedAssetSerialRangeCount) &&
           (sOotPspLoadedAssetSerialRanges[last].ramStart < ramEnd)) {
        last++;
    }

    if (first == last) {
        return false;
    }

    if (last < sOotPspLoadedAssetSerialRangeCount) {
        memmove(&sOotPspLoadedAssetSerialRanges[first], &sOotPspLoadedAssetSerialRanges[last],
                (sOotPspLoadedAssetSerialRangeCount - last) * sizeof(sOotPspLoadedAssetSerialRanges[0]));
    }
    sOotPspLoadedAssetSerialRangeCount -= last - first;
    return true;
}

static void OotPsp_StoreLoadedAssetSerialRange(uintptr_t ramStart, uintptr_t ramEnd, u32 serial) {
    size_t left = 0;
    size_t right = sOotPspLoadedAssetSerialRangeCount;
    size_t insertIndex;

    if (sOotPspLoadedAssetSerialRangeCount >= OOT_PSP_LOADED_ASSET_RANGE_COUNT) {
        sOotPspLoadedAssetSerialRangeIndexComplete = false;
        return;
    }

    while (left < right) {
        size_t mid = left + ((right - left) / 2);

        if (sOotPspLoadedAssetSerialRanges[mid].ramStart < ramStart) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    insertIndex = left;
    if (insertIndex < sOotPspLoadedAssetSerialRangeCount) {
        memmove(&sOotPspLoadedAssetSerialRanges[insertIndex + 1],
                &sOotPspLoadedAssetSerialRanges[insertIndex],
                (sOotPspLoadedAssetSerialRangeCount - insertIndex) * sizeof(sOotPspLoadedAssetSerialRanges[0]));
    }

    sOotPspLoadedAssetSerialRanges[insertIndex].ramStart = ramStart;
    sOotPspLoadedAssetSerialRanges[insertIndex].ramEnd = ramEnd;
    sOotPspLoadedAssetSerialRanges[insertIndex].serial = serial;
    sOotPspLoadedAssetSerialRangeCount++;
}

static u32 OotPsp_FindLoadedAssetRangeSerial(uintptr_t ramStart) {
    size_t left = 0;
    size_t right = sOotPspLoadedAssetSerialRangeCount;
    const OotPspLoadedAssetSerialRange* range;

    while (left < right) {
        size_t mid = left + ((right - left) / 2);

        if (sOotPspLoadedAssetSerialRanges[mid].ramStart <= ramStart) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left == 0) {
        return 0;
    }

    range = &sOotPspLoadedAssetSerialRanges[left - 1];
    return (ramStart < range->ramEnd) ? range->serial : 0;
}

static void OotPsp_ClearLoadedAssetRange(void* ram, size_t size) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;
    s32 cleared;

    if (!OotPsp_RamRangeFromPtr(ram, size, &ramStart, &ramEnd)) {
        return;
    }

    cleared = OotPsp_ClearLoadedAssetSerialRanges(ramStart, ramEnd);

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if (OotPsp_RangesOverlap(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            OotPsp_ForgetNativeTextureRangeCache(range);
            range->ramStart = 0;
            range->ramEnd = 0;
            range->assetOffsetStart = 0;
            range->flags = 0;
            range->serial = 0;
            cleared = true;
        }
    }

    if (cleared) {
        OotPsp_ClearAssetRangeSerialCache();
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

    if (sOotPspLastNativeTextureRange == range) {
        sOotPspLastNativeTextureRange = NULL;
    }

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

static s32 OotPsp_IsNativeLoadedTextureByteRange(const OotPspLoadedAssetRange* range, uintptr_t ram) {
    return OotPsp_IsNativeLoadedAssetByteRange(range, ram) &&
           ((range->flags & OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS) != 0);
}

static void OotPsp_RememberNativeTextureRange(const OotPspLoadedAssetRange* range) {
    size_t i;

    if ((range == NULL) || ((range->flags & OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS) == 0)) {
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

static inline __attribute__((always_inline)) const OotPspLoadedAssetRange*
OotPsp_FindCachedNativeTextureRange(uintptr_t ram) {
    size_t i;

    if (OotPsp_IsNativeLoadedTextureByteRange(sOotPspLastNativeTextureRange, ram)) {
        return sOotPspLastNativeTextureRange;
    }

    for (i = 0; i < OOT_PSP_NATIVE_TEXTURE_RANGE_CACHE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = sOotPspNativeTextureRangeCache[i];

        if (OotPsp_IsNativeLoadedTextureByteRange(range, ram)) {
            sOotPspLastNativeTextureRange = range;
            return range;
        }
    }

    return NULL;
}

static s32 OotPsp_MapNativeExternalTextureByteInRange(const OotPspLoadedAssetRange* range, uintptr_t ram,
                                                      const void** mapped) {
    uintptr_t assetOffset;
    uintptr_t assetSpan;
    uintptr_t relativeAssetOffset;
    uintptr_t mappedRelativeAssetOffset;
    uintptr_t mappedRam;

    assetSpan = range->ramEnd - range->ramStart;
    if (range->assetOffsetStart > (UINTPTR_MAX - assetSpan)) {
        return false;
    }

    assetOffset = range->assetOffsetStart + (ram - range->ramStart);
    relativeAssetOffset = assetOffset - range->assetOffsetStart;
    mappedRelativeAssetOffset = relativeAssetOffset ^ 7U;

    if (mappedRelativeAssetOffset >= assetSpan) {
        return false;
    }

    mappedRam = range->ramStart + mappedRelativeAssetOffset;
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
    OotPsp_StoreLoadedAssetSerialRange(ramStart, ramEnd, serial);
    OotPsp_ClearAssetRangeSerialCache();

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
    if ((cacheOffset > cache->dataSize) || (size > (cache->dataSize - cacheOffset))) {
        return false;
    }
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
    size_t assetIndex;
    const OotPspExternalAsset* asset;

    if (OotPsp_FindContainingExternalAsset(vromStart, &assetIndex) != NULL) {
        if (OotPsp_IsExternalAssetSpanContiguous(assetIndex, vromStart, vromEnd)) {
            *normalizedStart = vromStart;
            *normalizedEnd = vromEnd;
            return true;
        }
    }

    asset = OotPsp_FindContainingOriginalExternalAssetRange(vromStart, vromEnd);
    if ((asset != NULL) &&
        OotPsp_TryTranslateAssetRange(asset, asset->originalVromStart, asset->originalVromEnd, vromStart, vromEnd,
                                      normalizedStart, normalizedEnd)) {
        return true;
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
    u32 serial;

    if (!OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return 0;
    }

    if (OotPsp_GetCachedAssetRangeSerial(ramStart, &serial)) {
        return serial;
    }

    serial = OotPsp_FindLoadedAssetRangeSerial(ramStart);
    if ((serial == 0) && !sOotPspLoadedAssetSerialRangeIndexComplete) {
        size_t i;

        for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
            const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

            if ((range->serial != 0) && (ramStart >= range->ramStart) && (ramStart < range->ramEnd)) {
                serial = range->serial;
                break;
            }
        }
    }

    OotPsp_RememberAssetRangeSerial(ramStart, serial);
    return serial;
}

s32 OotPsp_GetNativeExternalTextureRangeStart(const void* ptr, size_t size, uintptr_t* rangeStart) {
    uintptr_t ramStart;
    uintptr_t ramEnd;
    size_t i;

    if ((rangeStart == NULL) || !OotPsp_RamRangeFromPtr(ptr, size, &ramStart, &ramEnd)) {
        return false;
    }

    {
        const OotPspLoadedAssetRange* range = OotPsp_FindCachedNativeTextureRange(ramStart);

        if ((range != NULL) && OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            *rangeStart = range->ramStart;
            return true;
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if (((range->flags & (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) ==
             (OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) &&
            (range->serial != 0) && OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            OotPsp_RememberNativeTextureRange(range);
            sOotPspLastNativeTextureRange = range;
            *rangeStart = range->ramStart;
            return true;
        }
    }

    for (i = 0; i < OOT_PSP_LOADED_ASSET_RANGE_COUNT; i++) {
        const OotPspLoadedAssetRange* range = &sOotPspLoadedAssetRanges[i];

        if (OotPsp_IsNativeLoadedAssetByteRange(range, ramStart) &&
            OotPsp_RangeContains(range->ramStart, range->ramEnd, ramStart, ramEnd)) {
            *rangeStart = range->ramStart;
            return true;
        }
    }

    return false;
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

        if (!OotPsp_IsNativeLoadedTextureByteRange(range, ram)) {
            continue;
        }

        OotPsp_RememberNativeTextureRange(range);
        return OotPsp_MapNativeExternalTextureByteInRange(range, ram, mapped);
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

        return OotPsp_MapNativeExternalTextureByteInRange(range, ram, mapped);
    }

    return false;
}

static s32 OotPsp_AssetReadLocked(void* ram, uintptr_t vrom, size_t size) {
    uintptr_t normalizedVrom;
    uintptr_t normalizedEnd;
    uintptr_t cursor;
    size_t assetIndex;
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
        chunkVromStart = cursor;
        chunkRemaining = asset->vromEnd - cursor;
        chunkSize = remaining < chunkRemaining ? remaining : chunkRemaining;
        chunkOut = out;
        chunkRequestedSize = chunkSize;
        chunkSerial = OotPsp_NextLoadedAssetSerial();

        if (!OotPsp_ReadAssetFileRange(asset, offset, out, chunkSize, true)) {
            return OOT_PSP_ASSET_READ_FAILED;
        }

        out += chunkSize;
        cursor += chunkSize;
        remaining -= chunkSize;
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

static void OotPsp_WaitForForegroundAssetReads(void) {
    u32 waitStartUsec = sceKernelGetSystemTimeLow();

    while (sOotPspForegroundAssetReadWaiters > 0) {
        u32 nowUsec = sceKernelGetSystemTimeLow();

        if ((s32)(nowUsec - waitStartUsec) >= OOT_PSP_AUDIO_READ_BACKOFF_MAX_USEC) {
            break;
        }
        sceKernelDelayThread(OOT_PSP_AUDIO_READ_BACKOFF_USEC);
    }
}

static s32 OotPsp_AssetReadInternal(void* ram, uintptr_t vrom, size_t size, s32 isAudioRead, s32 urgentAudioRead) {
    s32 status;

    OotPsp_InitAssetSema();
    if (isAudioRead) {
        if (!urgentAudioRead) {
            OotPsp_WaitForForegroundAssetReads();
        }
    } else {
        sOotPspForegroundAssetReadWaiters++;
    }

    OotPsp_LockAssetLoader();
    if (!isAudioRead) {
        sOotPspForegroundAssetReadWaiters--;
    }

    status = OotPsp_AssetReadLocked(ram, vrom, size);
    OotPsp_UnlockAssetLoader();
    return status;
}

s32 OotPsp_AssetRead(void* ram, uintptr_t vrom, size_t size) {
    return OotPsp_AssetReadInternal(ram, vrom, size, false, false);
}

s32 OotPsp_AssetReadAudio(void* ram, uintptr_t vrom, size_t size) {
    return OotPsp_AssetReadInternal(ram, vrom, size, true, false);
}

s32 OotPsp_AssetReadAudioUrgent(void* ram, uintptr_t vrom, size_t size) {
    return OotPsp_AssetReadInternal(ram, vrom, size, true, true);
}
