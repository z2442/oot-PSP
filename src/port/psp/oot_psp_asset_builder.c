#include "oot_psp_asset_builder.h"

#include "oot_psp_asset_loader.h"
#include "oot_psp_renderer.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define OOT_PSP_INPUT_ROM_PATH             "data/basrom.z64"
#define OOT_PSP_INPUT_ROM_FALLBACK_PATH    "data/baserom.z64"
#define OOT_PSP_PACKED_ASSET_PATH          "data/segments/oot_psp_assets.bin"
#define OOT_PSP_PACKED_ASSET_TEMP_PATH     "data/segments/oot_psp_assets.tmp"
#define OOT_PSP_ROM_SIZE                   0x02000000U
#define OOT_PSP_ROM_CRC32                  0xCD16C529U
#define OOT_PSP_DMADATA_OFFSET             0x7430U
#define OOT_PSP_DMADATA_COUNT              1510U
#define OOT_PSP_TRANSFORM_ZERO_SELECTOR    56U
#define OOT_PSP_TRANSFORM_MAPPED_SELECTOR  57U
#define OOT_PSP_IO_CHUNK_SIZE              0x4000U
#define OOT_PSP_IO_ZERO_RETRY_COUNT        16
#define OOT_PSP_IO_ZERO_RETRY_USEC         1000

typedef struct OotPspDmaEntry {
    u32 vromStart;
    u32 vromEnd;
    u32 romStart;
    u32 romEnd;
} OotPspDmaEntry;

extern const u8 gOotPspAssetTransformCompressed[];
extern const u8 gOotPspAssetTransformCompressedEnd[];

static u32 sOotPspAssetBuilderProgress;
static s32 sOotPspAssetBuilderErrorShown;

static void OotPspAssetBuilder_ShowProgress(u32 progressPermille, const char* status) {
    sOotPspAssetBuilderProgress = progressPermille;
    OotPspRenderer_RenderFirstBootProgress(progressPermille, status, false);
}

static void OotPspAssetBuilder_ShowError(const char* status) {
    sOotPspAssetBuilderErrorShown = true;
    OotPspRenderer_RenderFirstBootProgress(sOotPspAssetBuilderProgress, status, true);
}

static u32 OotPspAssetBuilder_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u32 OotPspAssetBuilder_ReadBe32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static s32 OotPspAssetBuilder_ReadAt(SceUID fd, u32 offset, void* output, size_t size) {
    u8* cursor = output;
    s32 zeroReads = 0;

    if (sceIoLseek32(fd, (s32)offset, PSP_SEEK_SET) < 0) {
        return false;
    }

    while (size != 0) {
        size_t chunk = size > OOT_PSP_IO_CHUNK_SIZE ? OOT_PSP_IO_CHUNK_SIZE : size;
        s32 read = sceIoRead(fd, cursor, chunk);

        if (read < 0) {
            return false;
        }
        if (read == 0) {
            if (zeroReads++ >= OOT_PSP_IO_ZERO_RETRY_COUNT) {
                return false;
            }
            sceKernelDelayThread(OOT_PSP_IO_ZERO_RETRY_USEC);
            continue;
        }
        zeroReads = 0;
        cursor += read;
        size -= read;
    }
    return true;
}

static s32 OotPspAssetBuilder_WriteAt(SceUID fd, u32 offset, const void* input, size_t size) {
    const u8* cursor = input;
    s32 zeroWrites = 0;

    if (sceIoLseek32(fd, (s32)offset, PSP_SEEK_SET) < 0) {
        return false;
    }

    while (size != 0) {
        size_t chunk = size > OOT_PSP_IO_CHUNK_SIZE ? OOT_PSP_IO_CHUNK_SIZE : size;
        s32 written = sceIoWrite(fd, cursor, chunk);

        if (written < 0) {
            return false;
        }
        if (written == 0) {
            if (zeroWrites++ >= OOT_PSP_IO_ZERO_RETRY_COUNT) {
                return false;
            }
            sceKernelDelayThread(OOT_PSP_IO_ZERO_RETRY_USEC);
            continue;
        }
        zeroWrites = 0;
        cursor += written;
        size -= written;
    }
    return true;
}

