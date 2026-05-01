#include "kanread.h"
#include "message_data_static.h"
#include "printf.h"
#include "segment_symbols.h"
#include "translation.h"
#include "versions.h"
#include "dma.h"
#include "font.h"
#include "message.h"
#if PLATFORM_PSP
#include "oot_psp_asset_loader.h"
#endif

#if PLATFORM_PSP
static void Font_SwapWideMessageBufferPsp(u16* buffer, s32 count) {
    s32 i;

    for (i = 0; i < count; i++) {
        u16 value = buffer[i];

        buffer[i] = (value << 8) | (value >> 8);
    }
}
#endif

/**
 * Loads a texture from kanji for the requested `character` into the character texture buffer
 * at `codePointIndex`. The value of `character` is the SHIFT-JIS encoding of the character.
 */
void Font_LoadCharWide(Font* font, u16 character, u16 codePointIndex) {
#if OOT_NTSC
    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex],
                     (uintptr_t)_kanjiSegmentRomStart + Kanji_OffsetFromShiftJIS(character), FONT_CHAR_TEX_SIZE,
                     "../z_kanfont.c", UNK_LINE);
#endif
}

/**
 * Loads a texture from nes_font_static for the requested `character` into the character texture buffer
 * at `codePointIndex`. The value of `character` is the ASCII codepoint subtract ' '/0x20.
 */
void Font_LoadChar(Font* font, u8 character, u16 codePointIndex) {
    s32 offset = character * FONT_CHAR_TEX_SIZE;

    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex], (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 93);
}

