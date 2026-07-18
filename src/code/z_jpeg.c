#include "jpeg.h"

#include "array_count.h"
#include "attributes.h"
#include "gfx.h"
#include "printf.h"
#include "sys_ucode.h"
#include "terminal.h"
#include "translation.h"
#include "ultra64.h"

#if defined(TARGET_PSP)
#include "oot_psp_asset_loader.h"

#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <pspkernel.h>
#include <string.h>
#endif

#define MARKER_ESCAPE 0x00
#define MARKER_SOI 0xD8
#define MARKER_SOF 0xC0
#define MARKER_DHT 0xC4
#define MARKER_DQT 0xDB
#define MARKER_DRI 0xDD
#define MARKER_SOS 0xDA
#define MARKER_APP0 0xE0
#define MARKER_APP1 0xE1
#define MARKER_APP2 0xE2
#define MARKER_COM 0xFE
#define MARKER_EOI 0xD9

typedef enum JpegByteLayout {
    JPEG_BYTE_LAYOUT_INVALID,
    JPEG_BYTE_LAYOUT_DIRECT,
    JPEG_BYTE_LAYOUT_PSP_TEXTURE_WORDS,
} JpegByteLayout;

static u8 Jpeg_ReadByteWithLayout(const u8* data, size_t offset, JpegByteLayout layout) {
#if defined(TARGET_PSP)
    if (layout == JPEG_BYTE_LAYOUT_PSP_TEXTURE_WORDS) {
        return data[offset ^ 7U];
    }
#else
    (void)layout;
#endif

    return data[offset];
}

static JpegByteLayout Jpeg_GetByteLayout(const u8* data) {
    if ((data[0] == 0xFF) && (data[1] == MARKER_SOI) && (data[2] == 0xFF)) {
        return JPEG_BYTE_LAYOUT_DIRECT;
    }

#if defined(TARGET_PSP)
    if ((data[7] == 0xFF) && (data[6] == MARKER_SOI) && (data[5] == 0xFF)) {
        return JPEG_BYTE_LAYOUT_PSP_TEXTURE_WORDS;
    }
#endif

    return JPEG_BYTE_LAYOUT_INVALID;
}

s32 Jpeg_IsJpeg(void* data) {
    return Jpeg_GetByteLayout(data) != JPEG_BYTE_LAYOUT_INVALID;
}

/**
 * Configures and schedules a JPEG decoder task and waits for it to finish.
 */
void Jpeg_ScheduleDecoderTask(JpegContext* ctx) {
    static OSTask sJpegTask = {
        M_NJPEGTASK,                     // type
        0,                               // flags
        NULL,                            // ucode_boot
        0,                               // ucode_boot_size
        njpgdspMainTextStart,            // ucode
        SP_UCODE_SIZE,                   // ucode_size
        njpgdspMainDataStart,            // ucode_data
        SP_UCODE_DATA_SIZE,              // ucode_data_size
        NULL,                            // dram_stack
        0,                               // dram_stack_size
        NULL,                            // output_buff
        NULL,                            // output_buff_size
        NULL,                            // data_ptr
        sizeof(JpegTaskData),            // data_size
        NULL,                            // yield_data_ptr
        sizeof(ctx->workBuf->yieldData), // yield_data_size
    };

    JpegWork* workBuf = ctx->workBuf;
    s32 pad[2];

    workBuf->taskData.address = OS_K0_TO_PHYSICAL(&workBuf->data);
    workBuf->taskData.mode = ctx->mode;
    workBuf->taskData.mbCount = 4;
    workBuf->taskData.qTableYPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableY);
    workBuf->taskData.qTableUPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableU);
    workBuf->taskData.qTableVPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableV);

    sJpegTask.t.flags = 0;
    sJpegTask.t.ucode_boot = SysUcode_GetUCodeBoot();
    sJpegTask.t.ucode_boot_size = SysUcode_GetUCodeBootSize();
    sJpegTask.t.yield_data_ptr = workBuf->yieldData;
    sJpegTask.t.data_ptr = (u64*)&workBuf->taskData;

    ctx->scTask.next = NULL;
    ctx->scTask.flags = OS_SC_NEEDS_RSP;
    ctx->scTask.msgQueue = &ctx->mq;
    ctx->scTask.msg = NULL;
    ctx->scTask.framebuffer = NULL;
    ctx->scTask.list = sJpegTask;

    osSendMesg(&gScheduler.cmdQueue, (OSMesg)&ctx->scTask, OS_MESG_BLOCK);
    Sched_Notify(&gScheduler);
    osRecvMesg(&ctx->mq, NULL, OS_MESG_BLOCK);
}

