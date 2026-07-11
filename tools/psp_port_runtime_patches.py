#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.psp_port_asset_segments import ElfObject, R_MIPS_32, SHN_UNDEF
from tools.psp_port_asset_snapshot import permutation, word_partitions
from tools import version_config


STT_OBJECT = 1
STT_FILE = 4
SHN_ABS = 0xFFF1
TABLE_RE = re.compile(
    r'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), '
    r'(0x[0-9A-F]+), (0x[0-9A-F]+), "([^"]+)" \}'
)
ENCODED_RESOURCE_RE = re.compile(
    r"^(?P<segment>.+)_(?P<offset>[0-9A-Fa-f]{8})_(?P<kind>Tex|CITex|TLUT)$"
)


@dataclass
class Resource:
    segment: str
    offset: int
    kind: str


@dataclass
class Patch:
    source_file: str
    symbol: str
    source_vrom: int
    size: int
    payload: bytes
    relocations: list[tuple[int, int]]


def symbol_type(info: int) -> int:
    return info & 0xF


def external_vrom_starts(table: Path) -> dict[str, int]:
    return {match[6]: int(match[0], 16) for match in TABLE_RE.findall(table.read_text())}


def resource_map(version: str) -> tuple[dict[tuple[str, str], Resource], dict[str, str], dict[str, int]]:
    config = version_config.load_version_config(version)
    resources: dict[tuple[str, str], Resource] = {}
    source_segments: dict[str, str] = {}
    segment_bases: dict[str, int] = {}

    for asset in config.assets:
        path = ROOT / asset.xml_path
        if not path.is_file():
            continue
        root = ET.parse(path).getroot()
        base = asset.start_offset or 0
        source_key = path.stem

        for file_elem in root.iter("File"):
            segment = file_elem.attrib.get("Name")
            if not segment:
                continue
            source_segments[source_key] = segment
            segment_bases[segment] = base
            for elem in file_elem.iter():
                name = elem.attrib.get("Name")
                offset = elem.attrib.get("Offset")
                if (name is not None) and (offset is not None):
                    resources[(segment, name)] = Resource(segment, base + int(offset, 0), elem.tag)

    return resources, source_segments, segment_bases


def segment_for_source(source_file: str, source_segments: dict[str, str]) -> str | None:
    parts = Path(source_file).parts
    for part in parts:
        if part.startswith("ovl_"):
            return part

    stem = Path(source_file).stem
    if stem in source_segments:
        return source_segments[stem]
    return None


def build_payload(source: bytes, target: bytes, permutations: list[bytes]) -> bytes:
    selectors = bytearray()
    mappings = bytearray()
    source_positions: dict[int, int] = {}

    for source_offset, value in enumerate(source):
        source_positions.setdefault(value, source_offset)

    for offset in range(0, len(source), 8):
        source_block = source[offset : offset + 8]
        target_block = target[offset : offset + 8]
        selector = len(permutations) + 1

        if len(source_block) == 8:
            for index, mapping in enumerate(permutations):
                if bytes(source_block[source_index] for source_index in mapping) == target_block:
                    selector = index
                    break
            else:
                if target_block == bytes(8):
                    selector = len(permutations)

        if selector == len(permutations) + 1:
            for byte_offset, value in enumerate(target_block):
                original_offset = offset + byte_offset
                source_offset = source_positions.get(value, original_offset)
                delta = (value - source[source_offset]) & 0xFF
                mappings.extend(struct.pack("<IB", source_offset, delta))
        selectors.append(selector)

    return selectors + mappings


def linked_symbol_bytes(elf: ElfObject, symbol) -> bytes:
    section = elf.sections[symbol.shndx]
    section_data = elf.section_data[symbol.shndx]
    offset = symbol.value - section.addr
    return section_data[offset : offset + symbol.size]


