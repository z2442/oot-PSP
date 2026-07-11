#include "oot_psp_runtime_patch.h"

#include "oot_psp_asset_loader.h"

#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define OOT_PSP_PATCH_ZERO_SELECTOR   56U
#define OOT_PSP_PATCH_MAPPED_SELECTOR 57U

extern const u8 gOotPspRuntimePatchBlob[] __attribute__((weak));
extern const u8 gOotPspRuntimePatchBlobEnd[] __attribute__((weak));
extern u8 _ftext[];

#define OOT_PSP_CODE_NJPG_TEXT_OFFSET 0x000D5B20U
#define OOT_PSP_CODE_NJPG_TEXT_SIZE   0x00000AF0U
#define OOT_PSP_CODE_NJPG_DATA_OFFSET 0x00103CD0U
#define OOT_PSP_CODE_NJPG_DATA_SIZE   0x00000060U

static u32 OotPspRuntimePatch_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static s32 OotPspRuntimePatch_ReadLeS32(const u8* data) {
    return (s32)OotPspRuntimePatch_ReadLe32(data);
}

static s32 OotPspRuntimePatch_Transform(u8* data, size_t size, const u8* payload, size_t payloadSize,
                                        const u8* permutations, size_t permutationCount) {
    size_t selectorCount = (size + 7) / 8;
    const u8* selectors = payload;
    const u8* mappings;
    const u8* mappingEnd = payload + payloadSize;
    u8* mappedBytes = NULL;
    size_t mappedSize = 0;
    size_t mappedCursor = 0;
    size_t blockIndex;
    s32 ok = false;

    if (payloadSize < selectorCount) {
        return false;
    }
    mappings = selectors + selectorCount;
    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        if (selectors[blockIndex] == OOT_PSP_PATCH_MAPPED_SELECTOR) {
            size_t offset = blockIndex * 8;

            mappedSize += (size - offset) < 8 ? size - offset : 8;
        }
    }
    if ((size_t)(mappingEnd - mappings) != mappedSize * 5) {
        return false;
    }
    if (mappedSize != 0) {
        size_t i;

        mappedBytes = malloc(mappedSize);
        if (mappedBytes == NULL) {
            return false;
        }
        for (i = 0; i < mappedSize; i++) {
            u32 sourceOffset = OotPspRuntimePatch_ReadLe32(mappings);
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
        } else if (selector == OOT_PSP_PATCH_ZERO_SELECTOR) {
            memset(data + offset, 0, blockSize);
        } else if (selector == OOT_PSP_PATCH_MAPPED_SELECTOR) {
            if ((mappedSize - mappedCursor) < blockSize) {
                goto cleanup;
            }
            memcpy(data + offset, mappedBytes + mappedCursor, blockSize);
            mappedCursor += blockSize;
        } else {
            goto cleanup;
        }
    }
    ok = (mappings == mappingEnd) && (mappedCursor == mappedSize);

cleanup:
    free(mappedBytes);
    return ok;
}

s32 OotPspRuntimePatch_Apply(void) {
    const u8* cursor = gOotPspRuntimePatchBlob;
    const u8* end = gOotPspRuntimePatchBlobEnd;
    const u8* permutations;
    u32 patchCount;
    u32 permutationCount;
    u32 patchIndex;

    if (((size_t)(end - cursor) < 12) || (memcmp(cursor, "OPB1", 4) != 0)) {
        return false;
    }
    patchCount = OotPspRuntimePatch_ReadLe32(cursor + 4);
    permutationCount = OotPspRuntimePatch_ReadLe32(cursor + 8);
    cursor += 12;
    if ((permutationCount != OOT_PSP_PATCH_ZERO_SELECTOR) ||
        ((size_t)(end - cursor) < permutationCount * 8)) {
        return false;
    }
    permutations = cursor;
    cursor += permutationCount * 8;

    for (patchIndex = 0; patchIndex < patchCount; patchIndex++) {
        u32 destinationOffset;
        u32 sourceVrom;
        u32 size;
        u32 payloadSize;
        u32 compressedSize;
        u32 relocationCount;
        uLongf inflatedSize;
        u8* destination;
        u8* payload;
        u32 relocationIndex;

        if ((size_t)(end - cursor) < 24) {
            return false;
        }
        destinationOffset = OotPspRuntimePatch_ReadLe32(cursor + 0);
        sourceVrom = OotPspRuntimePatch_ReadLe32(cursor + 4);
        size = OotPspRuntimePatch_ReadLe32(cursor + 8);
        payloadSize = OotPspRuntimePatch_ReadLe32(cursor + 12);
        compressedSize = OotPspRuntimePatch_ReadLe32(cursor + 16);
        relocationCount = OotPspRuntimePatch_ReadLe32(cursor + 20);
        cursor += 24;
        if (((size_t)(end - cursor) < compressedSize) ||
            ((size_t)(end - cursor - compressedSize) < relocationCount * 8)) {
            return false;
        }

        destination = _ftext + destinationOffset;
        payload = malloc(payloadSize);
        if (payload == NULL) {
            return false;
        }
        inflatedSize = payloadSize;
        if ((uncompress(payload, &inflatedSize, cursor, compressedSize) != Z_OK) ||
            (inflatedSize != payloadSize) ||
            (OotPsp_AssetRead(destination, sourceVrom, size) != OOT_PSP_ASSET_READ_OK) ||
            !OotPspRuntimePatch_Transform(destination, size, payload, payloadSize, permutations, permutationCount)) {
            free(payload);
            return false;
        }
        free(payload);
        cursor += compressedSize;

        for (relocationIndex = 0; relocationIndex < relocationCount; relocationIndex++) {
            u32 relocationOffset = OotPspRuntimePatch_ReadLe32(cursor);
            s32 linkAdjustment = OotPspRuntimePatch_ReadLeS32(cursor + 4);
            u32* value;

            cursor += 8;
            if ((relocationOffset > size) || (size - relocationOffset < sizeof(*value))) {
                return false;
            }
            value = (u32*)(destination + relocationOffset);
            *value += (uintptr_t)_ftext + linkAdjustment;
        }
    }
    if (cursor != end) {
        return false;
    }
    for (patchIndex = 0; patchIndex < gOotPspExternalAssetCount; patchIndex++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[patchIndex];

        if (strcmp(asset->name, "code") == 0) {
            if ((OotPsp_AssetRead((void*)njpgdspMainTextStart,
                                  asset->vromStart + OOT_PSP_CODE_NJPG_TEXT_OFFSET,
                                  OOT_PSP_CODE_NJPG_TEXT_SIZE) != OOT_PSP_ASSET_READ_OK) ||
                (OotPsp_AssetRead((void*)njpgdspMainDataStart,
                                  asset->vromStart + OOT_PSP_CODE_NJPG_DATA_OFFSET,
                                  OOT_PSP_CODE_NJPG_DATA_SIZE) != OOT_PSP_ASSET_READ_OK)) {
                return false;
            }
            break;
        }
    }
    if (patchIndex == gOotPspExternalAssetCount) {
        return false;
    }
    sceKernelDcacheWritebackInvalidateAll();
    return true;
}