/**
 * Copies a 16x16 block of decoded image data to the Z-buffer.
 */
void Jpeg_CopyToZbuffer(u16* src, u16* zbuffer, s32 x, s32 y) {
    u16* dst = zbuffer + (((y * SCREEN_WIDTH) + x) * 16);
    s32 i;

    for (i = 0; i < 16; i++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
        dst[8] = src[8];
        dst[9] = src[9];
        dst[10] = src[10];
        dst[11] = src[11];
        dst[12] = src[12];
        dst[13] = src[13];
        dst[14] = src[14];
        dst[15] = src[15];

        src += 16;
        dst += SCREEN_WIDTH;
    }
}

/**
 * Reads an u16 from a possibly unaligned address in memory.
 *
 * Replaces unaligned 16-bit reads with a pair of aligned reads, allowing for reading the possibly
 * unaligned values in JPEG header files.
 */
u16 Jpeg_GetUnalignedU16(u8* ptr) {
    if (((uintptr_t)ptr & 1) == 0) {
        // Read the value normally if it's aligned to a 16-bit address.
        return *(u16*)ptr;
    } else {
        // Read unaligned values using two separate aligned memory accesses when it's not.
        return *(u16*)(ptr - 1) << 8 | (*(u16*)(ptr + 1) >> 8);
    }
}

/**
 * Parses the markers in the JPEG file, storing information such as the pointer to the image data
 * in `ctx` for later processing.
 */