static s32 OotPspAssetBuilder_CopyUncompressedAsset(SceUID romFd, SceUID outputFd, u32 romOffset,
                                                     u32 outputOffset, size_t size) {
    u8 buffer[OOT_PSP_IO_CHUNK_SIZE];

    while (size != 0) {
        size_t chunk = size > sizeof(buffer) ? sizeof(buffer) : size;

        if (!OotPspAssetBuilder_ReadAt(romFd, romOffset, buffer, chunk) ||
            !OotPspAssetBuilder_WriteAt(outputFd, outputOffset, buffer, chunk)) {
            return false;
        }
        romOffset += chunk;
        outputOffset += chunk;
        size -= chunk;
    }
    return true;
}

static s32 OotPspAssetBuilder_FileHasSize(const char* path, size_t expectedSize) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    SceOff size;

    if (fd < 0) {
        return false;
    }
    size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return (size >= 0) && ((size_t)size == expectedSize);
}

static SceUID OotPspAssetBuilder_OpenRom(char* pathBuffer, size_t pathBufferSize) {
    const char* path = OotPsp_ResolveRootPath(OOT_PSP_INPUT_ROM_PATH, pathBuffer, pathBufferSize);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);

    if (fd < 0) {
        path = OotPsp_ResolveRootPath(OOT_PSP_INPUT_ROM_FALLBACK_PATH, pathBuffer, pathBufferSize);
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    }
    return fd;
}

static s32 OotPspAssetBuilder_ValidateRom(SceUID fd) {
    static const u8 expectedHeader[16] = {
        0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F,
        0x80, 0x00, 0x04, 0x00, 0x00, 0x00, 0x14, 0x49,
    };
    u8* buffer;
    u8 header[16];
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t remaining = OOT_PSP_ROM_SIZE;
    size_t processed = 0;
    SceOff size = sceIoLseek32(fd, 0, PSP_SEEK_END);

    if ((size < 0) || ((u32)size != OOT_PSP_ROM_SIZE) || !OotPspAssetBuilder_ReadAt(fd, 0, header, sizeof(header)) ||
        (memcmp(header, expectedHeader, sizeof(header)) != 0)) {
        OotPspAssetBuilder_ShowError("NTSC 1.0 z64 ROM required");
        return false;
    }

    buffer = malloc(OOT_PSP_IO_CHUNK_SIZE);
    if (buffer == NULL) {
        OotPspAssetBuilder_ShowError("Not enough memory to validate ROM");
        return false;
    }
    if (sceIoLseek32(fd, 0, PSP_SEEK_SET) < 0) {
        free(buffer);
        OotPspAssetBuilder_ShowError("Could not read the ROM");
        return false;
    }
    while (remaining != 0) {
        size_t chunk = remaining > OOT_PSP_IO_CHUNK_SIZE ? OOT_PSP_IO_CHUNK_SIZE : remaining;
        s32 read = sceIoRead(fd, buffer, chunk);

        if (read <= 0) {
            free(buffer);
            OotPspAssetBuilder_ShowError("Could not read the ROM");
            return false;
        }
        crc = crc32(crc, buffer, read);
        remaining -= read;
        processed += read;
        if (((processed & 0xFFFFF) == 0) || (remaining == 0)) {
            char status[64];

            snprintf(status, sizeof(status), "Validating ROM: %lu / 32 MB", (unsigned long)(processed >> 20));
            OotPspAssetBuilder_ShowProgress((u32)((processed * 100) / OOT_PSP_ROM_SIZE), status);
        }
    }
    free(buffer);

    if ((u32)crc != OOT_PSP_ROM_CRC32) {
        OotPspAssetBuilder_ShowError("ROM checksum failed - NTSC 1.0 required");
        return false;
    }
    return true;
}

