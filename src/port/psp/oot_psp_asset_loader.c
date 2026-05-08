#include "oot_psp_asset_loader.h"
#include "segment_symbols.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#define OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS 0x08800000U
#define OOT_PSP_RELOCATION_BIAS_MIN         0x08000000U
#define OOT_PSP_RELOCATION_BIAS_MAX         0x10000000U

static char sOotPspAssetRoot[256];
static uintptr_t sOotPspPrxRelocationBias = OOT_PSP_DEFAULT_PRX_RELOCATION_BIAS;
static s32 sOotPspPrxRelocationBiasInitialized = false;

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

    if ((vrom > (UINTPTR_MAX - size)) ||
        !OotPsp_NormalizeVromRange(vrom, vrom + size, &normalizedVrom, &normalizedEnd)) {
        return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
    }

    cursor = normalizedVrom;
    (void)normalizedEnd;

    if (OotPsp_FindContainingExternalAsset(normalizedVrom, &assetIndex) == NULL) {
        return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
    }

    while (remaining != 0) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[assetIndex];
        uintptr_t offset;
        uintptr_t readOffset;
        size_t chunkRemaining;
        size_t chunkSize;

        if ((cursor < asset->vromStart) || (cursor >= asset->vromEnd)) {
            return OOT_PSP_ASSET_READ_NOT_EXTERNAL;
        }

        offset = cursor - asset->vromStart;
        readOffset = offset;
        chunkRemaining = asset->vromEnd - cursor;
        chunkSize = remaining < chunkRemaining ? remaining : chunkRemaining;

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
            int chunk = chunkSize > 0x4000 ? 0x4000 : (int)chunkSize;
            int read = sceIoRead(fd, out, chunk);

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
