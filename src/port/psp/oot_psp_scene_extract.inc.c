/* Scene records are discovered from the ROM's command graph. Graphics and
 * texture payloads keep their source offsets; no NTSC room recipe is applied
 * to a different room shape, actor count, or Master Quest display-list graph. */
typedef struct OotPspSceneExtract {
    const u8* source;
    u8* output;
    u8* visited;
    size_t size;
    u8 segment;
    s32 ok;
} OotPspSceneExtract;

static s32 OotPspScene_Range(OotPspSceneExtract* s, size_t offset, size_t count, size_t stride) {
    if (offset > s->size || (stride && count > (s->size - offset) / stride)) {
        s->ok = false;
        return false;
    }
    return true;
}

static void OotPspScene_Swap(OotPspSceneExtract* s, size_t offset, size_t count, size_t width) {
    size_t i, j;
    if (!OotPspScene_Range(s, offset, count, width)) return;
    for (i = 0; i < count; i++) {
        for (j = 0; j < width; j++) {
            s->output[offset + i * width + j] = s->source[offset + i * width + width - 1 - j];
            s->visited[offset + i * width + j] |= 8;
        }
    }
}

static size_t OotPspScene_Pointer(OotPspSceneExtract* s, size_t field) {
    u32 pointer;
    if (!OotPspScene_Range(s, field, 1, 4)) return s->size;
    pointer = OotPspAssetBuilder_ReadBe32(s->source + field);
    OotPspScene_Swap(s, field, 1, 4);
    if (!pointer) return s->size;
    if ((pointer >> 24) != s->segment || (pointer & 0xFFFFFF) >= s->size) {
        s->ok = false;
        return s->size;
    }
    return pointer & 0xFFFFFF;
}

static void OotPspScene_DList(OotPspSceneExtract* s, size_t offset, unsigned depth) {
    size_t cursor;
    if (offset == s->size) return;
    if (depth >= 64 || offset % 8 || !OotPspScene_Range(s, offset, 1, 8)) { s->ok = false; return; }
    if (s->visited[offset] & 1) return;
    s->visited[offset] |= 1;
    for (cursor = offset; OotPspScene_Range(s, cursor, 1, 8); cursor += 8) {
        u32 w0 = OotPspAssetBuilder_ReadBe32(s->source + cursor);
        u32 w1 = OotPspAssetBuilder_ReadBe32(s->source + cursor + 4);
        u8 op = w0 >> 24;
        size_t data = w1 & 0xFFFFFF, i;
        OotPspScene_Swap(s, cursor, 2, 4);
        if (op == 0xDF) return;
        if ((w1 >> 24) == s->segment) {
            if (op == 0xDE) OotPspScene_DList(s, data, depth + 1);
            else if (op == 1) {
                size_t count = (w0 >> 12) & 0xFF;
                if (OotPspScene_Range(s, data, count, 16))
                    for (i = 0; i < count; i++) OotPspScene_Swap(s, data + i * 16, 6, 2);
            } else if (op == 0xDA) OotPspScene_Swap(s, data, 32, 2);
            /* MOVEMEM lights contain byte colors/directions, not u32 fields. */
        }
        if (op == 0xDE && (w0 & 0x10000)) return;
    }
}

static void OotPspScene_Background(OotPspSceneExtract* s, size_t offset) {
    OotPspScene_Swap(s, offset, 3, 4);
    OotPspScene_Swap(s, offset + 12, 2, 2);
    OotPspScene_Swap(s, offset + 18, 2, 2);
}

