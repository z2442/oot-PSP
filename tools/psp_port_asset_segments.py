#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import itertools
import re
import shutil
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import dmadata
from tools import version_config

EXTERNAL_VROM_BASE = 0x20000000
EXTERNAL_VROM_ALIGN = 16
RUNTIME_SEGMENT_DIR = Path("data/segments")
PACKED_ASSET_FILENAME = "oot_psp_assets.bin"
NATIVE_ASSET_FLAG = 1
TEXTURE_WORDS_FLAG = 2
TEXTURE_WORD_ALIGN = 8
TEXTURE_FORMAT_BITS = {
    "rgba16": 16,
    "rgba32": 32,
    "ia4": 4,
    "ia8": 8,
    "ia16": 16,
    "i4": 4,
    "i8": 8,
    "ci4": 4,
    "ci8": 8,
}
TEXTURE_TLUT_BYTES = {
    "ci4": 32,
    "ci8": 512,
}

DECLARE_RE = re.compile(r"\bDECLARE_(?:ROM_)?SEGMENT\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)")
DECLARE_OVERLAY_RE = re.compile(r"\bDECLARE_OVERLAY_SEGMENT\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)")
OFFSET_TOKEN_RE = re.compile(r"(?<![0-9A-Fa-f])([0-9A-Fa-f]{6,8})(?![0-9A-Fa-f])")
SOURCE_DECL_RE = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\s+|\s*\*\s*)+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)*=",
    re.MULTILINE,
)
U64_SOURCE_DECL_RE = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?u64\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b[^;=]*=", re.MULTILINE
)
COLLISION_AUX_SUFFIXES = ("BgCamList", "SurfaceTypes", "PolyList", "VtxList", "WaterBoxes")
SKIP_NAMES = {
    "makerom",
    "boot",
    "code",
    "dmadata",
    "n64dd",
}

ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
SHN_UNDEF = 0
STB_LOCAL = 0
R_MIPS_32 = 2
RUNTIME_SYMBOL_SENTINELS = {
    "gIdentityMtx": 0x0E000001,
}


@dataclass
class ElfSection:
    name: str
    type: int
    flags: int
    offset: int
    size: int
    link: int
    info: int
    addralign: int
    entsize: int


@dataclass
class ElfSymbol:
    name: str
    value: int
    size: int
    info: int
    shndx: int


@dataclass
class ElfReloc:
    offset: int
    sym_index: int
    type: int
    symtab_index: int


def read_c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        end = len(data)
    return data[offset:end].decode("utf-8", errors="replace")


def texture_size_bytes(format_name: str, width: int, height: int) -> int | None:
    bits = TEXTURE_FORMAT_BITS.get(format_name.lower())

    if bits is None:
        return None

    return (width * height * bits + 7) // 8


def texture_storage_range(offset: int, size: int) -> tuple[int, int]:
    return offset, offset + size


def copy_native_texture_words(output: bytearray, source: bytes, start: int, end: int) -> None:
    limit = min(len(output), len(source))
    start = max(0, min(start, limit))
    end = max(0, min(end, limit))

    if end <= start:
        return

    for offset in range(start, end, TEXTURE_WORD_ALIGN):
        word_end = min(offset + TEXTURE_WORD_ALIGN, end)
        output[offset:word_end] = source[offset:word_end][::-1]