#if PLATFORM_IQUE
void Font_LoadCharCHN(Font* font, u16 character, u16 codePointIndex) {
    s32 offset = character * FONT_CHAR_TEX_SIZE;

    DMA_REQUEST_SYNC(&font->charTexBuf[codePointIndex], (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
}
#endif

/**
 * Loads a message box icon from message_static, such as the ending triangle/square or choice arrow into the
 * icon buffer.
 * The different icons are given in the MessageBoxIcon enum.
 */
void Font_LoadMessageBoxIcon(Font* font, u16 icon) {
    DMA_REQUEST_SYNC(font->iconBuf,
                     (uintptr_t)_message_staticSegmentRomStart + 4 * MESSAGE_STATIC_TEX_SIZE +
                         icon * FONT_CHAR_TEX_SIZE,
                     FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 100);
}

/**
 * Loads a full set of character textures based on their ordering in the message with text id 0xFFFC into
 * the font buffer.
 */
void Font_LoadOrderedFont(Font* font) {
    s32 size;
    s32 len;
    s32 codePointIndex;
    s32 fontBufIndex;
    u32 offset;
    const char* messageDataStart;
    u16* msgBufWide;
#if PLATFORM_PSP
    const OotPspMessageEntry* pspMessageEntry;
#endif

#if OOT_NTSC && !PLATFORM_IQUE
#if PLATFORM_PSP
    pspMessageEntry = OotPsp_FindMessageEntry(gOotPspJpnMessageEntries, gOotPspJpnMessageEntriesCount, 0xFFFC);
    if (pspMessageEntry == NULL) {
        PRINTF("oot-psp font message 0xFFFC missing\n");
        return;
    }

    font->msgOffset = pspMessageEntry->vromStart -
                      OotPsp_NormalizeVrom((uintptr_t)_jpn_message_data_staticSegmentRomStart);
    size = font->msgLength = pspMessageEntry->vromEnd - pspMessageEntry->vromStart;
#else
    messageDataStart = (const char*)_jpn_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_jpn - messageDataStart;
    size = font->msgLength = _message_0xFFFD_jpn - _message_0xFFFC_jpn;
#endif
    len = (u32)size / 2;
    DMA_REQUEST_SYNC(font->msgBufWide, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset, size,
                     "../z_kanfont.c", UNK_LINE);
#if PLATFORM_PSP
    Font_SwapWideMessageBufferPsp(font->msgBufWide, len);
#endif

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len);

    fontBufIndex = 0;
    for (codePointIndex = 0; font->msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {
        if (len < codePointIndex) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (font->msgBufWide[codePointIndex] != MESSAGE_WIDE_NEWLINE) {
            offset = Kanji_OffsetFromShiftJIS(font->msgBufWide[codePointIndex]);
            DMA_REQUEST_SYNC(&font->fontBuf[fontBufIndex * 8], (uintptr_t)_kanjiSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#elif OOT_PAL
#if PLATFORM_PSP
    pspMessageEntry = OotPsp_FindMessageEntry(gOotPspNesMessageEntries, gOotPspNesMessageEntriesCount, 0xFFFC);
    if (pspMessageEntry == NULL) {
        PRINTF("oot-psp font message 0xFFFC missing\n");
        return;
    }

    font->msgOffset = pspMessageEntry->vromStart -
                      OotPsp_NormalizeVrom((uintptr_t)_nes_message_data_staticSegmentRomStart);
    size = font->msgLength = pspMessageEntry->vromEnd - pspMessageEntry->vromStart;
#else
    messageDataStart = (const char*)_nes_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_nes - messageDataStart;
    size = font->msgLength = _message_0xFFFD_nes - _message_0xFFFC_nes;
#endif
    len = size;
    DMA_REQUEST_SYNC(font->msgBuf, (uintptr_t)_nes_message_data_staticSegmentRomStart + font->msgOffset, len,
                     "../z_kanfont.c", 122);

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len * 1);

    fontBufIndex = 0;
    for (codePointIndex = 0; font->msgBuf[codePointIndex] != MESSAGE_END; codePointIndex++) {
        if (codePointIndex > (len * 1)) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (font->msgBuf[codePointIndex] != MESSAGE_NEWLINE) {
            PRINTF("nes_mes_buf[%d]=%d\n", codePointIndex, font->msgBuf[codePointIndex]);

            offset = (font->msgBuf[codePointIndex] - ' ') * FONT_CHAR_TEX_SIZE;
            DMA_REQUEST_SYNC(font->fontBuf + fontBufIndex * 8, (uintptr_t)_nes_font_staticSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", 134);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#elif PLATFORM_IQUE
#if PLATFORM_PSP
    pspMessageEntry = OotPsp_FindMessageEntry(gOotPspJpnMessageEntries, gOotPspJpnMessageEntriesCount, 0xFFFC);
    if (pspMessageEntry == NULL) {
        PRINTF("oot-psp font message 0xFFFC missing\n");
        return;
    }

    font->msgOffset = pspMessageEntry->vromStart -
                      OotPsp_NormalizeVrom((uintptr_t)_jpn_message_data_staticSegmentRomStart);
    size = font->msgLength = pspMessageEntry->vromEnd - pspMessageEntry->vromStart;
#else
    messageDataStart = (const char*)_jpn_message_data_staticSegmentStart;
    font->msgOffset = _message_0xFFFC_jpn - messageDataStart;
    size = font->msgLength = _message_0xFFFD_jpn - _message_0xFFFC_jpn;
#endif
    len = (u32)size / 2;
    DMA_REQUEST_SYNC(font->msgBufWide, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset, size,
                     "../z_kanfont.c", UNK_LINE);
#if PLATFORM_PSP
    Font_SwapWideMessageBufferPsp(font->msgBufWide, len);
#endif

    PRINTF("msg_data=%x,  msg_data0=%x   jj=%x\n", font->msgOffset, font->msgLength, len);

    // Workaround for EGCS internal compiler error (see docs/compilers.md)
    msgBufWide = font->msgBufWide;
    fontBufIndex = 0;
    for (codePointIndex = 0; msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {
        if (len < codePointIndex) {
            PRINTF(T("ＥＲＲＯＲ！！  エラー！！！  error───！！！！\n", "ERROR!!  Error!!!  error───!!!!\n"));
            return;
        }

        if (msgBufWide[codePointIndex] != MESSAGE_WIDE_NEWLINE) {
            offset = Kanji_OffsetFromShiftJIS(msgBufWide[codePointIndex]);
            DMA_REQUEST_SYNC(&font->fontBuf[fontBufIndex * 8], (uintptr_t)_kanjiSegmentRomStart + offset,
                             FONT_CHAR_TEX_SIZE, "../z_kanfont.c", UNK_LINE);
            fontBufIndex += FONT_CHAR_TEX_SIZE / 8;
        }
    }
#endif
}