static s32 OotPspAssetBuilder_LoadDmaTable(SceUID fd, OotPspDmaEntry* entries) {
    u8 raw[OOT_PSP_DMADATA_COUNT * 16];
    size_t i;

    if (!OotPspAssetBuilder_ReadAt(fd, OOT_PSP_DMADATA_OFFSET, raw, sizeof(raw))) {
        return false;
    }
    for (i = 0; i < OOT_PSP_DMADATA_COUNT; i++) {
        const u8* input = &raw[i * 16];

        entries[i].vromStart = OotPspAssetBuilder_ReadBe32(input + 0);
        entries[i].vromEnd = OotPspAssetBuilder_ReadBe32(input + 4);
        entries[i].romStart = OotPspAssetBuilder_ReadBe32(input + 8);
        entries[i].romEnd = OotPspAssetBuilder_ReadBe32(input + 12);
    }
    return true;
}

static const OotPspDmaEntry* OotPspAssetBuilder_FindDmaEntry(const OotPspDmaEntry* entries,
                                                             const OotPspExternalAsset* asset) {
    size_t i;

    for (i = 0; i < OOT_PSP_DMADATA_COUNT; i++) {
        if ((entries[i].vromStart == asset->originalVromStart) && (entries[i].vromEnd == asset->originalVromEnd)) {
            return &entries[i];
        }
    }
    return NULL;
}

static s32 OotPspAssetBuilder_DecompressYaz0(const u8* input, size_t inputSize, u8* output, size_t outputSize) {
    const u8* inputEnd = input + inputSize;
    u8* outputStart = output;
    u8* outputEnd = output + outputSize;
    u8 code = 0;
    u32 validBits = 0;

    if ((inputSize < 16) || (memcmp(input, "Yaz0", 4) != 0) ||
        (OotPspAssetBuilder_ReadBe32(input + 4) != outputSize)) {
        return false;
    }
    input += 16;

    while (output < outputEnd) {
        if (validBits == 0) {
            if (input >= inputEnd) {
                return false;
            }
            code = *input++;
            validBits = 8;
        }

        if ((code & 0x80) != 0) {
            if (input >= inputEnd) {
                return false;
            }
            *output++ = *input++;
        } else {
            size_t distance;
            size_t length;
            u8* copy;

            if ((size_t)(inputEnd - input) < 2) {
                return false;
            }
            distance = (((size_t)input[0] & 0x0F) << 8) | input[1];
            length = input[0] >> 4;
            input += 2;
            if (length == 0) {
                if (input >= inputEnd) {
                    return false;
                }
                length = (size_t)*input++ + 0x12;
            } else {
                length += 2;
            }
            if ((distance + 1 > (size_t)(output - outputStart)) || (length > (size_t)(outputEnd - output))) {
                return false;
            }
            copy = output - distance - 1;
            while (length-- != 0) {
                *output++ = *copy++;
            }
        }
        code <<= 1;
        validBits--;
    }
    return true;
}

static s32 OotPspAssetBuilder_LoadAsset(SceUID romFd, const OotPspDmaEntry* dma, u8** output,
                                        size_t outputSize) {
    size_t storedSize = dma->romEnd != 0 ? dma->romEnd - dma->romStart : outputSize;
    u8* stored;
    u8* decoded;

    if ((dma->romStart == 0xFFFFFFFFU) || (storedSize == 0)) {
        return false;
    }
    stored = malloc(storedSize);
    if ((stored == NULL) || !OotPspAssetBuilder_ReadAt(romFd, dma->romStart, stored, storedSize)) {
        free(stored);
        return false;
    }
    if (dma->romEnd == 0) {
        *output = stored;
        return true;
    }

    decoded = malloc(outputSize);
    if ((decoded == NULL) || !OotPspAssetBuilder_DecompressYaz0(stored, storedSize, decoded, outputSize)) {
        free(decoded);
        free(stored);
        return false;
    }
    free(stored);
    *output = decoded;
    return true;
}

