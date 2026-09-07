#!/usr/bin/env python3
"""Compare a private NTSC 1.0 conversion with the established native snapshot.

The snapshot contains conversion recipes, not ROM bytes. Requires crunch64 and
PyYAML from requirements.txt. Never writes ROM contents into the repository.
"""
import argparse
from pathlib import Path
import struct
import zipfile
import zlib
import crunch64
import version_config
from psp_port_asset_union import parse_entries

ROOT = Path(__file__).resolve().parents[1]


def compare(rom_path, pack_path, table_path):
    rom = rom_path.read_bytes()
    config = version_config.load_version_config('ntsc-1.0')
    names = list(config.dmadata_segments)
    with zipfile.ZipFile(ROOT / 'assets/psp/ntsc-1.0/generated.zip') as archive:
        old = parse_entries(archive.read('oot_psp_asset_segments.c').decode())
    new = {entry.name: entry for entry in parse_entries(table_path.read_text())}
    recipe = (ROOT / 'assets/psp/ntsc-1.0/asset_transform.z').read_bytes()
    assert recipe[:4] == b'OPZ4'
    count, permutations = struct.unpack_from('<II', recipe, 4)
    cursor = 12 + permutations * 8
    pack = pack_path.open('rb')
    for _ in range(count):
        index, size, payload_size, compressed = struct.unpack_from('<IIII', recipe, cursor)
        cursor += 16
        payload = zlib.decompress(recipe[cursor:cursor + compressed]); cursor += compressed
        assert len(payload) == payload_size
        asset = old[index]
        dma = config.dmadata_start + names.index(asset.name) * 16
        start, end, physical, packed_end = struct.unpack_from('>IIII', rom, dma)
        source = (crunch64.yaz0.decompress(rom[physical:packed_end]) if packed_end
                  else rom[physical:physical + end - start])
        selectors = (size + 7) // 8
        mapping = selectors
        expected = bytearray(size)
        for offset in range(0, size, 8):
            selector = payload[offset // 8]
            length = min(8, size - offset)
            if selector < permutations:
                order = recipe[12 + selector * 8:20 + selector * 8]
                expected[offset:offset + length] = bytes(source[offset + i] for i in order[:length])
            elif selector == 57:
                for i in range(length):
                    source_offset, delta = struct.unpack_from('<IB', payload, mapping); mapping += 5
                    expected[offset + i] = (source[source_offset] + delta) & 255
            else:
                assert selector == 56
        pack.seek(new[asset.name].file_offset)
        actual = pack.read(size)
        if actual != expected:
            first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
            raise AssertionError(f'{asset.name} differs at 0x{first:X}')
    pack.close()
    print(f'All {count} NTSC 1.0 native assets match the established snapshot byte-for-byte')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    parser.add_argument('pack', type=Path)
    parser.add_argument('--table', type=Path, default=ROOT / 'build/psp-port/ntsc-1.0/oot_psp_asset_segments.c')
    args = parser.parse_args()
    compare(args.rom, args.pack, args.table)
