#include "oot_psp_runtime_patch.h"

#include "oot_psp_asset_loader.h"
#include "oot_psp_rom_profiles.h"

#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define OOT_PSP_PATCH_ZERO_SELECTOR   56U
#define OOT_PSP_PATCH_MAPPED_SELECTOR 57U
#define OOT_PSP_PATCH_TEXTURE_WORDS   (1U << 0)

extern const u8 gOotPspRuntimePatchBlob[] __attribute__((weak));
extern const u8 gOotPspRuntimePatchBlobEnd[] __attribute__((weak));
extern u8 _ftext[];

static u32 OotPspRuntimePatch_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
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
    const OotPspRomProfile* profile = OotPspRomProfiles_GetActive();
    const u8* cursor = gOotPspRuntimePatchBlob;
    const u8* end = gOotPspRuntimePatchBlobEnd;
    const u8* permutations;
    u32 patchCount;
    u32 permutationCount;
    u32 patchIndex;
    u32 profileCount = 0;
    size_t activeProfileIndex = OotPspRomProfiles_GetActiveIndex();
    s32 hasPatchFlags;
    s32 hasProfileSources = false;
    s32 hasDirectRelocations = false;

    if ((profile == NULL) || ((size_t)(end - cursor) < 12)) {
        return false;
    }
    if (memcmp(cursor, "OPB4", 4) == 0) {
        if ((size_t)(end - cursor) < 16) {
            return false;
        }
        hasPatchFlags = true;
        hasProfileSources = true;
        hasDirectRelocations = true;
        profileCount = OotPspRuntimePatch_ReadLe32(cursor + 12);
    } else if (memcmp(cursor, "OPB3", 4) == 0) {
        if ((size_t)(end - cursor) < 16) {
            return false;
        }
        hasPatchFlags = true;
        hasProfileSources = true;
        profileCount = OotPspRuntimePatch_ReadLe32(cursor + 12);
    } else if (memcmp(cursor, "OPB2", 4) == 0) {
        hasPatchFlags = true;
    } else if (memcmp(cursor, "OPB1", 4) == 0) {
        /* Retain compatibility with old generated blobs. They carry no range
         * metadata and therefore cannot identify patched texture words. */
        hasPatchFlags = false;
    } else {
        return false;
    }
    patchCount = OotPspRuntimePatch_ReadLe32(cursor + 4);
    permutationCount = OotPspRuntimePatch_ReadLe32(cursor + 8);
    cursor += hasProfileSources ? 16 : 12;
    if (hasProfileSources && ((profileCount != gOotPspRomProfileCount) || (activeProfileIndex >= profileCount))) {
        return false;
    }
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
        u32 patchFlags;
        uLongf inflatedSize;
        u8* destination;
        u8* payload;
        u32 relocationIndex;
        s32 inflateResult;
        s32 readResult;

        if (hasProfileSources) {
            size_t sourceTableSize = (size_t)profileCount * 4;

            if (((size_t)(end - cursor) < 24) || ((size_t)(end - cursor - 24) < sourceTableSize)) {
                return false;
            }
            destinationOffset = OotPspRuntimePatch_ReadLe32(cursor + 0);
            size = OotPspRuntimePatch_ReadLe32(cursor + 4);
            payloadSize = OotPspRuntimePatch_ReadLe32(cursor + 8);
            compressedSize = OotPspRuntimePatch_ReadLe32(cursor + 12);
            relocationCount = OotPspRuntimePatch_ReadLe32(cursor + 16);
            patchFlags = OotPspRuntimePatch_ReadLe32(cursor + 20);
            cursor += 24;
            sourceVrom = OotPspRuntimePatch_ReadLe32(cursor + activeProfileIndex * 4);
            cursor += sourceTableSize;
        } else {
            if ((size_t)(end - cursor) < (hasPatchFlags ? 28 : 24)) {
                return false;
            }
            destinationOffset = OotPspRuntimePatch_ReadLe32(cursor + 0);
            sourceVrom = OotPspRuntimePatch_ReadLe32(cursor + 4);
            size = OotPspRuntimePatch_ReadLe32(cursor + 8);
            payloadSize = OotPspRuntimePatch_ReadLe32(cursor + 12);
            compressedSize = OotPspRuntimePatch_ReadLe32(cursor + 16);
            relocationCount = OotPspRuntimePatch_ReadLe32(cursor + 20);
            patchFlags = hasPatchFlags ? OotPspRuntimePatch_ReadLe32(cursor + 24) : 0;
            cursor += hasPatchFlags ? 28 : 24;
        }
        if (((size_t)(end - cursor) < compressedSize) ||
            ((size_t)(end - cursor - compressedSize) < relocationCount * 8)) {
            return false;
        }

        destination = _ftext + destinationOffset;
        payload = malloc(payloadSize);
        if (payload == NULL) {
            printf("oot-psp runtime patch %lu allocation failed size=%lu\n",
                   (unsigned long)patchIndex, (unsigned long)payloadSize);
            return false;
        }
        inflatedSize = payloadSize;
        inflateResult = uncompress(payload, &inflatedSize, cursor, compressedSize);
        if ((inflateResult != Z_OK) || (inflatedSize != payloadSize)) {
            printf("oot-psp runtime patch %lu inflate failed result=%ld got=%lu expected=%lu\n",
                   (unsigned long)patchIndex, (long)inflateResult, (unsigned long)inflatedSize,
                   (unsigned long)payloadSize);
            free(payload);
            return false;
        }
        readResult = OotPsp_AssetRead(destination, sourceVrom, size);
        if (readResult != OOT_PSP_ASSET_READ_OK) {
            printf("oot-psp runtime patch %lu read failed status=%ld vrom=%08lx size=%lu\n",
                   (unsigned long)patchIndex, (long)readResult, (unsigned long)sourceVrom,
                   (unsigned long)size);
            free(payload);
            return false;
        }
        if (!OotPspRuntimePatch_Transform(destination, size, payload, payloadSize,
                                          permutations, permutationCount)) {
            printf("oot-psp runtime patch %lu transform failed vrom=%08lx size=%lu payload=%lu\n",
                   (unsigned long)patchIndex, (unsigned long)sourceVrom, (unsigned long)size,
                   (unsigned long)payloadSize);
            free(payload);
            return false;
        }
        free(payload);
        cursor += compressedSize;

        if ((patchFlags & OOT_PSP_PATCH_TEXTURE_WORDS) &&
            !OotPsp_MarkLoadedExternalAssetRangeFlags(
                destination, size, OOT_PSP_EXTERNAL_ASSET_NATIVE | OOT_PSP_EXTERNAL_ASSET_TEXTURE_WORDS)) {
            printf("oot-psp runtime patch %lu native texture range failed dst=%08lx size=%lu\n",
                   (unsigned long)patchIndex, (unsigned long)(uintptr_t)destination,
                   (unsigned long)size);
            return false;
        }

        for (relocationIndex = 0; relocationIndex < relocationCount; relocationIndex++) {
            u32 relocationOffset = OotPspRuntimePatch_ReadLe32(cursor);
            u32 relocationValue = OotPspRuntimePatch_ReadLe32(cursor + 4);
            u32* value;

            cursor += 8;
            if ((relocationOffset > size) || (size - relocationOffset < sizeof(*value))) {
                printf("oot-psp runtime patch %lu relocation failed offset=%lu size=%lu\n",
                       (unsigned long)patchIndex, (unsigned long)relocationOffset,
                       (unsigned long)size);
                return false;
            }
            value = (u32*)(destination + relocationOffset);
            if (hasDirectRelocations) {
                *value = (uintptr_t)_ftext + relocationValue;
            } else {
                *value += (uintptr_t)_ftext + (s32)relocationValue;
            }
        }
    }
    if (cursor != end) {
        printf("oot-psp runtime patch blob has trailing data size=%lu\n", (unsigned long)(end - cursor));
        return false;
    }
    for (patchIndex = 0; patchIndex < gOotPspExternalAssetCount; patchIndex++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[patchIndex];

        if (strcmp(asset->name, "code") == 0) {
            if ((OotPsp_AssetRead((void*)njpgdspMainTextStart,
                                  asset->vromStart + profile->njpgTextOffset,
                                  profile->njpgTextSize) != OOT_PSP_ASSET_READ_OK) ||
                (OotPsp_AssetRead((void*)njpgdspMainDataStart,
                                  asset->vromStart + profile->njpgDataOffset,
                                  profile->njpgDataSize) != OOT_PSP_ASSET_READ_OK)) {
                printf("oot-psp runtime JPEG microcode read failed profile=%s code=%08lx\n",
                       profile->name, (unsigned long)asset->vromStart);
                return false;
            }
            break;
        }
    }
    if (patchIndex == gOotPspExternalAssetCount) {
        printf("oot-psp runtime patch code asset is missing\n");
        return false;
    }
    sceKernelDcacheWritebackInvalidateAll();
    return true;
}