static s32 OotPspAssetBuilder_TransformAsset(u8* data, size_t size, size_t assetIndex, const u8** manifestCursor,
                                             const u8* manifestEnd, const u8* permutations,
                                             size_t permutationCount) {
    const u8* cursor = *manifestCursor;
    u32 manifestIndex;
    u32 manifestSize;
    u32 payloadSize;
    u32 compressedSize;
    uLongf inflatedSize;
    u8* payload = NULL;
    const u8* selectors;
    const u8* mappings;
    const u8* mappingEnd;
    u8* mappedBytes = NULL;
    size_t mappedSize = 0;
    size_t mappedCursor = 0;
    size_t selectorCount = (size + 7) / 8;
    size_t blockIndex;
    s32 ok = false;

    if ((size_t)(manifestEnd - cursor) < 16) {
        return false;
    }
    manifestIndex = OotPspAssetBuilder_ReadLe32(cursor + 0);
    manifestSize = OotPspAssetBuilder_ReadLe32(cursor + 4);
    payloadSize = OotPspAssetBuilder_ReadLe32(cursor + 8);
    compressedSize = OotPspAssetBuilder_ReadLe32(cursor + 12);
    cursor += 16;
    if ((manifestIndex != assetIndex) || (manifestSize != size) || (payloadSize < selectorCount) ||
        ((size_t)(manifestEnd - cursor) < compressedSize)) {
        return false;
    }
    payload = malloc(payloadSize);
    if (payload == NULL) {
        return false;
    }
    inflatedSize = payloadSize;
    if ((uncompress(payload, &inflatedSize, cursor, compressedSize) != Z_OK) || (inflatedSize != payloadSize)) {
        goto cleanup;
    }
    cursor += compressedSize;
    selectors = payload;
    mappings = selectors + selectorCount;
    mappingEnd = payload + payloadSize;

    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        if (selectors[blockIndex] == OOT_PSP_TRANSFORM_MAPPED_SELECTOR) {
            size_t offset = blockIndex * 8;

            mappedSize += (size - offset) < 8 ? size - offset : 8;
        }
    }
    if ((size_t)(mappingEnd - mappings) != mappedSize * 5) {
        goto cleanup;
    }
    if (mappedSize != 0) {
        size_t i;

        mappedBytes = malloc(mappedSize);
        if (mappedBytes == NULL) {
            goto cleanup;
        }
        for (i = 0; i < mappedSize; i++) {
            u32 sourceOffset = OotPspAssetBuilder_ReadLe32(mappings);
            u8 delta = mappings[4];

            mappings += 5;
            if (sourceOffset >= size) {
                goto cleanup;
            }
            mappedBytes[i] = data[sourceOffset] + delta;
        }
    }

    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        size_t offset = blockIndex * 8;
        size_t blockSize = (size - offset) < 8 ? size - offset : 8;
        u8 selector = selectors[blockIndex];
        u8 source[8];
        size_t i;

        memcpy(source, data + offset, blockSize);
        if (selector < permutationCount) {
            const u8* mapping = permutations + (selector * 8);

            if (blockSize != 8) {
                goto cleanup;
            }
            for (i = 0; i < 8; i++) {
                data[offset + i] = source[mapping[i]];
            }
        } else if (selector == OOT_PSP_TRANSFORM_ZERO_SELECTOR) {
            memset(data + offset, 0, blockSize);
        } else if (selector == OOT_PSP_TRANSFORM_MAPPED_SELECTOR) {
            if ((mappedSize - mappedCursor) < blockSize) {
                goto cleanup;
            }
            memcpy(data + offset, mappedBytes + mappedCursor, blockSize);
            mappedCursor += blockSize;
        } else {
            goto cleanup;
        }
    }
    if ((mappings != mappingEnd) || (mappedCursor != mappedSize)) {
        goto cleanup;
    }
    *manifestCursor = cursor;
    ok = true;