def symbol_relocations(elf: ElfObject, symbol) -> list[tuple[int, int]]:
    result: dict[int, int] = {}
    section = elf.sections[symbol.shndx]
    symbol_data = linked_symbol_bytes(elf, symbol)

    for relocation in elf.relocs_by_section.get(symbol.shndx, []):
        absolute_offset = relocation.offset if relocation.offset >= section.addr else section.addr + relocation.offset
        if not (symbol.value <= absolute_offset < symbol.value + symbol.size):
            continue
        if relocation.type != R_MIPS_32:
            continue
        target = elf.symtabs[relocation.symtab_index][relocation.sym_index]
        if (target.shndx not in (SHN_UNDEF, SHN_ABS)) and target.value < 0x20000000:
            relative_offset = absolute_offset - symbol.value
            result[relative_offset] = struct.unpack_from("<I", symbol_data, relative_offset)[0]
    return sorted(result.items())


def create_manifest(version: str, elf_path: Path, table_path: Path, extracted_dir: Path, output: Path) -> None:
    config = version_config.load_version_config(version)
    elf = ElfObject(elf_path)
    resources, source_segments, segment_bases = resource_map(version)
    vrom_starts = external_vrom_starts(table_path)
    resources_by_name: dict[str, list[Resource]] = {}
    for (_segment, name), resource in resources.items():
        if resource.segment in vrom_starts:
            resources_by_name.setdefault(name, []).append(resource)
    permutations = [permutation(parts) for parts in word_partitions(8)]
    patches: list[Patch] = []
    patch_keys: set[tuple[str, str]] = set()
    objects_by_value: dict[int, list[object]] = {}
    for symbols in elf.symtabs.values():
        for candidate in symbols:
            if symbol_type(candidate.info) == STT_OBJECT and candidate.size != 0:
                bucket = objects_by_value.setdefault(candidate.value, [])
                if not any(existing.name == candidate.name and existing.size == candidate.size for existing in bucket):
                    bucket.append(candidate)

    for symbols in elf.symtabs.values():
        source_file: str | None = None
        for symbol in symbols:
            kind = symbol_type(symbol.info)
            if kind == STT_FILE:
                source_file = symbol.name
                continue
            if (
                (source_file is None)
                or (kind != STT_OBJECT)
                or (symbol.size == 0)
                or (symbol.shndx in (SHN_UNDEF, SHN_ABS))
                or (symbol.shndx >= len(elf.sections))
            ):
                continue

            segment = segment_for_source(source_file, source_segments)
            resource = resources.get((segment, symbol.name)) if segment is not None else None
            if resource is None:
                # Linked PSP ELFs do not reliably keep STT_FILE markers adjacent
                # to global data symbols. Fall back to an unambiguous XML resource
                # name rather than silently leaving the clean placeholder empty.
                candidates = resources_by_name.get(symbol.name, [])
                if len(candidates) == 1:
                    resource = candidates[0]
                    segment = resource.segment
            if resource is None:
                match = ENCODED_RESOURCE_RE.fullmatch(symbol.name)
                if match is not None:
                    encoded_segment = match.group("segment")
                    if (encoded_segment in vrom_starts) and (encoded_segment in segment_bases):
                        segment = encoded_segment
                        resource = Resource(
                            segment,
                            segment_bases[segment] + int(match.group("offset"), 16),
                            match.group("kind"),
                        )
            if (resource is None) or (segment not in vrom_starts):
                continue

            raw_segment = (extracted_dir / segment).read_bytes()
            end = resource.offset + symbol.size
            if end > len(raw_segment):
                raise ValueError(f"{source_file}:{symbol.name} exceeds raw {segment}")

            source = raw_segment[resource.offset:end]
            target = linked_symbol_bytes(elf, symbol)
            if len(target) != symbol.size:
                raise ValueError(f"could not read linked bytes for {source_file}:{symbol.name}")

            patch_key = (source_file, symbol.name)
            if patch_key in patch_keys:
                continue
            patches.append(
                Patch(source_file, symbol.name, vrom_starts[segment] + resource.offset, symbol.size,
                      build_payload(source, target, permutations), symbol_relocations(elf, symbol))
            )
            patch_keys.add(patch_key)

            # ZAPD emits an Animation header plus separate frame-data and
            # joint-index arrays. Only the header has an XML offset, so derive
            # the two child ranges from its original pointers and relocations.
            if resource.kind in ("Animation", "CurveAnimation", "Collision") and symbol.size >= 12:
                segment_vram = config.dmadata_segments[segment].vram
                targets = relocation_targets(elf, symbol)
                if resource.kind == "Animation":
                    pointer_offsets = (4, 8)
                elif resource.kind == "CurveAnimation":
                    pointer_offsets = (0, 4, 8)
                else:
                    pointer_offsets = tuple(targets)
                for pointer_offset in pointer_offsets:
                    child_symbol = targets.get(pointer_offset)
                    if (child_symbol is None) or (child_symbol.size == 0):
                        linked_pointer = struct.unpack_from("<I", target, pointer_offset)[0]
                        candidates = objects_by_value.get(linked_pointer, [])
                        child_symbol = candidates[0] if len(candidates) == 1 else None
                    if child_symbol is None or child_symbol.size == 0:
                        continue
                    child_key = (source_file, child_symbol.name)
                    if child_key in patch_keys:
                        continue
                    pointer = struct.unpack_from(">I", raw_segment, resource.offset + pointer_offset)[0]
                    if (segment_vram is not None) and (pointer >= segment_vram):
                        child_offset = pointer - segment_vram
                    else:
                        child_offset = pointer & 0x00FFFFFF
                    child_end = child_offset + child_symbol.size
                    if child_end > len(raw_segment):
                        raise ValueError(f"{source_file}:{child_symbol.name} exceeds raw {segment}")
                    child_source = raw_segment[child_offset:child_end]
                    child_target = linked_symbol_bytes(elf, child_symbol)
                    patches.append(
                        Patch(source_file, child_symbol.name, vrom_starts[segment] + child_offset,
                              child_symbol.size, build_payload(child_source, child_target, permutations),
                              symbol_relocations(elf, child_symbol))
                    )
                    patch_keys.add(child_key)

    output_data = bytearray(b"OPR2" + struct.pack("<II", len(patches), len(permutations)))
    for mapping in permutations:
        output_data.extend(mapping)

    for patch in patches:
        source_name = patch.source_file.encode("utf-8")
        symbol_name = patch.symbol.encode("utf-8")
        compressed = zlib.compress(patch.payload, level=9)
        output_data.extend(
            struct.pack(
                "<HHIIIII",
                len(source_name),
                len(symbol_name),
                patch.source_vrom,
                patch.size,
                len(patch.payload),
                len(compressed),
                len(patch.relocations),
            )
        )
        output_data.extend(source_name)
        output_data.extend(symbol_name)
        output_data.extend(compressed)
        for offset, target_value in patch.relocations:
            output_data.extend(struct.pack("<II", offset, target_value))

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(output_data)
    print(f"wrote {len(patches)} clean runtime patch recipes to {output} ({len(output_data)} bytes)")