def parse_xml_int(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        return int(text, 16)


def elf_symbol_bind(info: int) -> int:
    return info >> 4


class ElfObject:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        self.sections: list[ElfSection] = []
        self.symtabs: dict[int, list[ElfSymbol]] = {}
        self.relocs_by_section: dict[int, list[ElfReloc]] = {}
        self.section_data: dict[int, bytes] = {}
        self._parse()

    def _parse(self) -> None:
        ident = self.data[:16]

        if (ident[:4] != ELF_MAGIC) or (ident[4] != ELFCLASS32) or (ident[5] != ELFDATA2LSB):
            raise ValueError(f"{self.path} is not a 32-bit little-endian ELF object")

        header = struct.unpack_from("<16sHHIIIIIHHHHHH", self.data, 0)
        e_shoff = header[6]
        e_shentsize = header[11]
        e_shnum = header[12]
        e_shstrndx = header[13]
        raw_sections: list[tuple[int, int, int, int, int, int, int, int, int, int]] = []

        for index in range(e_shnum):
            offset = e_shoff + (index * e_shentsize)
            raw_sections.append(struct.unpack_from("<IIIIIIIIII", self.data, offset))

        shstr = raw_sections[e_shstrndx]
        shstr_data = self.data[shstr[4] : shstr[4] + shstr[5]]

        for raw in raw_sections:
            name = read_c_string(shstr_data, raw[0])
            self.sections.append(
                ElfSection(
                    name=name,
                    type=raw[1],
                    flags=raw[2],
                    offset=raw[4],
                    size=raw[5],
                    link=raw[6],
                    info=raw[7],
                    addralign=raw[8],
                    entsize=raw[9],
                )
            )

        for index, section in enumerate(self.sections):
            if section.type == SHT_PROGBITS:
                self.section_data[index] = self.data[section.offset : section.offset + section.size]
            elif section.type == SHT_NOBITS:
                self.section_data[index] = bytes(section.size)

            if section.type == SHT_SYMTAB:
                strtab = self.sections[section.link]
                strtab_data = self.data[strtab.offset : strtab.offset + strtab.size]
                symbols: list[ElfSymbol] = []
                entsize = section.entsize or 16

                for offset in range(section.offset, section.offset + section.size, entsize):
                    st_name, st_value, st_size, st_info, _st_other, st_shndx = struct.unpack_from(
                        "<IIIBBH", self.data, offset
                    )
                    symbols.append(
                        ElfSymbol(
                            name=read_c_string(strtab_data, st_name) if st_name != 0 else "",
                            value=st_value,
                            size=st_size,
                            info=st_info,
                            shndx=st_shndx,
                        )
                    )

                self.symtabs[index] = symbols

        for index, section in enumerate(self.sections):
            if section.type != SHT_REL:
                continue

            relocs: list[ElfReloc] = []
            entsize = section.entsize or 8

            for offset in range(section.offset, section.offset + section.size, entsize):
                r_offset, r_info = struct.unpack_from("<II", self.data, offset)
                relocs.append(
                    ElfReloc(
                        offset=r_offset,
                        sym_index=r_info >> 8,
                        type=r_info & 0xFF,
                        symtab_index=section.link,
                    )
                )

            self.relocs_by_section.setdefault(section.info, []).extend(relocs)

    def iter_symbols(self) -> itertools.chain[ElfSymbol]:
        return itertools.chain.from_iterable(self.symtabs.values())


class NativeAssetContext:
    def __init__(self, version: str, entries: list[dict[str, object]], build_root: Path | None = None) -> None:
        self.version = version
        if build_root is None:
            build_root = Path("build") / "psp-port" / version
        self.build_root = build_root if build_root.is_absolute() else ROOT / build_root
        self.entries_by_name = {str(entry["name"]): entry for entry in entries}
        self.segment_ids: dict[str, int] = {}
        self.xml_symbol_offsets: dict[str, dict[str, int]] = {}
        self.inferred_symbol_offsets: dict[str, dict[str, int]] = {}
        self.segment_object_paths: dict[str, list[Path]] = {}
        self.elf_cache: dict[Path, ElfObject] = {}
        self.native_segment_cache: dict[str, bytes | None] = {}
        self.texture_ranges_by_segment: dict[str, list[tuple[int, int]]] = {}
        self.global_symbol_values: dict[str, int] = {}
        self._load_xml_offsets()
        self._discover_native_objects()
        self.global_symbol_values.update(RUNTIME_SYMBOL_SENTINELS)
        self._infer_section_relative_offsets()
        self._infer_source_order_offsets()
        self._index_native_u64_symbol_ranges()
        self._index_segment_symbols()

    def _should_use_xml(self, path: Path) -> bool:
        stem = path.stem

        if self.version.startswith("pal"):
            return True

        variant_suffixes = ("_pal", "_pal_n64", "_v2", "_v2_mq", "_v3", "_v3_mq", "_ique", "_mq")
        return not stem.endswith(variant_suffixes)

    def _add_texture_range(self, segment_name: str, offset: int, size: int) -> None:
        if size <= 0:
            return

        self.texture_ranges_by_segment.setdefault(segment_name, []).append(texture_storage_range(offset, size))

    def _load_xml_offsets(self) -> None:
        xml_roots = [
            ROOT / "assets" / "xml" / "objects",
            ROOT / "assets" / "xml" / "scenes",
            ROOT / "assets" / "xml" / "textures",
            ROOT / "assets" / "xml" / "misc",
        ]

        for xml_path in itertools.chain.from_iterable(root.glob("**/*.xml") for root in xml_roots if root.exists()):
            if not self._should_use_xml(xml_path):
                continue

            try:
                root = ET.parse(xml_path).getroot()
            except ET.ParseError as exc:
                raise ValueError(f"failed to parse {xml_path}: {exc}") from exc

            for file_elem in root.iter("File"):
                segment_name = file_elem.attrib.get("Name")
                segment_text = file_elem.attrib.get("Segment")

                if not segment_name:
                    continue

                if segment_text is not None:
                    self.segment_ids[segment_name] = int(segment_text, 0)

                offsets = self.xml_symbol_offsets.setdefault(segment_name, {})
                offsets.setdefault(segment_name, 0)

                for elem in file_elem.iter():
                    if elem.tag == "Texture":
                        format_text = elem.attrib.get("Format")
                        width_text = elem.attrib.get("Width")
                        height_text = elem.attrib.get("Height")
                        offset_text = elem.attrib.get("Offset")

                        if (
                            (format_text is not None)
                            and (width_text is not None)
                            and (height_text is not None)
                            and (offset_text is not None)
                        ):
                            texture_size = texture_size_bytes(format_text, int(width_text, 0), int(height_text, 0))

                            if texture_size is not None:
                                self._add_texture_range(segment_name, parse_xml_int(offset_text), texture_size)

                        tlut_offset_text = elem.attrib.get("TlutOffset")
                        if (format_text is not None) and (tlut_offset_text is not None):
                            tlut_size = TEXTURE_TLUT_BYTES.get(format_text.lower())

                            if tlut_size is not None:
                                self._add_texture_range(segment_name, parse_xml_int(tlut_offset_text), tlut_size)

                        external_tlut = elem.attrib.get("ExternalTlut")
                        external_tlut_offset_text = elem.attrib.get("ExternalTlutOffset")
                        if (
                            (format_text is not None)
                            and (external_tlut is not None)
                            and (external_tlut_offset_text is not None)
                        ):
                            tlut_size = TEXTURE_TLUT_BYTES.get(format_text.lower())

                            if tlut_size is not None:
                                self._add_texture_range(
                                    external_tlut, parse_xml_int(external_tlut_offset_text), tlut_size
                                )

                    symbol_name = elem.attrib.get("Name")
                    offset_text = elem.attrib.get("Offset")

                    if (symbol_name is None) or (offset_text is None):
                        continue

                    offsets[symbol_name] = parse_xml_int(offset_text)

    def _add_object_paths(self, segment_name: str, *patterns: str) -> None:
        paths: list[Path] = []

        for pattern in patterns:
            paths.extend(path for path in sorted(self.build_root.glob(pattern)) if path.is_file())

        if paths:
            self.segment_object_paths[segment_name] = paths

    def _discover_native_objects(self) -> None:
        for segment_name in self.entries_by_name:
            self._add_object_paths(
                segment_name,
                f"assets/objects/{segment_name}/*.o",
                f"extracted/{self.version}/assets/objects/{segment_name}/*.o",
                f"assets/scenes/*/*/{segment_name}.o",
                f"extracted/{self.version}/assets/scenes/*/*/{segment_name}.o",
                f"assets/textures/{segment_name}/*.o",
                f"extracted/{self.version}/assets/textures/{segment_name}/*.o",
                f"assets/misc/{segment_name}/*.o",
                f"extracted/{self.version}/assets/misc/{segment_name}/*.o",
            )

    def _load_object(self, path: Path) -> ElfObject:
        obj = self.elf_cache.get(path)

        if obj is None:
            obj = ElfObject(path)
            self.elf_cache[path] = obj

        return obj

    def _asset_kind_for_object_path(self, path: Path) -> str | None:
        try:
            parts = path.relative_to(self.build_root).parts
        except ValueError:
            return None

        for index in range(len(parts) - 1):
            if parts[index] == "assets":
                kind = parts[index + 1]
                if kind in {"objects", "scenes", "textures", "misc"}:
                    return kind

        return None

    def _symbol_offset_from_name(self, segment_id: int, segment_size: int, symbol_name: str) -> int | None:
        candidates: list[int] = []

        if symbol_name.endswith(COLLISION_AUX_SUFFIXES):
            return None

        for match in OFFSET_TOKEN_RE.finditer(symbol_name):
            token = match.group(1)
            value = int(token, 16)

            if len(token) == 8:
                if (value >> 24) == segment_id:
                    candidates.append(value & 0x00FFFFFF)
                elif value < segment_size:
                    candidates.append(value)
            elif value < segment_size:
                candidates.append(value)

        if not candidates:
            return None

        return candidates[-1]

    def _symbol_offset(self, segment_name: str, symbol_name: str) -> int | None:
        entry = self.entries_by_name[segment_name]
        segment_size = int(entry["size"])
        segment_id = self.segment_ids.get(segment_name)
        offsets = self.xml_symbol_offsets.get(segment_name, {})
        inferred_offsets = self.inferred_symbol_offsets.get(segment_name, {})

        if symbol_name in offsets:
            return offsets[symbol_name]

        if symbol_name in inferred_offsets:
            return inferred_offsets[symbol_name]

        if segment_id is None:
            return None

        return self._symbol_offset_from_name(segment_id, segment_size, symbol_name)

    def _symbol_offset_without_inferred(self, segment_name: str, symbol_name: str) -> int | None:
        entry = self.entries_by_name[segment_name]
        segment_size = int(entry["size"])
        segment_id = self.segment_ids.get(segment_name)
        offsets = self.xml_symbol_offsets.get(segment_name, {})

        if symbol_name in offsets:
            return offsets[symbol_name]

        if segment_id is None:
            return None

        return self._symbol_offset_from_name(segment_id, segment_size, symbol_name)

    def _segment_symbol_value(self, segment_name: str, offset: int) -> int | None:
        segment_id = self.segment_ids.get(segment_name)

        if segment_id is None:
            return None

        return (segment_id << 24) | offset

    def _source_path_for_object(self, path: Path) -> Path | None:
        try:
            relative = path.relative_to(self.build_root)
        except ValueError:
            return None

        source = ROOT / relative.with_suffix(".c")
        return source if source.is_file() else None

    def _source_symbol_order(self, path: Path) -> list[str]:
        source = self._source_path_for_object(path)

        if source is None:
            return []

        return [match.group("name") for match in SOURCE_DECL_RE.finditer(source.read_text())]

    def _source_u64_symbols(self, path: Path) -> set[str]:
        source = self._source_path_for_object(path)

        if source is None:
            return set()

        return {match.group("name") for match in U64_SOURCE_DECL_RE.finditer(source.read_text())}

    def _object_symbol_layout(self, obj: ElfObject) -> dict[str, tuple[int, int]]:
        layout: dict[str, tuple[int, int]] = {}

        for symbol in obj.iter_symbols():
            if (not symbol.name) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                continue

            if symbol.shndx >= len(obj.sections):
                continue

            section = obj.sections[symbol.shndx]
            if (section.flags & SHF_ALLOC) == 0:
                continue

            layout[symbol.name] = (symbol.size, max(section.addralign, 1))

        return layout

    def _infer_section_relative_offsets(self) -> None:
        for segment_name, paths in self.segment_object_paths.items():
            inferred = self.inferred_symbol_offsets.setdefault(segment_name, {})
            segment_size = int(self.entries_by_name[segment_name]["size"])

            for path in paths:
                obj = self._load_object(path)
                symbols_by_section: dict[int, list[ElfSymbol]] = {}
                anchors_by_section: dict[int, list[tuple[int, int]]] = {}

                for symbol in obj.iter_symbols():
                    if (not symbol.name) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                        continue

                    if symbol.shndx >= len(obj.sections):
                        continue

                    section = obj.sections[symbol.shndx]
                    if (section.flags & SHF_ALLOC) == 0:
                        continue

                    symbols_by_section.setdefault(symbol.shndx, []).append(symbol)

                    known_offset = self._symbol_offset_without_inferred(segment_name, symbol.name)
                    if known_offset is None:
                        known_offset = inferred.get(symbol.name)

                    if known_offset is not None:
                        anchors_by_section.setdefault(symbol.shndx, []).append((symbol.value, known_offset))

                for section_index, symbols in symbols_by_section.items():
                    anchors = anchors_by_section.get(section_index)

                    if not anchors:
                        continue

                    for symbol in symbols:
                        if (self._symbol_offset_without_inferred(segment_name, symbol.name) is not None) or (
                            symbol.name in inferred
                        ):
                            continue

                        candidates: set[int] = set()

                        for anchor_value, anchor_offset in anchors:
                            if symbol.value >= anchor_value:
                                offset = anchor_offset + (symbol.value - anchor_value)
                            else:
                                delta = anchor_value - symbol.value
                                if anchor_offset < delta:
                                    continue
                                offset = anchor_offset - delta

                            if offset + symbol.size <= segment_size:
                                candidates.add(offset)

                        if len(candidates) == 1:
                            inferred[symbol.name] = candidates.pop()

    def _infer_source_order_offsets(self) -> None:
        for segment_name, paths in self.segment_object_paths.items():
            inferred = self.inferred_symbol_offsets.setdefault(segment_name, {})
            segment_size = int(self.entries_by_name[segment_name]["size"])

            for path in paths:
                obj = self._load_object(path)
                layout = self._object_symbol_layout(obj)
                ordered_symbols = [
                    symbol_name for symbol_name in self._source_symbol_order(path) if symbol_name in layout
                ]
                cursor: int | None = None

                for symbol_name in ordered_symbols:
                    symbol_size, symbol_align = layout[symbol_name]
                    known_offset = self._symbol_offset_without_inferred(segment_name, symbol_name)

                    if known_offset is not None:
                        cursor = known_offset + symbol_size
                        continue

                    if cursor is None:
                        continue

                    offset = align(cursor, symbol_align)
                    if offset + symbol_size > segment_size:
                        continue

                    inferred.setdefault(symbol_name, offset)
                    cursor = offset + symbol_size

                cursor = None

                for symbol_name in reversed(ordered_symbols):
                    symbol_size, symbol_align = layout[symbol_name]
                    known_offset = self._symbol_offset_without_inferred(segment_name, symbol_name)

                    if known_offset is None:
                        known_offset = inferred.get(symbol_name)

                    if known_offset is not None:
                        cursor = known_offset
                        continue

                    if cursor is None:
                        continue

                    offset = (cursor - symbol_size) & -symbol_align
                    if offset < 0:
                        continue

                    inferred[symbol_name] = offset
                    cursor = offset

    def _index_native_u64_symbol_ranges(self) -> None:
        for segment_name, paths in self.segment_object_paths.items():
            segment_size = int(self.entries_by_name[segment_name]["size"])

            for path in paths:
                texture_symbols = self._source_u64_symbols(path)

                if not texture_symbols:
                    continue

                obj = self._load_object(path)

                for symbol in obj.iter_symbols():
                    if (symbol.name not in texture_symbols) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                        continue

                    if symbol.shndx >= len(obj.sections):
                        continue

                    section = obj.sections[symbol.shndx]
                    if (section.flags & SHF_ALLOC) == 0:
                        continue

                    offset = self._symbol_offset(segment_name, symbol.name)
                    if (offset is None) or (offset >= segment_size):
                        continue

                    self._add_texture_range(segment_name, offset, min(symbol.size, segment_size - offset))

    def _index_segment_symbols(self) -> None:
        for segment_name, entry in self.entries_by_name.items():
            self.global_symbol_values[f"_{segment_name}SegmentRomStart"] = int(entry["vrom_start"])
            self.global_symbol_values[f"_{segment_name}SegmentRomEnd"] = int(entry["vrom_end"])

            segment_id = self.segment_ids.get(segment_name)
            if segment_id is not None:
                self.global_symbol_values[f"_{segment_name}SegmentStart"] = segment_id << 24
                self.global_symbol_values[f"_{segment_name}SegmentEnd"] = (segment_id << 24) | int(entry["size"])

        for segment_name, paths in self.segment_object_paths.items():
            for path in paths:
                obj = self._load_object(path)

                for symbol in obj.iter_symbols():
                    if (not symbol.name) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                        continue

                    if symbol.shndx >= len(obj.sections):
                        continue

                    section = obj.sections[symbol.shndx]
                    if (section.flags & SHF_ALLOC) == 0:
                        continue

                    offset = self._symbol_offset(segment_name, symbol.name)
                    if offset is None:
                        continue

                    value = self._segment_symbol_value(segment_name, offset)
                    if value is not None:
                        self.global_symbol_values.setdefault(symbol.name, value)

    def linker_symbol_values(self, asset_kinds: set[str]) -> dict[str, int]:
        values: dict[str, int] = {}

        for segment_name, paths in self.segment_object_paths.items():
            for path in paths:
                if self._asset_kind_for_object_path(path) not in asset_kinds:
                    continue

                obj = self._load_object(path)

                for symbol in obj.iter_symbols():
                    if (not symbol.name) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                        continue

                    if symbol.name.startswith("_") or (symbol.name in RUNTIME_SYMBOL_SENTINELS):
                        continue

                    if elf_symbol_bind(symbol.info) == STB_LOCAL:
                        continue

                    if symbol.shndx >= len(obj.sections):
                        continue

                    section = obj.sections[symbol.shndx]
                    if (section.flags & SHF_ALLOC) == 0:
                        continue

                    value = self.global_symbol_values.get(symbol.name)
                    if value is not None:
                        values.setdefault(symbol.name, value)

        return values

    def _resolve_relocation_symbol(self, current_segment: str, symbol: ElfSymbol, addend: int) -> int:
        if symbol.name in self.global_symbol_values:
            return (self.global_symbol_values[symbol.name] + addend) & 0xFFFFFFFF

        if (symbol.name != "") and (symbol.shndx != SHN_UNDEF):
            offset = self._symbol_offset(current_segment, symbol.name)
            value = self._segment_symbol_value(current_segment, offset) if offset is not None else None

            if value is not None:
                return (value + addend) & 0xFFFFFFFF

        raise ValueError(f"unresolved asset relocation symbol {symbol.name!r} in segment {current_segment}")

    def _patched_sections(self, segment_name: str, obj: ElfObject) -> dict[int, bytearray]:
        patched = {index: bytearray(data) for index, data in obj.section_data.items()}

        for section_index, relocs in obj.relocs_by_section.items():
            if section_index not in patched:
                continue

            section_data = patched[section_index]

            for reloc in relocs:
                if reloc.type != R_MIPS_32:
                    raise ValueError(f"unsupported relocation type {reloc.type} in {obj.path}")

                if reloc.offset + 4 > len(section_data):
                    raise ValueError(f"relocation out of range in {obj.path}")

                symbols = obj.symtabs[reloc.symtab_index]
                symbol = symbols[reloc.sym_index]
                addend = struct.unpack_from("<I", section_data, reloc.offset)[0]
                value = self._resolve_relocation_symbol(segment_name, symbol, addend)
                struct.pack_into("<I", section_data, reloc.offset, value)

        return patched

    def _copy_native_texture_ranges(self, entry: dict[str, object], output: bytearray) -> None:
        segment_name = str(entry["name"])
        ranges = self.texture_ranges_by_segment.get(segment_name)

        if not ranges:
            return

        source = entry["source"]
        assert isinstance(source, Path)
        source_data = source.read_bytes()

        for start, end in ranges:
            copy_native_texture_words(output, source_data, start, end)

    def build_native_segment(self, entry: dict[str, object]) -> bytes | None:
        segment_name = str(entry["name"])
        if segment_name in self.native_segment_cache:
            return self.native_segment_cache[segment_name]

        paths = self.segment_object_paths.get(segment_name)

        if not paths:
            self.native_segment_cache[segment_name] = None
            return None

        output = bytearray(int(entry["size"]))
        copied = 0

        for path in paths:
            obj = self._load_object(path)
            patched_sections = self._patched_sections(segment_name, obj)

            for symbol in obj.iter_symbols():
                if (not symbol.name) or (symbol.size == 0) or (symbol.shndx == SHN_UNDEF):
                    continue

                if symbol.shndx not in patched_sections:
                    continue

                section = obj.sections[symbol.shndx]
                if (section.flags & SHF_ALLOC) == 0:
                    continue

                offset = self._symbol_offset(segment_name, symbol.name)
                if offset is None:
                    continue

                section_data = patched_sections[symbol.shndx]
                data = section_data[symbol.value : symbol.value + symbol.size]
                end = offset + len(data)

                if end > len(output):
                    raise ValueError(
                        f"{symbol.name} at 0x{offset:X}..0x{end:X} exceeds {segment_name} size 0x{len(output):X}"
                    )

                output[offset:end] = data
                copied += 1

        if copied == 0:
            self.native_segment_cache[segment_name] = None
            return None

        self._copy_native_texture_ranges(entry, output)
        data = bytes(output)
        self.native_segment_cache[segment_name] = data
        return data
MESSAGE_TABLE_LAYOUTS = {
    "NTSC": {
        "jpn": ("sJpnMessageEntryTable", "jpn_message_data_static", None),
        "nes": ("sNesMessageEntryTable", "nes_message_data_static", None),
        "staff": ("sStaffMessageEntryTable", "staff_message_data_static", None),
    },
    "PAL": {
        "nes": ("sNesMessageEntryTable", "nes_message_data_static", None),
        "ger": ("sGerMessageEntryTable", "ger_message_data_static", "nes"),
        "fra": ("sFraMessageEntryTable", "fra_message_data_static", "nes"),
        "staff": ("sStaffMessageEntryTable", "staff_message_data_static", None),
    },
    "CN": {
        "jpn": ("sJpnMessageEntryTable", "jpn_message_data_static", None),
        "nes": ("sNesMessageEntryTable", "nes_message_data_static", None),
        "staff": ("sStaffMessageEntryTable", "staff_message_data_static", None),
    },
}


def declared_segment_names() -> list[str]:
    text = (ROOT / "include/segment_symbols.h").read_text()
    names: list[str] = []
    seen: set[str] = set()

    for match in DECLARE_RE.finditer(text):
        name = match.group("name")
        if name not in seen:
            names.append(name)
            seen.add(name)

    for match in DECLARE_OVERLAY_RE.finditer(text):
        name = f"ovl_{match.group('name')}"
        if name not in seen:
            names.append(name)
            seen.add(name)

    return names


def should_emit_raw_segment(name: str, baserom_dir: Path) -> bool:
    if name in SKIP_NAMES:
        return False
    return (baserom_dir / name).is_file()


def read_original_vrom_ranges(version: str) -> dict[str, tuple[int, int]]:
    config = version_config.load_version_config(version)
    rom_path = ROOT / "baseroms" / version / "baserom-decompressed.z64"
    rom_data = memoryview(rom_path.read_bytes())
    entries = dmadata.read_dmadata(rom_data, config.dmadata_start)
    ranges: dict[str, tuple[int, int]] = {}

    for name, entry in zip(config.dmadata_segments.keys(), entries):
        ranges[name] = (entry.vrom_start, entry.vrom_end)

    return ranges


def c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def read_message_table_entry(data: bytes, offset: int) -> tuple[int, int, int]:
    text_id, type_pos, addr = struct.unpack(">HBxI", data[offset : offset + 8])
    return text_id, type_pos, addr


def segment_offset(addr: int) -> int:
    return addr & ~0x0F000000


def build_entries(version: str) -> list[dict[str, object]]:
    config = version_config.load_version_config(version)
    baserom_dir = ROOT / "extracted" / version / "baserom"
    original_ranges = read_original_vrom_ranges(version)
    cursor = EXTERNAL_VROM_BASE
    entries: list[dict[str, object]] = []

    for name in config.dmadata_segments.keys():
        if not should_emit_raw_segment(name, baserom_dir):
            continue

        source = baserom_dir / name
        size = source.stat().st_size
        start = align(cursor, EXTERNAL_VROM_ALIGN)
        end = start + size
        original_start, original_end = original_ranges[name]
        cursor = align(end, EXTERNAL_VROM_ALIGN)
        entries.append(
            {
                "name": name,
                "source": source,
                "size": size,
                "vrom_start": start,
                "vrom_end": end,
                "original_vrom_start": original_start,
                "original_vrom_end": original_end,
                "runtime_path": RUNTIME_SEGMENT_DIR / f"{name}.bin",
            }
        )

    return entries


def read_complete_message_table(
    code: bytes,
    code_vram: int,
    table_addr: int,
    segment_data: bytes,
    external_vrom_start: int,
) -> list[dict[str, int]]:
    code_offset = table_addr - code_vram
    table_entries: list[tuple[int, int, int]] = []
    messages: list[dict[str, int]] = []

    while True:
        entry = read_message_table_entry(code, code_offset)
        code_offset += 8
        table_entries.append(entry)
        if entry[0] == 0xFFFF:
            break

    for index, (text_id, type_pos, addr) in enumerate(table_entries[:-1]):
        next_text_id = table_entries[index + 1][0]
        next_addr = table_entries[index + 1][2]
        start_offset = segment_offset(addr)
        end_offset = segment_offset(next_addr) if next_text_id != 0xFFFF else len(segment_data)

        messages.append(
            {
                "text_id": text_id,
                "type_pos": type_pos,
                "vrom_start": external_vrom_start + start_offset,
                "vrom_end": external_vrom_start + end_offset,
            }
        )

    return messages


def read_child_message_table(
    code: bytes,
    code_vram: int,
    table_addr: int,
    segment_data: bytes,
    external_vrom_start: int,
    parent_messages: list[dict[str, int]],
) -> list[dict[str, int]]:
    code_offset = table_addr - code_vram
    messages: list[dict[str, int]] = []

    for parent in parent_messages:
        text_id = parent["text_id"]

        if text_id == 0xFFFC:
            continue

        curr = struct.unpack(">I", code[code_offset : code_offset + 4])[0]
        next_addr = struct.unpack(">I", code[code_offset + 4 : code_offset + 8])[0]
        code_offset += 4

        start_offset = segment_offset(curr)
        end_offset = segment_offset(next_addr) if text_id != 0xFFFD else len(segment_data)
        messages.append(
            {
                "text_id": text_id,
                "type_pos": parent["type_pos"],
                "vrom_start": external_vrom_start + start_offset,
                "vrom_end": external_vrom_start + end_offset,
            }
        )

    return messages


def build_message_entries(version: str, entries: list[dict[str, object]]) -> dict[str, list[dict[str, int]]]:
    config = version_config.load_version_config(version)
    layout = MESSAGE_TABLE_LAYOUTS.get(config.text_lang)
    baserom_dir = ROOT / "extracted" / version / "baserom"
    entry_by_name = {str(entry["name"]): entry for entry in entries}
    message_entries: dict[str, list[dict[str, int]]] = {
        "jpn": [],
        "nes": [],
        "ger": [],
        "fra": [],
        "staff": [],
    }

    if layout is None:
        return message_entries

    code = (baserom_dir / "code").read_bytes()
    code_vram = config.dmadata_segments["code"].vram

    for key, (table_name, segment_name, parent_key) in layout.items():
        entry = entry_by_name.get(segment_name)
        table_addr = config.variables.get(table_name)

        if (entry is None) or (table_addr is None):
            continue

        source = entry["source"]
        assert isinstance(source, Path)
        segment_data = source.read_bytes()
        external_vrom_start = int(entry["vrom_start"])

        if parent_key is None:
            message_entries[key] = read_complete_message_table(
                code, code_vram, table_addr, segment_data, external_vrom_start
            )
        else:
            message_entries[key] = read_child_message_table(
                code,
                code_vram,
                table_addr,
                segment_data,
                external_vrom_start,
                message_entries[parent_key],
            )

    return message_entries


def copy_data_files(entries: list[dict[str, object]], data_dir: Path, native_assets: NativeAssetContext) -> None:
    data_dir.mkdir(parents=True, exist_ok=True)
    packed_path = data_dir / PACKED_ASSET_FILENAME

    with packed_path.open("wb") as out:
        for entry in entries:
            source = entry["source"]
            assert isinstance(source, Path)
            native_data = native_assets.build_native_segment(entry)
            offset = align(out.tell(), EXTERNAL_VROM_ALIGN)

            if offset != out.tell():
                out.write(bytes(offset - out.tell()))

            entry["file_offset"] = offset

            if native_data is not None:
                out.write(native_data)
            else:
                with source.open("rb") as src:
                    shutil.copyfileobj(src, out, length=1024 * 1024)

    for stale in data_dir.glob("*.bin"):
        if stale.name != PACKED_ASSET_FILENAME:
            stale.unlink()


def annotate_asset_flags(entries: list[dict[str, object]], native_assets: NativeAssetContext) -> None:
    for entry in entries:
        name = str(entry["name"])
        flags = 0

        if native_assets.build_native_segment(entry) is not None:
            flags |= NATIVE_ASSET_FLAG
            if name in native_assets.texture_ranges_by_segment:
                flags |= TEXTURE_WORDS_FLAG

        entry["flags"] = flags


def emit_asm(output: Path, entries: list[dict[str, object]], native_assets: NativeAssetContext) -> None:
    lines: list[str] = [
        "/* Generated by tools/psp_port_asset_segments.py. */",
        "/* Raw bytes live in data/segments next to EBOOT.PBP; these are PSP-side ROM handles. */",
        "",
    ]

    for entry in entries:
        name = entry["name"]
        vrom_start = entry["vrom_start"]
        vrom_end = entry["vrom_end"]
        lines.extend(
            [
                f".global _{name}SegmentStart",
                f".equ _{name}SegmentStart, 0x{vrom_start:08X}",
                f".global _{name}SegmentEnd",
                f".equ _{name}SegmentEnd, 0x{vrom_end:08X}",
                f".global _{name}SegmentRomStart",
                f".equ _{name}SegmentRomStart, 0x{vrom_start:08X}",
                f".global _{name}SegmentRomEnd",
                f".equ _{name}SegmentRomEnd, 0x{vrom_end:08X}",
                "",
            ]
        )

    linker_symbols = native_assets.linker_symbol_values({"objects", "scenes"})
    if linker_symbols:
        lines.extend(
            [
                "/* Object and scene asset data is loaded at runtime; expose symbols as segmented addresses. */",
                "",
            ]
        )

        for name, value in sorted(linker_symbols.items()):
            lines.extend(
                [
                    f".global {name}",
                    f".equ {name}, 0x{value:08X}",
                    "",
                ]
            )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))