void Jpeg_ParseMarkers(u8* ptr, JpegContext* ctx) {
    u32 exit = false;

    ctx->dqtCount = 0;
    ctx->dhtCount = 0;

    while (true) {
        if (exit) {
            break;
        }

        // 0xFF indicates the start of a JPEG marker, so look for the next.
        if (*ptr++ == 0xFF) {
            switch (*ptr++) {
                case MARKER_ESCAPE: {
                    // Compressed value 0xFF is stored as 0xFF00 to escape it, so ignore it.
                    break;
                }
                case MARKER_SOI: {
                    // Start of Image
                    PRINTF("MARKER_SOI\n");
                    break;
                }
                case MARKER_APP0: {
                    // Application marker for JFIF
                    PRINTF("MARKER_APP0 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_APP1: {
                    // Application marker for EXIF
                    PRINTF("MARKER_APP1 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_APP2: {
                    PRINTF("MARKER_APP2 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DQT: {
                    // Define Quantization Table, stored for later processing
                    PRINTF("MARKER_DQT %d %d %02x\n", ctx->dqtCount, Jpeg_GetUnalignedU16(ptr), ptr[2]);
                    ctx->dqtPtr[ctx->dqtCount++] = ptr + 2;
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DHT: {
                    // Define Huffman Table, stored for later processing
                    PRINTF("MARKER_DHT %d %d %02x\n", ctx->dhtCount, Jpeg_GetUnalignedU16(ptr), ptr[2]);
                    ctx->dhtPtr[ctx->dhtCount++] = ptr + 2;
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DRI: {
                    // Define Restart Interval
                    PRINTF("MARKER_DRI %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_SOF: {
                    // Start of Frame, stores important metadata of the image.
                    // Only used for extracting the sampling factors (ctx->mode).
                    PRINTF(T("MARKER_SOF   %d "
                             "精度%02x 垂直%d 水平%d compo%02x "
                             "(1:Y)%d (H0=2,V0=1(422) or 2(420))%02x (量子化テーブル)%02x "
                             "(2:Cb)%d (H1=1,V1=1)%02x (量子化テーブル)%02x "
                             "(3:Cr)%d (H2=1,V2=1)%02x (量子化テーブル)%02x\n",
                             "MARKER_SOF   %d "
                             "accuracy%02x vertical%d horizontal%d compo%02x "
                             "(1:Y)%d (H0=2,V0=1(422) or 2(420))%02x (quantization tables)%02x "
                             "(2:Cb)%d (H1=1,V1=1)%02x (quantization tables)%02x "
                             "(3:Cr)%d (H2=1,V2=1)%02x (quantization tables)%02x\n"),
                           Jpeg_GetUnalignedU16(ptr),
                           // precision, height, width, component count (assumed to be 3)
                           ptr[2], Jpeg_GetUnalignedU16(ptr + 3), Jpeg_GetUnalignedU16(ptr + 5), ptr[7],
                           //
                           ptr[8], ptr[9], ptr[10],   // Y component
                           ptr[11], ptr[12], ptr[13], // Cb component
                           ptr[14], ptr[15], ptr[16]  // Cr component
                    );

                    if (ptr[9] == 0x21) {
                        // component Y : V0 == 1
                        ctx->mode = 0;
                    } else if (ptr[9] == 0x22) {
                        // component Y : V0 == 2
                        ctx->mode = 2;
                    }
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_SOS: {
                    // Start of Scan marker, indicates the start of the image data.
                    PRINTF("MARKER_SOS %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    ctx->imageData = ptr;
                    break;
                }
                case MARKER_EOI: {
                    // End of Image
                    PRINTF("MARKER_EOI\n");
                    exit = true;
                    break;
                }
                default: {
                    PRINTF(T("マーカー不明 %02x\n", "Unknown marker %02x\n"), ptr[-1]);
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
            }
        }
    }
}

#if defined(TARGET_PSP)
#define JPEG_PSP_MAX_WIDTH SCREEN_WIDTH
#define JPEG_PSP_MAX_HEIGHT SCREEN_HEIGHT
#define JPEG_PSP_RGBA_SIZE (JPEG_PSP_MAX_WIDTH * JPEG_PSP_MAX_HEIGHT * 4)
#define JPEG_PSP_RGBA16_SIZE (JPEG_PSP_MAX_WIDTH * JPEG_PSP_MAX_HEIGHT * sizeof(u16))
#define JPEG_PSP_DECODED_IMAGE_MAX 64

static u8 sPspJpegInput[JPEG_PSP_RGBA16_SIZE] __attribute__((aligned(64)));
static u8 sPspJpegRgba[JPEG_PSP_RGBA_SIZE] __attribute__((aligned(64)));
static void* sPspDecodedImages[JPEG_PSP_DECODED_IMAGE_MAX];
static s32 sPspDecodedImageCount;

typedef struct JpegPspErrorManager {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} JpegPspErrorManager;

static void JpegPsp_ErrorExit(j_common_ptr common) {
    JpegPspErrorManager* error = (JpegPspErrorManager*)common->err;

    (*common->err->format_message)(common, error->message);
    longjmp(error->jump, 1);
}

s32 JpegPsp_WasDecoded(void* data) {
    s32 i;

    for (i = 0; i < sPspDecodedImageCount; i++) {
        if (sPspDecodedImages[i] == data) {
            return true;
        }
    }

    return false;
}

static void JpegPsp_RecordDecodedImage(void* data) {
    if (JpegPsp_WasDecoded(data)) {
        return;
    }

    if (sPspDecodedImageCount < JPEG_PSP_DECODED_IMAGE_MAX) {
        sPspDecodedImages[sPspDecodedImageCount++] = data;
    }
}

static s32 JpegPsp_CopyCompressedImage(const u8* data, JpegByteLayout layout, u8* out, size_t outCapacity,
                                       size_t* outSize) {
    u8 prev = 0;

    for (size_t i = 0; i < outCapacity; i++) {
        u8 cur = Jpeg_ReadByteWithLayout(data, i, layout);

        out[i] = cur;
        if ((prev == 0xFF) && (cur == MARKER_EOI)) {
            *outSize = i + 1;
            return true;
        }

        prev = cur;
    }

    return false;
}

static u16 JpegPsp_ReadBe16(const u8* data) {
    return (u16)((data[0] << 8) | data[1]);
}

static s32 JpegPsp_ReadDimensions(const u8* data, size_t size, int* width, int* height) {
    size_t pos = 2;

    while (pos + 4 <= size) {
        u8 marker;
        u16 markerSize;

        while ((pos < size) && (data[pos] != 0xFF)) {
            pos++;
        }

        while ((pos < size) && (data[pos] == 0xFF)) {
            pos++;
        }

        if (pos >= size) {
            break;
        }

        marker = data[pos++];
        if ((marker == MARKER_SOI) || (marker == MARKER_EOI)) {
            continue;
        }

        if (pos + 2 > size) {
            break;
        }

        markerSize = JpegPsp_ReadBe16(&data[pos]);
        if ((markerSize < 2) || (pos + markerSize > size)) {
            break;
        }

        if (marker == MARKER_SOF) {
            if (markerSize < 7) {
                break;
            }

            *height = JpegPsp_ReadBe16(&data[pos + 3]);
            *width = JpegPsp_ReadBe16(&data[pos + 5]);
            return true;
        }

        if (marker == MARKER_SOS) {
            break;
        }

        pos += markerSize;
    }

    return false;
}

static void JpegPsp_WriteOutputByte(u8* dst, size_t offset, u8 value, s32 textureWords) {
    if (textureWords) {
        const void* mapped;

        if (OotPsp_MapNativeExternalTextureByte(dst + offset, &mapped)) {
            dst = (u8*)mapped;
            offset = 0;
        } else {
            offset ^= 7U;
        }
    }

    dst[offset] = value;
}

static void JpegPsp_WriteOutputU16(u8* dst, size_t offset, u16 value, s32 textureWords) {
    JpegPsp_WriteOutputByte(dst, offset, value >> 8, textureWords);
    JpegPsp_WriteOutputByte(dst, offset + 1, value & 0xFF, textureWords);
}

static s32 JpegPsp_Decode(void* data, void* output) {
    const u8* src = data;
    u8* dst = output;
    JpegByteLayout layout = Jpeg_GetByteLayout(src);
    struct jpeg_decompress_struct decoder;
    JpegPspErrorManager error;
    JSAMPROW row[1];
    size_t jpegSize = 0;
    int width;
    int height;
    int decodedWidth;
    int decodedHeight;
    size_t rgbStride;
    s32 textureWords;

    if (layout == JPEG_BYTE_LAYOUT_INVALID) {
        return -1;
    }

    if (!JpegPsp_CopyCompressedImage(src, layout, sPspJpegInput, sizeof(sPspJpegInput), &jpegSize)) {
        osSyncPrintf("oot-psp jpeg eoi not found\n");
        return -1;
    }

    if (!JpegPsp_ReadDimensions(sPspJpegInput, jpegSize, &width, &height)) {
        osSyncPrintf("oot-psp jpeg dimensions not found\n");
        return -1;
    }

    if ((width <= 0) || (height <= 0) || (width > JPEG_PSP_MAX_WIDTH) || (height > JPEG_PSP_MAX_HEIGHT)) {
        osSyncPrintf("oot-psp jpeg unsupported dimensions %dx%d\n", width, height);
        return -1;
    }

    memset(&decoder, 0, sizeof(decoder));
    memset(&error, 0, sizeof(error));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = JpegPsp_ErrorExit;
    if (setjmp(error.jump) != 0) {
        if (decoder.mem != NULL) {
            jpeg_destroy_decompress(&decoder);
        }
        osSyncPrintf("oot-psp jpeg software decode failed: %s\n", error.message);
        return -1;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, sPspJpegInput, jpegSize);
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);

    decodedWidth = decoder.output_width;
    decodedHeight = decoder.output_height;
    if ((decodedWidth != width) || (decodedHeight != height) || (decoder.output_components != 3) ||
        (decodedWidth <= 0) || (decodedHeight <= 0) || (decodedWidth > JPEG_PSP_MAX_WIDTH) ||
        (decodedHeight > JPEG_PSP_MAX_HEIGHT)) {
        osSyncPrintf("oot-psp jpeg bad decoded dimensions %dx%d\n", decodedWidth, decodedHeight);
        jpeg_abort_decompress(&decoder);
        jpeg_destroy_decompress(&decoder);
        return -1;
    }

    rgbStride = (size_t)decodedWidth * 3;
    while (decoder.output_scanline < decoder.output_height) {
        row[0] = &sPspJpegRgba[(size_t)decoder.output_scanline * rgbStride];
        jpeg_read_scanlines(&decoder, row, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);

    /*
     * JPEG room images are stored as texture-word assets while compressed, but the
     * registered subrange only covers the compressed bytes. The decoded 320x240
     * image still has to preserve that byte layout because the PSP texture importer
     * reads the room segment through the same mapped texture-word path.
     */
    textureWords = (layout == JPEG_BYTE_LAYOUT_PSP_TEXTURE_WORDS) ||
                   OotPsp_IsNativeExternalTextureRange(output, JPEG_PSP_RGBA16_SIZE);
    memset(dst, 0, JPEG_PSP_RGBA16_SIZE);

    for (int y = 0; y < decodedHeight; y++) {
        for (int x = 0; x < decodedWidth; x++) {
            size_t rgbaOff = ((size_t)y * decodedWidth + x) * 3;
            size_t outOff = ((size_t)y * SCREEN_WIDTH + x) * sizeof(u16);
            u8 r = sPspJpegRgba[rgbaOff + 0];
            u8 g = sPspJpegRgba[rgbaOff + 1];
            u8 b = sPspJpegRgba[rgbaOff + 2];
            u16 rgba16 = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;

            JpegPsp_WriteOutputU16(dst, outOff, rgba16, textureWords);
        }
    }

    sceKernelDcacheWritebackRange(dst, JPEG_PSP_RGBA16_SIZE);
    if ((layout == JPEG_BYTE_LAYOUT_PSP_TEXTURE_WORDS) ||
        OotPsp_IsNativeExternalTextureRange(output, JPEG_PSP_RGBA16_SIZE)) {
        JpegPsp_RecordDecodedImage(output);
    }
    return 0;
}
#else
s32 JpegPsp_WasDecoded(void* data) {
    (void)data;
    return false;
}
#endif

s32 Jpeg_Decode(void* data, void* zbuffer, void* work, u32 workSize) {
#if defined(TARGET_PSP)
    (void)work;
    (void)workSize;

    return JpegPsp_Decode(data, zbuffer);
#else
    s32 y;
    s32 x;
    u32 j;
    u32 i;
    JpegContext ctx;
    JpegHuffmanTable hTables[4];
    JpegDecoder decoder;
    JpegDecoderState state;
    JpegWork* workBuff;
    UNUSED_NDEBUG OSTime diff;
    OSTime time;
    OSTime curTime;

    workBuff = work;

    time = osGetTime();
    // (?) I guess MB_SIZE=0x180, PROC_OF_MBS=5 which means data is not a part of JpegWork
    ASSERT(workSize >= sizeof(JpegWork), "worksize >= sizeof(JPEGWork) + MB_SIZE * (PROC_OF_MBS - 1)", "../z_jpeg.c",
           527);

    osCreateMesgQueue(&ctx.mq, &ctx.msg, 1);
    Sched_FlushTaskQueue();

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** fifoバッファの同期待ち time = %6.3f ms ***\n",
             "*** Wait for synchronization of fifo buffer time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    ctx.workBuf = workBuff;
    Jpeg_ParseMarkers(data, &ctx);

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 各セグメントのマーカーのチェック time = %6.3f ms ***\n",
             "*** Check markers for each segment time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    switch (ctx.dqtCount) {
        case 1:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 3);
            break;
        case 2:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableU, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableV, 1);
            break;
        case 3:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableU, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[2], &workBuff->qTableV, 1);
            break;
        default:
            return -1;
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 量子化テーブル作成 time = %6.3f ms ***\n", "*** Create quantization table time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    switch (ctx.dhtCount) {
        case 1:
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[0], &hTables[0], workBuff->codesLengths, workBuff->codes, 4)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            break;
        case 4:
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[0], &hTables[0], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[1], &hTables[1], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[2], &hTables[2], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[3], &hTables[3], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            break;
        default:
            return -1;
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** ハフマンテーブル作成 time = %6.3f ms ***\n", "*** Huffman table creation time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    decoder.imageData = ctx.imageData;
    decoder.mode = ctx.mode;
    decoder.unk_05 = 2;
    decoder.hTablePtrs[0] = &hTables[0];
    decoder.hTablePtrs[1] = &hTables[1];
    decoder.hTablePtrs[2] = &hTables[2];
    decoder.hTablePtrs[3] = &hTables[3];
    decoder.unk_18 = 0;

    x = y = 0;
    for (i = 0; i < 300; i += 4) {
        if (JpegDecoder_Decode(&decoder, (u16*)workBuff->data, 4, i != 0, &state)) {
            PRINTF_COLOR_RED();
            PRINTF("Error : Can't decode jpeg\n");
            PRINTF_RST();
        } else {
            Jpeg_ScheduleDecoderTask(&ctx);
            osInvalDCache(&workBuff->data, sizeof(workBuff->data[0]));

            for (j = 0; j < ARRAY_COUNT(workBuff->data); j++) {
                Jpeg_CopyToZbuffer(workBuff->data[j], zbuffer, x, y);
                x++;

                if (x >= 20) {
                    x = 0;
                    y++;
                }
            }
        }
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 展開 & 描画 time = %6.3f ms ***\n", "*** Unfold & draw time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    return 0;
#endif
}
