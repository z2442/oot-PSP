#include "oot_psp_asset_builder.h"

#include "oot_psp_asset_identity.h"
#include "oot_psp_animation_layouts.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_rom_profiles.h"
#include "oot_psp_scene_layouts.h"
#include "oot_psp_skeleton_layouts.h"

#include <pspiofilemgr.h>
#if defined(OOT_PSP_UNPACKER_MODULE)
#include "oot_psp_unpacker_ui.h"
#else
#include "oot_psp_renderer.h"
#endif
#include <pspkernel.h>
#include <psputils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define OOT_PSP_ROM_MAX_SIZE                0x08000000U
#define OOT_PSP_AUDIO_SEGMENT_MAX_SIZE      0x01000000U
#define OOT_PSP_TRANSFORM_ZERO_SELECTOR    56U
#define OOT_PSP_TRANSFORM_MAPPED_SELECTOR  57U
#define OOT_PSP_IO_CHUNK_SIZE              0x4000U
#define OOT_PSP_IO_ZERO_RETRY_COUNT        16
#define OOT_PSP_IO_ZERO_RETRY_USEC         1000
#define OOT_PSP_NINTENDO_LOGO_V1_SIZE       0x2E50U
#define OOT_PSP_NINTENDO_LOGO_TEXTURE0_END  0x1C00U
#define OOT_PSP_NINTENDO_LOGO_V1_DLIST      0x27A0U
#define OOT_PSP_NINTENDO_LOGO_V2_DLIST      0x2720U
#define OOT_PSP_NINTENDO_LOGO_DLIST_SIZE    0x02B0U
#define OOT_PSP_NINTENDO_LOGO_TEXTURE1_SIZE 0x0400U
#define OOT_PSP_NINTENDO_LOGO_MAX_SIZE      0x00010000U

typedef struct OotPspDmaEntry {
    u32 vromStart;
    u32 vromEnd;
    u32 romStart;
    u32 romEnd;
} OotPspDmaEntry;

typedef struct OotPspRomImage {
    SceUID fd;
    u32 size;
    u8 swapMask;
    u8 digest[16];
    const OotPspRomProfile* profile;
} OotPspRomImage;

typedef struct OotPspAssetIdentity {
    const OotPspRomProfile* profile;
    u8 digest[16];
    u32 packedSize;
    u32 codeSize;
    OotPspExternalSceneAssetLayout scene[OOT_PSP_SCENE_ASSET_MAX];
    OotPspExternalAudioAssetLayout audio[OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT];
    OotPspExternalMessageAssetLayout message[OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT];
} OotPspAssetIdentity;

static const char* sOotPspAudioAssetNames[OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT] = {
    "Audiobank",
    "Audioseq",
    "Audiotable",
};

static const char* sOotPspMessageAssetNames[OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT] = {
    "jpn_message_data_static",
    "nes_message_data_static",
    "ger_message_data_static",
    "fra_message_data_static",
    "staff_message_data_static",
};

static const char* sOotPspInputRomPaths[] = {
    "data/basrom.z64",
    "data/baserom.z64",
    "data/basrom.n64",
    "data/baserom.n64",
    "data/basrom.v64",
    "data/baserom.v64",
};

extern const u8 gOotPspAssetTransformCompressed[];
extern const u8 gOotPspAssetTransformCompressedEnd[];

static u32 sOotPspAssetBuilderProgress;
static s32 sOotPspAssetBuilderErrorShown;

static s32 OotPspAssetBuilder_ActivateLayout(const OotPspAssetIdentity* identity) {
#if defined(OOT_PSP_UNPACKER_MODULE)
    (void)identity;
    return true;
#else
    OotPsp_SetExternalCodeAssetSize(identity->codeSize);
    return OotPsp_SetExternalSceneAssetLayout(identity->scene) &&
           OotPsp_SetExternalAudioAssetLayout(identity->audio) &&
           OotPsp_SetExternalMessageAssetLayout(identity->message);
#endif
}

static void OotPspAssetBuilder_ShowProgress(u32 progressPermille, const char* status) {
    sOotPspAssetBuilderProgress = progressPermille;
#if defined(OOT_PSP_UNPACKER_MODULE)
    OotPspUnpackerUI_Draw(progressPermille, status, false);
#else
    OotPspRenderer_RenderFirstBootProgress(progressPermille, status, false);
#endif
}

static void OotPspAssetBuilder_ShowError(const char* status) {
    sOotPspAssetBuilderErrorShown = true;
#if defined(OOT_PSP_UNPACKER_MODULE)
    OotPspUnpackerUI_Draw(sOotPspAssetBuilderProgress, status, true);
#else
    OotPspRenderer_RenderFirstBootProgress(sOotPspAssetBuilderProgress, status, true);
#endif
}

static u32 OotPspAssetBuilder_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u32 OotPspAssetBuilder_ReadBe32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static u16 OotPspAssetBuilder_ReadBe16(const u8* data) {
    return ((u16)data[0] << 8) | (u16)data[1];
}