def emit_message_entries(lines: list[str], symbol: str, entries: list[dict[str, int]]) -> None:
    lines.append(f"const OotPspMessageEntry {symbol}[] = {{")

    if entries:
        for entry in entries:
            lines.append(
                f"    {{ 0x{entry['text_id']:04X}, 0x{entry['type_pos']:02X}, 0, "
                f"0x{entry['vrom_start']:08X}, 0x{entry['vrom_end']:08X} }},"
            )
    else:
        lines.append("    { 0xFFFF, 0, 0, 0, 0 },")

    lines.append("};")
    lines.append(f"const size_t {symbol}Count = {len(entries)};")
    lines.append("")


def merge_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []

    for start, end in sorted(ranges):
        if end <= start:
            continue

        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))

    return merged


def build_texture_range_entries(
    entries: list[dict[str, object]], native_assets: NativeAssetContext
) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []

    for entry in entries:
        flags = int(entry["flags"])

        if (flags & (NATIVE_ASSET_FLAG | TEXTURE_WORDS_FLAG)) != (NATIVE_ASSET_FLAG | TEXTURE_WORDS_FLAG):
            continue

        segment_name = str(entry["name"])
        segment_size = int(entry["size"])
        vrom_start = int(entry["vrom_start"])

        for start, end in native_assets.texture_ranges_by_segment.get(segment_name, []):
            clipped_start = max(0, min(start, segment_size))
            clipped_end = max(0, min(end, segment_size))

            if clipped_end > clipped_start:
                ranges.append((vrom_start + clipped_start, vrom_start + clipped_end))

    return merge_ranges(ranges)


