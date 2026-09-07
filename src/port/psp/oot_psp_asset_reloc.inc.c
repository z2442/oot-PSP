/* Pointer discovery for the first-boot converter. Only fields owned by a
 * parsed structure are relocations. In particular, two animation indices or
 * texture pixels can look exactly like a segmented address. */
static void OotPspAssetBuilder_MarkPointer(u8* fields, size_t size, size_t offset) {
    if ((offset % 4 == 0) && (offset <= size) && (size - offset >= 4)) {
        fields[offset / 4] |= 1;
    }
}

static void OotPspAssetBuilder_MarkDListPointers(
    const u8* source, size_t size, size_t offset, u8* fields, unsigned depth, u8 segment) {
    size_t cursor;

    if ((depth >= 64) || (offset % 8 != 0) || (offset >= size) || (fields[offset / 4] & 2)) {
        return;
    }
    fields[offset / 4] |= 2;
    for (cursor = offset; cursor <= size && size - cursor >= 8; cursor += 8) {
        u32 word0 = OotPspAssetBuilder_ReadBe32(source + cursor);
        u32 pointer = OotPspAssetBuilder_ReadBe32(source + cursor + 4);
        u8 opcode = word0 >> 24;

        if (opcode == 0xDF || opcode == 0xB8) {
            return;
        }
        switch (opcode) {
            case 0x01: case 0x04: /* vertices (F3DEX2 / F3DEX) */
            case 0xDA: case 0xDC: /* matrices / lights */
            case 0xFD:           /* texture image */
                OotPspAssetBuilder_MarkPointer(fields, size, cursor + 4);
                break;
            case 0xDE: case 0x06:
                OotPspAssetBuilder_MarkPointer(fields, size, cursor + 4);
                if ((pointer >> 24) == segment) {
                    OotPspAssetBuilder_MarkDListPointers(source, size, pointer & 0xFFFFFF, fields, depth + 1, segment);
                }
                if (word0 & 0x00010000U) {
                    return;
                }
                break;
        }
    }
}

static void OotPspAssetBuilder_MarkCollisionPointers(
    const u8* source, size_t size, size_t offset, u8* fields) {
    static const u8 members[] = {16, 24, 28, 32, 40};
    size_t i;

    if (offset > size || size - offset < 44) return;
    for (i = 0; i < sizeof(members); i++) {
        OotPspAssetBuilder_MarkPointer(fields, size, offset + members[i]);
    }
    /* Camera records contain a pointer to Vec3s data after two s16 fields. */
    {
        u32 camera = OotPspAssetBuilder_ReadBe32(source + offset + 32);
        size_t cursor = camera & 0xFFFFFF;
        if ((camera >> 24) >= 2 && (camera >> 24) <= 6) {
            while (cursor <= size && size - cursor >= 8) {
                u32 points = OotPspAssetBuilder_ReadBe32(source + cursor + 4);
                if ((points >> 24) != (camera >> 24) || (points & 0xFFFFFF) >= size) break;
                OotPspAssetBuilder_MarkPointer(fields, size, cursor + 4);
                cursor += 8;
            }
        }
    }
}

static void OotPspAssetBuilder_MarkRoomShapePointers(
    const u8* source, size_t size, size_t offset, u8* fields) {
    size_t count, stride, entry, i;
    u8 type;
    u32 entries;

    if (offset > size || size - offset < 8) return;
    type = source[offset];
    if (type > 2) return;
    entries = OotPspAssetBuilder_ReadBe32(source + offset + 4);
    entry = entries & 0xFFFFFF;
    count = type == 1 ? 1 : source[offset + 1];
    stride = type == 2 ? 16 : 8;
    OotPspAssetBuilder_MarkPointer(fields, size, offset + 4);
    if (type != 1) OotPspAssetBuilder_MarkPointer(fields, size, offset + 8);
    if (entry <= size && count <= (size - entry) / stride) {
        for (i = 0; i < count; i++) {
            size_t field = entry + i * stride + (type == 2 ? 8 : 0);
            OotPspAssetBuilder_MarkPointer(fields, size, field);
            OotPspAssetBuilder_MarkPointer(fields, size, field + 4);
            for (size_t slot = 0; slot < 2; slot++) {
                u32 pointer = OotPspAssetBuilder_ReadBe32(source + field + slot * 4);
                if (pointer && (pointer >> 24) == (entries >> 24))
                    OotPspAssetBuilder_MarkDListPointers(source, size,
                        pointer & 0xFFFFFF, fields, 0, entries >> 24);
            }
        }
    }
    if (type == 1 && size - offset >= 16) {
        if (source[offset + 1] == 1) {
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 8);
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 16);
        } else if (source[offset + 1] == 2) {
            count = source[offset + 8];
            entry = OotPspAssetBuilder_ReadBe32(source + offset + 12) & 0xFFFFFF;
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 12);
            if (entry <= size && count <= (size - entry) / 28) {
                for (i = 0; i < count; i++) {
                    OotPspAssetBuilder_MarkPointer(fields, size, entry + i * 28 + 4);
                    OotPspAssetBuilder_MarkPointer(fields, size, entry + i * 28 + 12);
                }
            }
        }
    }
}