static void OotPspScene_Mesh(OotPspSceneExtract* s, size_t offset) {
    size_t entries, count, stride, i;
    u8 type, amount;
    if (!OotPspScene_Range(s, offset, 1, 8)) return;
    type = s->source[offset]; amount = s->source[offset + 1];
    if (type > 2) { s->ok = false; return; }
    entries = OotPspScene_Pointer(s, offset + 4);
    count = type == 1 ? 1 : amount; stride = type == 2 ? 16 : 8;
    if (type != 1) OotPspScene_Swap(s, offset + 8, 1, 4); /* one-past-end pointer */
    if (!OotPspScene_Range(s, entries, count, stride)) return;
    for (i = 0; i < count; i++) {
        size_t field = entries + i * stride;
        if (type == 2) { OotPspScene_Swap(s, field, 4, 2); field += 8; }
        OotPspScene_DList(s, OotPspScene_Pointer(s, field), 0);
        OotPspScene_DList(s, OotPspScene_Pointer(s, field + 4), 0);
    }
    if (type == 1) {
        if (amount == 1) OotPspScene_Background(s, offset + 8);
        else if (amount == 2 && OotPspScene_Range(s, offset, 1, 16)) {
            count = s->source[offset + 8]; entries = OotPspScene_Pointer(s, offset + 12);
            if (!OotPspScene_Range(s, entries, count, 28)) return;
            for (i = 0; i < count; i++) {
                OotPspScene_Swap(s, entries + i * 28, 1, 2);
                OotPspScene_Background(s, entries + i * 28 + 4);
            }
        } else s->ok = false;
    }
}

static size_t OotPspScene_Collision(OotPspSceneExtract* s, size_t offset) {
    size_t vertices, polys, surfaces, cameras, water, count, i, surfaceCount = 0, cameraCount = 0, exits = 0;
    if (!OotPspScene_Range(s, offset, 1, 44)) return 0;
    OotPspScene_Swap(s, offset, 7, 2);
    vertices = OotPspScene_Pointer(s, offset + 16);
    count = OotPspAssetBuilder_ReadBe16(s->source + offset + 12);
    OotPspScene_Swap(s, vertices, count * 3, 2);
    count = OotPspAssetBuilder_ReadBe16(s->source + offset + 20);
    polys = OotPspScene_Pointer(s, offset + 24);
    OotPspScene_Swap(s, offset + 20, 1, 2);
    if (!OotPspScene_Range(s, polys, count, 16)) return 0;
    for (i = 0; i < count; i++) {
        size_t type = OotPspAssetBuilder_ReadBe16(s->source + polys + i * 16);
        if (type + 1 > surfaceCount) surfaceCount = type + 1;
    }
    OotPspScene_Swap(s, polys, count * 8, 2);
    surfaces = OotPspScene_Pointer(s, offset + 28);
    if (!OotPspScene_Range(s, surfaces, surfaceCount, 8)) return 0;
    for (i = 0; i < surfaceCount; i++) {
        u32 bits = OotPspAssetBuilder_ReadBe32(s->source + surfaces + i * 8);
        size_t camera = bits & 0xFF, exit = (bits >> 8) & 0x1F;
        if (camera + 1 > cameraCount) cameraCount = camera + 1;
        if (exit > exits) exits = exit;
    }
    OotPspScene_Swap(s, surfaces, surfaceCount * 2, 4);
    cameras = OotPspScene_Pointer(s, offset + 32);
    if (cameras != s->size && OotPspScene_Range(s, cameras, cameraCount, 8)) {
        for (i = 0; i < cameraCount; i++) {
            size_t field = cameras + i * 8, points;
            s16 pointCount = (s16)OotPspAssetBuilder_ReadBe16(s->source + field + 2);
            OotPspScene_Swap(s, field, 2, 2);
            points = OotPspScene_Pointer(s, field + 4);
            if (pointCount > 0 && points != s->size) OotPspScene_Swap(s, points, (size_t)pointCount * 3, 2);
        }
    }
    count = OotPspAssetBuilder_ReadBe16(s->source + offset + 36);
    OotPspScene_Swap(s, offset + 36, 1, 2);
    water = OotPspScene_Pointer(s, offset + 40);
    if (OotPspScene_Range(s, water, count, 16)) for (i = 0; i < count; i++) {
        OotPspScene_Swap(s, water + i * 16, 5, 2);
        OotPspScene_Swap(s, water + i * 16 + 12, 1, 4);
    }
    return exits;
}