def emit_texture_ranges(lines: list[str], ranges: list[tuple[int, int]]) -> None:
    lines.append("const OotPspExternalAssetTextureRange gOotPspExternalAssetTextureRanges[] = {")

    if ranges:
        for start, end in ranges:
            lines.append(f"    {{ 0x{start:08X}, 0x{end:08X} }},")
    else:
        lines.append("    { 0, 0 },")

    lines.append("};")
    lines.append(f"const size_t gOotPspExternalAssetTextureRangeCount = {len(ranges)};")
    lines.append("")


def emit_table(
    output: Path,
    entries: list[dict[str, object]],
    message_entries: dict[str, list[dict[str, int]]],
    native_assets: NativeAssetContext,
) -> None:
    lines: list[str] = [
        "/* Generated by tools/psp_port_asset_segments.py. */",
        '#include "oot_psp_asset_loader.h"',
        "",
        "const OotPspExternalAsset gOotPspExternalAssets[] = {",
    ]

    for entry in entries:
        path = entry["runtime_path"]
        assert isinstance(path, Path)
        lines.append(
            f"    {{ 0x{entry['vrom_start']:08X}, 0x{entry['vrom_end']:08X}, "
            f"0x{entry['original_vrom_start']:08X}, 0x{entry['original_vrom_end']:08X}, "
            f"0x{entry['flags']:08X}, "
            f"0x{entry['file_offset']:08X}, "
            f'"{c_string(path.as_posix())}" }},'
        )

    lines.extend(
        [
            "};",
            "",
            "const size_t gOotPspExternalAssetCount =",
            "    sizeof(gOotPspExternalAssets) / sizeof(gOotPspExternalAssets[0]);",
            "",
        ]
    )

    emit_texture_ranges(lines, build_texture_range_entries(entries, native_assets))

    emit_message_entries(lines, "gOotPspJpnMessageEntries", message_entries["jpn"])
    emit_message_entries(lines, "gOotPspNesMessageEntries", message_entries["nes"])
    emit_message_entries(lines, "gOotPspGerMessageEntries", message_entries["ger"])
    emit_message_entries(lines, "gOotPspFraMessageEntries", message_entries["fra"])
    emit_message_entries(lines, "gOotPspStaffMessageEntries", message_entries["staff"])

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))


def emit(version: str, asm_output: Path, table_output: Path, data_dir: Path, build_root: Path | None = None) -> None:
    entries = build_entries(version)
    native_assets = NativeAssetContext(version, entries, build_root)
    annotate_asset_flags(entries, native_assets)
    message_entries = build_message_entries(version, entries)
    copy_data_files(entries, data_dir, native_assets)
    emit_asm(asm_output, entries, native_assets)
    emit_table(table_output, entries, message_entries, native_assets)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate PSP external raw asset segment symbols and data files.")
    parser.add_argument("version")
    parser.add_argument("asm_output", type=Path)
    parser.add_argument("table_output", type=Path)
    parser.add_argument("data_dir", type=Path)
    parser.add_argument("--build-root", type=Path, default=None)
    args = parser.parse_args()
    emit(args.version, args.asm_output, args.table_output, args.data_dir, args.build_root)


if __name__ == "__main__":
    main()