static void OotPspAssetBuilder_WriteLe32(u8* data, u32 value) {
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static void OotPspAssetBuilder_WriteBe32(u8* data, u32 value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
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

static s32 OotPspAssetBuilder_ReadRomAt(const OotPspRomImage* rom, u32 offset, void* output, size_t size) {
    u8* cursor = output;
    u8 raw[OOT_PSP_IO_CHUNK_SIZE + 4];

    if ((rom == NULL) || (offset > rom->size) || (size > (size_t)(rom->size - offset))) {
        return false;
    }
    if (rom->swapMask == 0) {
        return OotPspAssetBuilder_ReadAt(rom->fd, offset, output, size);
    }

    while (size != 0) {
        size_t chunk = size > OOT_PSP_IO_CHUNK_SIZE ? OOT_PSP_IO_CHUNK_SIZE : size;
        u32 logicalStart = offset;
        u32 logicalEnd = offset + chunk;
        u32 rawStart = logicalStart & ~(u32)rom->swapMask;
        u32 rawEnd = (logicalEnd + rom->swapMask) & ~(u32)rom->swapMask;
        size_t i;

        if (!OotPspAssetBuilder_ReadAt(rom->fd, rawStart, raw, rawEnd - rawStart)) {
            return false;
        }
        for (i = 0; i < chunk; i++) {
            cursor[i] = raw[((logicalStart + i) ^ rom->swapMask) - rawStart];
        }
        cursor += chunk;
        offset += chunk;
        size -= chunk;
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

static s32 OotPspAssetBuilder_CopyUncompressedAsset(const OotPspRomImage* rom, SceUID outputFd, u32 romOffset,
                                                     u32 outputOffset, size_t size) {
    u8 buffer[OOT_PSP_IO_CHUNK_SIZE];

    while (size != 0) {
        size_t chunk = size > sizeof(buffer) ? sizeof(buffer) : size;

        if (!OotPspAssetBuilder_ReadRomAt(rom, romOffset, buffer, chunk) ||
            !OotPspAssetBuilder_WriteAt(outputFd, outputOffset, buffer, chunk)) {
            return false;
        }
        romOffset += chunk;
        outputOffset += chunk;
        size -= chunk;
    }
    return true;
}

static s32 OotPspAssetBuilder_WriteZeros(SceUID fd, u32 offset, size_t size) {
    static const u8 zero[OOT_PSP_IO_CHUNK_SIZE];

    while (size != 0) {
        size_t chunk = size > sizeof(zero) ? sizeof(zero) : size;

        if (!OotPspAssetBuilder_WriteAt(fd, offset, zero, chunk)) {
            return false;
        }
        offset += chunk;
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

static s32 OotPspAssetBuilder_LoadIdentity(const char* path, OotPspAssetIdentity* identity) {
    const OotPspExternalAsset* codeAsset;
    u8 data[OOT_PSP_ASSET_ID_SIZE];
    char profileName[33];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    s32 ok;
    size_t i;

    if (fd < 0) {
        return false;
    }
    ok = (sceIoLseek32(fd, 0, PSP_SEEK_END) == (SceOff)sizeof(data)) &&
         OotPspAssetBuilder_ReadAt(fd, 0, data, sizeof(data));
    sceIoClose(fd);
    if (!ok || (memcmp(data, OOT_PSP_ASSET_ID_MAGIC, 4) != 0) ||
        (OotPspAssetBuilder_ReadLe32(data + 4) != OOT_PSP_ASSET_ID_VERSION)) {
        return false;
    }
    memcpy(profileName, data + 32, 32);
    profileName[32] = '\0';
    identity->profile = OotPspRomProfiles_FindName(profileName);
    identity->packedSize = OotPspAssetBuilder_ReadLe32(data + 8);
    identity->codeSize = OotPspAssetBuilder_ReadLe32(data + 12);
    memcpy(identity->digest, data + 16, sizeof(identity->digest));
    for (i = 0; i < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; i++) {
        identity->audio[i].fileOffset = OotPspAssetBuilder_ReadLe32(data + 64 + (i * 8));
        identity->audio[i].size = OotPspAssetBuilder_ReadLe32(data + 68 + (i * 8));
    }
    for (i = 0; i < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; i++) {
        identity->message[i].fileOffset = OotPspAssetBuilder_ReadLe32(data + 88 + (i * 8));
        identity->message[i].size = OotPspAssetBuilder_ReadLe32(data + 92 + (i * 8));
    }
    for (i = 0; i < OOT_PSP_SCENE_ASSET_MAX; i++) {
        OotPspExternalSceneAssetLayout* scene = &identity->scene[i];
        scene->assetIndex = OotPspAssetBuilder_ReadLe32(data + 128 + i * 16);
        scene->fileOffset = OotPspAssetBuilder_ReadLe32(data + 132 + i * 16);
        scene->size = OotPspAssetBuilder_ReadLe32(data + 136 + i * 16);
        scene->sourceVromStart = OotPspAssetBuilder_ReadLe32(data + 140 + i * 16);
        if (scene->size && (scene->assetIndex >= gOotPspExternalAssetCount ||
            scene->size > OOT_PSP_SCENE_VROM_STRIDE || scene->fileOffset > identity->packedSize ||
            scene->size > identity->packedSize - scene->fileOffset)) return false;
    }
    if ((identity->profile == NULL) || (identity->codeSize == 0) || (gOotPspExternalAssetCount == 0)) {
        return false;
    }
    codeAsset = &gOotPspExternalAssets[gOotPspExternalAssetCount - 1];
    if ((strcmp(codeAsset->name, "code") != 0) || (codeAsset->fileOffset > 0xFFFFFFFFU) ||
        (identity->codeSize > 0xFFFFFFFFU - codeAsset->fileOffset) ||
        (identity->packedSize < codeAsset->fileOffset + identity->codeSize)) {
        return false;
    }
    for (i = 0; i < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; i++) {
        if ((identity->audio[i].size == 0) || (identity->audio[i].fileOffset > identity->packedSize) ||
            (identity->audio[i].size > identity->packedSize - identity->audio[i].fileOffset)) {
            return false;
        }
    }
    for (i = 0; i < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; i++) {
        if ((identity->message[i].size != 0) &&
            ((identity->message[i].fileOffset > identity->packedSize) ||
             (identity->message[i].size > identity->packedSize - identity->message[i].fileOffset))) {
            return false;
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_WriteIdentity(const char* path, const char* tempPath,
                                            const OotPspAssetIdentity* identity) {
    u8 data[OOT_PSP_ASSET_ID_SIZE];
    SceUID fd;
    s32 ok;

    memset(data, 0, sizeof(data));
    memcpy(data, OOT_PSP_ASSET_ID_MAGIC, 4);
    OotPspAssetBuilder_WriteLe32(data + 4, OOT_PSP_ASSET_ID_VERSION);
    OotPspAssetBuilder_WriteLe32(data + 8, identity->packedSize);
    OotPspAssetBuilder_WriteLe32(data + 12, identity->codeSize);
    memcpy(data + 16, identity->digest, sizeof(identity->digest));
    strncpy((char*)data + 32, identity->profile->name, 31);
    {
        size_t i;

        for (i = 0; i < OOT_PSP_SCENE_ASSET_MAX; i++) {
            OotPspAssetBuilder_WriteLe32(data + 128 + i * 16, identity->scene[i].assetIndex);
            OotPspAssetBuilder_WriteLe32(data + 132 + i * 16, identity->scene[i].fileOffset);
            OotPspAssetBuilder_WriteLe32(data + 136 + i * 16, identity->scene[i].size);
            OotPspAssetBuilder_WriteLe32(data + 140 + i * 16, identity->scene[i].sourceVromStart);
        }
        for (i = 0; i < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; i++) {
            OotPspAssetBuilder_WriteLe32(data + 64 + (i * 8), identity->audio[i].fileOffset);
            OotPspAssetBuilder_WriteLe32(data + 68 + (i * 8), identity->audio[i].size);
        }
        for (i = 0; i < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; i++) {
            OotPspAssetBuilder_WriteLe32(data + 88 + (i * 8), identity->message[i].fileOffset);
            OotPspAssetBuilder_WriteLe32(data + 92 + (i * 8), identity->message[i].size);
        }
    }

    sceIoRemove(tempPath);
    fd = sceIoOpen(tempPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return false;
    }
    ok = OotPspAssetBuilder_WriteAt(fd, 0, data, sizeof(data));
    sceIoClose(fd);
    if (!ok) {
        sceIoRemove(tempPath);
        return false;
    }
    sceIoRemove(path);
    if (sceIoRename(tempPath, path) < 0) {
        sceIoRemove(tempPath);
        return false;
    }
    return true;
}

static SceUID OotPspAssetBuilder_OpenRom(char* pathBuffer, size_t pathBufferSize) {
    size_t i;

    for (i = 0; i < sizeof(sOotPspInputRomPaths) / sizeof(sOotPspInputRomPaths[0]); i++) {
        const char* path = OotPsp_ResolveRootPath(sOotPspInputRomPaths[i], pathBuffer, pathBufferSize);
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);

        if (fd >= 0) {
            return fd;
        }
    }
    /* Retain the historical names as an explicit preference. Otherwise find
     * a ROM by extension in the installation's data directory. */
    {
        char directory[512], selected[sizeof(((SceIoDirent*)0)->d_name)] = {0};
        char relative[sizeof(selected) + 6];
        SceIoDirent entry;
        const char* root = OotPsp_ResolveRootPath("data", directory, sizeof(directory));
        SceUID dir = sceIoDopen(root);
        int read;
        if (dir < 0) return -1;
        memset(&entry, 0, sizeof(entry));
        while ((read = sceIoDread(dir, &entry)) > 0) {
            size_t length = strlen(entry.d_name);
            if (!FIO_S_ISDIR(entry.d_stat.st_mode) && length >= 4 &&
                entry.d_name[length - 4] == '.' &&
                (entry.d_name[length - 3] == 'z' || entry.d_name[length - 3] == 'Z') &&
                entry.d_name[length - 2] == '6' && entry.d_name[length - 1] == '4') {
                if (selected[0]) { sceIoDclose(dir); return -2; }
                memcpy(selected, entry.d_name, length + 1);
            }
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
        if (read < 0 || !selected[0]) return -1;
        snprintf(relative, sizeof(relative), "data/%s", selected);
        root = OotPsp_ResolveRootPath(relative, pathBuffer, pathBufferSize);
        if (root != pathBuffer || strlen(root) >= pathBufferSize) return -1;
        return sceIoOpen(root, PSP_O_RDONLY, 0);
    }
}

static s32 OotPspAssetBuilder_IdentifyRom(SceUID fd, OotPspRomImage* rom) {
    SceKernelUtilsMd5Context md5;
    SceOff fileSize;
    u8 rawMagic[4];
    u8* buffer;
    size_t processed = 0;

    memset(rom, 0, sizeof(*rom));
    rom->fd = fd;
    fileSize = sceIoLseek32(fd, 0, PSP_SEEK_END);
    if ((fileSize <= 0) || ((u32)fileSize > OOT_PSP_ROM_MAX_SIZE) ||
        !OotPspAssetBuilder_ReadAt(fd, 0, rawMagic, sizeof(rawMagic))) {
        OotPspAssetBuilder_ShowError("ROM size or header is invalid");
        return false;
    }
    rom->size = (u32)fileSize;
    if (memcmp(rawMagic, "\x80\x37\x12\x40", 4) == 0) {
        rom->swapMask = 0;
    } else if (memcmp(rawMagic, "\x37\x80\x40\x12", 4) == 0) {
        rom->swapMask = 1;
    } else if (memcmp(rawMagic, "\x40\x12\x37\x80", 4) == 0) {
        rom->swapMask = 3;
    } else {
        OotPspAssetBuilder_ShowError("ROM is not z64, v64, or n64 byte order");
        return false;
    }

    buffer = malloc(OOT_PSP_IO_CHUNK_SIZE);
    if ((buffer == NULL) || (sceKernelUtilsMd5BlockInit(&md5) < 0)) {
        free(buffer);
        OotPspAssetBuilder_ShowError("Not enough memory to identify ROM");
        return false;
    }
    while (processed < rom->size) {
        size_t remaining = rom->size - processed;
        size_t chunk = remaining > OOT_PSP_IO_CHUNK_SIZE ? OOT_PSP_IO_CHUNK_SIZE : remaining;

        if (!OotPspAssetBuilder_ReadRomAt(rom, processed, buffer, chunk) ||
            (sceKernelUtilsMd5BlockUpdate(&md5, buffer, chunk) < 0)) {
            free(buffer);
            OotPspAssetBuilder_ShowError("Could not read the ROM");
            return false;
        }
        processed += chunk;
        if (((processed & 0xFFFFF) == 0) || (processed == rom->size)) {
            char status[64];

            snprintf(status, sizeof(status), "Identifying ROM: %lu / %lu MB", (unsigned long)(processed >> 20),
                     (unsigned long)((rom->size + 0xFFFFF) >> 20));
            OotPspAssetBuilder_ShowProgress((u32)(((u64)processed * 100) / rom->size), status);
        }
    }
    free(buffer);
    if (sceKernelUtilsMd5BlockResult(&md5, rom->digest) < 0) {
        OotPspAssetBuilder_ShowError("Could not checksum the ROM");
        return false;
    }
    rom->profile = OotPspRomProfiles_FindDigest(rom->digest);
    if (rom->profile == NULL) {
        OotPspAssetBuilder_ShowError("This Ocarina of Time ROM is not supported");
        return false;
    }
    OotPspRomProfiles_SetActive(rom->profile, rom->digest);
    return true;
}

static s32 OotPspAssetBuilder_LoadDmaTable(const OotPspRomImage* rom, OotPspDmaEntry* entries) {
    const OotPspRomProfile* profile = rom->profile;
    size_t rawSize = (size_t)profile->dmadataCount * 16;
    u8* raw = malloc(rawSize);
    size_t i;
    s32 ok = false;

    if ((raw == NULL) || !OotPspAssetBuilder_ReadRomAt(rom, profile->dmadataOffset, raw, rawSize)) {
        free(raw);
        return false;
    }
    for (i = 0; i < profile->dmadataCount; i++) {
        const u8* input = &raw[i * 16];

        entries[i].vromStart = OotPspAssetBuilder_ReadBe32(input + 0);
        entries[i].vromEnd = OotPspAssetBuilder_ReadBe32(input + 4);
        entries[i].romStart = OotPspAssetBuilder_ReadBe32(input + 8);
        entries[i].romEnd = OotPspAssetBuilder_ReadBe32(input + 12);
        if ((entries[i].vromEnd < entries[i].vromStart) ||
            ((entries[i].romStart != 0xFFFFFFFFU) &&
             ((entries[i].romStart >= rom->size) ||
              ((entries[i].romEnd != 0) &&
               ((entries[i].romEnd <= entries[i].romStart) || (entries[i].romEnd > rom->size)))))) {
            goto cleanup;
        }
    }
    ok = true;

cleanup:
    free(raw);
    return ok;
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

static s32 OotPspAssetBuilder_DecompressDeflate(const u8* input, size_t inputSize, u8* output,
                                                size_t outputSize) {
    z_stream stream;
    s32 result;

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef*)input;
    stream.avail_in = inputSize;
    stream.next_out = output;
    stream.avail_out = outputSize;
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return false;
    }
    result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return (result == Z_STREAM_END) && (stream.total_out == outputSize);
}

static s32 OotPspAssetBuilder_LoadAsset(const OotPspRomImage* rom, const OotPspDmaEntry* dma, u8** output,
                                        size_t outputSize) {
    size_t storedSize = dma->romEnd != 0 ? dma->romEnd - dma->romStart : outputSize;
    u8* stored;
    u8* decoded;

    if ((dma->romStart == 0xFFFFFFFFU) || (storedSize == 0)) {
        return false;
    }
    stored = malloc(storedSize);
    if ((stored == NULL) || !OotPspAssetBuilder_ReadRomAt(rom, dma->romStart, stored, storedSize)) {
        free(stored);
        return false;
    }
    if (dma->romEnd == 0) {
        *output = stored;
        return true;
    }

    decoded = malloc(outputSize);
    if ((decoded == NULL) ||
        !(((rom->profile->flags & OOT_PSP_ROM_PROFILE_DEFLATE) != 0)
              ? OotPspAssetBuilder_DecompressDeflate(stored, storedSize, decoded, outputSize)
              : OotPspAssetBuilder_DecompressYaz0(stored, storedSize, decoded, outputSize))) {
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

static s32 OotPspAssetBuilder_AdvanceTransform(size_t size, size_t assetIndex, const u8** manifestCursor,
                                               const u8* manifestEnd) {
    const u8* cursor = *manifestCursor;
    u32 manifestIndex;
    u32 manifestSize;
    u32 compressedSize;

    if ((size_t)(manifestEnd - cursor) < 16) {
        return false;
    }
    manifestIndex = OotPspAssetBuilder_ReadLe32(cursor + 0);
    manifestSize = OotPspAssetBuilder_ReadLe32(cursor + 4);
    compressedSize = OotPspAssetBuilder_ReadLe32(cursor + 12);
    cursor += 16;
    if ((manifestIndex != assetIndex) || (manifestSize != size) ||
        ((size_t)(manifestEnd - cursor) < compressedSize)) {
        return false;
    }
    *manifestCursor = cursor + compressedSize;
    return true;
}

static s32 OotPspAssetBuilder_IsOptionalRegionalAsset(const OotPspExternalAsset* asset) {
    return (strcmp(asset->name, "kanji") == 0) || (strcmp(asset->name, "icon_item_jpn_static") == 0) ||
           (strcmp(asset->name, "icon_item_ger_static") == 0) ||
           (strcmp(asset->name, "icon_item_fra_static") == 0) ||
           (strcmp(asset->name, "jpn_message_data_static") == 0);
}

static s32 OotPspAssetBuilder_IsNintendoLogoAsset(const OotPspExternalAsset* asset) {
    return strcmp(asset->name, "nintendo_rogo_static") == 0;
}

static s32 OotPspAssetBuilder_IsSupportedNintendoLogoLayout(size_t sourceSize, size_t targetSize,
                                                            size_t sourceDlistOffset) {
    return (targetSize == OOT_PSP_NINTENDO_LOGO_V1_SIZE) &&
           ((sourceDlistOffset == OOT_PSP_NINTENDO_LOGO_V1_DLIST) ||
            (sourceDlistOffset == OOT_PSP_NINTENDO_LOGO_V2_DLIST)) &&
           (sourceSize >= sourceDlistOffset + 8) && (sourceSize <= OOT_PSP_NINTENDO_LOGO_MAX_SIZE);
}

static void OotPspAssetBuilder_ReverseBytes(u8* data, size_t size) {
    size_t left;

    for (left = 0; left < size / 2; left++) {
        u8 value = data[left];

        data[left] = data[size - left - 1];
        data[size - left - 1] = value;
    }
}

static s32 OotPspAssetBuilder_TransformNintendoLogo(const u8* source, size_t sourceSize,
                                                    size_t sourceDlistOffset, u8** output, size_t targetSize) {
    u8* data;
    size_t dlistSize = 0;
    size_t textureOffset = 0;
    size_t offset;

    if ((source == NULL) || (output == NULL) ||
        !OotPspAssetBuilder_IsSupportedNintendoLogoLayout(sourceSize, targetSize, sourceDlistOffset)) {
        return false;
    }
    while (sourceDlistOffset + dlistSize <= sourceSize - 8) {
        u32 word0 = OotPspAssetBuilder_ReadBe32(source + sourceDlistOffset + dlistSize);
        u32 word1 = OotPspAssetBuilder_ReadBe32(source + sourceDlistOffset + dlistSize + 4);

        dlistSize += 8;
        if (((word0 >> 24) == 0xFD) && ((word1 >> 24) == 1)) {
            size_t candidate = word1 & 0x00FFFFFFU;

            if ((candidate >= sourceDlistOffset) &&
                (candidate <= sourceSize - OOT_PSP_NINTENDO_LOGO_TEXTURE1_SIZE)) {
                textureOffset = candidate;
            }
        }
        if ((word0 >> 24) == 0xDF) {
            break;
        }
    }
    if ((dlistSize == 0) || (dlistSize > OOT_PSP_NINTENDO_LOGO_DLIST_SIZE) ||
        (OotPspAssetBuilder_ReadBe32(source + sourceDlistOffset + dlistSize - 8) >> 24 != 0xDF) ||
        (textureOffset == 0)) {
        return false;
    }
    data = malloc(targetSize);
    if (data == NULL) {
        return false;
    }
    memset(data, 0, targetSize);
    memcpy(data, source, sourceDlistOffset);
    memcpy(data + OOT_PSP_NINTENDO_LOGO_V1_DLIST, source + sourceDlistOffset, dlistSize);
    memcpy(data + OOT_PSP_NINTENDO_LOGO_V1_DLIST + OOT_PSP_NINTENDO_LOGO_DLIST_SIZE,
           source + textureOffset, OOT_PSP_NINTENDO_LOGO_TEXTURE1_SIZE);

    /* Relocate any segment-1 reference into the trailing texture (or its end)
     * from the selected ROM's compact layout to the canonical PSP slots. */
    for (offset = OOT_PSP_NINTENDO_LOGO_V1_DLIST;
         offset < OOT_PSP_NINTENDO_LOGO_V1_DLIST + dlistSize; offset += 8) {
        u32 word1 = OotPspAssetBuilder_ReadBe32(data + offset + 4);

        if ((word1 >> 24) == 1) {
            size_t addressOffset = word1 & 0x00FFFFFFU;

            if ((addressOffset >= textureOffset) &&
                (addressOffset <= textureOffset + OOT_PSP_NINTENDO_LOGO_TEXTURE1_SIZE)) {
                word1 = 0x01000000U +
                        (OOT_PSP_NINTENDO_LOGO_V1_DLIST + OOT_PSP_NINTENDO_LOGO_DLIST_SIZE) +
                        (addressOffset - textureOffset);
                data[offset + 4] = word1 >> 24;
                data[offset + 5] = word1 >> 16;
                data[offset + 6] = word1 >> 8;
                data[offset + 7] = word1;
            }
        }
    }

    /* Match the native little-endian representation generated from either XML
     * schema: u64 texture words, Vtx scalar fields, then two u32 Gfx words. */
    for (offset = 0; offset < OOT_PSP_NINTENDO_LOGO_TEXTURE0_END; offset += 8) {
        OotPspAssetBuilder_ReverseBytes(data + offset, 8);
    }
    for (offset = OOT_PSP_NINTENDO_LOGO_TEXTURE0_END;
         offset < OOT_PSP_NINTENDO_LOGO_V1_DLIST; offset += 16) {
        size_t fieldOffset;

        for (fieldOffset = 0; fieldOffset < 12; fieldOffset += 2) {
            OotPspAssetBuilder_ReverseBytes(data + offset + fieldOffset, 2);
        }
    }
    for (offset = OOT_PSP_NINTENDO_LOGO_V1_DLIST;
         offset < OOT_PSP_NINTENDO_LOGO_V1_DLIST + OOT_PSP_NINTENDO_LOGO_DLIST_SIZE; offset += 8) {
        OotPspAssetBuilder_ReverseBytes(data + offset, 4);
        OotPspAssetBuilder_ReverseBytes(data + offset + 4, 4);
    }
    for (offset = OOT_PSP_NINTENDO_LOGO_V1_DLIST + OOT_PSP_NINTENDO_LOGO_DLIST_SIZE;
         offset < targetSize; offset += 8) {
        OotPspAssetBuilder_ReverseBytes(data + offset, 8);
    }

    *output = data;
    return true;
}

static s32 OotPspAssetBuilder_GetAudioAssetIndex(const OotPspExternalAsset* asset) {
    size_t i;

    for (i = 0; i < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; i++) {
        if (strcmp(asset->name, sOotPspAudioAssetNames[i]) == 0) {
            return (s32)i;
        }
    }
    return -1;
}

static s32 OotPspAssetBuilder_GetMessageAssetIndex(const OotPspExternalAsset* asset) {
    size_t i;

    for (i = 0; i < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; i++) {
        if (strcmp(asset->name, sOotPspMessageAssetNames[i]) == 0) {
            return (s32)i;
        }
    }
    return -1;
}

static u32 OotPspAssetBuilder_Align16(u32 value) {
    return (value + 15U) & ~15U;
}

static const OotPspDmaEntry* OotPspAssetBuilder_GetAssetDma(const OotPspRomImage* rom,
                                                            const OotPspDmaEntry* entries, size_t assetIndex) {
    u16 dmaIndex = rom->profile->assetDmaIndices[assetIndex];

    if ((dmaIndex == OOT_PSP_ROM_PROFILE_DMA_MISSING) || (dmaIndex >= rom->profile->dmadataCount)) {
        return NULL;
    }
    return &entries[dmaIndex];
}

static const OotPspRomAssetSplice* OotPspAssetBuilder_GetAssetSplices(
    const OotPspRomProfile* profile, size_t assetIndex, size_t* count) {
    size_t low = 0;
    size_t high = profile->assetSpliceCount;
    size_t first;
    size_t end;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (profile->assetSplices[middle].assetIndex < assetIndex) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    first = low;
    end = first;
    while ((end < profile->assetSpliceCount) &&
           (profile->assetSplices[end].assetIndex == assetIndex)) {
        end++;
    }
    *count = end - first;
    return *count != 0 ? &profile->assetSplices[first] : NULL;
}

static const OotPspRomAssetSplice* OotPspAssetBuilder_FindSourceSplice(
    const OotPspRomAssetSplice* splices, size_t count, u8 segmentId, u32 sourceOffset) {
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (splices[middle].sourceOffset < sourceOffset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    while ((low < count) && (splices[low].sourceOffset == sourceOffset)) {
        if (splices[low].sourceSegmentId == segmentId) {
            return &splices[low];
        }
        low++;
    }
    return NULL;
}

static const OotPspRomAssetSplice* OotPspAssetBuilder_FindContainingSourceSplice(
    const OotPspRomAssetSplice* splices, size_t count, u8 segmentId,
    u32 sourceOffset) {
    size_t low = 0;
    size_t high = count;
    size_t checked = 0;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (splices[middle].sourceOffset <= sourceOffset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    while ((low != 0) && (checked < 32)) {
        const OotPspRomAssetSplice* splice = &splices[--low];
        size_t size = splice->copySize & OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;
        size_t delta = sourceOffset - splice->sourceOffset;

        checked++;
        if ((splice->sourceSegmentId == segmentId) && (delta < size)) {
            return splice;
        }
    }
    return NULL;
}

static s32 OotPspAssetBuilder_FindDListSize(const u8* source, size_t limit, size_t* copySize) {
    size_t offset;

    for (offset = 0; (offset + 8) <= limit; offset += 8) {
        u32 word0 = OotPspAssetBuilder_ReadBe32(source + offset);
        u32 word1 = OotPspAssetBuilder_ReadBe32(source + offset + 4);
        u8 opcode = word0 >> 24;

        if (((word0 == 0xDF000000U) || (word0 == 0xB8000000U)) && (word1 == 0)) {
            *copySize = offset + 8;
            return true;
        }
        /* A no-push display-list call is a tail branch and terminates the
         * current F3DEX/F3DEX2 list without a separate G_ENDDL. */
        if (((opcode == 0xDE) || (opcode == 0x06)) && ((word0 & 0x00FF0000U) == 0x00010000U)) {
            *copySize = offset + 8;
            return true;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_FindSceneCommandsSize(const u8* source, size_t limit,
                                                     size_t* copySize) {
    size_t offset;

    for (offset = 0; (offset + 8) <= limit; offset += 8) {
        if ((OotPspAssetBuilder_ReadBe32(source + offset) == 0x14000000U) &&
            (OotPspAssetBuilder_ReadBe32(source + offset + 4) == 0)) {
            *copySize = offset + 8;
            return true;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_FindCutsceneSize(const u8* source, size_t limit,
                                                size_t* copySize) {
    size_t wordCount = limit / sizeof(u32);
    size_t cursor = 2;
    u32 commandCount;
    u32 commandIndex;

    if (wordCount < 2) {
        return false;
    }
    commandCount = OotPspAssetBuilder_ReadBe32(source);
    if (commandCount > wordCount) {
        return false;
    }
    /* The header count excludes CS_END_OF_SCRIPT. Its first word is the
     * engine-visible terminator; the following zero is only macro padding
     * and may overlap the next canonical resource when revisions differ. */
    for (commandIndex = 0; commandIndex <= commandCount; commandIndex++) {
        u32 command;
        size_t itemWords = 12;
        u32 itemCount;

        if (cursor >= wordCount) {
            return false;
        }
        command = OotPspAssetBuilder_ReadBe32(source + cursor * sizeof(u32));
        if (command == 0xFFFFFFFFU) {
            *copySize = (cursor + 1) * sizeof(u32);
            return true;
        }
        if ((command == 1) || (command == 2) || (command == 5) || (command == 6)) {
            s32 done = false;

            if ((cursor + 3) > wordCount) {
                return false;
            }
            cursor += 3;
            while (!done) {
                u8 continueFlag;

                if ((cursor + 4) > wordCount) {
                    return false;
                }
                continueFlag = source[cursor * sizeof(u32)];
                if ((continueFlag != 0) && (continueFlag != 0xFF)) {
                    return false;
                }
                done = continueFlag == 0xFF;
                cursor += 4;
            }
            continue;
        }
        if ((command == 7) || (command == 8)) {
            if ((cursor + 7) > wordCount) {
                return false;
            }
            cursor += 7;
            continue;
        }
        if ((command == 45) || (command == 1000)) {
            if ((cursor + 4) > wordCount) {
                return false;
            }
            cursor += 4;
            continue;
        }
        if ((cursor + 2) > wordCount) {
            return false;
        }
        itemCount = OotPspAssetBuilder_ReadBe32(source + (cursor + 1) * sizeof(u32));
        if ((command == 9) || (command == 19) || (command == 140)) {
            itemWords = 3;
        }
        cursor += 2;
        if ((itemCount > (wordCount - cursor) / itemWords)) {
            return false;
        }
        cursor += itemCount * itemWords;
    }
    return false;
}

static s32 OotPspAssetBuilder_ResolveSpliceCopySize(const OotPspRomAssetSplice* splice,
                                                     const u8* source, size_t limit,
                                                     size_t* copySize) {
    switch (splice->copySize & OOT_PSP_ROM_ASSET_SPLICE_COPY_MODE_MASK) {
        case OOT_PSP_ROM_ASSET_SPLICE_COPY_EXACT:
            *copySize = limit;
            return true;
        case OOT_PSP_ROM_ASSET_SPLICE_COPY_DLIST:
            return OotPspAssetBuilder_FindDListSize(source, limit, copySize);
        case OOT_PSP_ROM_ASSET_SPLICE_COPY_SCENE_COMMANDS:
            return OotPspAssetBuilder_FindSceneCommandsSize(source, limit, copySize);
        case OOT_PSP_ROM_ASSET_SPLICE_COPY_CUTSCENE:
            return OotPspAssetBuilder_FindCutsceneSize(source, limit, copySize);
        default:
            *copySize = limit; /* fixed-size typed resource */
            return true;
    }
}

#define OOT_PSP_SCENE_LAYOUT_HEADER_COMMAND 0xFFU
#define OOT_PSP_SCENE_LAYOUT_CHILD_PAYLOAD  0U
#define OOT_PSP_SCENE_LAYOUT_CHILD_MESH_ENTRIES      6U
#define OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST        7U
#define OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS 8U
#define OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_SOURCE      9U
#define OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_TLUT       10U
#define OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_VERTICES   11U
#define OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_DATA       12U
#define OOT_PSP_SCENE_LAYOUT_GRAPH_NONE          0xFFFFU
#define OOT_PSP_SCENE_LAYOUT_COMMAND_END    0x14U
#define OOT_PSP_SCENE_LAYOUT_COMMAND_ALT    0x18U
#define OOT_PSP_SCENE_LAYOUT_COMMAND_MAX    0x1AU
#define OOT_PSP_SCENE_LAYOUT_ALT_MAX        19U

static const OotPspSceneLayoutAnchor* OotPspAssetBuilder_GetSceneLayoutAnchors(
    size_t assetIndex, size_t* count) {
    size_t low = 0;
    size_t high = OOT_PSP_SCENE_LAYOUT_ANCHOR_COUNT;
    size_t first;
    size_t end;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (sOotPspSceneLayoutAnchors[middle].assetIndex < assetIndex) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    first = low;
    end = first;
    while ((end < OOT_PSP_SCENE_LAYOUT_ANCHOR_COUNT) &&
           (sOotPspSceneLayoutAnchors[end].assetIndex == assetIndex)) {
        end++;
    }
    *count = end - first;
    return *count != 0 ? &sOotPspSceneLayoutAnchors[first] : NULL;
}

#define OOT_PSP_ANIMATION_LAYOUT_HEADER        0U
#define OOT_PSP_ANIMATION_LAYOUT_FRAME_DATA    1U
#define OOT_PSP_ANIMATION_LAYOUT_JOINT_INDICES 2U
#define OOT_PSP_ANIMATION_HEADER_SIZE          16U

static const OotPspAnimationLayoutAnchor* OotPspAssetBuilder_GetAnimationLayoutAnchors(
    size_t assetIndex, size_t* count) {
    size_t low = 0;
    size_t high = OOT_PSP_ANIMATION_LAYOUT_ANCHOR_COUNT;
    size_t first;
    size_t end;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (sOotPspAnimationLayoutAnchors[middle].assetIndex < assetIndex) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    first = low;
    end = first;
    while ((end < OOT_PSP_ANIMATION_LAYOUT_ANCHOR_COUNT) &&
           (sOotPspAnimationLayoutAnchors[end].assetIndex == assetIndex)) {
        end++;
    }
    *count = end - first;
    return *count != 0 ? &sOotPspAnimationLayoutAnchors[first] : NULL;
}

static s32 OotPspAssetBuilder_ValidateSourceAnimationHeader(
    const u8* source, size_t sourceSize, const OotPspAnimationLayoutAnchor* header,
    size_t headerOffset, size_t* frameOffset, size_t* jointOffset) {
    u32 framePointer;
    u32 jointPointer;
    size_t frameValueCount;
    size_t jointCount;
    size_t jointIndex;
    u16 frameCount;
    u16 staticIndexMax;

    if ((headerOffset > sourceSize) ||
        (OOT_PSP_ANIMATION_HEADER_SIZE > sourceSize - headerOffset)) {
        return false;
    }
    /* PAL animations may have been resampled for 50 Hz. Counts belong to the
     * source header, not the canonical layout used to locate linked symbols. */
    frameCount = OotPspAssetBuilder_ReadBe16(source + headerOffset);
    staticIndexMax = OotPspAssetBuilder_ReadBe16(source + headerOffset + 0x0C);
    if ((frameCount == 0) || (frameCount > 0x7FFF)) {
        return false;
    }
    framePointer = OotPspAssetBuilder_ReadBe32(source + headerOffset + 4);
    jointPointer = OotPspAssetBuilder_ReadBe32(source + headerOffset + 8);
    *frameOffset = framePointer & 0x00FFFFFFU;
    *jointOffset = jointPointer & 0x00FFFFFFU;
    if (((framePointer >> 24) != header->segmentId) ||
        ((jointPointer >> 24) != header->segmentId) ||
        !(*frameOffset < *jointOffset && *jointOffset < headerOffset) ||
        (((*jointOffset - *frameOffset) & 1U) != 0) ||
        ((headerOffset - *jointOffset) < 6)) {
        return false;
    }
    frameValueCount = (*jointOffset - *frameOffset) / sizeof(u16);
    if (staticIndexMax > frameValueCount) {
        return false;
    }
    jointCount = (headerOffset - *jointOffset) / 6;
    for (jointIndex = 0; jointIndex < jointCount; jointIndex++) {
        size_t component;

        for (component = 0; component < 3; component++) {
            u16 index = OotPspAssetBuilder_ReadBe16(
                source + *jointOffset + jointIndex * 6 + component * sizeof(u16));
            size_t end = index < staticIndexMax
                             ? (size_t)index + 1
                             : (size_t)index + frameCount;

            if (end > frameValueCount) {
                return false;
            }
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_FindSourceAnimationHeader(
    const u8* source, size_t sourceSize, const OotPspAnimationLayoutAnchor* header,
    size_t expectedOffset, size_t* headerOffset, size_t* frameOffset,
    size_t* jointOffset) {
    size_t distance;
    size_t candidate;
    size_t bestOffset = 0;
    size_t bestFrame = 0;
    size_t bestJoint = 0;
    size_t bestDistance = (size_t)-1;
    s32 found = false;

    expectedOffset &= ~(size_t)3;
    if (OotPspAssetBuilder_ValidateSourceAnimationHeader(
            source, sourceSize, header, expectedOffset, frameOffset, jointOffset)) {
        *headerOffset = expectedOffset;
        return true;
    }
    for (distance = 4; distance <= 0x4000; distance += 4) {
        if ((expectedOffset >= distance) &&
            OotPspAssetBuilder_ValidateSourceAnimationHeader(
                source, sourceSize, header, expectedOffset - distance,
                frameOffset, jointOffset)) {
            *headerOffset = expectedOffset - distance;
            return true;
        }
        if ((expectedOffset <= sourceSize) && (distance <= sourceSize - expectedOffset) &&
            OotPspAssetBuilder_ValidateSourceAnimationHeader(
                source, sourceSize, header, expectedOffset + distance,
                frameOffset, jointOffset)) {
            *headerOffset = expectedOffset + distance;
            return true;
        }
    }
    for (candidate = 0;
         (candidate <= sourceSize) &&
         (OOT_PSP_ANIMATION_HEADER_SIZE <= sourceSize - candidate);
         candidate += 4) {
        size_t candidateFrame;
        size_t candidateJoint;
        size_t candidateDistance = candidate > expectedOffset
                                       ? candidate - expectedOffset
                                       : expectedOffset - candidate;

        if ((candidateDistance < bestDistance) &&
            OotPspAssetBuilder_ValidateSourceAnimationHeader(
                source, sourceSize, header, candidate, &candidateFrame,
                &candidateJoint)) {
            bestOffset = candidate;
            bestFrame = candidateFrame;
            bestJoint = candidateJoint;
            bestDistance = candidateDistance;
            found = true;
        }
    }
    if (found) {
        *headerOffset = bestOffset;
        *frameOffset = bestFrame;
        *jointOffset = bestJoint;
    }
    return found;
}

static s32 OotPspAssetBuilder_ResolveAnimationLayoutAnchor(
    const OotPspAnimationLayoutAnchor* anchor, size_t sourceHeaderOffset,
    size_t sourceFrameOffset, size_t sourceJointOffset, size_t* sourceOffset,
    size_t* sourceAnchorSize) {
    switch (anchor->kind) {
        case OOT_PSP_ANIMATION_LAYOUT_HEADER:
            *sourceOffset = sourceHeaderOffset;
            *sourceAnchorSize = OOT_PSP_ANIMATION_HEADER_SIZE;
            return true;
        case OOT_PSP_ANIMATION_LAYOUT_FRAME_DATA:
            *sourceOffset = sourceFrameOffset;
            *sourceAnchorSize = sourceJointOffset - sourceFrameOffset;
            return *sourceAnchorSize != 0;
        case OOT_PSP_ANIMATION_LAYOUT_JOINT_INDICES:
            *sourceOffset = sourceJointOffset;
            *sourceAnchorSize = sourceHeaderOffset - sourceJointOffset;
            return *sourceAnchorSize != 0;
        default:
            return false;
    }
}

#define OOT_PSP_SKELETON_LAYOUT_HEADER 0U
#define OOT_PSP_SKELETON_LAYOUT_TABLE  1U
#define OOT_PSP_SKELETON_LAYOUT_LIMB   2U
#define OOT_PSP_SKELETON_LAYOUT_DLIST  3U
#define OOT_PSP_SKELETON_LAYOUT_SKIN_DATA       4U
#define OOT_PSP_SKELETON_LAYOUT_SKIN_MODIFS     5U
#define OOT_PSP_SKELETON_LAYOUT_SKIN_VERTICES   6U
#define OOT_PSP_SKELETON_LAYOUT_SKIN_TRANSFORMS 7U
#define OOT_PSP_SKELETON_LIMB_STANDARD 0U
#define OOT_PSP_SKELETON_LIMB_LOD      1U
#define OOT_PSP_SKELETON_LIMB_SKIN     2U
#define OOT_PSP_SKELETON_LIMB_CURVE    3U
#define OOT_PSP_SKIN_LIMB_ANIMATED      4U
#define OOT_PSP_SKIN_LIMB_NORMAL       11U
#define OOT_PSP_SKIN_DATA_SIZE         12U
#define OOT_PSP_SKIN_MODIF_SIZE        16U
#define OOT_PSP_SKIN_VERTEX_SIZE       10U
#define OOT_PSP_SKIN_TRANSFORM_SIZE    10U

static const OotPspSkeletonLayoutAnchor* OotPspAssetBuilder_GetSkeletonLayoutAnchors(
    size_t assetIndex, size_t* count) {
    size_t low = 0;
    size_t high = OOT_PSP_SKELETON_LAYOUT_ANCHOR_COUNT;
    size_t first;
    size_t end;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (sOotPspSkeletonLayoutAnchors[middle].assetIndex < assetIndex) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    first = low;
    end = first;
    while ((end < OOT_PSP_SKELETON_LAYOUT_ANCHOR_COUNT) &&
           (sOotPspSkeletonLayoutAnchors[end].assetIndex == assetIndex)) {
        end++;
    }
    *count = end - first;
    return *count != 0 ? &sOotPspSkeletonLayoutAnchors[first] : NULL;
}

static s32 OotPspAssetBuilder_GetSkeletonLimbLayout(
    u8 limbType, size_t* limbSize, size_t* childField, size_t* siblingField) {
    switch (limbType) {
        case OOT_PSP_SKELETON_LIMB_STANDARD:
            *limbSize = 12;
            *childField = 6;
            *siblingField = 7;
            return true;
        case OOT_PSP_SKELETON_LIMB_LOD:
        case OOT_PSP_SKELETON_LIMB_SKIN:
            *limbSize = 16;
            *childField = 6;
            *siblingField = 7;
            return true;
        case OOT_PSP_SKELETON_LIMB_CURVE:
            *limbSize = 12;
            *childField = 0;
            *siblingField = 1;
            return true;
        default:
            return false;
    }
}

static s32 OotPspAssetBuilder_ValidateSourceSkinLimb(
    const u8* source, size_t sourceSize, u8 segmentId, size_t limbOffset) {
    u32 segmentType = OotPspAssetBuilder_ReadBe32(source + limbOffset + 8);
    u32 segmentPointer = OotPspAssetBuilder_ReadBe32(source + limbOffset + 12);
    size_t segmentOffset = segmentPointer & 0x00FFFFFFU;
    size_t dListSize;

    if (segmentPointer == 0) {
        return segmentType != OOT_PSP_SKIN_LIMB_ANIMATED &&
               segmentType != OOT_PSP_SKIN_LIMB_NORMAL;
    }
    if ((segmentPointer >> 24) != segmentId) {
        return false;
    }
    if (segmentType == OOT_PSP_SKIN_LIMB_NORMAL) {
        return (segmentOffset < sourceSize) &&
               OotPspAssetBuilder_FindDListSize(
                   source + segmentOffset, sourceSize - segmentOffset, &dListSize);
    }
    if (segmentType == OOT_PSP_SKIN_LIMB_ANIMATED) {
        u16 modifCount;
        u32 modifsPointer;
        u32 dListPointer;
        size_t modifsOffset;
        size_t modifIndex;

        if ((segmentOffset > sourceSize) ||
            (OOT_PSP_SKIN_DATA_SIZE > sourceSize - segmentOffset)) {
            return false;
        }
        modifCount = OotPspAssetBuilder_ReadBe16(source + segmentOffset + 2);
        modifsPointer = OotPspAssetBuilder_ReadBe32(source + segmentOffset + 4);
        dListPointer = OotPspAssetBuilder_ReadBe32(source + segmentOffset + 8);
        modifsOffset = modifsPointer & 0x00FFFFFFU;
        if ((dListPointer == 0) || ((dListPointer >> 24) != segmentId) ||
            ((dListPointer & 0x00FFFFFFU) >= sourceSize) ||
            !OotPspAssetBuilder_FindDListSize(
                source + (dListPointer & 0x00FFFFFFU),
                sourceSize - (dListPointer & 0x00FFFFFFU), &dListSize)) {
            return false;
        }
        if (modifCount == 0) {
            return true;
        }
        if (((modifsPointer >> 24) != segmentId) || (modifsOffset > sourceSize) ||
            ((size_t)modifCount * OOT_PSP_SKIN_MODIF_SIZE > sourceSize - modifsOffset)) {
            return false;
        }
        for (modifIndex = 0; modifIndex < modifCount; modifIndex++) {
            size_t modifOffset = modifsOffset + modifIndex * OOT_PSP_SKIN_MODIF_SIZE;
            u16 vertexCount = OotPspAssetBuilder_ReadBe16(source + modifOffset);
            u16 transformCount = OotPspAssetBuilder_ReadBe16(source + modifOffset + 2);
            u32 verticesPointer = OotPspAssetBuilder_ReadBe32(source + modifOffset + 8);
            u32 transformsPointer = OotPspAssetBuilder_ReadBe32(source + modifOffset + 12);
            size_t verticesOffset = verticesPointer & 0x00FFFFFFU;
            size_t transformsOffset = transformsPointer & 0x00FFFFFFU;

            if ((vertexCount != 0) &&
                (((verticesPointer >> 24) != segmentId) || (verticesOffset > sourceSize) ||
                 ((size_t)vertexCount * OOT_PSP_SKIN_VERTEX_SIZE > sourceSize - verticesOffset))) {
                return false;
            }
            if ((transformCount != 0) &&
                (((transformsPointer >> 24) != segmentId) || (transformsOffset > sourceSize) ||
                 ((size_t)transformCount * OOT_PSP_SKIN_TRANSFORM_SIZE >
                  sourceSize - transformsOffset))) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static s32 OotPspAssetBuilder_ValidateSourceSkeletonHeader(
    const u8* source, size_t sourceSize, const OotPspSkeletonLayoutAnchor* header,
    size_t headerOffset, size_t* tableOffset) {
    size_t limbSize;
    size_t childField;
    size_t siblingField;
    size_t tableSize = (size_t)header->limbCount * sizeof(u32);
    u32 tablePointer;
    size_t i;

    if ((headerOffset > sourceSize) || (header->targetSize > sourceSize - headerOffset) ||
        !OotPspAssetBuilder_GetSkeletonLimbLayout(
            header->limbType, &limbSize, &childField, &siblingField) ||
        (source[headerOffset + 4] != header->limbCount) ||
        ((header->targetSize == 12) &&
         (source[headerOffset + 8] != header->dListCount))) {
        return false;
    }
    tablePointer = OotPspAssetBuilder_ReadBe32(source + headerOffset);
    *tableOffset = tablePointer & 0x00FFFFFFU;
    if (((tablePointer >> 24) != header->segmentId) ||
        (*tableOffset > sourceSize) || (tableSize > sourceSize - *tableOffset)) {
        return false;
    }
    for (i = 0; i < header->limbCount; i++) {
        u32 limbPointer = OotPspAssetBuilder_ReadBe32(
            source + *tableOffset + i * sizeof(u32));
        size_t limbOffset = limbPointer & 0x00FFFFFFU;
        u8 child;
        u8 sibling;

        if (((limbPointer >> 24) != header->segmentId) ||
            (limbOffset > sourceSize) || (limbSize > sourceSize - limbOffset)) {
            return false;
        }
        child = source[limbOffset + childField];
        sibling = source[limbOffset + siblingField];
        if (((child != 0xFF) && (child >= header->limbCount)) ||
            ((sibling != 0xFF) && (sibling >= header->limbCount))) {
            return false;
        }
        if ((header->limbType == OOT_PSP_SKELETON_LIMB_SKIN) &&
            !OotPspAssetBuilder_ValidateSourceSkinLimb(
                source, sourceSize, header->segmentId, limbOffset)) {
            return false;
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_FindSourceSkeletonHeader(
    const u8* source, size_t sourceSize, const OotPspSkeletonLayoutAnchor* header,
    size_t expectedOffset, size_t* headerOffset, size_t* tableOffset) {
    size_t distance;
    size_t candidate;
    size_t bestOffset = 0;
    size_t bestTable = 0;
    size_t bestDistance = (size_t)-1;
    s32 found = false;

    expectedOffset &= ~(size_t)3;
    if (OotPspAssetBuilder_ValidateSourceSkeletonHeader(
            source, sourceSize, header, expectedOffset, tableOffset)) {
        *headerOffset = expectedOffset;
        return true;
    }
    for (distance = 4; distance <= 0x4000; distance += 4) {
        if ((expectedOffset >= distance) &&
            OotPspAssetBuilder_ValidateSourceSkeletonHeader(
                source, sourceSize, header, expectedOffset - distance, tableOffset)) {
            *headerOffset = expectedOffset - distance;
            return true;
        }
        if ((expectedOffset <= sourceSize) && (distance <= sourceSize - expectedOffset) &&
            OotPspAssetBuilder_ValidateSourceSkeletonHeader(
                source, sourceSize, header, expectedOffset + distance, tableOffset)) {
            *headerOffset = expectedOffset + distance;
            return true;
        }
    }
    for (candidate = 0; candidate < sourceSize; candidate += 4) {
        size_t candidateTable;
        size_t candidateDistance = candidate > expectedOffset
                                       ? candidate - expectedOffset
                                       : expectedOffset - candidate;

        if ((candidateDistance >= bestDistance) ||
            !OotPspAssetBuilder_ValidateSourceSkeletonHeader(
                source, sourceSize, header, candidate, &candidateTable)) {
            continue;
        }
        bestOffset = candidate;
        bestTable = candidateTable;
        bestDistance = candidateDistance;
        found = true;
    }
    if (found) {
        *headerOffset = bestOffset;
        *tableOffset = bestTable;
    }
    return found;
}

static s32 OotPspAssetBuilder_ResolveSkeletonLayoutAnchor(
    const OotPspSkeletonLayoutAnchor* anchor, const u8* source, size_t sourceSize,
    size_t sourceHeaderOffset, size_t sourceTableOffset, size_t* sourceOffset,
    size_t* sourceAnchorSize) {
    size_t limbSize;
    size_t childField;
    size_t siblingField;
    u32 pointer;
    size_t limbOffset;

    if (!OotPspAssetBuilder_GetSkeletonLimbLayout(
            anchor->limbType, &limbSize, &childField, &siblingField)) {
        return false;
    }
    switch (anchor->kind) {
        case OOT_PSP_SKELETON_LAYOUT_HEADER:
            *sourceOffset = sourceHeaderOffset;
            *sourceAnchorSize = anchor->targetSize;
            return true;
        case OOT_PSP_SKELETON_LAYOUT_TABLE:
            *sourceOffset = sourceTableOffset;
            *sourceAnchorSize = (size_t)anchor->limbCount * sizeof(u32);
            return true;
        case OOT_PSP_SKELETON_LAYOUT_LIMB:
        case OOT_PSP_SKELETON_LAYOUT_DLIST:
        case OOT_PSP_SKELETON_LAYOUT_SKIN_DATA:
        case OOT_PSP_SKELETON_LAYOUT_SKIN_MODIFS:
        case OOT_PSP_SKELETON_LAYOUT_SKIN_VERTICES:
        case OOT_PSP_SKELETON_LAYOUT_SKIN_TRANSFORMS:
            if (anchor->limbIndex >= anchor->limbCount) {
                return false;
            }
            pointer = OotPspAssetBuilder_ReadBe32(
                source + sourceTableOffset + (size_t)anchor->limbIndex * sizeof(u32));
            limbOffset = pointer & 0x00FFFFFFU;
            if (((pointer >> 24) != anchor->segmentId) ||
                (limbOffset > sourceSize) || (limbSize > sourceSize - limbOffset)) {
                return false;
            }
            if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_LIMB) {
                *sourceOffset = limbOffset;
                *sourceAnchorSize = limbSize;
                return true;
            }
            if (anchor->limbType == OOT_PSP_SKELETON_LIMB_SKIN) {
                u32 segmentType = OotPspAssetBuilder_ReadBe32(source + limbOffset + 8);
                u32 skinPointer = OotPspAssetBuilder_ReadBe32(source + limbOffset + 12);
                size_t skinOffset = skinPointer & 0x00FFFFFFU;

                if ((skinPointer == 0) || ((skinPointer >> 24) != anchor->segmentId)) {
                    return false;
                }
                if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_DLIST) {
                    if (segmentType == OOT_PSP_SKIN_LIMB_NORMAL) {
                        pointer = skinPointer;
                    } else if (segmentType == OOT_PSP_SKIN_LIMB_ANIMATED) {
                        if ((skinOffset > sourceSize) ||
                            (OOT_PSP_SKIN_DATA_SIZE > sourceSize - skinOffset)) {
                            return false;
                        }
                        pointer = OotPspAssetBuilder_ReadBe32(source + skinOffset + 8);
                    } else {
                        return false;
                    }
                } else {
                    u16 modifCount;
                    u32 modifsPointer;
                    size_t modifsOffset;
                    size_t modifOffset;
                    u16 count;

                    if ((segmentType != OOT_PSP_SKIN_LIMB_ANIMATED) ||
                        (skinOffset > sourceSize) ||
                        (OOT_PSP_SKIN_DATA_SIZE > sourceSize - skinOffset)) {
                        return false;
                    }
                    if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_SKIN_DATA) {
                        *sourceOffset = skinOffset;
                        *sourceAnchorSize = OOT_PSP_SKIN_DATA_SIZE;
                        return true;
                    }
                    modifCount = OotPspAssetBuilder_ReadBe16(source + skinOffset + 2);
                    modifsPointer = OotPspAssetBuilder_ReadBe32(source + skinOffset + 4);
                    modifsOffset = modifsPointer & 0x00FFFFFFU;
                    if (((modifsPointer >> 24) != anchor->segmentId) ||
                        (modifsOffset > sourceSize) ||
                        ((size_t)modifCount * OOT_PSP_SKIN_MODIF_SIZE >
                         sourceSize - modifsOffset)) {
                        return false;
                    }
                    if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_SKIN_MODIFS) {
                        *sourceOffset = modifsOffset;
                        *sourceAnchorSize = (size_t)modifCount * OOT_PSP_SKIN_MODIF_SIZE;
                        return *sourceAnchorSize != 0;
                    }
                    if (anchor->childIndex >= modifCount) {
                        return false;
                    }
                    modifOffset = modifsOffset +
                                   (size_t)anchor->childIndex * OOT_PSP_SKIN_MODIF_SIZE;
                    if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_SKIN_VERTICES) {
                        count = OotPspAssetBuilder_ReadBe16(source + modifOffset);
                        pointer = OotPspAssetBuilder_ReadBe32(source + modifOffset + 8);
                        *sourceAnchorSize = (size_t)count * OOT_PSP_SKIN_VERTEX_SIZE;
                    } else if (anchor->kind == OOT_PSP_SKELETON_LAYOUT_SKIN_TRANSFORMS) {
                        count = OotPspAssetBuilder_ReadBe16(source + modifOffset + 2);
                        pointer = OotPspAssetBuilder_ReadBe32(source + modifOffset + 12);
                        *sourceAnchorSize = (size_t)count * OOT_PSP_SKIN_TRANSFORM_SIZE;
                    } else {
                        return false;
                    }
                    *sourceOffset = pointer & 0x00FFFFFFU;
                    return (*sourceAnchorSize != 0) &&
                           ((pointer >> 24) == anchor->segmentId) &&
                           (*sourceOffset <= sourceSize) &&
                           (*sourceAnchorSize <= sourceSize - *sourceOffset);
                }
            } else if (anchor->kind != OOT_PSP_SKELETON_LAYOUT_DLIST) {
                return false;
            } else if (anchor->limbType == OOT_PSP_SKELETON_LIMB_STANDARD) {
                pointer = OotPspAssetBuilder_ReadBe32(source + limbOffset + 8);
            } else if (anchor->limbType == OOT_PSP_SKELETON_LIMB_LOD) {
                if (anchor->childIndex >= 2) {
                    return false;
                }
                pointer = OotPspAssetBuilder_ReadBe32(
                    source + limbOffset + 8 + (size_t)anchor->childIndex * sizeof(u32));
            } else if (anchor->limbType == OOT_PSP_SKELETON_LIMB_CURVE) {
                if (anchor->childIndex >= 2) {
                    return false;
                }
                pointer = OotPspAssetBuilder_ReadBe32(
                    source + limbOffset + 4 + (size_t)anchor->childIndex * sizeof(u32));
            } else {
                return false;
            }
            *sourceOffset = pointer & 0x00FFFFFFU;
            if (((pointer >> 24) != anchor->segmentId) || (*sourceOffset >= sourceSize) ||
                !OotPspAssetBuilder_FindDListSize(
                    source + *sourceOffset, sourceSize - *sourceOffset, sourceAnchorSize)) {
                return false;
            }
            return true;
        default:
            return false;
    }
}

static s32 OotPspAssetBuilder_FindSourceSceneHeader(
    const u8* source, size_t sourceSize, u8 segmentId, u8 headerIndex,
    size_t* headerOffset, size_t* headerSize) {
    size_t primarySize;
    size_t alternateOffset = 0;
    size_t alternateEnd = sourceSize;
    size_t commandOffset;
    s32 foundAlternate = false;

    if (!OotPspAssetBuilder_FindSceneCommandsSize(source, sourceSize, &primarySize)) {
        return false;
    }
    if (headerIndex == 0) {
        *headerOffset = 0;
        *headerSize = primarySize;
        return true;
    }
    if (headerIndex > OOT_PSP_SCENE_LAYOUT_ALT_MAX) {
        return false;
    }
    for (commandOffset = 0; commandOffset < primarySize; commandOffset += 8) {
        u8 commandId = source[commandOffset];
        u32 pointer = OotPspAssetBuilder_ReadBe32(source + commandOffset + 4);

        if ((pointer >> 24) != segmentId) {
            continue;
        }
        if (commandId == OOT_PSP_SCENE_LAYOUT_COMMAND_ALT) {
            alternateOffset = pointer & 0x00FFFFFFU;
            foundAlternate = true;
        }
    }
    if (!foundAlternate || (alternateOffset >= sourceSize)) {
        return false;
    }
    for (commandOffset = 0; commandOffset < primarySize; commandOffset += 8) {
        u8 commandId = source[commandOffset];
        u32 pointer = OotPspAssetBuilder_ReadBe32(source + commandOffset + 4);
        size_t pointerOffset = pointer & 0x00FFFFFFU;

        if ((commandId != OOT_PSP_SCENE_LAYOUT_COMMAND_ALT) &&
            ((pointer >> 24) == segmentId) && (pointerOffset > alternateOffset) &&
            (pointerOffset < alternateEnd)) {
            alternateEnd = pointerOffset;
        }
    }
    {
        size_t entryOffset = alternateOffset + ((size_t)headerIndex - 1) * sizeof(u32);
        u32 pointer;
        size_t offset;
        size_t size;

        if ((entryOffset > alternateEnd) || (sizeof(u32) > alternateEnd - entryOffset)) {
            return false;
        }
        pointer = OotPspAssetBuilder_ReadBe32(source + entryOffset);
        offset = pointer & 0x00FFFFFFU;
        if (((pointer >> 24) != segmentId) || (offset >= sourceSize) ||
            !OotPspAssetBuilder_FindSceneCommandsSize(
                source + offset, sourceSize - offset, &size)) {
            return false;
        }
        *headerOffset = offset;
        *headerSize = size;
        return true;
    }
}

static s32 OotPspAssetBuilder_FindSourceSceneCommand(
    const u8* source, size_t headerOffset, size_t headerSize, u8 commandId,
    u8 occurrence, const u8** command) {
    size_t offset;
    u8 seen = 0;

    for (offset = 0; offset < headerSize; offset += 8) {
        const u8* candidate = source + headerOffset + offset;

        if (candidate[0] == commandId) {
            if (seen == occurrence) {
                *command = candidate;
                return true;
            }
            seen++;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_GetSourceScenePayloadSize(
    const u8* source, size_t sourceSize, const u8* command, size_t payloadOffset,
    size_t* payloadSize) {
    u8 commandId = command[0];
    size_t count = command[1];

    switch (commandId) {
        case 0x00:
        case 0x01:
        case 0x0E:
            *payloadSize = count * 0x10;
            return *payloadSize != 0;
        case 0x02:
            *payloadSize = 8;
            return true;
        case 0x03:
            *payloadSize = 0x2C;
            return true;
        case 0x04:
            *payloadSize = count * 8;
            return *payloadSize != 0;
        case 0x06:
        case 0x13:
            *payloadSize = 2;
            return true;
        case 0x0A:
            if ((payloadOffset > sourceSize) || (2 > sourceSize - payloadOffset)) {
                return false;
            }
            if ((source[payloadOffset] == 0) || (source[payloadOffset] == 2)) {
                *payloadSize = 0x0C;
            } else if ((source[payloadOffset] == 1) &&
                       (source[payloadOffset + 1] == 1)) {
                *payloadSize = 0x20;
            } else if ((source[payloadOffset] == 1) &&
                       (source[payloadOffset + 1] == 2)) {
                *payloadSize = 0x10;
            } else {
                return false;
            }
            return *payloadSize <= sourceSize - payloadOffset;
        case 0x0B:
            *payloadSize = count * 2;
            return *payloadSize != 0;
        case 0x0C:
            *payloadSize = count * 0x0E;
            return *payloadSize != 0;
        case 0x0D:
            *payloadSize = 8;
            return true;
        case 0x0F:
            *payloadSize = count * 0x16;
            return *payloadSize != 0;
        case 0x17:
            return (payloadOffset < sourceSize) &&
                   OotPspAssetBuilder_FindCutsceneSize(
                       source + payloadOffset, sourceSize - payloadOffset, payloadSize);
        case OOT_PSP_SCENE_LAYOUT_COMMAND_ALT:
            *payloadSize = 4;
            return true;
        default:
            return false;
    }
}

static s32 OotPspAssetBuilder_ResolveSceneLayoutAnchor(
    const OotPspSceneLayoutAnchor* anchor, const u8* source, size_t sourceSize,
    u8 segmentId, const size_t* sourceDListOffsets, size_t sourceDListCount,
    size_t* sourceOffset, size_t* sourceAnchorSize) {
    size_t headerOffset;
    size_t headerSize;
    const u8* command;
    u32 pointer;
    size_t payloadOffset;
    size_t payloadSize;

    if (!OotPspAssetBuilder_FindSourceSceneHeader(
            source, sourceSize, segmentId, anchor->headerIndex, &headerOffset, &headerSize)) {
        return false;
    }
    if (anchor->commandId == OOT_PSP_SCENE_LAYOUT_HEADER_COMMAND) {
        *sourceOffset = headerOffset;
        *sourceAnchorSize = headerSize;
        return true;
    }
    if (!OotPspAssetBuilder_FindSourceSceneCommand(
            source, headerOffset, headerSize, anchor->commandId, anchor->occurrence, &command)) {
        return false;
    }
    pointer = OotPspAssetBuilder_ReadBe32(command + 4);
    payloadOffset = pointer & 0x00FFFFFFU;
    if (((pointer >> 24) != segmentId) || (payloadOffset >= sourceSize) ||
        !OotPspAssetBuilder_GetSourceScenePayloadSize(
            source, sourceSize, command, payloadOffset, &payloadSize)) {
        return false;
    }
    if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_PAYLOAD) {
        *sourceOffset = payloadOffset;
        *sourceAnchorSize = payloadSize;
        return true;
    }
    if ((command[0] == 0x0A) && (payloadSize >= 8)) {
        u8 shapeType = source[payloadOffset];
        u8 amountType = source[payloadOffset + 1];
        size_t entryCount = shapeType == 1 ? 1 : amountType;
        size_t entrySize = shapeType == 2 ? 0x10 : 8;
        u32 entryPointer = OotPspAssetBuilder_ReadBe32(source + payloadOffset + 4);
        size_t entryOffset = entryPointer & 0x00FFFFFFU;

        if ((shapeType > 2) || ((entryPointer >> 24) != segmentId) ||
            (entryCount == 0) || (entryOffset > sourceSize) ||
            (entryCount * entrySize > sourceSize - entryOffset)) {
            return false;
        }
        if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_ENTRIES) {
            *sourceOffset = entryOffset;
            *sourceAnchorSize = entryCount * entrySize;
            return true;
        }
        if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) {
            size_t field;
            size_t dListOffset;

            if (anchor->parentGraphIndex != OOT_PSP_SCENE_LAYOUT_GRAPH_NONE) {
                size_t parentOffset;
                size_t commandOffset;

                if ((anchor->parentGraphIndex >= sourceDListCount) ||
                    (sourceDListOffsets[anchor->parentGraphIndex] == (size_t)-1)) {
                    return false;
                }
                parentOffset = sourceDListOffsets[anchor->parentGraphIndex];
                commandOffset = parentOffset + (size_t)anchor->commandIndex * sizeof(u64);
                if ((commandOffset > sourceSize) ||
                    (sizeof(u64) > sourceSize - commandOffset) ||
                    ((source[commandOffset] != 0xDE) &&
                     (source[commandOffset] != 0x06))) {
                    return false;
                }
                pointer = OotPspAssetBuilder_ReadBe32(source + commandOffset + 4);
            } else {
                if ((anchor->childIndex >= entryCount) || (anchor->childSlot >= 2)) {
                    return false;
                }
                field = shapeType == 2 ? 8 : 0;
                pointer = OotPspAssetBuilder_ReadBe32(
                    source + entryOffset + (size_t)anchor->childIndex * entrySize +
                    field + (size_t)anchor->childSlot * sizeof(u32));
            }
            dListOffset = pointer & 0x00FFFFFFU;
            if ((pointer == 0) || ((pointer >> 24) != segmentId) ||
                (dListOffset >= sourceSize) ||
                !OotPspAssetBuilder_FindDListSize(
                    source + dListOffset, sourceSize - dListOffset, sourceAnchorSize)) {
                return false;
            }
            *sourceOffset = dListOffset;
            return true;
        }
        if ((anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_VERTICES) ||
            (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_DATA)) {
            size_t parentOffset;
            size_t commandOffset;
            u32 word0;
            u8 opcode;

            if ((anchor->parentGraphIndex >= sourceDListCount) ||
                (sourceDListOffsets[anchor->parentGraphIndex] == (size_t)-1)) {
                return false;
            }
            parentOffset = sourceDListOffsets[anchor->parentGraphIndex];
            commandOffset = parentOffset + (size_t)anchor->commandIndex * sizeof(u64);
            if ((commandOffset > sourceSize) ||
                (sizeof(u64) > sourceSize - commandOffset)) {
                return false;
            }
            word0 = OotPspAssetBuilder_ReadBe32(source + commandOffset);
            opcode = word0 >> 24;
            if (opcode != anchor->childSlot) {
                return false;
            }
            pointer = OotPspAssetBuilder_ReadBe32(source + commandOffset + 4);
            *sourceOffset = pointer & 0x00FFFFFFU;
            if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_VERTICES) {
                if (opcode == 0x01) {
                    *sourceAnchorSize = (size_t)((word0 >> 12) & 0xFFU) * 16;
                } else if (opcode == 0x04) {
                    *sourceAnchorSize = (size_t)((word0 >> 10) & 0x3FU) * 16;
                } else {
                    return false;
                }
            } else if (opcode == 0xDA) {
                *sourceAnchorSize = 0x40;
            } else if ((opcode == 0xDC) || (opcode == 0xFD)) {
                *sourceAnchorSize = 1;
            } else {
                return false;
            }
            return (*sourceAnchorSize != 0) && ((pointer >> 24) == segmentId) &&
                   (*sourceOffset <= sourceSize) &&
                   (*sourceAnchorSize <= sourceSize - *sourceOffset);
        }
        if ((shapeType == 1) &&
            (anchor->child >= OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS) &&
            (anchor->child <= OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_TLUT)) {
            size_t backgroundCount;
            size_t backgroundsOffset;
            size_t backgroundOffset;
            size_t fieldBase;

            if (amountType == 1) {
                backgroundCount = 1;
                backgroundsOffset = payloadOffset;
            } else if ((amountType == 2) && (payloadSize >= 0x10)) {
                u32 backgroundsPointer =
                    OotPspAssetBuilder_ReadBe32(source + payloadOffset + 0x0C);

                backgroundCount = source[payloadOffset + 8];
                backgroundsOffset = backgroundsPointer & 0x00FFFFFFU;
                if (((backgroundsPointer >> 24) != segmentId) ||
                    (backgroundCount == 0) || (backgroundsOffset > sourceSize) ||
                    (backgroundCount * 0x1C > sourceSize - backgroundsOffset)) {
                    return false;
                }
                if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS) {
                    *sourceOffset = backgroundsOffset;
                    *sourceAnchorSize = backgroundCount * 0x1C;
                    return true;
                }
            } else {
                return false;
            }
            if ((anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS) ||
                (anchor->childIndex >= backgroundCount)) {
                return false;
            }
            backgroundOffset = amountType == 1
                                   ? backgroundsOffset
                                   : backgroundsOffset +
                                         (size_t)anchor->childIndex * 0x1C;
            fieldBase = amountType == 1 ? 8 : 4;
            if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_IMAGE_SOURCE) {
                u16 width = OotPspAssetBuilder_ReadBe16(
                    source + backgroundOffset + fieldBase + 0x0C);
                u16 height = OotPspAssetBuilder_ReadBe16(
                    source + backgroundOffset + fieldBase + 0x0E);
                u8 size = source[backgroundOffset + fieldBase + 0x11];

                pointer = OotPspAssetBuilder_ReadBe32(
                    source + backgroundOffset + fieldBase);
                *sourceAnchorSize = size <= 3
                                        ? (((size_t)width * height * (4U << size)) + 7) / 8
                                        : 0;
            } else {
                u16 count = OotPspAssetBuilder_ReadBe16(
                    source + backgroundOffset + fieldBase + 0x14);

                pointer = OotPspAssetBuilder_ReadBe32(
                    source + backgroundOffset + fieldBase + 8);
                *sourceAnchorSize = (size_t)count * sizeof(u16);
            }
            *sourceOffset = pointer & 0x00FFFFFFU;
            return (*sourceAnchorSize != 0) && ((pointer >> 24) == segmentId) &&
                   (*sourceOffset <= sourceSize) &&
                   (*sourceAnchorSize <= sourceSize - *sourceOffset);
        }
    }
    if ((command[0] == 0x03) && (payloadSize >= 0x2C) &&
        (payloadOffset <= sourceSize) && (0x2C <= sourceSize - payloadOffset)) {
        static const u8 pointerFields[] = { 0, 0x10, 0x18, 0x1C, 0x20, 0x28 };
        static const u8 countFields[] = { 0, 0x0C, 0x14, 0, 0, 0x24 };
        static const u8 elementSizes[] = { 0, 6, 0x10, 8, 8, 0x10 };
        u8 child = anchor->child;
        u32 childPointer;
        size_t childSize;

        if (child >= sizeof(pointerFields)) {
            return false;
        }
        childPointer = OotPspAssetBuilder_ReadBe32(
            source + payloadOffset + pointerFields[child]);
        if ((childPointer >> 24) != segmentId) {
            return false;
        }
        childSize = elementSizes[child];
        if (countFields[child] != 0) {
            childSize *= OotPspAssetBuilder_ReadBe16(
                source + payloadOffset + countFields[child]);
        }
        if (childSize == 0) {
            return false;
        }
        *sourceOffset = childPointer & 0x00FFFFFFU;
        *sourceAnchorSize = childSize;
        return (*sourceOffset < sourceSize) && (childSize <= sourceSize - *sourceOffset);
    }
    return false;
}

static void OotPspAssetBuilder_SortSplicesBySource(
    OotPspRomAssetSplice* splices, size_t count) {
    size_t i;

    for (i = 1; i < count; i++) {
        OotPspRomAssetSplice value = splices[i];
        size_t cursor = i;

        while ((cursor != 0) &&
               ((splices[cursor - 1].sourceOffset > value.sourceOffset) ||
                ((splices[cursor - 1].sourceOffset == value.sourceOffset) &&
                 (splices[cursor - 1].targetOffset > value.targetOffset)))) {
            splices[cursor] = splices[cursor - 1];
            cursor--;
        }
        splices[cursor] = value;
    }
}

typedef struct OotPspAssetBuilderTransformSpan {
    size_t targetOffset;
    size_t sourceOffset;
    size_t size;
    size_t sourceOrder;
} OotPspAssetBuilderTransformSpan;

static void OotPspAssetBuilder_SortTransformSpansByTarget(
    OotPspAssetBuilderTransformSpan* spans, size_t count) {
    size_t i;

    for (i = 1; i < count; i++) {
        OotPspAssetBuilderTransformSpan value = spans[i];
        size_t cursor = i;

        while ((cursor != 0) && (spans[cursor - 1].targetOffset > value.targetOffset)) {
            spans[cursor] = spans[cursor - 1];
            cursor--;
        }
        spans[cursor] = value;
    }
}

static void OotPspAssetBuilder_SortTransformSpansBySource(
    OotPspAssetBuilderTransformSpan* spans, size_t count) {
    size_t i;

    for (i = 1; i < count; i++) {
        OotPspAssetBuilderTransformSpan value = spans[i];
        size_t cursor = i;

        while ((cursor != 0) && (spans[cursor - 1].sourceOffset > value.sourceOffset)) {
            spans[cursor] = spans[cursor - 1];
            cursor--;
        }
        spans[cursor] = value;
    }
}

static const OotPspAssetBuilderTransformSpan* OotPspAssetBuilder_FindTransformSpan(
    const OotPspAssetBuilderTransformSpan* spans, size_t count, size_t offset) {
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (spans[middle].targetOffset <= offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low != 0) {
        const OotPspAssetBuilderTransformSpan* span = &spans[low - 1];

        if ((offset - span->targetOffset) < span->size) {
            return span;
        }
    }
    return NULL;
}

static size_t OotPspAssetBuilder_MapTransformOffset(
    const OotPspAssetBuilderTransformSpan* resources, size_t resourceCount,
    const OotPspAssetBuilderTransformSpan* gaps, size_t gapCount, size_t offset) {
    const OotPspAssetBuilderTransformSpan* span =
        OotPspAssetBuilder_FindTransformSpan(resources, resourceCount, offset);

    if (span == NULL) {
        span = OotPspAssetBuilder_FindTransformSpan(gaps, gapCount, offset);
    }
    return span != NULL ? span->sourceOffset + offset - span->targetOffset : offset;
}

static s32 OotPspAssetBuilder_TransformAdaptedAsset(
    const u8* source, size_t sourceSize, u8** output, size_t targetSize, size_t assetIndex,
    const u8** manifestCursor, const u8* manifestEnd, const u8* permutations,
    size_t permutationCount, const OotPspRomAssetSplice* splices, size_t spliceCount,
    const size_t* copySizes) {
    const u8* cursor = *manifestCursor;
    u32 manifestIndex;
    u32 manifestSize;
    u32 payloadSize;
    u32 compressedSize;
    uLongf inflatedSize;
    u8* payload = NULL;
    const u8* selectors;
    const u8* mappings;
    const u8* mappingCursor;
    const u8* mappingEnd;
    OotPspAssetBuilderTransformSpan* resources = NULL;
    OotPspAssetBuilderTransformSpan* gaps = NULL;
    OotPspAssetBuilderTransformSpan* gapDestinations = NULL;
    size_t gapCount = 0;
    size_t selectorCount = (targetSize + 7) / 8;
    size_t blockIndex;
    u8* target = NULL;
    s32 ok = false;

    if ((size_t)(manifestEnd - cursor) < 16) {
        return false;
    }
    manifestIndex = OotPspAssetBuilder_ReadLe32(cursor + 0);
    manifestSize = OotPspAssetBuilder_ReadLe32(cursor + 4);
    payloadSize = OotPspAssetBuilder_ReadLe32(cursor + 8);
    compressedSize = OotPspAssetBuilder_ReadLe32(cursor + 12);
    cursor += 16;
    if ((manifestIndex != assetIndex) || (manifestSize != targetSize) ||
        (payloadSize < selectorCount) || ((size_t)(manifestEnd - cursor) < compressedSize)) {
        return false;
    }
    payload = malloc(payloadSize);
    resources = malloc((spliceCount != 0 ? spliceCount : 1) * sizeof(*resources));
    gaps = malloc((spliceCount + 1) * sizeof(*gaps));
    gapDestinations = malloc((spliceCount + 1) * sizeof(*gapDestinations));
    target = calloc(1, targetSize);
    if ((payload == NULL) || (resources == NULL) || (gaps == NULL) ||
        (gapDestinations == NULL) || (target == NULL)) {
        goto cleanup;
    }
    inflatedSize = payloadSize;
    if ((uncompress(payload, &inflatedSize, cursor, compressedSize) != Z_OK) ||
        (inflatedSize != payloadSize)) {
        goto cleanup;
    }
    cursor += compressedSize;
    selectors = payload;
    mappings = selectors + selectorCount;
    mappingCursor = mappings;
    mappingEnd = payload + payloadSize;

    memcpy(target, source, sourceSize < targetSize ? sourceSize : targetSize);
    for (blockIndex = 0; blockIndex < spliceCount; blockIndex++) {
        resources[blockIndex].targetOffset = splices[blockIndex].targetOffset;
        resources[blockIndex].sourceOffset = splices[blockIndex].sourceOffset;
        resources[blockIndex].size = copySizes[blockIndex];
        resources[blockIndex].sourceOrder = blockIndex;
    }
    OotPspAssetBuilder_SortTransformSpansByTarget(resources, spliceCount);

    /* XML anchors describe the named resources. When two anchors are adjacent
     * in both layouts, their intervening unnamed data is the same semantic
     * span shifted by the revision's insertion/removal delta. Align that span
     * to the following anchor: regional padding and revision-only records are
     * normally inserted immediately after the preceding named resource, while
     * recursively extracted scene headers and their lists precede the next
     * resource. Right alignment keeps those native pointers and recipes in
     * lockstep even when the unnamed gap itself changed size. */
    if ((spliceCount != 0) && (resources[0].sourceOrder == 0)) {
        size_t size = resources[0].targetOffset < resources[0].sourceOffset
                          ? resources[0].targetOffset
                          : resources[0].sourceOffset;

        if (size != 0) {
            gaps[gapCount].targetOffset = 0;
            gaps[gapCount].sourceOffset = 0;
            gaps[gapCount].size = size;
            gaps[gapCount].sourceOrder = 0;
            gapCount++;
        }
    }
    for (blockIndex = 0; (blockIndex + 1) < spliceCount; blockIndex++) {
        const OotPspAssetBuilderTransformSpan* left = &resources[blockIndex];
        const OotPspAssetBuilderTransformSpan* right = &resources[blockIndex + 1];
        size_t targetOffset;
        size_t sourceOffset;
        size_t targetAvailable;
        size_t sourceAvailable;
        size_t size;

        if (right->sourceOrder != left->sourceOrder + 1) {
            continue;
        }
        targetOffset = left->targetOffset + left->size;
        sourceOffset = left->sourceOffset + left->size;
        if ((targetOffset > right->targetOffset) || (sourceOffset > right->sourceOffset)) {
            continue;
        }
        targetAvailable = right->targetOffset - targetOffset;
        sourceAvailable = right->sourceOffset - sourceOffset;
        size = targetAvailable < sourceAvailable ? targetAvailable : sourceAvailable;
        if (size != 0) {
            gaps[gapCount].targetOffset = right->targetOffset - size;
            gaps[gapCount].sourceOffset = right->sourceOffset - size;
            gaps[gapCount].size = size;
            gaps[gapCount].sourceOrder = 0;
            gapCount++;
        }
    }
    if ((spliceCount != 0) && (resources[spliceCount - 1].sourceOrder == spliceCount - 1)) {
        const OotPspAssetBuilderTransformSpan* last = &resources[spliceCount - 1];
        size_t targetOffset = last->targetOffset + last->size;
        size_t sourceOffset = last->sourceOffset + last->size;

        if ((targetOffset <= targetSize) && (sourceOffset <= sourceSize)) {
            size_t targetAvailable = targetSize - targetOffset;
            size_t sourceAvailable = sourceSize - sourceOffset;
            size_t size = targetAvailable < sourceAvailable ? targetAvailable : sourceAvailable;

            if (size != 0) {
                gaps[gapCount].targetOffset = targetOffset;
                gaps[gapCount].sourceOffset = sourceOffset;
                gaps[gapCount].size = size;
                gaps[gapCount].sourceOrder = 0;
                gapCount++;
            }
        }
    }
    OotPspAssetBuilder_SortTransformSpansByTarget(gaps, gapCount);
    memcpy(gapDestinations, gaps, gapCount * sizeof(*gaps));
    OotPspAssetBuilder_SortTransformSpansBySource(gapDestinations, gapCount);
    for (blockIndex = 0; blockIndex < gapCount; blockIndex++) {
        size_t swap = gapDestinations[blockIndex].targetOffset;

        gapDestinations[blockIndex].targetOffset = gapDestinations[blockIndex].sourceOffset;
        gapDestinations[blockIndex].sourceOffset = swap;
    }

    for (blockIndex = 0; blockIndex < selectorCount; blockIndex++) {
        size_t offset = blockIndex * 8;
        size_t blockSize = (targetSize - offset) < 8 ? targetSize - offset : 8;
        u8 selector = selectors[blockIndex];
        const u8* permutation = selector < permutationCount ? permutations + selector * 8 : NULL;
        size_t i;

        if ((selector < permutationCount) && (blockSize != 8)) {
            goto cleanup;
        }
        if ((selector >= permutationCount) &&
            (selector != OOT_PSP_TRANSFORM_ZERO_SELECTOR) &&
            (selector != OOT_PSP_TRANSFORM_MAPPED_SELECTOR)) {
            goto cleanup;
        }
        for (i = 0; i < blockSize; i++) {
            size_t canonicalOffset = offset + i;
            const OotPspAssetBuilderTransformSpan* resource =
                OotPspAssetBuilder_FindTransformSpan(resources, spliceCount, canonicalOffset);
            const OotPspAssetBuilderTransformSpan* gap = resource == NULL
                ? OotPspAssetBuilder_FindTransformSpan(gaps, gapCount, canonicalOffset)
                : NULL;
            size_t destinationOffset = canonicalOffset;
            size_t sourceOffset = 0;
            u8 delta = 0;
            u8 value;
            s32 relocated = (resource != NULL) || (gap != NULL);
            s32 write = true;

            if (selector == OOT_PSP_TRANSFORM_MAPPED_SELECTOR) {
                if ((size_t)(mappingEnd - mappingCursor) < 5) {
                    goto cleanup;
                }
                sourceOffset = OotPspAssetBuilder_ReadLe32(mappingCursor);
                delta = mappingCursor[4];
                mappingCursor += 5;
            } else if (selector < permutationCount) {
                sourceOffset = offset + permutation[i];
            }

            if (resource != NULL) {
                destinationOffset = canonicalOffset;
            } else if (gap != NULL) {
                destinationOffset = gap->sourceOffset + canonicalOffset - gap->targetOffset;
                if (OotPspAssetBuilder_FindTransformSpan(resources, spliceCount, destinationOffset) != NULL) {
                    write = false;
                }
            } else if (OotPspAssetBuilder_FindTransformSpan(
                           gapDestinations, gapCount, destinationOffset) != NULL) {
                write = false;
            }
            if (!write || (destinationOffset >= targetSize)) {
                continue;
            }
            if (selector == OOT_PSP_TRANSFORM_ZERO_SELECTOR) {
                value = 0;
            } else {
                if (relocated) {
                    sourceOffset = OotPspAssetBuilder_MapTransformOffset(
                        resources, spliceCount, gaps, gapCount, sourceOffset);
                }
                if (sourceOffset >= sourceSize) {
                    if (relocated) {
                        goto cleanup;
                    }
                    continue;
                }
                value = source[sourceOffset] + delta;
            }
            target[destinationOffset] = value;
        }
    }
    if (mappingCursor != mappingEnd) {
        goto cleanup;
    }
    *manifestCursor = cursor;
    *output = target;
    target = NULL;
    ok = true;

cleanup:
    free(target);
    free(gapDestinations);
    free(gaps);
    free(resources);
    free(payload);
    return ok;
}

#include "oot_psp_asset_reloc.inc.c"

static s32 OotPspAssetBuilder_AdaptAssetLayout(const OotPspRomProfile* profile, size_t assetIndex,
                                                u8* source, size_t sourceSize, u8** output,
                                                size_t targetSize, const u8** manifestCursor,
                                                const u8* manifestEnd, const u8* permutations,
                                                size_t permutationCount, s32 transform) {
    const OotPspRomAssetSplice* profileSplices;
    const OotPspRomAssetSplice* splices;
    const OotPspAnimationLayoutAnchor* animationAnchors;
    const OotPspSceneLayoutAnchor* sceneAnchors;
    const OotPspSkeletonLayoutAnchor* skeletonAnchors;
    OotPspRomAssetSplice* adaptedSplices = NULL;
    u8* pointerFields = calloc((sourceSize + 3) / 4, 1);
    size_t* sourceDListOffsets = NULL;
    size_t profileSpliceCount;
    size_t animationAnchorCount;
    size_t sceneAnchorCount;
    size_t skeletonAnchorCount;
    size_t spliceCount;
    size_t* copySizes = NULL;
    u8* target = NULL;
    size_t i;
    s32 ok = false;

    if (pointerFields == NULL) return false;
    profileSplices = OotPspAssetBuilder_GetAssetSplices(
        profile, assetIndex, &profileSpliceCount);
    animationAnchors = OotPspAssetBuilder_GetAnimationLayoutAnchors(
        assetIndex, &animationAnchorCount);
    sceneAnchors = OotPspAssetBuilder_GetSceneLayoutAnchors(
        assetIndex, &sceneAnchorCount);
    skeletonAnchors = OotPspAssetBuilder_GetSkeletonLayoutAnchors(
        assetIndex, &skeletonAnchorCount);
    splices = profileSplices;
    spliceCount = profileSpliceCount;
    for (i = 0; i < profileSpliceCount; i++) {
        OotPspAssetBuilder_MarkResourcePointers(source, sourceSize, splices[i].sourceOffset,
            splices[i].copySize & OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK,
            splices[i].copySize >> 28, pointerFields, splices[i].sourceSegmentId);
    }
    if ((animationAnchorCount != 0) || (sceneAnchorCount != 0) ||
        (skeletonAnchorCount != 0)) {
        size_t capacity = profileSpliceCount + animationAnchorCount +
                          sceneAnchorCount + skeletonAnchorCount;

        adaptedSplices = malloc(capacity * sizeof(*adaptedSplices));
        if (adaptedSplices == NULL) {
            goto cleanup;
        }
        if (sceneAnchorCount != 0) {
            sourceDListOffsets = malloc(sceneAnchorCount * sizeof(*sourceDListOffsets));
            if (sourceDListOffsets == NULL) {
                goto cleanup;
            }
            for (i = 0; i < sceneAnchorCount; i++) {
                sourceDListOffsets[i] = (size_t)-1;
            }
        }
        if (profileSpliceCount != 0) {
            memcpy(adaptedSplices, profileSplices,
                   profileSpliceCount * sizeof(*adaptedSplices));
        }
        if (animationAnchorCount != 0) {
            u16 activeAnimation = 0xFFFF;
            size_t sourceHeaderOffset = 0;
            size_t sourceFrameOffset = 0;
            size_t sourceJointOffset = 0;
            s32 sourceAnimationFound = false;

            for (i = 0; i < animationAnchorCount; i++) {
                const OotPspAnimationLayoutAnchor* anchor = &animationAnchors[i];
                size_t sourceOffset;
                size_t sourceAnchorSize;
                size_t copySize;
                size_t existing;

                if ((anchor->animationIndex != activeAnimation) ||
                    (anchor->kind == OOT_PSP_ANIMATION_LAYOUT_HEADER)) {
                    size_t expectedOffset = anchor->targetOffset;

                    activeAnimation = anchor->animationIndex;
                    for (existing = 0; existing < spliceCount; existing++) {
                        if (adaptedSplices[existing].targetOffset == anchor->targetOffset) {
                            expectedOffset = adaptedSplices[existing].sourceOffset;
                            break;
                        }
                    }
                    sourceAnimationFound = OotPspAssetBuilder_FindSourceAnimationHeader(
                        source, sourceSize, anchor, expectedOffset, &sourceHeaderOffset,
                        &sourceFrameOffset, &sourceJointOffset);
                }
                if (!sourceAnimationFound ||
                    !OotPspAssetBuilder_ResolveAnimationLayoutAnchor(
                        anchor, sourceHeaderOffset, sourceFrameOffset, sourceJointOffset,
                        &sourceOffset, &sourceAnchorSize)) {
                    continue;
                }
                if (anchor->kind == OOT_PSP_ANIMATION_LAYOUT_HEADER) {
                    OotPspAssetBuilder_MarkResourcePointers(source, sourceSize, sourceOffset,
                        sourceAnchorSize, 4, pointerFields, anchor->segmentId);
                }
                copySize = anchor->targetSize < sourceAnchorSize
                               ? anchor->targetSize
                               : sourceAnchorSize;
                if ((copySize == 0) ||
                    (copySize > OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK) ||
                    (anchor->targetOffset >= targetSize) || (sourceOffset >= sourceSize)) {
                    continue;
                }
                for (existing = 0; existing < spliceCount; existing++) {
                    size_t existingSize = adaptedSplices[existing].copySize &
                                          OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;

                    if (adaptedSplices[existing].targetOffset == anchor->targetOffset) {
                        adaptedSplices[existing].assetIndex = assetIndex;
                        adaptedSplices[existing].sourceSegmentId = anchor->segmentId;
                        adaptedSplices[existing].targetSegmentId = anchor->segmentId;
                        adaptedSplices[existing].sourceOffset = sourceOffset;
                        if (existingSize < copySize) {
                            adaptedSplices[existing].copySize = copySize;
                        }
                        break;
                    }
                }
                if (existing == spliceCount) {
                    adaptedSplices[spliceCount].assetIndex = assetIndex;
                    adaptedSplices[spliceCount].sourceSegmentId = anchor->segmentId;
                    adaptedSplices[spliceCount].targetSegmentId = anchor->segmentId;
                    adaptedSplices[spliceCount].targetOffset = anchor->targetOffset;
                    adaptedSplices[spliceCount].sourceOffset = sourceOffset;
                    adaptedSplices[spliceCount].copySize = copySize;
                    spliceCount++;
                }
            }
        }
        for (i = 0; i < sceneAnchorCount; i++) {
            const OotPspSceneLayoutAnchor* anchor = &sceneAnchors[i];
            size_t sourceOffset;
            size_t sourceAnchorSize;
            size_t copySize;
            size_t existing;

            if (!OotPspAssetBuilder_ResolveSceneLayoutAnchor(
                    anchor, source, sourceSize, anchor->segmentId,
                    sourceDListOffsets, sceneAnchorCount,
                    &sourceOffset, &sourceAnchorSize)) {
                continue;
            }
            if ((anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) &&
                (anchor->graphIndex < sceneAnchorCount)) {
                sourceDListOffsets[anchor->graphIndex] = sourceOffset;
            }
            if (anchor->commandId == OOT_PSP_SCENE_LAYOUT_HEADER_COMMAND) {
                OotPspAssetBuilder_MarkScenePointers(source, sourceSize, sourceOffset, sourceAnchorSize, pointerFields);
            } else if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) {
                OotPspAssetBuilder_MarkDListPointers(source, sourceSize, sourceOffset, pointerFields, 0, anchor->segmentId);
            }
            copySize = anchor->targetSize < sourceAnchorSize
                           ? anchor->targetSize
                           : sourceAnchorSize;
            if ((copySize == 0) ||
                (copySize > OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK) ||
                (anchor->targetOffset >= targetSize) || (sourceOffset >= sourceSize)) {
                continue;
            }
            for (existing = 0; existing < spliceCount; existing++) {
                size_t existingSize = adaptedSplices[existing].copySize &
                                      OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;
                size_t targetDelta = anchor->targetOffset -
                                     adaptedSplices[existing].targetOffset;
                size_t sourceDelta = sourceOffset -
                                     adaptedSplices[existing].sourceOffset;

                if (adaptedSplices[existing].targetOffset == anchor->targetOffset) {
                    adaptedSplices[existing].assetIndex = assetIndex;
                    adaptedSplices[existing].sourceSegmentId = anchor->segmentId;
                    adaptedSplices[existing].targetSegmentId = anchor->segmentId;
                    adaptedSplices[existing].sourceOffset = sourceOffset;
                    if (existingSize < copySize) {
                        adaptedSplices[existing].copySize = copySize;
                    }
                    break;
                }
                if ((anchor->child >= OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) &&
                    (anchor->targetOffset > adaptedSplices[existing].targetOffset) &&
                    (sourceOffset > adaptedSplices[existing].sourceOffset) &&
                    (targetDelta == sourceDelta) && (targetDelta < existingSize) &&
                    (copySize <= existingSize - targetDelta)) {
                    break;
                }
            }
            if (existing == spliceCount) {
                adaptedSplices[spliceCount].assetIndex = assetIndex;
                adaptedSplices[spliceCount].sourceSegmentId = anchor->segmentId;
                adaptedSplices[spliceCount].targetSegmentId = anchor->segmentId;
                adaptedSplices[spliceCount].targetOffset = anchor->targetOffset;
                adaptedSplices[spliceCount].sourceOffset = sourceOffset;
                adaptedSplices[spliceCount].copySize = copySize;
                spliceCount++;
            }
        }
        if (skeletonAnchorCount != 0) {
            u8 activeSkeleton = 0xFF;
            size_t sourceHeaderOffset = 0;
            size_t sourceTableOffset = 0;
            s32 sourceSkeletonFound = false;

            for (i = 0; i < skeletonAnchorCount; i++) {
                const OotPspSkeletonLayoutAnchor* anchor = &skeletonAnchors[i];
                size_t sourceOffset;
                size_t sourceAnchorSize;
                size_t copySize;
                size_t existing;

                if ((anchor->skeletonIndex != activeSkeleton) ||
                    (anchor->kind == OOT_PSP_SKELETON_LAYOUT_HEADER)) {
                    size_t expectedOffset = anchor->targetOffset;

                    activeSkeleton = anchor->skeletonIndex;
                    for (existing = 0; existing < spliceCount; existing++) {
                        if (adaptedSplices[existing].targetOffset == anchor->targetOffset) {
                            expectedOffset = adaptedSplices[existing].sourceOffset;
                            break;
                        }
                    }
                    sourceSkeletonFound = OotPspAssetBuilder_FindSourceSkeletonHeader(
                        source, sourceSize, anchor, expectedOffset,
                        &sourceHeaderOffset, &sourceTableOffset);
                }
                if (!sourceSkeletonFound ||
                    !OotPspAssetBuilder_ResolveSkeletonLayoutAnchor(
                        anchor, source, sourceSize, sourceHeaderOffset,
                        sourceTableOffset, &sourceOffset, &sourceAnchorSize)) {
                    continue;
                }
                {
                    unsigned kind = 0;
                    switch (anchor->kind) {
                        case OOT_PSP_SKELETON_LAYOUT_HEADER: kind = 5; break;
                        case OOT_PSP_SKELETON_LAYOUT_TABLE: kind = 6; break;
                        case OOT_PSP_SKELETON_LAYOUT_DLIST: kind = 1; break;
                        case OOT_PSP_SKELETON_LAYOUT_LIMB: kind = 7 + anchor->limbType; break;
                        case OOT_PSP_SKELETON_LAYOUT_SKIN_DATA: kind = 10; break;
                        case OOT_PSP_SKELETON_LAYOUT_SKIN_MODIFS: {
                            size_t field;
                            for (field = 0; field + 16 <= sourceAnchorSize; field += 16) {
                                OotPspAssetBuilder_MarkResourcePointers(source, sourceSize,
                                    sourceOffset + field, 16, 8, pointerFields, anchor->segmentId);
                            }
                            break;
                        }
                    }
                    OotPspAssetBuilder_MarkResourcePointers(source, sourceSize, sourceOffset,
                        sourceAnchorSize, kind, pointerFields, anchor->segmentId);
                }
                copySize = anchor->targetSize < sourceAnchorSize
                               ? anchor->targetSize
                               : sourceAnchorSize;
                if ((copySize == 0) ||
                    (copySize > OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK) ||
                    (anchor->targetOffset >= targetSize) || (sourceOffset >= sourceSize)) {
                    continue;
                }
                for (existing = 0; existing < spliceCount; existing++) {
                    size_t existingSize = adaptedSplices[existing].copySize &
                                          OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;

                    if (adaptedSplices[existing].targetOffset == anchor->targetOffset) {
                        adaptedSplices[existing].assetIndex = assetIndex;
                        adaptedSplices[existing].sourceSegmentId = anchor->segmentId;
                        adaptedSplices[existing].targetSegmentId = anchor->segmentId;
                        adaptedSplices[existing].sourceOffset = sourceOffset;
                        if (existingSize < copySize) {
                            adaptedSplices[existing].copySize = copySize;
                        }
                        break;
                    }
                }
                if (existing == spliceCount) {
                    adaptedSplices[spliceCount].assetIndex = assetIndex;
                    adaptedSplices[spliceCount].sourceSegmentId = anchor->segmentId;
                    adaptedSplices[spliceCount].targetSegmentId = anchor->segmentId;
                    adaptedSplices[spliceCount].targetOffset = anchor->targetOffset;
                    adaptedSplices[spliceCount].sourceOffset = sourceOffset;
                    adaptedSplices[spliceCount].copySize = copySize;
                    spliceCount++;
                }
            }
        }
        OotPspAssetBuilder_SortSplicesBySource(adaptedSplices, spliceCount);
        splices = adaptedSplices;
    }
    copySizes = malloc((spliceCount != 0 ? spliceCount : 1) * sizeof(*copySizes));
    if (copySizes == NULL) {
        goto cleanup;
    }
    for (i = 0; i < spliceCount; i++) {
        size_t copySize = splices[i].copySize & OOT_PSP_ROM_ASSET_SPLICE_COPY_SIZE_MASK;

        if ((splices[i].targetOffset >= targetSize) || (splices[i].sourceOffset >= sourceSize)) {
            goto cleanup;
        }
        if (copySize > targetSize - splices[i].targetOffset) {
            copySize = targetSize - splices[i].targetOffset;
        }
        if (copySize > sourceSize - splices[i].sourceOffset) {
            copySize = sourceSize - splices[i].sourceOffset;
        }
        if (!OotPspAssetBuilder_ResolveSpliceCopySize(
                &splices[i], source + splices[i].sourceOffset, copySize, &copySize)) {
            goto cleanup;
        }
        copySizes[i] = copySize;
    }

    /* Relocate only pointer fields discovered from typed resource roots.
     * Animation indices, vertices and pixels are never guessed to be pointers. */
    for (i = 0; (i + sizeof(u32)) <= sourceSize; i += sizeof(u32)) {
        u32 value = OotPspAssetBuilder_ReadBe32(source + i);
        u8 segmentId = value >> 24;
        const OotPspRomAssetSplice* splice;

        if ((segmentId == 0) || !(pointerFields[i / 4] & 1)) {
            continue;
        }
        splice = OotPspAssetBuilder_FindSourceSplice(splices, spliceCount, segmentId,
                                                      value & 0x00FFFFFFU);
        if (splice == NULL) {
            splice = OotPspAssetBuilder_FindContainingSourceSplice(
                splices, spliceCount, segmentId, value & 0x00FFFFFFU);
        }
        if (splice != NULL) {
            size_t delta = (value & 0x00FFFFFFU) - splice->sourceOffset;

            if ((splice->targetOffset > 0x00FFFFFFU) ||
                (delta > 0x00FFFFFFU - splice->targetOffset)) {
                continue;
            }
            OotPspAssetBuilder_WriteBe32(
                source + i, ((u32)splice->targetSegmentId << 24) |
                                (splice->targetOffset + delta));
        }
    }

    if (transform) {
        if (!OotPspAssetBuilder_TransformAdaptedAsset(
                source, sourceSize, &target, targetSize, assetIndex, manifestCursor,
                manifestEnd, permutations, permutationCount, splices, spliceCount, copySizes)) {
            goto cleanup;
        }
    } else {
        target = calloc(1, targetSize);
        if (target == NULL) {
            goto cleanup;
        }
        /* Preserve bytes whose layout is not described by XML. Sparse scene
         * XML, for example, can move one named path while the adjacent
         * alternate scene headers remain at their native offsets. */
        memcpy(target, source, sourceSize < targetSize ? sourceSize : targetSize);
        for (i = 0; i < spliceCount; i++) {
            memcpy(target + splices[i].targetOffset, source + splices[i].sourceOffset,
                   copySizes[i]);
        }
    }
    *output = target;
    target = NULL;
    ok = true;

cleanup:
    free(target);
    free(copySizes);
    free(sourceDListOffsets);
    free(pointerFields);
    free(adaptedSplices);
    return ok;
}

#define OOT_PSP_SCENE_COMMAND_SIZE        8U
#define OOT_PSP_SCENE_COMMAND_MAX         0x1AU
#define OOT_PSP_SCENE_COMMAND_END         0x14U
#define OOT_PSP_SCENE_COMMAND_COLLISION   0x03U
#define OOT_PSP_SCENE_COMMAND_ALT_HEADERS 0x18U
#define OOT_PSP_SCENE_ALT_HEADER_MAX      19U
#define OOT_PSP_SCENE_HEADER_MAX          64U

static u16 OotPspAssetBuilder_ReadLe16(const u8* data) {
    return (u16)data[0] | ((u16)data[1] << 8);
}

static s32 OotPspAssetBuilder_StringEndsWith(const char* value, const char* suffix) {
    size_t valueLength = strlen(value);
    size_t suffixLength = strlen(suffix);

    return (valueLength >= suffixLength) &&
           (strcmp(value + valueLength - suffixLength, suffix) == 0);
}

static s32 OotPspAssetBuilder_GetSceneAssetSegment(const char* name, u8* segmentId) {
    const char* room = strstr(name, "_room_");

    if (OotPspAssetBuilder_StringEndsWith(name, "_scene")) {
        *segmentId = 2;
        return true;
    }
    if (room != NULL) {
        const char* number = room + strlen("_room_");

        if (*number == '\0') {
            return false;
        }
        while ((*number >= '0') && (*number <= '9')) {
            number++;
        }
        if (*number == '\0') {
            *segmentId = 3;
            return true;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_ValidateScenePointer(u32 pointer, u8 segmentId, size_t assetSize,
                                                    size_t requiredSize, size_t* offset) {
    size_t pointerOffset;

    if ((pointer >> 24) != segmentId) {
        return false;
    }
    pointerOffset = pointer & 0x00FFFFFFU;
    if ((pointerOffset > assetSize) || (requiredSize > assetSize - pointerOffset)) {
        return false;
    }
    if (offset != NULL) {
        *offset = pointerOffset;
    }
    return true;
}

static s32 OotPspAssetBuilder_ValidateCollisionHeader(const u8* data, size_t assetSize,
                                                      u8 segmentId, size_t offset) {
    static const u8 pointerOffsets[] = { 0x10, 0x18, 0x1C, 0x20, 0x28 };
    size_t i;

    if ((offset > assetSize) || (0x2C > assetSize - offset)) {
        return false;
    }
    for (i = 0; i < sizeof(pointerOffsets); i++) {
        u32 pointer = OotPspAssetBuilder_ReadLe32(data + offset + pointerOffsets[i]);
        size_t requiredSize = 1;

        if (pointer == 0) {
            continue;
        }
        if (pointerOffsets[i] == 0x10) {
            requiredSize = (size_t)OotPspAssetBuilder_ReadLe16(data + offset + 0x0C) * 6;
        } else if (pointerOffsets[i] == 0x18) {
            requiredSize = (size_t)OotPspAssetBuilder_ReadLe16(data + offset + 0x14) * 0x10;
        } else if (pointerOffsets[i] == 0x28) {
            requiredSize = (size_t)OotPspAssetBuilder_ReadLe16(data + offset + 0x24) * 0x10;
        }
        if (!OotPspAssetBuilder_ValidateScenePointer(
                pointer, segmentId, assetSize, requiredSize, NULL)) {
            return false;
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_ValidateNativeDListPointer(
    const u8* data, size_t assetSize, u8 segmentId, u32 pointer);

static s32 OotPspAssetBuilder_ValidateRoomShape(const u8* data, size_t assetSize,
                                                u8 segmentId, size_t offset) {
    u8 shapeType;
    u8 amountType;
    size_t entryCount;
    size_t entrySize;
    u32 entryPointer;
    size_t entryOffset;
    size_t entryIndex;

    if ((offset > assetSize) || (8 > assetSize - offset)) {
        return false;
    }
    shapeType = data[offset];
    amountType = data[offset + 1];
    if (shapeType > 2) {
        return false;
    }
    entryCount = shapeType == 1 ? 1 : amountType;
    entrySize = shapeType == 2 ? 0x10 : 8;
    entryPointer = OotPspAssetBuilder_ReadLe32(data + offset + 4);
    entryOffset = entryPointer & 0x00FFFFFFU;
    if ((entryCount == 0) || ((entryPointer >> 24) != segmentId) ||
        (entryOffset > assetSize) || (entryCount * entrySize > assetSize - entryOffset)) {
        return false;
    }
    if (shapeType != 1) {
        u32 entryEndPointer;

        if ((offset > assetSize) || (0x0C > assetSize - offset)) {
            return false;
        }
        entryEndPointer = OotPspAssetBuilder_ReadLe32(data + offset + 8);
        if (((entryEndPointer >> 24) != segmentId) ||
            ((entryEndPointer & 0x00FFFFFFU) != entryOffset + entryCount * entrySize)) {
            return false;
        }
    }
    for (entryIndex = 0; entryIndex < entryCount; entryIndex++) {
        size_t field = shapeType == 2 ? 8 : 0;
        size_t slot;

        for (slot = 0; slot < 2; slot++) {
            u32 pointer = OotPspAssetBuilder_ReadLe32(
                data + entryOffset + entryIndex * entrySize + field + slot * sizeof(u32));

            if ((pointer != 0) && !OotPspAssetBuilder_ValidateNativeDListPointer(
                                    data, assetSize, segmentId, pointer)) {
                return false;
            }
        }
    }
    if (shapeType == 1) {
        size_t backgroundCount;
        size_t backgroundsOffset;
        size_t backgroundIndex;

        if (amountType == 1) {
            if ((offset > assetSize) || (0x20 > assetSize - offset)) {
                return false;
            }
            backgroundCount = 1;
            backgroundsOffset = offset;
        } else if (amountType == 2) {
            u32 backgroundsPointer;

            if ((offset > assetSize) || (0x10 > assetSize - offset)) {
                return false;
            }
            backgroundCount = data[offset + 8];
            backgroundsPointer = OotPspAssetBuilder_ReadLe32(data + offset + 0x0C);
            backgroundsOffset = backgroundsPointer & 0x00FFFFFFU;
            if ((backgroundCount == 0) || ((backgroundsPointer >> 24) != segmentId) ||
                (backgroundsOffset > assetSize) ||
                (backgroundCount * 0x1C > assetSize - backgroundsOffset)) {
                return false;
            }
        } else {
            return false;
        }
        for (backgroundIndex = 0; backgroundIndex < backgroundCount; backgroundIndex++) {
            size_t backgroundOffset = amountType == 1
                                          ? backgroundsOffset
                                          : backgroundsOffset + backgroundIndex * 0x1C;
            size_t fieldBase = amountType == 1 ? 8 : 4;
            u32 sourcePointer = OotPspAssetBuilder_ReadLe32(
                data + backgroundOffset + fieldBase);
            u32 tlutPointer = OotPspAssetBuilder_ReadLe32(
                data + backgroundOffset + fieldBase + 8);
            u16 width = OotPspAssetBuilder_ReadLe16(
                data + backgroundOffset + fieldBase + 0x0C);
            u16 height = OotPspAssetBuilder_ReadLe16(
                data + backgroundOffset + fieldBase + 0x0E);
            u8 size = data[backgroundOffset + fieldBase + 0x11];
            u16 tlutCount = OotPspAssetBuilder_ReadLe16(
                data + backgroundOffset + fieldBase + 0x14);
            size_t sourceSize = size <= 3
                                    ? (((size_t)width * height * (4U << size)) + 7) / 8
                                    : 0;

            if ((sourceSize == 0) || !OotPspAssetBuilder_ValidateScenePointer(
                                       sourcePointer, segmentId, assetSize, sourceSize, NULL) ||
                ((tlutPointer != 0) &&
                 !OotPspAssetBuilder_ValidateScenePointer(
                     tlutPointer, segmentId, assetSize, (size_t)tlutCount * sizeof(u16), NULL))) {
                return false;
            }
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_ValidateNativeSceneAsset(const char* name, const u8* data,
                                                        size_t assetSize) {
    size_t headers[OOT_PSP_SCENE_HEADER_MAX];
    size_t headerCount = 1;
    size_t headerIndex;
    u8 segmentId;

    if (!OotPspAssetBuilder_GetSceneAssetSegment(name, &segmentId)) {
        return true;
    }
    headers[0] = 0;
    for (headerIndex = 0; headerIndex < headerCount; headerIndex++) {
        size_t headerOffset = headers[headerIndex];
        size_t commandOffset = headerOffset;
        s32 terminated = false;

        while ((commandOffset <= assetSize) &&
               (OOT_PSP_SCENE_COMMAND_SIZE <= assetSize - commandOffset)) {
            const u8* command = data + commandOffset;
            u8 commandId = command[0];
            u8 count = command[1];
            u32 pointer = OotPspAssetBuilder_ReadLe32(command + 4);
            size_t pointerOffset = 0;
            size_t requiredSize = 1;
            s32 hasPointer = false;

            if (commandId == OOT_PSP_SCENE_COMMAND_END) {
                terminated = true;
                break;
            }
            if (commandId >= OOT_PSP_SCENE_COMMAND_MAX) {
                printf("oot-psp invalid scene command asset=%s header=%08lx cmd=%08lx id=%u\n",
                       name, (unsigned long)headerOffset, (unsigned long)commandOffset,
                       (unsigned int)commandId);
                return false;
            }
            switch (commandId) {
                case 0x00:
                case 0x01:
                    hasPointer = true;
                    requiredSize = (size_t)count * 0x10;
                    break;
                case 0x02:
                    hasPointer = true;
                    break;
                case OOT_PSP_SCENE_COMMAND_COLLISION:
                    hasPointer = true;
                    requiredSize = 0x2C;
                    break;
                case 0x04:
                    hasPointer = true;
                    requiredSize = (size_t)count * 8;
                    break;
                case 0x06:
                case 0x0A:
                    hasPointer = true;
                    break;
                case 0x0B:
                    hasPointer = true;
                    requiredSize = (size_t)count * 2;
                    break;
                case 0x0C:
                    hasPointer = true;
                    requiredSize = (size_t)count * 0x0E;
                    break;
                case 0x0D:
                    hasPointer = true;
                    requiredSize = 8;
                    break;
                case 0x0E:
                    hasPointer = true;
                    requiredSize = (size_t)count * 0x10;
                    break;
                case 0x0F:
                    hasPointer = true;
                    requiredSize = (size_t)count * 0x16;
                    break;
                case 0x13:
                    hasPointer = true;
                    requiredSize = 2;
                    break;
                case 0x17:
                    hasPointer = true;
                    requiredSize = 4;
                    break;
                case OOT_PSP_SCENE_COMMAND_ALT_HEADERS:
                    hasPointer = true;
                    requiredSize = 4;
                    break;
                default:
                    break;
            }
            if (hasPointer && (pointer != 0) &&
                !OotPspAssetBuilder_ValidateScenePointer(
                    pointer, segmentId, assetSize, requiredSize, &pointerOffset)) {
                printf("oot-psp invalid scene pointer asset=%s header=%08lx cmd=%08lx id=%u raw=%08lx\n",
                       name, (unsigned long)headerOffset, (unsigned long)commandOffset,
                       (unsigned int)commandId, (unsigned long)pointer);
                return false;
            }
            if (hasPointer && (pointer == 0) && (requiredSize != 0) && (count != 0)) {
                printf("oot-psp missing scene pointer asset=%s header=%08lx cmd=%08lx id=%u count=%u\n",
                       name, (unsigned long)headerOffset, (unsigned long)commandOffset,
                       (unsigned int)commandId, (unsigned int)count);
                return false;
            }
            if ((commandId == OOT_PSP_SCENE_COMMAND_COLLISION) && (pointer != 0) &&
                !OotPspAssetBuilder_ValidateCollisionHeader(
                    data, assetSize, segmentId, pointerOffset)) {
                printf("oot-psp invalid collision data asset=%s header=%08lx raw=%08lx\n",
                       name, (unsigned long)headerOffset, (unsigned long)pointer);
                return false;
            }
            if ((commandId == 0x0A) && (pointer != 0) &&
                !OotPspAssetBuilder_ValidateRoomShape(
                    data, assetSize, segmentId, pointerOffset)) {
                printf("oot-psp invalid room shape asset=%s header=%08lx raw=%08lx\n",
                       name, (unsigned long)headerOffset, (unsigned long)pointer);
                return false;
            }
            if ((commandId == OOT_PSP_SCENE_COMMAND_ALT_HEADERS) && (pointer != 0)) {
                size_t listIndex;
                size_t listEnd = assetSize;
                size_t probeOffset;
                s32 invalidSeen = false;

                /* Alternate-header arrays have no terminator. Their end is
                 * the nearest following structure referenced by another
                 * command in the same header. Without this bound, room mesh
                 * pointers after a short list can look like late headers. */
                for (probeOffset = headerOffset;
                     (probeOffset <= assetSize) &&
                     (OOT_PSP_SCENE_COMMAND_SIZE <= assetSize - probeOffset);
                     probeOffset += OOT_PSP_SCENE_COMMAND_SIZE) {
                    u8 probeId = data[probeOffset];
                    u32 probePointer = OotPspAssetBuilder_ReadLe32(data + probeOffset + 4);
                    size_t probePointerOffset = probePointer & 0x00FFFFFFU;

                    if ((probeId != OOT_PSP_SCENE_COMMAND_ALT_HEADERS) &&
                        ((probePointer >> 24) == segmentId) &&
                        (probePointerOffset > pointerOffset) &&
                        (probePointerOffset < listEnd)) {
                        listEnd = probePointerOffset;
                    }
                    if (probeId == OOT_PSP_SCENE_COMMAND_END) {
                        break;
                    }
                }

                for (listIndex = 0; listIndex < OOT_PSP_SCENE_ALT_HEADER_MAX; listIndex++) {
                    size_t entryOffset = pointerOffset + listIndex * sizeof(u32);
                    u32 child;
                    size_t childOffset;
                    size_t i;
                    s32 known = false;

                    if ((entryOffset > listEnd) || (sizeof(u32) > listEnd - entryOffset)) {
                        break;
                    }
                    child = OotPspAssetBuilder_ReadLe32(data + entryOffset);
                    if (child == 0) {
                        continue;
                    }
                    if (!OotPspAssetBuilder_ValidateScenePointer(
                            child, segmentId, assetSize, OOT_PSP_SCENE_COMMAND_SIZE,
                            &childOffset) ||
                        (data[childOffset] >= OOT_PSP_SCENE_COMMAND_MAX)) {
                        invalidSeen = true;
                        continue;
                    }
                    if (invalidSeen) {
                        printf("oot-psp invalid alternate header list asset=%s header=%08lx list=%08lx index=%lu\n",
                               name, (unsigned long)headerOffset, (unsigned long)pointerOffset,
                               (unsigned long)listIndex);
                        return false;
                    }
                    for (i = 0; i < headerCount; i++) {
                        if (headers[i] == childOffset) {
                            known = true;
                            break;
                        }
                    }
                    if (!known) {
                        if (headerCount == OOT_PSP_SCENE_HEADER_MAX) {
                            printf("oot-psp too many scene headers asset=%s\n", name);
                            return false;
                        }
                        headers[headerCount++] = childOffset;
                    }
                }
            }
            commandOffset += OOT_PSP_SCENE_COMMAND_SIZE;
        }
        if (!terminated) {
            printf("oot-psp unterminated scene header asset=%s header=%08lx\n",
                   name, (unsigned long)headerOffset);
            return false;
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_ValidateNativeDListPointer(
    const u8* data, size_t assetSize, u8 segmentId, u32 pointer) {
    size_t offset = pointer & 0x00FFFFFFU;
    size_t cursor;

    if ((pointer == 0) || ((pointer >> 24) != segmentId) ||
        (offset > assetSize) || (sizeof(u64) > assetSize - offset)) {
        return false;
    }
    for (cursor = offset; sizeof(u64) <= assetSize - cursor; cursor += sizeof(u64)) {
        if ((OotPspAssetBuilder_ReadLe32(data + cursor) >> 24) == 0xDF) {
            return true;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_FindNativeSceneCommandsSize(
    const u8* data, size_t limit, size_t* size) {
    size_t offset;

    for (offset = 0; sizeof(u64) <= limit - offset; offset += sizeof(u64)) {
        if ((data[offset] == OOT_PSP_SCENE_COMMAND_END) &&
            (OotPspAssetBuilder_ReadLe32(data + offset + 4) == 0)) {
            *size = offset + sizeof(u64);
            return true;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_FindNativeSceneHeader(
    const u8* data, size_t assetSize, u8 segmentId, u8 headerIndex,
    size_t* headerOffset, size_t* headerSize) {
    size_t primarySize;
    size_t commandOffset;
    u32 alternatePointer = 0;

    if (!OotPspAssetBuilder_FindNativeSceneCommandsSize(
            data, assetSize, &primarySize)) {
        return false;
    }
    if (headerIndex == 0) {
        *headerOffset = 0;
        *headerSize = primarySize;
        return true;
    }
    for (commandOffset = 0; commandOffset < primarySize;
         commandOffset += sizeof(u64)) {
        if (data[commandOffset] == OOT_PSP_SCENE_COMMAND_ALT_HEADERS) {
            alternatePointer = OotPspAssetBuilder_ReadLe32(data + commandOffset + 4);
            break;
        }
    }
    if ((alternatePointer >> 24) == segmentId) {
        size_t alternateOffset = alternatePointer & 0x00FFFFFFU;
        size_t entryOffset = alternateOffset +
                             ((size_t)headerIndex - 1) * sizeof(u32);
        u32 pointer;

        if ((entryOffset > assetSize) || (sizeof(u32) > assetSize - entryOffset)) {
            return false;
        }
        pointer = OotPspAssetBuilder_ReadLe32(data + entryOffset);
        *headerOffset = pointer & 0x00FFFFFFU;
        return ((pointer >> 24) == segmentId) && (*headerOffset < assetSize) &&
               OotPspAssetBuilder_FindNativeSceneCommandsSize(
                   data + *headerOffset, assetSize - *headerOffset, headerSize);
    }
    return false;
}

static s32 OotPspAssetBuilder_FindNativeSceneCommand(
    const u8* data, size_t headerOffset, size_t headerSize, u8 commandId,
    u8 occurrence, const u8** command) {
    size_t offset;
    u8 seen = 0;

    for (offset = 0; offset < headerSize; offset += sizeof(u64)) {
        const u8* candidate = data + headerOffset + offset;

        if (candidate[0] == commandId) {
            if (seen == occurrence) {
                *command = candidate;
                return true;
            }
            seen++;
        }
    }
    return false;
}

static s32 OotPspAssetBuilder_ValidateNativeSceneGraphAsset(
    size_t assetIndex, const char* name, const u8* data, size_t assetSize) {
    const OotPspSceneLayoutAnchor* anchors;
    size_t anchorCount;
    size_t* dListOffsets = NULL;
    size_t anchorIndex;
    s32 ok = false;

    anchors = OotPspAssetBuilder_GetSceneLayoutAnchors(assetIndex, &anchorCount);
    if (anchorCount == 0) {
        return true;
    }
    dListOffsets = malloc(anchorCount * sizeof(*dListOffsets));
    if (dListOffsets == NULL) {
        return false;
    }
    for (anchorIndex = 0; anchorIndex < anchorCount; anchorIndex++) {
        dListOffsets[anchorIndex] = (size_t)-1;
    }
    for (anchorIndex = 0; anchorIndex < anchorCount; anchorIndex++) {
        const OotPspSceneLayoutAnchor* anchor = &anchors[anchorIndex];

        if ((anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) &&
            (anchor->graphIndex < anchorCount)) {
            dListOffsets[anchor->graphIndex] = anchor->targetOffset;
        }
    }
    for (anchorIndex = 0; anchorIndex < anchorCount; anchorIndex++) {
        const OotPspSceneLayoutAnchor* anchor = &anchors[anchorIndex];

        if (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_MESH_DLIST) {
            if (!OotPspAssetBuilder_ValidateNativeDListPointer(
                    data, assetSize, anchor->segmentId,
                    ((u32)anchor->segmentId << 24) | anchor->targetOffset)) {
                printf("oot-psp invalid room dlist asset=%s target=%08lx graph=%u\n",
                       name, (unsigned long)anchor->targetOffset,
                       (unsigned int)anchor->graphIndex);
                goto cleanup;
            }
            if (anchor->parentGraphIndex == OOT_PSP_SCENE_LAYOUT_GRAPH_NONE) {
                size_t headerOffset;
                size_t headerSize;
                const u8* command;
                u32 shapePointer;
                size_t shapeOffset;
                u8 shapeType;
                size_t entryCount;
                size_t entrySize;
                u32 entryPointer;
                size_t entryOffset;
                size_t field;
                u32 rootPointer;

                if (!OotPspAssetBuilder_FindNativeSceneHeader(
                        data, assetSize, anchor->segmentId, anchor->headerIndex,
                        &headerOffset, &headerSize) ||
                    !OotPspAssetBuilder_FindNativeSceneCommand(
                        data, headerOffset, headerSize, anchor->commandId,
                        anchor->occurrence, &command)) {
                    goto cleanup;
                }
                shapePointer = OotPspAssetBuilder_ReadLe32(command + 4);
                shapeOffset = shapePointer & 0x00FFFFFFU;
                if (((shapePointer >> 24) != anchor->segmentId) ||
                    (shapeOffset > assetSize) || (8 > assetSize - shapeOffset)) {
                    goto cleanup;
                }
                shapeType = data[shapeOffset];
                entryCount = shapeType == 1 ? 1 : data[shapeOffset + 1];
                entrySize = shapeType == 2 ? 0x10 : 8;
                entryPointer = OotPspAssetBuilder_ReadLe32(data + shapeOffset + 4);
                entryOffset = entryPointer & 0x00FFFFFFU;
                if ((shapeType > 2) || (anchor->childIndex >= entryCount) ||
                    (anchor->childSlot >= 2) ||
                    ((entryPointer >> 24) != anchor->segmentId) ||
                    (entryOffset > assetSize) ||
                    (entryCount * entrySize > assetSize - entryOffset)) {
                    goto cleanup;
                }
                field = shapeType == 2 ? 8 : 0;
                rootPointer = OotPspAssetBuilder_ReadLe32(
                    data + entryOffset + (size_t)anchor->childIndex * entrySize +
                    field + (size_t)anchor->childSlot * sizeof(u32));
                if (((rootPointer >> 24) != anchor->segmentId) ||
                    ((rootPointer & 0x00FFFFFFU) != anchor->targetOffset)) {
                    printf("oot-psp invalid room root asset=%s header=%u raw=%08lx target=%08lx\n",
                           name, (unsigned int)anchor->headerIndex,
                           (unsigned long)rootPointer,
                           (unsigned long)anchor->targetOffset);
                    goto cleanup;
                }
            }
        }
        if (anchor->parentGraphIndex != OOT_PSP_SCENE_LAYOUT_GRAPH_NONE) {
            size_t parentOffset;
            size_t commandOffset;
            u32 pointer;
            u8 opcode;

            if ((anchor->parentGraphIndex >= anchorCount) ||
                (dListOffsets[anchor->parentGraphIndex] == (size_t)-1)) {
                goto cleanup;
            }
            parentOffset = dListOffsets[anchor->parentGraphIndex];
            commandOffset = parentOffset + (size_t)anchor->commandIndex * sizeof(u64);
            if ((commandOffset > assetSize) ||
                (sizeof(u64) > assetSize - commandOffset)) {
                goto cleanup;
            }
            opcode = OotPspAssetBuilder_ReadLe32(data + commandOffset) >> 24;
            pointer = OotPspAssetBuilder_ReadLe32(data + commandOffset + 4);
            if ((opcode != anchor->childSlot) ||
                ((pointer >> 24) != anchor->segmentId) ||
                ((pointer & 0x00FFFFFFU) != anchor->targetOffset)) {
                printf("oot-psp invalid room graph pointer asset=%s graph=%u cmd=%u raw=%08lx target=%08lx\n",
                       name, (unsigned int)anchor->parentGraphIndex,
                       (unsigned int)anchor->commandIndex, (unsigned long)pointer,
                       (unsigned long)anchor->targetOffset);
                goto cleanup;
            }
        }
        if (((anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_VERTICES) ||
             (anchor->child == OOT_PSP_SCENE_LAYOUT_CHILD_DLIST_DATA)) &&
            ((anchor->targetOffset > assetSize) ||
             (anchor->targetSize > assetSize - anchor->targetOffset))) {
            goto cleanup;
        }
    }
    ok = true;

cleanup:
    free(dListOffsets);
    return ok;
}

static s32 OotPspAssetBuilder_ValidateNativeSkinLimb(
    const u8* data, size_t assetSize, u8 segmentId, size_t limbOffset) {
    u32 segmentType = OotPspAssetBuilder_ReadLe32(data + limbOffset + 8);
    u32 segmentPointer = OotPspAssetBuilder_ReadLe32(data + limbOffset + 12);
    size_t segmentOffset = segmentPointer & 0x00FFFFFFU;

    if (segmentPointer == 0) {
        return segmentType != OOT_PSP_SKIN_LIMB_ANIMATED &&
               segmentType != OOT_PSP_SKIN_LIMB_NORMAL;
    }
    if ((segmentPointer >> 24) != segmentId) {
        return false;
    }
    if (segmentType == OOT_PSP_SKIN_LIMB_NORMAL) {
        return OotPspAssetBuilder_ValidateNativeDListPointer(
            data, assetSize, segmentId, segmentPointer);
    }
    if (segmentType == OOT_PSP_SKIN_LIMB_ANIMATED) {
        u16 modifCount;
        u32 modifsPointer;
        u32 dListPointer;
        size_t modifsOffset;
        size_t modifIndex;

        if ((segmentOffset > assetSize) ||
            (OOT_PSP_SKIN_DATA_SIZE > assetSize - segmentOffset)) {
            return false;
        }
        modifCount = OotPspAssetBuilder_ReadLe16(data + segmentOffset + 2);
        modifsPointer = OotPspAssetBuilder_ReadLe32(data + segmentOffset + 4);
        dListPointer = OotPspAssetBuilder_ReadLe32(data + segmentOffset + 8);
        modifsOffset = modifsPointer & 0x00FFFFFFU;
        if (!OotPspAssetBuilder_ValidateNativeDListPointer(
                data, assetSize, segmentId, dListPointer)) {
            return false;
        }
        if (modifCount == 0) {
            return true;
        }
        if (((modifsPointer >> 24) != segmentId) || (modifsOffset > assetSize) ||
            ((size_t)modifCount * OOT_PSP_SKIN_MODIF_SIZE > assetSize - modifsOffset)) {
            return false;
        }
        for (modifIndex = 0; modifIndex < modifCount; modifIndex++) {
            size_t modifOffset = modifsOffset + modifIndex * OOT_PSP_SKIN_MODIF_SIZE;
            u16 vertexCount = OotPspAssetBuilder_ReadLe16(data + modifOffset);
            u16 transformCount = OotPspAssetBuilder_ReadLe16(data + modifOffset + 2);
            u32 verticesPointer = OotPspAssetBuilder_ReadLe32(data + modifOffset + 8);
            u32 transformsPointer = OotPspAssetBuilder_ReadLe32(data + modifOffset + 12);
            size_t verticesOffset = verticesPointer & 0x00FFFFFFU;
            size_t transformsOffset = transformsPointer & 0x00FFFFFFU;

            if ((vertexCount != 0) &&
                (((verticesPointer >> 24) != segmentId) || (verticesOffset > assetSize) ||
                 ((size_t)vertexCount * OOT_PSP_SKIN_VERTEX_SIZE > assetSize - verticesOffset))) {
                return false;
            }
            if ((transformCount != 0) &&
                (((transformsPointer >> 24) != segmentId) || (transformsOffset > assetSize) ||
                 ((size_t)transformCount * OOT_PSP_SKIN_TRANSFORM_SIZE >
                  assetSize - transformsOffset))) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static s32 OotPspAssetBuilder_ValidateNativeSkeletonAsset(
    size_t assetIndex, const char* name, const u8* data, size_t assetSize) {
    const OotPspSkeletonLayoutAnchor* anchors;
    size_t anchorCount;
    size_t anchorIndex;

    anchors = OotPspAssetBuilder_GetSkeletonLayoutAnchors(assetIndex, &anchorCount);
    for (anchorIndex = 0; anchorIndex < anchorCount; anchorIndex++) {
        const OotPspSkeletonLayoutAnchor* header = &anchors[anchorIndex];
        size_t limbSize;
        size_t childField;
        size_t siblingField;
        size_t tableSize;
        size_t tableOffset;
        u32 tablePointer;
        size_t limbIndex;

        if (header->kind != OOT_PSP_SKELETON_LAYOUT_HEADER) {
            continue;
        }
        tableSize = (size_t)header->limbCount * sizeof(u32);
        if ((header->targetOffset > assetSize) ||
            (header->targetSize > assetSize - header->targetOffset) ||
            !OotPspAssetBuilder_GetSkeletonLimbLayout(
                header->limbType, &limbSize, &childField, &siblingField) ||
            (data[header->targetOffset + 4] != header->limbCount) ||
            ((header->targetSize == 12) &&
             (data[header->targetOffset + 8] != header->dListCount))) {
            printf("oot-psp invalid skeleton header asset=%s header=%08lx index=%u\n",
                   name, (unsigned long)header->targetOffset,
                   (unsigned int)header->skeletonIndex);
            return false;
        }
        tablePointer = OotPspAssetBuilder_ReadLe32(data + header->targetOffset);
        tableOffset = tablePointer & 0x00FFFFFFU;
        if (((tablePointer >> 24) != header->segmentId) ||
            (tableOffset > assetSize) || (tableSize > assetSize - tableOffset)) {
            printf("oot-psp invalid skeleton table asset=%s header=%08lx raw=%08lx\n",
                   name, (unsigned long)header->targetOffset,
                   (unsigned long)tablePointer);
            return false;
        }
        for (limbIndex = 0; limbIndex < header->limbCount; limbIndex++) {
            u32 limbPointer = OotPspAssetBuilder_ReadLe32(
                data + tableOffset + limbIndex * sizeof(u32));
            size_t limbOffset = limbPointer & 0x00FFFFFFU;
            u8 child;
            u8 sibling;

            if (((limbPointer >> 24) != header->segmentId) ||
                (limbOffset > assetSize) || (limbSize > assetSize - limbOffset)) {
                printf("oot-psp invalid skeleton limb asset=%s header=%08lx limb=%lu raw=%08lx\n",
                       name, (unsigned long)header->targetOffset,
                       (unsigned long)limbIndex, (unsigned long)limbPointer);
                return false;
            }
            child = data[limbOffset + childField];
            sibling = data[limbOffset + siblingField];
            if (((child != 0xFF) && (child >= header->limbCount)) ||
                ((sibling != 0xFF) && (sibling >= header->limbCount))) {
                printf("oot-psp invalid skeleton links asset=%s header=%08lx limb=%lu child=%u sibling=%u\n",
                       name, (unsigned long)header->targetOffset,
                       (unsigned long)limbIndex, (unsigned int)child,
                       (unsigned int)sibling);
                return false;
            }
            if ((header->limbType == OOT_PSP_SKELETON_LIMB_SKIN) &&
                !OotPspAssetBuilder_ValidateNativeSkinLimb(
                    data, assetSize, header->segmentId, limbOffset)) {
                printf("oot-psp invalid skin graph asset=%s header=%08lx limb=%lu\n",
                       name, (unsigned long)header->targetOffset,
                       (unsigned long)limbIndex);
                return false;
            }
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_ValidateNativeAnimationAsset(
    size_t assetIndex, const char* name, const u8* data, size_t assetSize) {
    const OotPspAnimationLayoutAnchor* anchors;
    size_t anchorCount;
    size_t anchorIndex;

    anchors = OotPspAssetBuilder_GetAnimationLayoutAnchors(assetIndex, &anchorCount);
    for (anchorIndex = 0; anchorIndex < anchorCount; anchorIndex++) {
        const OotPspAnimationLayoutAnchor* header = &anchors[anchorIndex];
        const OotPspAnimationLayoutAnchor* frame;
        const OotPspAnimationLayoutAnchor* joints;
        u32 framePointer;
        u32 jointPointer;
        size_t frameValueCount;
        size_t jointCount;
        size_t jointIndex;
        u16 frameCount;
        u16 staticIndexMax;

        if (header->kind != OOT_PSP_ANIMATION_LAYOUT_HEADER) {
            continue;
        }
        if ((anchorIndex + 2 >= anchorCount) ||
            (anchors[anchorIndex + 1].animationIndex != header->animationIndex) ||
            (anchors[anchorIndex + 1].kind != OOT_PSP_ANIMATION_LAYOUT_FRAME_DATA) ||
            (anchors[anchorIndex + 2].animationIndex != header->animationIndex) ||
            (anchors[anchorIndex + 2].kind != OOT_PSP_ANIMATION_LAYOUT_JOINT_INDICES)) {
            printf("oot-psp incomplete animation graph asset=%s index=%u\n",
                   name, (unsigned int)header->animationIndex);
            return false;
        }
        frame = &anchors[anchorIndex + 1];
        joints = &anchors[anchorIndex + 2];
        if ((header->targetOffset > assetSize) ||
            (OOT_PSP_ANIMATION_HEADER_SIZE > assetSize - header->targetOffset) ||
            (frame->targetOffset > assetSize) ||
            (frame->targetSize > assetSize - frame->targetOffset) ||
            (joints->targetOffset > assetSize) ||
            (joints->targetSize > assetSize - joints->targetOffset)) {
            printf("oot-psp invalid animation header asset=%s header=%08lx index=%u\n",
                   name, (unsigned long)header->targetOffset,
                   (unsigned int)header->animationIndex);
            return false;
        }
        framePointer = OotPspAssetBuilder_ReadLe32(data + header->targetOffset + 4);
        jointPointer = OotPspAssetBuilder_ReadLe32(data + header->targetOffset + 8);
        if (((framePointer >> 24) != header->segmentId) ||
            ((framePointer & 0x00FFFFFFU) != frame->targetOffset) ||
            ((jointPointer >> 24) != header->segmentId) ||
            ((jointPointer & 0x00FFFFFFU) != joints->targetOffset)) {
            printf("oot-psp invalid animation pointers asset=%s header=%08lx frame=%08lx joints=%08lx\n",
                   name, (unsigned long)header->targetOffset,
                   (unsigned long)framePointer, (unsigned long)jointPointer);
            return false;
        }
        frameValueCount = frame->targetSize / sizeof(u16);
        jointCount = joints->targetSize / 6;
        frameCount = OotPspAssetBuilder_ReadLe16(data + header->targetOffset);
        staticIndexMax = OotPspAssetBuilder_ReadLe16(data + header->targetOffset + 0x0C);
        if ((frameCount == 0) || (frameCount > 0x7FFF) || (staticIndexMax > frameValueCount)) {
            printf("oot-psp invalid animation counts asset=%s header=%08lx frames=%u static=%u\n",
                   name, (unsigned long)header->targetOffset, frameCount, staticIndexMax);
            return false;
        }
        for (jointIndex = 0; jointIndex < jointCount; jointIndex++) {
            size_t component;

            for (component = 0; component < 3; component++) {
                u16 index = OotPspAssetBuilder_ReadLe16(
                    data + joints->targetOffset + jointIndex * 6 +
                    component * sizeof(u16));
                size_t end = index < staticIndexMax
                                 ? (size_t)index + 1
                                 : (size_t)index + frameCount;

                if (end > frameValueCount) {
                    printf("oot-psp invalid animation indices asset=%s header=%08lx joint=%lu "
                           "component=%lu value=%u frames=%u static=%u values=%lu\n",
                           name, (unsigned long)header->targetOffset,
                           (unsigned long)jointIndex, (unsigned long)component, index,
                           frameCount, staticIndexMax, (unsigned long)frameValueCount);
                    return false;
                }
            }
        }
    }
    return true;
}

static s32 OotPspAssetBuilder_PreflightLayout(const OotPspRomImage* rom, const OotPspDmaEntry* entries,
                                              const u8* manifestCursor, const u8* manifestEnd,
                                              u32 manifestEntryCount, u32* packedSize, u32* codeSize,
                                              OotPspExternalAudioAssetLayout audioLayouts
                                                  [OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT],
                                              OotPspExternalMessageAssetLayout messageLayouts
                                                  [OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT],
                                              const char** incompatibleAsset) {
    const u8* checkCursor = manifestCursor;
    u32 nativeSeen = 0;
    size_t assetIndex;

    *packedSize = 0;
    *codeSize = 0;
    *incompatibleAsset = NULL;
    for (assetIndex = 0; assetIndex < gOotPspExternalAssetCount; assetIndex++) {
        const OotPspExternalAsset* asset = &gOotPspExternalAssets[assetIndex];
        const OotPspDmaEntry* dma = OotPspAssetBuilder_GetAssetDma(rom, entries, assetIndex);
        size_t targetSize = asset->vromEnd - asset->vromStart;
        size_t outputSize = targetSize;
        s32 audioIndex = OotPspAssetBuilder_GetAudioAssetIndex(asset);
        s32 messageIndex = OotPspAssetBuilder_GetMessageAssetIndex(asset);

        if (asset->flags != 0) {
            if (!OotPspAssetBuilder_AdvanceTransform(targetSize, assetIndex, &checkCursor, manifestEnd)) {
                *incompatibleAsset = asset->name;
                return false;
            }
            nativeSeen++;
        }

        if (dma == NULL) {
            if (!OotPspAssetBuilder_IsOptionalRegionalAsset(asset)) {
                *incompatibleAsset = asset->name;
                return false;
            }
        } else {
            size_t sourceSize;

            if (dma->vromEnd <= dma->vromStart) {
                *incompatibleAsset = asset->name;
                return false;
            }
            sourceSize = dma->vromEnd - dma->vromStart;

            if (strcmp(asset->name, "code") == 0) {
                outputSize = sourceSize;
                *codeSize = sourceSize;
            } else if (audioIndex >= 0) {
                outputSize = sourceSize;
                audioLayouts[audioIndex].size = sourceSize;
            } else if (messageIndex >= 0) {
                outputSize = sourceSize;
                messageLayouts[messageIndex].size = sourceSize;
            } else if (OotPspAssetBuilder_IsNintendoLogoAsset(asset) &&
                       !OotPspAssetBuilder_IsSupportedNintendoLogoLayout(
                           sourceSize, targetSize, rom->profile->nintendoLogoDlistOffset)) {
                *incompatibleAsset = asset->name;
                return false;
            }
        }

        if ((audioIndex >= 0) || (messageIndex >= 0)) {
            continue;
        }
        if ((asset->fileOffset > 0xFFFFFFFFU) || (outputSize > 0xFFFFFFFFU - asset->fileOffset)) {
            *incompatibleAsset = asset->name;
            return false;
        }
        if ((u32)(asset->fileOffset + outputSize) > *packedSize) {
            *packedSize = asset->fileOffset + outputSize;
        }
    }

    if (gOotPspExternalAssetCount <= OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT) {
        *incompatibleAsset = "audio packing";
        return false;
    }
    {
        u32 audioRegionEnd = gOotPspExternalAssets[OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT].fileOffset;
        u32 audioTotal = 0;
        u32 cursor;

        for (assetIndex = 0; assetIndex < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; assetIndex++) {
            if ((audioLayouts[assetIndex].size == 0) ||
                (audioLayouts[assetIndex].size > OOT_PSP_AUDIO_SEGMENT_MAX_SIZE) ||
                (audioLayouts[assetIndex].size > 0xFFFFFFFFU - audioTotal)) {
                *incompatibleAsset = sOotPspAudioAssetNames[assetIndex];
                return false;
            }
            audioTotal = OotPspAssetBuilder_Align16(audioTotal + audioLayouts[assetIndex].size);
        }
        cursor = audioTotal <= audioRegionEnd ? 0 : OotPspAssetBuilder_Align16(*packedSize);
        for (assetIndex = 0; assetIndex < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT; assetIndex++) {
            audioLayouts[assetIndex].fileOffset = cursor;
            if (audioLayouts[assetIndex].size > 0xFFFFFFFFU - cursor) {
                *incompatibleAsset = sOotPspAudioAssetNames[assetIndex];
                return false;
            }
            cursor += audioLayouts[assetIndex].size;
            if ((assetIndex + 1) < OOT_PSP_EXTERNAL_AUDIO_ASSET_COUNT) {
                cursor = OotPspAssetBuilder_Align16(cursor);
            }
        }
        if (cursor > *packedSize) {
            *packedSize = cursor;
        }
    }
    {
        u32 cursor = OotPspAssetBuilder_Align16(*packedSize);

        for (assetIndex = 0; assetIndex < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; assetIndex++) {
            u16 dmaIndex = rom->profile->messageDmaIndices[assetIndex];

            if ((dmaIndex == OOT_PSP_ROM_PROFILE_DMA_MISSING) ||
                (dmaIndex >= rom->profile->dmadataCount)) {
                messageLayouts[assetIndex].fileOffset = 0;
                messageLayouts[assetIndex].size = 0;
                continue;
            }
            if (entries[dmaIndex].vromEnd <= entries[dmaIndex].vromStart) {
                *incompatibleAsset = sOotPspMessageAssetNames[assetIndex];
                return false;
            }
            messageLayouts[assetIndex].size = entries[dmaIndex].vromEnd - entries[dmaIndex].vromStart;
            if ((messageLayouts[assetIndex].size > OOT_PSP_AUDIO_SEGMENT_MAX_SIZE) ||
                (messageLayouts[assetIndex].size > 0xFFFFFFFFU - cursor)) {
                *incompatibleAsset = sOotPspMessageAssetNames[assetIndex];
                return false;
            }
            messageLayouts[assetIndex].fileOffset = cursor;
            cursor += messageLayouts[assetIndex].size;
            if ((assetIndex + 1) < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT) {
                cursor = OotPspAssetBuilder_Align16(cursor);
            }
        }
        if (cursor > *packedSize) {
            *packedSize = cursor;
        }
    }
    return (nativeSeen == manifestEntryCount) && (checkCursor == manifestEnd) && (*codeSize != 0);
}

#include "oot_psp_scene_extract.inc.c"

static s32 OotPspAssetBuilder_Build(const OotPspRomImage* rom, const char* outputPath, const char* tempPath,
                                    OotPspAssetIdentity* identity) {
    const size_t compressedSize = gOotPspAssetTransformCompressedEnd - gOotPspAssetTransformCompressed;
    const u8* compressed = gOotPspAssetTransformCompressed;
    const u8* manifestCursor;
    const u8* manifestEnd;
    const u8* permutations;
    u32 manifestEntryCount;
    u32 permutationCount;
    u32 nativeSeen = 0;
    u32 packedSize;
    u32 codeSize;
    const char* incompatibleAsset;
    OotPspDmaEntry* dmaEntries = NULL;
    SceUID outputFd = -1;
    size_t assetIndex;
    size_t sceneIndex = 0;
    s32 ok = false;

    memset(identity, 0, sizeof(*identity));

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

    dmaEntries = malloc(sizeof(*dmaEntries) * rom->profile->dmadataCount);
    if (dmaEntries == NULL) {
        OotPspAssetBuilder_ShowError("Not enough memory for asset setup");
        goto cleanup;
    }

    if (!OotPspAssetBuilder_LoadDmaTable(rom, dmaEntries)) {
        OotPspAssetBuilder_ShowError("Could not read the ROM file table");
        goto cleanup;
    }
    if (!OotPspAssetBuilder_PreflightLayout(rom, dmaEntries, manifestCursor, manifestEnd, manifestEntryCount,
                                            &packedSize, &codeSize, identity->audio, identity->message,
                                            &incompatibleAsset)) {
        char status[64];

        snprintf(status, sizeof(status), "%.18s layout differs at %.24s", rom->profile->name,
                 incompatibleAsset != NULL ? incompatibleAsset : "conversion data");
        OotPspAssetBuilder_ShowError(status);
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
        const OotPspDmaEntry* dma = OotPspAssetBuilder_GetAssetDma(rom, dmaEntries, assetIndex);
        size_t assetSize = asset->vromEnd - asset->vromStart;
        size_t sourceSize = dma != NULL ? dma->vromEnd - dma->vromStart : 0;
        uintptr_t outputOffset = asset->fileOffset;
        s32 audioIndex = OotPspAssetBuilder_GetAudioAssetIndex(asset);
        s32 messageIndex = OotPspAssetBuilder_GetMessageAssetIndex(asset);
        size_t spliceCount;
        size_t animationAnchorCount;
        size_t sceneAnchorCount;
        size_t skeletonAnchorCount;
        s32 needsLayoutAdaptation =
            (OotPspAssetBuilder_GetAssetSplices(
                 rom->profile, assetIndex, &spliceCount) != NULL) ||
            (OotPspAssetBuilder_GetAnimationLayoutAnchors(
                 assetIndex, &animationAnchorCount) != NULL) ||
            (OotPspAssetBuilder_GetSceneLayoutAnchors(
                 assetIndex, &sceneAnchorCount) != NULL) ||
            (OotPspAssetBuilder_GetSkeletonLayoutAnchors(
                 assetIndex, &skeletonAnchorCount) != NULL);
        u8* data = NULL;
        char status[64];

        if ((assetIndex & 7) == 0) {
            snprintf(status, sizeof(status), "Converting assets: %lu / %lu", (unsigned long)assetIndex,
                     (unsigned long)gOotPspExternalAssetCount);
            OotPspAssetBuilder_ShowProgress(
                100 + (u32)((assetIndex * 900) / gOotPspExternalAssetCount), status);
        }

        if (dma == NULL) {
            if ((asset->flags != 0) &&
                !OotPspAssetBuilder_AdvanceTransform(assetSize, assetIndex, &manifestCursor, manifestEnd)) {
                snprintf(status, sizeof(status), "Could not skip %.39s", asset->name);
                OotPspAssetBuilder_ShowError(status);
                goto cleanup;
            }
            if (!OotPspAssetBuilder_WriteZeros(outputFd, asset->fileOffset, assetSize)) {
                OotPspAssetBuilder_ShowError("Could not write regional placeholders");
                goto cleanup;
            }
            nativeSeen += asset->flags != 0;
            continue;
        }
        /* Runtime scene storage follows the actual DMA size, independently of
         * the canonical object/symbol layout. NTSC 1.0 retains its proven pack. */
        {
            u8 sceneSegment;
            if (strcmp(rom->profile->name, "ntsc-1.0") != 0 &&
                OotPspAssetBuilder_GetSceneAssetSegment(asset->name, &sceneSegment)) {
                u8* raw = NULL;
                u8* native = NULL;
                if (sceneIndex >= OOT_PSP_SCENE_ASSET_MAX || sourceSize > OOT_PSP_SCENE_VROM_STRIDE ||
                    !OotPspAssetBuilder_AdvanceTransform(assetSize, assetIndex, &manifestCursor, manifestEnd) ||
                    !OotPspAssetBuilder_LoadAsset(rom, dma, &raw, sourceSize) ||
                    !OotPspAssetBuilder_ExtractScene(rom->profile, assetIndex, raw, sourceSize, sceneSegment, &native) ||
                    !OotPspAssetBuilder_ValidateNativeSceneAsset(asset->name, native, sourceSize)) {
                    snprintf(status, sizeof(status), "Could not extract scene %.30s", asset->name);
                    OotPspAssetBuilder_ShowError(status);
                    free(raw); free(native); goto cleanup;
                }
                free(raw);
                packedSize = OotPspAssetBuilder_Align16(packedSize);
                if (sourceSize > 0xFFFFFFFFU - packedSize ||
                    !OotPspAssetBuilder_WriteAt(outputFd, packedSize, native, sourceSize)) {
                    free(native); goto cleanup;
                }
                free(native);
                identity->scene[sceneIndex].assetIndex = assetIndex;
                identity->scene[sceneIndex].fileOffset = packedSize;
                identity->scene[sceneIndex].size = sourceSize;
                identity->scene[sceneIndex].sourceVromStart = dma->vromStart;
                sceneIndex++;
                packedSize += sourceSize;
                nativeSeen++;
                continue;
            }
        }
        if (strcmp(asset->name, "code") == 0) {
            assetSize = dma->vromEnd - dma->vromStart;
        } else if (audioIndex >= 0) {
            assetSize = dma->vromEnd - dma->vromStart;
            outputOffset = identity->audio[audioIndex].fileOffset;
        } else if (messageIndex >= 0) {
            assetSize = dma->vromEnd - dma->vromStart;
            outputOffset = identity->message[messageIndex].fileOffset;
        }
        needsLayoutAdaptation =
            needsLayoutAdaptation ||
            ((sourceSize != assetSize) && (strcmp(asset->name, "code") != 0) && (audioIndex < 0) &&
             (messageIndex < 0) && !OotPspAssetBuilder_IsNintendoLogoAsset(asset));

        if ((asset->flags == 0) && (dma->romEnd == 0) && !needsLayoutAdaptation) {
            if (!OotPspAssetBuilder_CopyUncompressedAsset(rom, outputFd, dma->romStart, outputOffset,
                                                           assetSize)) {
                OotPspAssetBuilder_ShowError("Could not copy data from the ROM");
                goto cleanup;
            }
        } else {
            size_t loadSize = (OotPspAssetBuilder_IsNintendoLogoAsset(asset) || needsLayoutAdaptation)
                                  ? sourceSize
                                  : assetSize;

            if (!OotPspAssetBuilder_LoadAsset(rom, dma, &data, loadSize)) {
                snprintf(status, sizeof(status), "Could not extract %.36s", asset->name);
                OotPspAssetBuilder_ShowError(status);
                free(data);
                goto cleanup;
            }
            if (OotPspAssetBuilder_IsNintendoLogoAsset(asset)) {
                u8* nativeData = NULL;

                if (!OotPspAssetBuilder_AdvanceTransform(assetSize, assetIndex, &manifestCursor, manifestEnd) ||
                    !OotPspAssetBuilder_TransformNintendoLogo(
                        data, loadSize, rom->profile->nintendoLogoDlistOffset, &nativeData, assetSize)) {
                    snprintf(status, sizeof(status), "Could not convert %.36s", asset->name);
                    OotPspAssetBuilder_ShowError(status);
                    free(data);
                    free(nativeData);
                    goto cleanup;
                }
                free(data);
                data = nativeData;
            } else {
                if (needsLayoutAdaptation) {
                    u8* adaptedData = NULL;

                    if (!OotPspAssetBuilder_AdaptAssetLayout(rom->profile, assetIndex, data, loadSize,
                                                             &adaptedData, assetSize, &manifestCursor,
                                                             manifestEnd, permutations, permutationCount,
                                                             asset->flags != 0)) {
                        snprintf(status, sizeof(status), "Could not adapt %.38s", asset->name);
                        OotPspAssetBuilder_ShowError(status);
                        free(data);
                        goto cleanup;
                    }
                    free(data);
                    data = adaptedData;
                }
                if ((asset->flags != 0) && !needsLayoutAdaptation &&
                    !OotPspAssetBuilder_TransformAsset(data, assetSize, assetIndex, &manifestCursor,
                                                       manifestEnd, permutations, permutationCount)) {
                    snprintf(status, sizeof(status), "Could not convert %.36s", asset->name);
                    OotPspAssetBuilder_ShowError(status);
                    free(data);
                    goto cleanup;
                }
            }
            if ((asset->flags != 0) &&
                (!OotPspAssetBuilder_ValidateNativeAnimationAsset(
                     assetIndex, asset->name, data, assetSize) ||
                 !OotPspAssetBuilder_ValidateNativeSceneAsset(asset->name, data, assetSize) ||
                 !OotPspAssetBuilder_ValidateNativeSceneGraphAsset(
                     assetIndex, asset->name, data, assetSize) ||
                 !OotPspAssetBuilder_ValidateNativeSkeletonAsset(
                     assetIndex, asset->name, data, assetSize))) {
                snprintf(status, sizeof(status), "Invalid native %.38s", asset->name);
                OotPspAssetBuilder_ShowError(status);
                free(data);
                goto cleanup;
            }
            if (!OotPspAssetBuilder_WriteAt(outputFd, outputOffset, data, assetSize)) {
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
    for (assetIndex = 0; assetIndex < OOT_PSP_EXTERNAL_MESSAGE_ASSET_COUNT; assetIndex++) {
        const OotPspDmaEntry* dma;
        u16 dmaIndex = rom->profile->messageDmaIndices[assetIndex];
        size_t externalIndex;
        s32 alreadyWritten = false;
        size_t assetSize;
        u8* data = NULL;

        for (externalIndex = 0; externalIndex < gOotPspExternalAssetCount; externalIndex++) {
            if (strcmp(gOotPspExternalAssets[externalIndex].name,
                       sOotPspMessageAssetNames[assetIndex]) == 0) {
                alreadyWritten = true;
                break;
            }
        }
        if (alreadyWritten || (identity->message[assetIndex].size == 0) ||
            (dmaIndex == OOT_PSP_ROM_PROFILE_DMA_MISSING) || (dmaIndex >= rom->profile->dmadataCount)) {
            continue;
        }
        dma = &dmaEntries[dmaIndex];
        assetSize = dma->vromEnd - dma->vromStart;
        if (dma->romEnd == 0) {
            if (!OotPspAssetBuilder_CopyUncompressedAsset(rom, outputFd, dma->romStart,
                                                           identity->message[assetIndex].fileOffset,
                                                           assetSize)) {
                OotPspAssetBuilder_ShowError("Could not copy regional message data");
                goto cleanup;
            }
        } else {
            if (!OotPspAssetBuilder_LoadAsset(rom, dma, &data, assetSize) ||
                !OotPspAssetBuilder_WriteAt(outputFd, identity->message[assetIndex].fileOffset,
                                             data, assetSize)) {
                free(data);
                OotPspAssetBuilder_ShowError("Could not extract regional message data");
                goto cleanup;
            }
            free(data);
        }
    }
    sceIoClose(outputFd);
    outputFd = -1;
    sceIoRemove(outputPath);
    if (sceIoRename(tempPath, outputPath) < 0) {
        OotPspAssetBuilder_ShowError("Could not finish the asset file");
        goto cleanup;
    }
    identity->profile = rom->profile;
    memcpy(identity->digest, rom->digest, sizeof(identity->digest));
    identity->packedSize = packedSize;
    identity->codeSize = codeSize;
    if (!OotPspAssetBuilder_ActivateLayout(identity)) {
        OotPspAssetBuilder_ShowError("Could not activate ROM asset layout");
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
    char identityBuffer[384];
    char identityTempBuffer[384];
    char romBuffer[384];
    char directoryBuffer[384];
    const char* outputPath = OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_PATH, outputBuffer, sizeof(outputBuffer));
    const char* tempPath = OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_TEMP_PATH, tempBuffer, sizeof(tempBuffer));
    const char* identityPath =
        OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_ID_PATH, identityBuffer, sizeof(identityBuffer));
    const char* identityTempPath =
        OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_ID_TEMP_PATH, identityTempBuffer, sizeof(identityTempBuffer));
    const char* directoryPath;
    OotPspAssetIdentity identity;
    OotPspRomImage rom;
    SceUID romFd;
    s32 ok;

    if (gOotPspExternalAssetCount == 0) {
        return false;
    }
    if (OotPspAssetBuilder_LoadIdentity(identityPath, &identity) &&
        OotPspAssetBuilder_FileHasSize(outputPath, identity.packedSize) &&
        OotPspAssetBuilder_ActivateLayout(&identity)) {
        OotPspRomProfiles_SetActive(identity.profile, identity.digest);
        return true;
    }
    sOotPspAssetBuilderProgress = 0;
    sOotPspAssetBuilderErrorShown = false;
    OotPspAssetBuilder_ShowProgress(0, "Checking Ocarina of Time ROM");

    directoryPath = OotPsp_ResolveRootPath("data", directoryBuffer, sizeof(directoryBuffer));
    sceIoMkdir(directoryPath, 0777);
    directoryPath = OotPsp_ResolveRootPath("data/segments", directoryBuffer, sizeof(directoryBuffer));
    sceIoMkdir(directoryPath, 0777);

    romFd = OotPspAssetBuilder_OpenRom(romBuffer, sizeof(romBuffer));
    if (romFd < 0) {
        OotPspAssetBuilder_ShowError(romFd == -2 ? "Multiple .z64 files: keep one ROM in data" :
                                                   "ROM not found in the data directory");
        sceKernelDelayThread(3000000);
        return false;
    }
    ok = OotPspAssetBuilder_IdentifyRom(romFd, &rom);
    if (ok) {
        char status[64];

        snprintf(status, sizeof(status), "Preparing %s assets", rom.profile->name);
        OotPspAssetBuilder_ShowProgress(100, status);
        ok = OotPspAssetBuilder_Build(&rom, outputPath, tempPath, &identity);
    }
    sceIoClose(romFd);
    if (ok && !OotPspAssetBuilder_WriteIdentity(identityPath, identityTempPath, &identity)) {
        sceIoRemove(outputPath);
        OotPspAssetBuilder_ShowError("Could not save the ROM asset identity");
        ok = false;
    }
    if (ok) {
        char status[64];

        snprintf(status, sizeof(status), "%s asset setup complete", identity.profile->name);
        OotPspAssetBuilder_ShowProgress(1000, status);
        sceKernelDelayThread(250000);
    } else {
        if (!sOotPspAssetBuilderErrorShown) {
            OotPspAssetBuilder_ShowError("Asset setup failed");
        }
        sceKernelDelayThread(3000000);
    }
    return ok;
}

#if !defined(OOT_PSP_UNPACKER_MODULE)
s32 OotPspAssetBuilder_ActivateExisting(const char* expectedProfileName) {
    char outputBuffer[384];
    char identityBuffer[384];
    const char* outputPath = OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_PATH, outputBuffer, sizeof(outputBuffer));
    const char* identityPath =
        OotPsp_ResolveRootPath(OOT_PSP_PACKED_ASSET_ID_PATH, identityBuffer, sizeof(identityBuffer));
    OotPspAssetIdentity identity;

    if (!OotPspAssetBuilder_LoadIdentity(identityPath, &identity) ||
        (expectedProfileName == NULL) || (strcmp(identity.profile->name, expectedProfileName) != 0) ||
        !OotPspAssetBuilder_FileHasSize(outputPath, identity.packedSize) ||
        !OotPspAssetBuilder_ActivateLayout(&identity)) {
        return false;
    }
    OotPspRomProfiles_SetActive(identity.profile, identity.digest);
    return true;
}
#endif