static void OotPspScene_Cutscene(OotPspSceneExtract* s, size_t offset) {
    size_t length, cursor, i, count;
    if (offset >= s->size || !OotPspAssetBuilder_FindCutsceneSize(s->source + offset, s->size - offset, &length)) {
        s->ok = false; return;
    }
    OotPspScene_Swap(s, offset, 2, 4);
    cursor = offset + 8;
    while (cursor < offset + length) {
        u32 cmd = OotPspAssetBuilder_ReadBe32(s->source + cursor);
        OotPspScene_Swap(s, cursor, 1, 4); cursor += 4;
        if (cmd == 0xFFFFFFFF) return;
        if (cmd == 1 || cmd == 2 || cmd == 5 || cmd == 6 || cmd == 7 || cmd == 8) {
            OotPspScene_Swap(s, cursor, 4, 2); cursor += 8;
            do {
                s32 stop = s->source[cursor] == 0xFF || cmd == 7 || cmd == 8;
                OotPspScene_Swap(s, cursor + 2, 1, 2);
                OotPspScene_Swap(s, cursor + 4, 1, 4);
                OotPspScene_Swap(s, cursor + 8, 4, 2); cursor += 16;
                if (stop) break;
            } while (cursor < offset + length);
            continue;
        }
        count = OotPspAssetBuilder_ReadBe32(s->source + cursor);
        OotPspScene_Swap(s, cursor, 1, 4); cursor += 4;
        if (cmd == 45 || cmd == 1000) { OotPspScene_Swap(s, cursor, 4, 2); cursor += 8; continue; }
        for (i = 0; i < count; i++) {
            if (cmd == 19) { OotPspScene_Swap(s, cursor, 6, 2); cursor += 12; }
            else if (cmd == 9 || cmd == 140) {
                OotPspScene_Swap(s, cursor, 3, 2);
                if (cmd == 9) OotPspScene_Swap(s, cursor + 10, 1, 2);
                else OotPspScene_Swap(s, cursor + 8, 1, 4);
                cursor += 12;
            } else {
                size_t halfwords = (cmd == 3 || cmd == 4 || cmd == 86 || cmd == 87 || cmd == 124) ? 4 : 6;
                OotPspScene_Swap(s, cursor, halfwords, 2);
                if (cmd == 4) { s->output[cursor] = s->source[cursor]; s->output[cursor + 1] = s->source[cursor + 1]; }
                OotPspScene_Swap(s, cursor + halfwords * 2, (48 - halfwords * 2) / 4, 4);
                cursor += 48;
            }
        }
    }
}

static s32 OotPspScene_IsHeader(const u8* data, size_t size, size_t offset) {
    size_t cursor;
    for (cursor = offset; cursor <= size && size - cursor >= 8; cursor += 8) {
        if (data[cursor] > 0x19) return false;
        if (data[cursor] == 0x14) return true;
    }
    return false;
}