static void OotPspAssetBuilder_MarkScenePointers(
    const u8* source, size_t size, size_t offset, size_t length, u8* fields) {
    size_t cursor;

    if (offset > size || length > size - offset) return;
    for (cursor = offset; cursor + 8 <= offset + length; cursor += 8) {
        u8 command = source[cursor];
        u32 pointer = OotPspAssetBuilder_ReadBe32(source + cursor + 4);
        size_t payload = pointer & 0xFFFFFF;
        if (command == 0x14) break;
        if ((pointer >> 24) < 2 || (pointer >> 24) > 6 || payload >= size) continue;
        switch (command) {
            case 0: case 1: case 2: case 3: case 4: case 6: case 0x0A:
            case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            case 0x13: case 0x17: case 0x18:
                OotPspAssetBuilder_MarkPointer(fields, size, cursor + 4);
                break;
            default:
                continue;
        }
        if (command == 3) OotPspAssetBuilder_MarkCollisionPointers(source, size, payload, fields);
        if (command == 0x0A) OotPspAssetBuilder_MarkRoomShapePointers(source, size, payload, fields);
        if (command == 0x0D || command == 2) OotPspAssetBuilder_MarkPointer(fields, size, payload + 4);
        if (command == 0x18) {
            size_t slot;
            for (slot = 0; slot < OOT_PSP_SCENE_LAYOUT_ALT_MAX; slot++) {
                size_t field = payload + slot * 4;
                size_t child, childSize;
                u32 childPointer;
                if (field > size || size - field < 4) break;
                childPointer = OotPspAssetBuilder_ReadBe32(source + field);
                if (childPointer == 0) continue;
                child = childPointer & 0xFFFFFF;
                if ((childPointer >> 24) != (pointer >> 24) || child >= size ||
                    !OotPspAssetBuilder_FindSceneCommandsSize(source + child, size - child, &childSize)) break;
                OotPspAssetBuilder_MarkPointer(fields, size, field);
            }
        }
    }
}

/* Values 0..3 describe copies terminated by a format marker. Higher values
 * describe fixed-size records and their pointer fields (see profile generator). */
static void OotPspAssetBuilder_MarkResourcePointers(
    const u8* source, size_t size, size_t offset, size_t length, unsigned kind, u8* fields, u8 segment) {
    size_t i;
    if (offset > size || length > size - offset) return;
    switch (kind) {
        case 1: OotPspAssetBuilder_MarkDListPointers(source, size, offset, fields, 0, segment); break;
        case 2: OotPspAssetBuilder_MarkScenePointers(source, size, offset, length, fields); break;
        case 4: case 14:
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 4);
            if (length >= 12) OotPspAssetBuilder_MarkPointer(fields, size, offset + 8);
            break;
        case 5: OotPspAssetBuilder_MarkPointer(fields, size, offset); break;
        case 6:
            for (i = 0; i + 4 <= length; i += 4) OotPspAssetBuilder_MarkPointer(fields, size, offset + i);
            break;
        case 7: OotPspAssetBuilder_MarkPointer(fields, size, offset + 8); break;
        case 8:
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 8);
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 12);
            break;
        case 9: OotPspAssetBuilder_MarkPointer(fields, size, offset + 12); break;
        case 10:
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 4);
            OotPspAssetBuilder_MarkPointer(fields, size, offset + 8);
            break;
        case 11: OotPspAssetBuilder_MarkCollisionPointers(source, size, offset, fields); break;
        case 12: case 15:
            for (i = 0; i + 8 <= length; i += 8) OotPspAssetBuilder_MarkPointer(fields, size, offset + i + 4);
            break;
        case 13:
            for (i = 0; i < 12; i += 4) OotPspAssetBuilder_MarkPointer(fields, size, offset + i);
            break;
    }
}
