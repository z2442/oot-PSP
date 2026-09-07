#!/usr/bin/env python3
"""Check the lifecycle exports the PSP kernel needs to start/stop a PRX."""
import argparse
from pathlib import Path
import struct


def check_module(data: bytes, require_stop: bool = False) -> None:
    if data[:4] == b'\0PBP':
        start, end = struct.unpack_from('<II', data, 32)
        data = data[start:end or len(data)]
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected a little-endian ELF32 PSP module')
    shoff = struct.unpack_from('<I', data, 32)[0]
    entsize, count, names_index = struct.unpack_from('<HHH', data, 46)
    if entsize != 40 or names_index >= count:
        raise ValueError('invalid ELF section table')
    sections = [struct.unpack_from('<10I', data, shoff + i * entsize) for i in range(count)]
    names = sections[names_index]
    strings = data[names[4]:names[4] + names[5]]
    named = {}
    for section in sections:
        end = strings.find(b'\0', section[0])
        named[strings[section[0]:end].decode()] = section

    def at(address, size):
        for section in sections:
            if section[1] != 8 and section[3] <= address and address + size <= section[3] + section[5]:
                offset = section[4] + address - section[3]
                return data[offset:offset + size]
        raise ValueError(f'export table address outside module: {address:#x}')

    entries = named.get('.lib.ent')
    if entries is None or entries[5] < 16:
        raise ValueError('missing PRX lifecycle exports (possibly stripped by --gc-sections)')
    cursor, end = entries[4], entries[4] + entries[5]
    required = {0xD632ACDB, 0xF01D73A7}  # module_start, module_info
    if require_stop:
        required.add(0xCEE8593C)
    while cursor + 16 <= end:
        name, version, attributes, words, variables, functions, table = struct.unpack_from('<IHHBBHI', data, cursor)
        if words < 4:
            raise ValueError('invalid PRX export entry size')
        if name == 0 and attributes & 0x8000:
            nids = set(struct.unpack('<' + 'I' * (variables + functions), at(table, (variables + functions) * 4)))
            if required <= nids:
                return
        cursor += words * 4
    raise ValueError('PRX is missing required start/stop/module-info exports')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--require-stop', action='store_true')
    args = parser.parse_args()
    check_module(args.module.read_bytes(), args.require_stop)
    print(f'PSP lifecycle exports verified: {args.module}')