static void OotPspScene_Header(OotPspSceneExtract* s, size_t offset, unsigned depth) {
    size_t cursor, length, exitCount = 0, exitOffset = s->size;
    if (depth >= 32 || offset >= s->size || !OotPspScene_IsHeader(s->source, s->size, offset) ||
        !OotPspAssetBuilder_FindSceneCommandsSize(s->source + offset, s->size - offset, &length)) {
        s->ok = false; return;
    }
    if (s->visited[offset] & 2) return;
    s->visited[offset] |= 2;
    for (cursor = offset; cursor < offset + length; cursor += 8) {
        u8 cmd = s->source[cursor], count = s->source[cursor + 1];
        size_t data, i;
        if (cmd == 0x14) break;
        if (cmd == 5 || cmd == 0x10 || cmd == 0x11 || cmd == 0x12 || cmd == 0x15 || cmd == 0x16) continue;
        if (cmd == 7 || cmd == 8 || cmd == 9 || cmd == 0x19) { OotPspScene_Swap(s, cursor + 4, 1, 4); continue; }
        data = OotPspScene_Pointer(s, cursor + 4);
        if (data == s->size) continue;
        switch (cmd) {
            case 0: case 1: OotPspScene_Swap(s, data, (size_t)count * 8, 2); break;
            case 3: exitCount = OotPspScene_Collision(s, data); break;
            case 4:
                OotPspScene_Swap(s, data, (size_t)count * 2, 4);
                if (OotPspScene_Range(s, data, count, 8))
                    for (i = 0; i < (size_t)count * 8; i++) s->visited[data + i] |= 16;
                break;
            case 6: break; /* spawn entries are pairs of bytes */
            case 0x0A: OotPspScene_Mesh(s, data); break;
            case 0x0B: OotPspScene_Swap(s, data, count, 2); break;
            case 0x0C:
                for (i = 0; i < count; i++) {
                    OotPspScene_Swap(s, data + i * 14 + 2, 3, 2);
                    OotPspScene_Swap(s, data + i * 14 + 12, 1, 2);
                } break;
            case 2: case 0x0D:
                for (i = data; i <= s->size && s->size - i >= 8; i += 8) {
                    u32 points = OotPspAssetBuilder_ReadBe32(s->source + i + 4);
                    size_t amount = cmd == 2 ? OotPspAssetBuilder_ReadBe16(s->source + i + 2) : s->source[i];
                    if ((points >> 24) != s->segment || !amount || (points & 0xFFFFFF) >= s->size || amount > (s->size - (points & 0xFFFFFF)) / 6) break;
                    OotPspScene_Pointer(s, i + 4);
                    if (cmd == 2) OotPspScene_Swap(s, i, 2, 2);
                    OotPspScene_Swap(s, points & 0xFFFFFF, amount * 3, 2);
                } break;
            case 0x0E:
                for (i = 0; i < count; i++) OotPspScene_Swap(s, data + i * 16 + 4, 6, 2);
                break;
            case 0x0F:
                for (i = 0; i < count; i++) OotPspScene_Swap(s, data + i * 22 + 18, 2, 2);
                break;
            case 0x13: exitOffset = data; break;
            case 0x17: OotPspScene_Cutscene(s, data); break;
            case 0x18: {
                size_t end = s->size, probe;
                for (probe = offset; probe < offset + length; probe += 8) {
                    u32 p = OotPspAssetBuilder_ReadBe32(s->source + probe + 4);
                    if (s->source[probe] != 0x18 && (p >> 24) == s->segment && (p & 0xFFFFFF) > data && (p & 0xFFFFFF) < end) end = p & 0xFFFFFF;
                }
                for (i = data; i + 4 <= end && i < data + 19 * 4; i += 4) {
                    u32 p = OotPspAssetBuilder_ReadBe32(s->source + i);
                    if (!p) { OotPspScene_Swap(s, i, 1, 4); continue; }
                    if ((p >> 24) != s->segment || !OotPspScene_IsHeader(s->source, s->size, p & 0xFFFFFF)) break;
                    OotPspScene_Swap(s, i, 1, 4);
                    OotPspScene_Header(s, p & 0xFFFFFF, depth + 1);
                }
                break;
            }
            default: s->ok = false; break;
        }
    }
    if (exitOffset != s->size) OotPspScene_Swap(s, exitOffset, exitCount, 2);
}

static s32 OotPspAssetBuilder_ExtractScene(const OotPspRomProfile* profile, size_t assetIndex,
                                             const u8* source, size_t size, u8 segment, u8** output) {
    OotPspSceneExtract scene = { source, malloc(size), calloc(size, 1), size, segment, true };
    if (!scene.output || !scene.visited) { free(scene.output); free(scene.visited); return false; }
    memcpy(scene.output, source, size);
    OotPspScene_Header(&scene, 0, 0);
    /* Named cutscenes and collision used directly by actors need not be
     * referenced by a scene command. XML supplies roots, never byte recipes. */
    {
        size_t count, i;
        const OotPspRomAssetSplice* roots = OotPspAssetBuilder_GetAssetSplices(profile, assetIndex, &count);
        for (i = 0; i < count && scene.ok; i++) {
            switch (roots[i].copySize >> 28) {
                case 1: OotPspScene_DList(&scene, roots[i].sourceOffset, 0); break;
                case 3: OotPspScene_Cutscene(&scene, roots[i].sourceOffset); break;
                case 11: OotPspScene_Collision(&scene, roots[i].sourceOffset); break;
            }
        }
    }
    free(scene.visited);
    if (!scene.ok) { free(scene.output); return false; }
    *output = scene.output;
    return true;
}
