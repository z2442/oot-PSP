/* Exercise the production converter with host file I/O and sanitizers. */
#include <assert.h>
#include "oot_psp_asset_builder.c"

extern char* mkdtemp(char*); /* Host POSIX API, absent from the game libc header. */
const char* hostRomPath;
void OotPspUnpackerUI_Draw(uint32_t progress, const char* status, bool error) {
    printf("%u/1000 %s%s\n", progress, error ? "ERROR: " : "", status);
}

static void test_byte_order(void) {
    const u8 normalized[] = { 0x80, 0x37, 0x12, 0x40, 1, 2, 3, 4, 5, 6, 7, 8 };
    u8 raw[sizeof(normalized)], result[sizeof(normalized)];
    unsigned masks[] = {0, 1, 3};
    for (size_t m = 0; m < 3; m++) {
        FILE* file = tmpfile();
        assert(file);
        for (size_t i = 0; i < sizeof(raw); i++) raw[i ^ masks[m]] = normalized[i];
        assert(fwrite(raw, 1, sizeof(raw), file) == sizeof(raw));
        fflush(file);
        OotPspRomImage rom = { .fd = fileno(file), .size = sizeof(raw), .swapMask = masks[m] };
        for (size_t offset = 0; offset < sizeof(raw); offset++) {
            for (size_t size = 0; size <= sizeof(raw) - offset; size++) {
                assert(OotPspAssetBuilder_ReadRomAt(&rom, offset, result, size));
                assert(memcmp(result, normalized + offset, size) == 0);
            }
        }
        assert(!OotPspAssetBuilder_ReadRomAt(&rom, 10, result, 3));
        fclose(file);
    }
}

static void test_compression(void) {
    u8 output[8];
    u8 yaz[] = {'Y','a','z','0', 0,0,0,8, 0,0,0,0,0,0,0,0, 0xC0,'A','B',0x40,1};
    assert(OotPspAssetBuilder_DecompressYaz0(yaz, sizeof(yaz), output, sizeof(output)));
    assert(memcmp(output, "ABABABAB", 8) == 0);
    assert(!OotPspAssetBuilder_DecompressYaz0(yaz, sizeof(yaz) - 1, output, sizeof(output)));
    yaz[20] = 0xFF; /* Back reference before output start. */
    assert(!OotPspAssetBuilder_DecompressYaz0(yaz, sizeof(yaz), output, sizeof(output)));
    u8 packed[64];
    z_stream z = {0};
    z.next_in = (Bytef*)"ABABABAB"; z.avail_in = 8;
    z.next_out = packed; z.avail_out = sizeof(packed);
    assert(deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    size_t size = z.total_out;
    deflateEnd(&z);
    assert(OotPspAssetBuilder_DecompressDeflate(packed, size, output, 8));
    assert(memcmp(output, "ABABABAB", 8) == 0);
    assert(!OotPspAssetBuilder_DecompressDeflate(packed, size - 1, output, 8));
}

static void test_scene_discovery(void) {
    u8 raw[256] = {0}, *native = NULL;
    /* A cullable room whose mesh, entries, and display lists are deliberately
     * nowhere near the NTSC room offsets. Colors/pixels resemble pointers. */
    raw[0] = 0x0A;
    OotPspAssetBuilder_WriteBe32(raw + 4, 0x03000040);
    raw[8] = 0x14;
    raw[0x40] = 2; raw[0x41] = 2;
    OotPspAssetBuilder_WriteBe32(raw + 0x44, 0x03000060);
    OotPspAssetBuilder_WriteBe32(raw + 0x48, 0x03000080);
    raw[0x60] = 0x12; raw[0x61] = 0x34;
    OotPspAssetBuilder_WriteBe32(raw + 0x68, 0x030000A0);
    OotPspAssetBuilder_WriteBe32(raw + 0x7C, 0x030000B0);
    OotPspAssetBuilder_WriteBe32(raw + 0xA0, 0xDE000000);
    OotPspAssetBuilder_WriteBe32(raw + 0xA4, 0x030000B0);
    OotPspAssetBuilder_WriteBe32(raw + 0xA8, 0xDF000000);
    OotPspAssetBuilder_WriteBe32(raw + 0xB0, 0xDF000000);
    OotPspAssetBuilder_WriteBe32(raw + 0xC0, 0x03000060);
    OotPspRomProfile profile = {0};
    assert(OotPspAssetBuilder_ExtractScene(&profile, 0, raw, sizeof(raw), 3, &native));
    assert(OotPspAssetBuilder_ReadLe32(native + 4) == 0x03000040);
    assert(native[0x60] == 0x34 && native[0x61] == 0x12);
    assert(OotPspAssetBuilder_ReadLe32(native + 0xA0) == 0xDE000000);
    assert(memcmp(native + 0xC0, raw + 0xC0, 4) == 0);
    assert(OotPspAssetBuilder_ValidateNativeSceneAsset("fixture_room_0", native, sizeof(raw)));
    free(native);
    raw[0x41] = 255;
    assert(!OotPspAssetBuilder_ExtractScene(&profile, 0, raw, sizeof(raw), 3, &native));
}

static void test_pointer_fields(void) {
    u8 source[64] = {0}, fields[16] = {0};
    /* PAL Sheik joint indices 0x0601,0x0643 looked like address 0x06010643. */
    OotPspAssetBuilder_WriteBe32(source, 0x06010643);
    OotPspAssetBuilder_WriteBe32(source + 36, 0x06000000);
    OotPspAssetBuilder_WriteBe32(source + 40, 0x06000018);
    OotPspAssetBuilder_MarkResourcePointers(source, sizeof(source), 32, 16, 4, fields, 6);
    assert(fields[0] == 0 && fields[9] == 1 && fields[10] == 1);
}

static void test_rom_discovery(void) {
    char directory[] = "/tmp/oot-rom-discovery-XXXXXX", executable[512], path[512], resolved[512];
    assert(mkdtemp(directory));
    snprintf(executable, sizeof(executable), "%s/EBOOT.PBP", directory);
    assert(OotPsp_SetAssetRoot(executable));
    snprintf(path, sizeof(path), "%s/data", directory); assert(mkdir(path, 0700) == 0);
    assert(OotPspAssetBuilder_OpenRom(resolved, sizeof(resolved)) == -1);
    snprintf(path, sizeof(path), "%s/data/My PAL ROM.Z64", directory);
    FILE* file = fopen(path, "wb"); assert(file); fclose(file);
    int fd = OotPspAssetBuilder_OpenRom(resolved, sizeof(resolved)); assert(fd >= 0); close(fd);
    assert(strstr(resolved, "My PAL ROM.Z64"));
    snprintf(path, sizeof(path), "%s/data/another.z64", directory);
    file = fopen(path, "wb"); assert(file); fclose(file);
    assert(OotPspAssetBuilder_OpenRom(resolved, sizeof(resolved)) == -2);
    unlink(path);
    snprintf(path, sizeof(path), "%s/data/My PAL ROM.Z64", directory); unlink(path);
    snprintf(path, sizeof(path), "%s/data", directory); rmdir(path); rmdir(directory);
}

int main(int argc, char** argv) {
    test_byte_order();
    test_compression();
    test_scene_discovery();
    test_pointer_fields();
    test_rom_discovery();
    puts("ROM byte order, bounds, Yaz0 and raw DEFLATE checks passed");
    if (argc == 3) {
        hostRomPath = argv[1];
        if (!OotPsp_SetAssetRoot(argv[2]) || !OotPspAssetBuilder_Ensure()) return 1;

    }
    return 0;
}
