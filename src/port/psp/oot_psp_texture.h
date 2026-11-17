#ifndef OOT_PSP_TEXTURE_H
#define OOT_PSP_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

/* Compiler u64 initializers and legacy packed textures store each word in
 * little-endian order. Convert at the input boundary, before publishing bytes
 * to the game or renderer. The last word may be shorter than eight bytes. */
static inline void OotPsp_TextureWordsToBytes(uint8_t* data, size_t size) {
    size_t offset;

    for (offset = 0; offset < size; offset += 8) {
        size_t count = size - offset < 8 ? size - offset : 8;
        size_t i;

        for (i = 0; i < count / 2; i++) {
            uint8_t value = data[offset + i];

            data[offset + i] = data[offset + count - 1 - i];
            data[offset + count - 1 - i] = value;
        }
    }
}

#endif