cleanup:
    free(mappedBytes);
    free(payload);
    return ok;
}

static s32 OotPspAssetBuilder_Build(SceUID romFd, const char* outputPath, const char* tempPath) {
    const size_t compressedSize = gOotPspAssetTransformCompressedEnd - gOotPspAssetTransformCompressed;
    const u8* compressed = gOotPspAssetTransformCompressed;
    const u8* manifestCursor;
    const u8* manifestEnd;
    const u8* permutations;
    u32 manifestEntryCount;
    u32 permutationCount;
    u32 nativeSeen = 0;
    OotPspDmaEntry* dmaEntries = NULL;
    SceUID outputFd = -1;
    size_t assetIndex;
    s32 ok = false;

    if ((compressedSize < 12) || (memcmp(compressed, "OPZ4", 4) != 0)) {
        OotPspAssetBuilder_ShowError("Conversion data is missing");
        return false;
    }
    OotPspAssetBuilder_ShowProgress(100, "Preparing conversion data");
    manifestEntryCount = OotPspAssetBuilder_ReadLe32(compressed + 4);
    permutationCount = OotPspAssetBuilder_ReadLe32(compressed + 8);
    if ((permutationCount != OOT_PSP_TRANSFORM_ZERO_SELECTOR) ||
        (compressedSize < 12 + (permutationCount * 8))) {
        OotPspAssetBuilder_ShowError("Conversion data is incompatible");
        return false;
    }
    permutations = compressed + 12;
    manifestCursor = permutations + (permutationCount * 8);
    manifestEnd = compressed + compressedSize;

    dmaEntries = malloc(sizeof(*dmaEntries) * OOT_PSP_DMADATA_COUNT);
    if (dmaEntries == NULL) {
        OotPspAssetBuilder_ShowError("Not enough memory for asset setup");
        goto cleanup;
    }

    if (!OotPspAssetBuilder_LoadDmaTable(romFd, dmaEntries)) {
        OotPspAssetBuilder_ShowError("Could not read the ROM file table");
        goto cleanup;
    }
    sceIoRemove(tempPath);
    outputFd = sceIoOpen(tempPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (outputFd < 0) {
        OotPspAssetBuilder_ShowError("Could not create the asset file");
        goto cleanup;
    }

    for (assetIndex = 0; assetIndex < gOotPspExternalAssetCount; assetIndex++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[assetIndex];
        const OotPspDmaEntry* dma = OotPspAssetBuilder_FindDmaEntry(dmaEntries, asset);
        size_t assetSize = asset->vromEnd - asset->vromStart;
        u8* data = NULL;
        char status[64];

        if ((assetIndex & 7) == 0) {
            snprintf(status, sizeof(status), "Converting assets: %lu / %lu", (unsigned long)assetIndex,
                     (unsigned long)gOotPspExternalAssetCount);
            OotPspAssetBuilder_ShowProgress(
                100 + (u32)((assetIndex * 900) / gOotPspExternalAssetCount), status);
        }

        if ((dma == NULL) || ((dma->vromEnd - dma->vromStart) != assetSize)) {
            snprintf(status, sizeof(status), "Could not extract %.36s", asset->name);
            OotPspAssetBuilder_ShowError(status);
            goto cleanup;
        }

        if ((asset->flags == 0) && (dma->romEnd == 0)) {
            if (!OotPspAssetBuilder_CopyUncompressedAsset(romFd, outputFd, dma->romStart, asset->fileOffset,
                                                           assetSize)) {
                OotPspAssetBuilder_ShowError("Could not copy data from the ROM");
                goto cleanup;
            }
        } else {
            if (!OotPspAssetBuilder_LoadAsset(romFd, dma, &data, assetSize)) {
                snprintf(status, sizeof(status), "Could not extract %.36s", asset->name);
                OotPspAssetBuilder_ShowError(status);
                free(data);
                goto cleanup;
            }
            if ((asset->flags != 0) &&
                !OotPspAssetBuilder_TransformAsset(data, assetSize, assetIndex, &manifestCursor, manifestEnd,
                                                   permutations, permutationCount)) {
                snprintf(status, sizeof(status), "Could not convert %.36s", asset->name);
                OotPspAssetBuilder_ShowError(status);
                free(data);
                goto cleanup;
            }
            if (!OotPspAssetBuilder_WriteAt(outputFd, asset->fileOffset, data, assetSize)) {
                OotPspAssetBuilder_ShowError("Could not write the asset file");
                free(data);
                goto cleanup;
            }
            free(data);
        }
        nativeSeen += asset->flags != 0;
        if ((assetIndex + 1) == gOotPspExternalAssetCount) {
            snprintf(status, sizeof(status), "Converting assets: %lu / %lu", (unsigned long)(assetIndex + 1),
                     (unsigned long)gOotPspExternalAssetCount);
            OotPspAssetBuilder_ShowProgress(1000, status);
        }
    }
    if ((nativeSeen != manifestEntryCount) || (manifestCursor != manifestEnd)) {
        OotPspAssetBuilder_ShowError("Conversion data did not match the ROM");
        goto cleanup;
    }
    sceIoClose(outputFd);
    outputFd = -1;
    sceIoRemove(outputPath);
    if (sceIoRename(tempPath, outputPath) < 0) {
        OotPspAssetBuilder_ShowError("Could not finish the asset file");
        goto cleanup;
    }
    ok = true;

cleanup:
    if (outputFd >= 0) {
        sceIoClose(outputFd);
    }
    if (!ok) {
        sceIoRemove(tempPath);
    }
    free(dmaEntries);
    return ok;
}

s32 OotPspAssetBuilder_Ensure(void) {
    char outputBuffer[384];
    char tempBuffer[384];
    char romBuffer[384];
    char directoryBuffer[384];
    const char* outputPath = OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_PATH, outputBuffer, sizeof(outputBuffer));
    const char* tempPath = OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_TEMP_PATH, tempBuffer, sizeof(tempBuffer));
    const char* directoryPath;
    size_t expectedSize;
    SceUID romFd;
    s32 ok;

    if (gOotPspExternalAssetCount == 0) {
        return false;
    }
    expectedSize = gOotPspExternalAssets[gOotPspExternalAssetCount - 1].fileOffset +
                   (gOotPspExternalAssets[gOotPspExternalAssetCount - 1].vromEnd -
                    gOotPspExternalAssets[gOotPspExternalAssetCount - 1].vromStart);
    if (OotPspAssetBuilder_FileHasSize(outputPath, expectedSize)) {
        return true;
    }

    sOotPspAssetBuilderProgress = 0;
    sOotPspAssetBuilderErrorShown = false;
    OotPspAssetBuilder_ShowProgress(0, "Checking NTSC 1.0 ROM");

    directoryPath = OotPsp_ResolveRootPath("data", directoryBuffer, sizeof(directoryBuffer));
    sceIoMkdir(directoryPath, 0777);
    directoryPath = OotPsp_ResolveRootPath("data/segments", directoryBuffer, sizeof(directoryBuffer));
    sceIoMkdir(directoryPath, 0777);

    romFd = OotPspAssetBuilder_OpenRom(romBuffer, sizeof(romBuffer));
    if (romFd < 0) {
        OotPspAssetBuilder_ShowError("ROM not found at data/basrom.z64");
        sceKernelDelayThread(3000000);
        return false;
    }
    ok = OotPspAssetBuilder_ValidateRom(romFd) && OotPspAssetBuilder_Build(romFd, outputPath, tempPath);
    sceIoClose(romFd);
    if (ok) {
        OotPspAssetBuilder_ShowProgress(1000, "Asset setup complete");
        sceKernelDelayThread(250000);
    } else {
        if (!sOotPspAssetBuilderErrorShown) {
            OotPspAssetBuilder_ShowError("Asset setup failed");
        }
        sceKernelDelayThread(3000000);
    }
    return ok;
}