def linked_objects_by_file(elf: ElfObject) -> dict[tuple[str, str], object]:
    result: dict[tuple[str, str], object] = {}
    for symbols in elf.symtabs.values():
        source_file: str | None = None
        for symbol in symbols:
            kind = symbol_type(symbol.info)
            if kind == STT_FILE:
                source_file = symbol.name
            elif (source_file is not None) and (kind == STT_OBJECT) and (symbol.size != 0):
                result[(source_file, symbol.name)] = symbol
    return result


def relocation_targets(elf: ElfObject, symbol) -> dict[int, object]:
    result: dict[int, object] = {}
    section = elf.sections[symbol.shndx]

    for relocation in elf.relocs_by_section.get(symbol.shndx, []):
        absolute_offset = relocation.offset if relocation.offset >= section.addr else section.addr + relocation.offset
        if not (symbol.value <= absolute_offset < symbol.value + symbol.size) or relocation.type != R_MIPS_32:
            continue
        target = elf.symtabs[relocation.symtab_index][relocation.sym_index]
        if (target.shndx not in (SHN_UNDEF, SHN_ABS)) and target.value < 0x20000000:
            result[absolute_offset - symbol.value] = target
    return result


def resolve_manifest(elf_path: Path, manifest_path: Path, output: Path, base_elf_path: Path | None = None) -> None:
    elf = ElfObject(elf_path)
    objects = linked_objects_by_file(elf)
    objects_by_name: dict[str, list[object]] = {}
    for (_source_file, name), candidate in objects.items():
        bucket = objects_by_name.setdefault(name, [])
        if not any(existing.value == candidate.value and existing.size == candidate.size for existing in bucket):
            bucket.append(candidate)
    data = manifest_path.read_bytes()
    if data[:4] != b"OPR2":
        raise ValueError("invalid runtime patch manifest")

    count, permutation_count = struct.unpack_from("<II", data, 4)
    cursor = 12
    permutation_data = data[cursor : cursor + permutation_count * 8]
    cursor += len(permutation_data)
    records: list[bytes] = []

    for _ in range(count):
        source_len, symbol_len, source_vrom, size, payload_size, compressed_size, reloc_count = struct.unpack_from(
            "<HHIIIII", data, cursor
        )
        cursor += 24
        source_file = data[cursor : cursor + source_len].decode("utf-8")
        cursor += source_len
        symbol_name = data[cursor : cursor + symbol_len].decode("utf-8")
        cursor += symbol_len
        compressed = data[cursor : cursor + compressed_size]
        cursor += compressed_size
        relocation_data = data[cursor : cursor + reloc_count * 8]
        cursor += len(relocation_data)
        symbol = objects.get((source_file, symbol_name))
        if symbol is None:
            candidates = [candidate for candidate in objects_by_name.get(symbol_name, []) if candidate.size == size]
            if len(candidates) == 1:
                symbol = candidates[0]
        if symbol is None:
            raise ValueError(f"clean ELF is missing {source_file}:{symbol_name}")
        if symbol.size != size:
            raise ValueError(f"clean ELF size mismatch for {source_file}:{symbol_name}")
        reference_relocations = list(struct.iter_unpack("<II", relocation_data))
        relocation_offsets_list = [offset for offset, _target in reference_relocations]
        targets = relocation_targets(elf, symbol)
        if set(relocation_offsets_list) != set(targets):
            raise ValueError(f"clean ELF relocation mismatch for {source_file}:{symbol_name}")
        clean_symbol_data = linked_symbol_bytes(elf, symbol)
        adjustments: list[int] = []
        for offset, reference_pointer_value in reference_relocations:
            clean_pointer_value = struct.unpack_from("<I", clean_symbol_data, offset)[0]
            adjustment = clean_pointer_value - reference_pointer_value
            if not -(1 << 31) <= adjustment < (1 << 31):
                raise ValueError(f"runtime relocation adjustment is too large for {source_file}:{symbol_name}")
            adjustments.append(adjustment)

        records.append(
            struct.pack(
                "<IIIIII",
                symbol.value,
                source_vrom,
                size,
                payload_size,
                compressed_size,
                reloc_count,
            )
            + compressed
            + b"".join(
                struct.pack("<Ii", offset, adjustment)
                for offset, adjustment in zip(relocation_offsets_list, adjustments)
            )
        )

    if cursor != len(data):
        raise ValueError("trailing runtime patch manifest data")

    output_data = bytearray(b"OPB1" + struct.pack("<II", count, permutation_count))
    output_data.extend(permutation_data)
    for record in records:
        output_data.extend(record)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(output_data)
    print(f"resolved {count} runtime patch destinations into {output} ({len(output_data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Build ROM-free PSP runtime asset patch recipes")
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create")
    create.add_argument("version")
    create.add_argument("elf", type=Path)
    create.add_argument("asset_table", type=Path)
    create.add_argument("extracted_dir", type=Path)
    create.add_argument("output", type=Path)

    resolve = subparsers.add_parser("resolve")
    resolve.add_argument("elf", type=Path)
    resolve.add_argument("manifest", type=Path)
    resolve.add_argument("output", type=Path)
    resolve.add_argument("--base-elf", type=Path)

    args = parser.parse_args()
    if args.command == "create":
        create_manifest(args.version, args.elf, args.asset_table, args.extracted_dir, args.output)
    else:
        resolve_manifest(args.elf, args.manifest, args.output, args.base_elf)


if __name__ == "__main__":
    main()
