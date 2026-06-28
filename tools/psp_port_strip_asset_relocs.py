#!/usr/bin/env python3
"""Disable PRX base relocations for generated absolute asset symbols.

The PSP asset build deliberately represents asset addresses as absolute values:
segmented addresses (0x01xxxxxx through 0x0Dxxxxxx) and external VROM handles
(0x20xxxxxx).  GNU ld resolves those values correctly but, when linked with
--emit-relocs, also retains MIPS relocation records for them.  psp-prxgen turns
those records into module-base relocations, corrupting the constants at load
time.  Change only relocations that name symbols emitted by the generated
asset-symbol assembly to R_MIPS_NONE.
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
from pathlib import Path


ELFCLASS32 = 1
ELFDATA2LSB = 1
ELFDATA2MSB = 2
SHT_RELA = 4
SHT_SYMTAB = 2
SHT_REL = 9
R_MIPS_NONE = 0

GLOBAL_RE = re.compile(r"^\s*\.global\s+([^\s,]+)\s*$")


def generated_symbol_names(path: Path) -> set[str]:
    names: set[str] = set()

    with path.open("r", encoding="utf-8") as source:
        for line in source:
            match = GLOBAL_RE.match(line)
            if match is not None:
                names.add(match.group(1))

    if not names:
        raise ValueError(f"no generated symbols found in {path}")
    return names


def read_c_string(data: bytearray, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="strict")


def strip_relocations(data: bytearray, generated_names: set[str]) -> tuple[int, set[str]]:
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != ELFCLASS32:
        raise ValueError("input is not an ELF32 file")
    if data[5] == ELFDATA2LSB:
        endian = "<"
    elif data[5] == ELFDATA2MSB:
        endian = ">"
    else:
        raise ValueError("ELF has an unsupported byte order")

    ehdr = struct.unpack_from(endian + "16sHHIIIIIHHHHHH", data, 0)
    section_offset = ehdr[6]
    section_entry_size = ehdr[11]
    section_count = ehdr[12]
    if section_entry_size < 40:
        raise ValueError("ELF section headers are too small")

    sections: list[tuple[int, ...]] = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        if offset + 40 > len(data):
            raise ValueError("ELF section header lies outside the file")
        sections.append(struct.unpack_from(endian + "IIIIIIIIII", data, offset))

    patched = 0
    patched_names: set[str] = set()
    for section in sections:
        section_type = section[1]
        if section_type not in (SHT_REL, SHT_RELA):
            continue

        relocation_offset = section[4]
        relocation_size = section[5]
        symbol_section_index = section[6]
        relocation_entry_size = section[9]
        minimum_entry_size = 8 if section_type == SHT_REL else 12
        if relocation_entry_size < minimum_entry_size or relocation_size % relocation_entry_size != 0:
            raise ValueError("ELF has malformed relocation entries")
        if symbol_section_index >= len(sections):
            raise ValueError("relocation section has an invalid symbol-table link")

        symbol_section = sections[symbol_section_index]
        if symbol_section[1] != SHT_SYMTAB:
            raise ValueError("relocation section does not reference a symbol table")
        symbol_offset = symbol_section[4]
        symbol_size = symbol_section[5]
        string_section_index = symbol_section[6]
        symbol_entry_size = symbol_section[9]
        if symbol_entry_size < 16 or symbol_size % symbol_entry_size != 0:
            raise ValueError("ELF has a malformed symbol table")
        if string_section_index >= len(sections):
            raise ValueError("symbol table has an invalid string-table link")
        string_section = sections[string_section_index]
        string_offset = string_section[4]
        string_size = string_section[5]
        if string_offset + string_size > len(data):
            raise ValueError("ELF string table lies outside the file")

        relocation_end = relocation_offset + relocation_size
        if relocation_end > len(data):
            raise ValueError("ELF relocation section lies outside the file")
        for entry_offset in range(relocation_offset, relocation_end, relocation_entry_size):
            relocation_info = struct.unpack_from(endian + "I", data, entry_offset + 4)[0]
            symbol_index = relocation_info >> 8
            relocation_type = relocation_info & 0xFF
            if relocation_type == R_MIPS_NONE:
                continue
            symbol_entry_offset = symbol_offset + symbol_index * symbol_entry_size
            if symbol_entry_offset + 16 > symbol_offset + symbol_size:
                raise ValueError("relocation references an invalid symbol")
            symbol_name_offset = struct.unpack_from(endian + "I", data, symbol_entry_offset)[0]
            if symbol_name_offset >= string_size:
                raise ValueError("symbol name lies outside the string table")
            name = read_c_string(data, string_offset + symbol_name_offset)
            if name not in generated_names:
                continue

            struct.pack_into(endian + "I", data, entry_offset + 4, symbol_index << 8 | R_MIPS_NONE)
            patched += 1
            patched_names.add(name)

    return patched, patched_names


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_elf", type=Path)
    parser.add_argument("asset_symbols", type=Path)
    parser.add_argument("output_elf", type=Path)
    args = parser.parse_args()

    names = generated_symbol_names(args.asset_symbols)
    args.output_elf.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.input_elf, args.output_elf)
    data = bytearray(args.output_elf.read_bytes())
    relocation_count, relocated_names = strip_relocations(data, names)
    if relocation_count == 0:
        raise RuntimeError("no generated asset relocations were found")
    args.output_elf.write_bytes(data)
    print(
        f"disabled {relocation_count} PRX relocations for "
        f"{len(relocated_names)} generated asset symbols"
    )


if __name__ == "__main__":
    main()
