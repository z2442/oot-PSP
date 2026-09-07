#!/usr/bin/env python3

from __future__ import annotations

import argparse
import dataclasses
import re
import struct
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXTERNAL_VROM_BASE = 0x20000000
NATIVE_ASSET_FLAGS = 3
ZERO_SELECTOR = 56
BASE_SNAPSHOT = ROOT / "assets/psp/ntsc-1.0/generated.zip"
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
ASSET_ENTRY_RE = re.compile(
    r'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), '
    r'(0x[0-9A-F]+), (0x[0-9A-F]+), "([^"]+)" \}'
)
TEXTURE_RANGE_RE = re.compile(r"    \{ (0x[0-9A-F]+), (0x[0-9A-F]+) \},")
REGIONAL_XMLS = {
    "icon_item_ger_static": ROOT / "assets/xml/textures/icon_item_ger_static.xml",
    "icon_item_fra_static": ROOT / "assets/xml/textures/icon_item_fra_static.xml",
}
RUNTIME_MESSAGE_SEGMENTS = {
    "ger_message_data_static": 0x46000000,
    "fra_message_data_static": 0x47000000,
}


def align(value: int, amount: int = 16) -> int:
    return (value + amount - 1) & -amount


def write_text_if_changed(path: Path, data: str) -> None:
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def write_bytes_if_changed(path: Path, data: bytes) -> None:
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


@dataclasses.dataclass(frozen=True)
class Resource:
    name: str
    offset: int
    size: int
    kind: str


@dataclasses.dataclass
class AssetEntry:
    vrom_start: int
    vrom_end: int
    original_start: int
    original_end: int
    flags: int
    file_offset: int
    name: str

    @property
    def size(self) -> int:
        return self.vrom_end - self.vrom_start

    def format(self) -> str:
        return (
            f'    {{ 0x{self.vrom_start:08X}, 0x{self.vrom_end:08X}, '
            f'0x{self.original_start:08X}, 0x{self.original_end:08X}, '
            f'0x{self.flags:08X}, 0x{self.file_offset:08X}, "{self.name}" }},'
        )


def texture_size(elem: ET.Element) -> int:
    bits = TEXTURE_FORMAT_BITS[elem.attrib["Format"].lower()]
    width = int(elem.attrib["Width"], 0)
    height = int(elem.attrib["Height"], 0)
    return (width * height * bits + 7) // 8


def read_texture_resources(path: Path, file_name: str | None = None) -> dict[str, Resource]:
    result: dict[str, Resource] = {}
    root = ET.parse(path).getroot()
    for file_elem in root.iter("File"):
        if file_name is not None and file_elem.attrib.get("Name") != file_name:
            continue
        for elem in file_elem.iter("Texture"):
            name = elem.attrib.get("Name")
            offset = elem.attrib.get("Offset")
            if name is None or offset is None:
                continue
            resource = Resource(name, int(offset, 0), texture_size(elem), "Texture")
            previous = result.get(name)
            if previous is not None and previous.size != resource.size:
                raise ValueError(f"conflicting texture definitions for {name}")
            result[name] = resource
    return result


def read_base_sizes() -> dict[str, int]:
    with zipfile.ZipFile(BASE_SNAPSHOT) as archive:
        text = archive.read("oot_psp_asset_segments.c").decode()
    return {entry.name: entry.size for entry in parse_entries(text)}


def config_file_elements(config: object) -> dict[str, list[tuple[Path, ET.Element]]]:
    result: dict[str, list[tuple[Path, ET.Element]]] = {}
    for asset in config.assets:
        path = ROOT / asset.xml_path
        for file_elem in ET.parse(path).getroot().iter("File"):
            name = file_elem.attrib.get("Name")
            if name:
                result.setdefault(name, []).append((path, file_elem))
    return result


def build_union_layout(base_sizes: dict[str, int]) -> dict[str, tuple[int, dict[str, Resource]]]:
    import version_config

    canonical = config_file_elements(version_config.load_version_config("ntsc-1.0"))
    canonical_symbols: dict[str, set[str]] = {}
    canonical_textures: dict[str, dict[str, Resource]] = {}
    for file_name, elements in canonical.items():
        symbols: set[str] = set()
        textures: dict[str, Resource] = {}
        for path, file_elem in elements:
            symbols.update(
                elem.attrib["Name"] for elem in file_elem.iter()
                if "Name" in elem.attrib and elem is not file_elem
            )
            textures.update(read_texture_resources(path, file_name))
        canonical_symbols[file_name] = symbols
        canonical_textures[file_name] = textures

    additions: dict[str, dict[str, Resource]] = {}
    invalid: set[str] = set()
    for version_dir in sorted((ROOT / "baseroms").iterdir()):
        if not (version_dir / "config.yml").is_file() or not (version_dir / "segments.csv").is_file():
            continue
        files = config_file_elements(version_config.load_version_config(version_dir.name))
        for file_name, elements in files.items():
            if file_name not in base_sizes or file_name not in canonical:
                continue
            base_symbols = canonical_symbols[file_name]
            for _path, file_elem in elements:
                for elem in file_elem.iter():
                    name = elem.attrib.get("Name")
                    offset = elem.attrib.get("Offset")
                    if name is None or offset is None or name in base_symbols:
                        continue
                    if elem.tag != "Texture":
                        invalid.add(file_name)
                        continue
                    resource = Resource(name, int(offset, 0), texture_size(elem), "Texture")
                    previous = additions.setdefault(file_name, {}).get(name)
                    if previous is not None and previous.size != resource.size:
                        raise ValueError(f"conflicting regional texture definitions for {name}")
                    additions[file_name][name] = resource

    result: dict[str, tuple[int, dict[str, Resource]]] = {}
    for file_name in sorted(additions.keys() - invalid):
        resources = dict(canonical_textures[file_name])
        cursor = align(base_sizes[file_name], 8)
        for name, source in sorted(additions[file_name].items()):
            cursor = align(cursor, 8)
            resources[name] = Resource(name, cursor, source.size, source.kind)
            cursor += source.size
        result[file_name] = (align(cursor), resources)

    for segment, path in REGIONAL_XMLS.items():
        resources = read_texture_resources(path, segment)
        size = align(max((resource.offset + resource.size for resource in resources.values()), default=0))
        result[segment] = (size, resources)
    return result


def union_schema(base_sizes: dict[str, int]) -> dict[str, tuple[int, dict[str, int]]]:
    import version_config

    config = version_config.load_version_config("ntsc-1.0")
    result: dict[str, tuple[int, dict[str, int]]] = {}
    for asset in config.assets:
        base = asset.start_offset or 0
        root = ET.parse(ROOT / asset.xml_path).getroot()
        for file_elem in root.iter("File"):
            name = file_elem.attrib.get("Name")
            if not name:
                continue
            offsets = {
                elem.attrib["Name"]: base + int(elem.attrib["Offset"], 0)
                for elem in file_elem.iter()
                if "Name" in elem.attrib and "Offset" in elem.attrib
            }
            result[name] = (int(file_elem.attrib.get("Segment", "0"), 0), offsets)

    canonical_sizes = read_base_sizes()
    layout = build_union_layout(canonical_sizes)
    for name, (_size, resources) in layout.items():
        if base_sizes.get(name) != layout[name][0]:
            raise ValueError(f"PSP asset table does not use the regional union layout for {name}")
        if name in result:
            segment_id = result[name][0]
            offsets = dict(result[name][1])
            offsets.update({symbol: resource.offset for symbol, resource in resources.items()})
        else:
            root = ET.parse(REGIONAL_XMLS[name]).getroot()
            file_elem = next(elem for elem in root.iter("File") if elem.attrib.get("Name") == name)
            segment_id = int(file_elem.attrib.get("Segment", "0"), 0)
            offsets = {symbol: resource.offset for symbol, resource in resources.items()}
        result[name] = (segment_id, offsets)
    return result


def parse_entries(text: str) -> list[AssetEntry]:
    return [
        AssetEntry(*(int(value, 16) for value in match[:6]), match[6])
        for match in ASSET_ENTRY_RE.findall(text)
    ]


def replace_asset_array(text: str, entries: list[AssetEntry]) -> str:
    start = text.index("const OotPspExternalAsset gOotPspExternalAssets[] = {")
    body_start = text.index("\n", start) + 1
    body_end = text.index("};", body_start)
    body = "\n".join(entry.format() for entry in entries) + "\n"
    return text[:body_start] + body + text[body_end:]


def add_legacy_ranges(text: str, old_entries: list[AssetEntry], new_entries: list[AssetEntry]) -> str:
    new_by_name = {entry.name: entry for entry in new_entries}
    ranges = [
        (old.vrom_start, old.vrom_end, new_by_name[old.name].vrom_start)
        for old in old_entries
        if old.name in new_by_name and old.vrom_start != new_by_name[old.name].vrom_start
    ]
    body = (
        "const OotPspExternalAssetLegacyRange gOotPspExternalAssetLegacyRanges[] = {\n"
        + "".join(f"    {{ 0x{start:08X}, 0x{end:08X}, 0x{target:08X} }},\n"
                  for start, end, target in ranges)
        + "};\n"
        + "const size_t gOotPspExternalAssetLegacyRangeCount = "
        + "sizeof(gOotPspExternalAssetLegacyRanges) / sizeof(gOotPspExternalAssetLegacyRanges[0]);\n\n"
    )
    marker = "const OotPspExternalAssetTextureRange gOotPspExternalAssetTextureRanges[] = {"
    return text.replace(marker, body + marker, 1)


def remap_ranges(text: str, old_entries: list[AssetEntry], new_entries: list[AssetEntry],
                 layout: dict[str, tuple[int, dict[str, Resource]]]) -> str:
    start = text.index("const OotPspExternalAssetTextureRange gOotPspExternalAssetTextureRanges[] = {")
    body_start = text.index("\n", start) + 1
    body_end = text.index("};", body_start)
    old_ranges = [(int(a, 16), int(b, 16)) for a, b in TEXTURE_RANGE_RE.findall(text[body_start:body_end])]
    new_by_name = {entry.name: entry for entry in new_entries}
    ranges: list[tuple[int, int]] = []

    for range_start, range_end in old_ranges:
        old = next(
            (entry for entry in old_entries if entry.vrom_start <= range_start < entry.vrom_end),
            None,
        )
        if old is None:
            continue
        new = new_by_name[old.name]
        ranges.append((new.vrom_start + range_start - old.vrom_start,
                       new.vrom_start + range_end - old.vrom_start))

    old_by_name = {entry.name: entry for entry in old_entries}
    for segment, (_size, resources) in layout.items():
        if segment not in old_by_name:
            continue
        entry = new_by_name[segment]
        base_size = old_by_name[segment].size
        for resource in resources.values():
            if resource.offset < base_size:
                continue
            ranges.append((entry.vrom_start + resource.offset,
                           entry.vrom_start + resource.offset + resource.size))
    for segment in REGIONAL_XMLS:
        entry = new_by_name[segment]
        for resource in layout[segment][1].values():
            ranges.append((entry.vrom_start + resource.offset,
                           entry.vrom_start + resource.offset + resource.size))

    ranges.sort()
    merged: list[tuple[int, int]] = []
    for range_start, range_end in ranges:
        if merged and range_start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], range_end))
        else:
            merged.append((range_start, range_end))
    body = "".join(f"    {{ 0x{a:08X}, 0x{b:08X} }},\n" for a, b in merged)
    count_marker = "const size_t gOotPspExternalAssetTextureRangeCount = "
    result = text[:body_start] + body + text[body_end:]
    result = re.sub(
        re.escape(count_marker) + r"\d+;",
        count_marker + f"{len(merged)};",
        result,
    )
    return result


def rewrite_asm(text: str, entries: list[AssetEntry], layout: dict[str, tuple[int, dict[str, Resource]]],
                segment_ids: dict[str, int]) -> str:
    for entry in entries:
        values = {
            f"_{entry.name}SegmentStart": entry.vrom_start,
            f"_{entry.name}SegmentEnd": entry.vrom_end,
            f"_{entry.name}SegmentRomStart": entry.vrom_start,
            f"_{entry.name}SegmentRomEnd": entry.vrom_end,
        }
        for symbol, value in values.items():
            pattern = rf"(\.equ {re.escape(symbol)}, )0x[0-9A-Fa-f]+"
            replacement = rf"\g<1>0x{value:08X}"
            if re.search(pattern, text):
                text = re.sub(pattern, replacement, text)
            else:
                text += f"\n.global {symbol}\n.equ {symbol}, 0x{value:08X}\n"

    existing = set(re.findall(r"^\.global ([A-Za-z_][A-Za-z0-9_]*)$", text, re.MULTILINE))
    for segment, (_size, resources) in layout.items():
        segment_id = segment_ids[segment]
        for resource in sorted(resources.values(), key=lambda item: (item.offset, item.name)):
            if resource.name in existing:
                continue
            value = (segment_id << 24) | resource.offset
            text += f"\n.global {resource.name}\n.equ {resource.name}, 0x{value:08X}\n"
            existing.add(resource.name)

    # Message banks are packed at runtime-sized virtual ranges recorded in the
    # asset identity. NTSC has no German/French table entries, but PAL game code
    # still needs stable link-time bases to calculate offsets into those ranges.
    for segment, start in RUNTIME_MESSAGE_SEGMENTS.items():
        end = start + 0x01000000
        for suffix, value in (
            ("SegmentStart", start),
            ("SegmentEnd", end),
            ("SegmentRomStart", start),
            ("SegmentRomEnd", end),
        ):
            symbol = f"_{segment}{suffix}"
            if symbol not in existing:
                text += f"\n.global {symbol}\n.equ {symbol}, 0x{value:08X}\n"
                existing.add(symbol)
    return text


def rewrite_scene_symbols(text: str, version: str) -> str:
    """Scene data retains the source ROM layout; object aliases stay canonical."""
    import version_config
    if version == "ntsc-1.0":
        return text
    for asset in version_config.load_version_config(version).assets:
        for file_elem in ET.parse(ROOT / asset.xml_path).getroot().iter("File"):
            name = file_elem.attrib.get("Name", "")
            if not (name.endswith("_scene") or "_room_" in name):
                continue
            segment = int(file_elem.attrib["Segment"], 0)
            for elem in file_elem.iter():
                symbol, offset = elem.attrib.get("Name"), elem.attrib.get("Offset")
                if symbol and offset:
                    value = (segment << 24) | ((asset.start_offset or 0) + int(offset, 0))
                    text = re.sub(rf"(\.equ {re.escape(symbol)}, )0x[0-9A-Fa-f]+",
                                  rf"\g<1>0x{value:08X}", text)
    return text


def permutation_index(permutations: list[bytes], mapping: bytes) -> int:
    try:
        return permutations.index(mapping)
    except ValueError as error:
        raise ValueError(f"asset transform lacks permutation {mapping.hex()}") from error


def rewrite_transform(data: bytes, old_entries: list[AssetEntry], new_entries: list[AssetEntry],
                      layout: dict[str, tuple[int, dict[str, Resource]]]) -> bytes:
    if data[:4] != b"OPZ4":
        raise ValueError("invalid PSP asset transform")
    record_count, permutation_count = struct.unpack_from("<II", data, 4)
    cursor = 12
    permutations = [data[cursor + i * 8:cursor + (i + 1) * 8] for i in range(permutation_count)]
    permutation_data = data[cursor:cursor + permutation_count * 8]
    cursor += len(permutation_data)
    records: dict[int, tuple[int, bytes]] = {}
    for _ in range(record_count):
        asset_index, size, payload_size, compressed_size = struct.unpack_from("<IIII", data, cursor)
        cursor += 16
        payload = zlib.decompress(data[cursor:cursor + compressed_size])
        cursor += compressed_size
        if len(payload) != payload_size:
            raise ValueError("invalid PSP asset transform payload")
        records[asset_index] = (size, payload)
    if cursor != len(data):
        raise ValueError("trailing PSP asset transform data")

    old_index = {entry.name: index for index, entry in enumerate(old_entries)}
    new_index = {entry.name: index for index, entry in enumerate(new_entries)}
    reverse_u64 = permutation_index(permutations, bytes(reversed(range(8))))
    output_records: list[tuple[int, int, bytes]] = []
    old_by_name = {entry.name: entry for entry in old_entries}

    for index, (old_size, payload) in records.items():
        name = old_entries[index].name
        target_index = new_index[name]
        target_size = new_entries[target_index].size
        old_selector_count = (old_size + 7) // 8
        mappings = payload[old_selector_count:]
        selectors = bytearray([ZERO_SELECTOR] * ((target_size + 7) // 8))
        selectors[:old_selector_count] = payload[:old_selector_count]
        if name in layout:
            for symbol, resource in layout[name][1].items():
                if resource.offset < old_by_name[name].size:
                    continue
                if (resource.offset % 8) or (resource.size % 8):
                    raise ValueError(f"unaligned regional texture {symbol}")
                selectors[resource.offset // 8:(resource.offset + resource.size) // 8] = bytes(
                    [reverse_u64] * (resource.size // 8)
                )
        output_records.append((target_index, target_size, bytes(selectors) + mappings))

    for segment in REGIONAL_XMLS:
        index = new_index[segment]
        size = new_entries[index].size
        selectors = bytearray([ZERO_SELECTOR] * ((size + 7) // 8))
        for resource in layout[segment][1].values():
            if (resource.offset % 8) or (resource.size % 8):
                raise ValueError(f"unaligned regional texture {resource.name}")
            selectors[resource.offset // 8:(resource.offset + resource.size) // 8] = bytes(
                [reverse_u64] * (resource.size // 8)
            )
        output_records.append((index, size, bytes(selectors)))

    output_records.sort()
    output = bytearray(b"OPZ4" + struct.pack("<II", len(output_records), permutation_count) + permutation_data)
    for index, size, payload in output_records:
        compressed = zlib.compress(payload, level=9)
        output.extend(struct.pack("<IIII", index, size, len(payload), len(compressed)))
        output.extend(compressed)
    return bytes(output)


def augment(build_dir: Path, base_transform: Path, output_transform: Path, version: str = "ntsc-1.0") -> None:
    table_path = build_dir / "oot_psp_asset_segments.c"
    asm_path = build_dir / "oot_psp_asset_segments.S"
    table_text = table_path.read_text()
    old_entries = parse_entries(table_text)
    old_by_name = {entry.name: entry for entry in old_entries}
    base_sizes = {name: entry.size for name, entry in old_by_name.items()}
    layout = build_union_layout(base_sizes)

    sizes = dict(base_sizes)
    sizes.update({name: size for name, (size, _resources) in layout.items()})

    ordered_names = [entry.name for entry in old_entries if entry.name != "code"]
    for segment in REGIONAL_XMLS:
        if segment not in ordered_names:
            ordered_names.append(segment)
    ordered_names.append("code")

    cursor = old_entries[0].vrom_start
    new_entries: list[AssetEntry] = []
    for name in ordered_names:
        old = old_by_name.get(name)
        size = sizes[name]
        entry = AssetEntry(
            cursor,
            cursor + size,
            old.original_start if old is not None else 0,
            old.original_end if old is not None else 0,
            old.flags if old is not None else NATIVE_ASSET_FLAGS,
            cursor - EXTERNAL_VROM_BASE,
            name,
        )
        new_entries.append(entry)
        cursor += size

    table_text = replace_asset_array(table_text, new_entries)
    table_text = add_legacy_ranges(table_text, old_entries, new_entries)
    table_text = remap_ranges(table_text, old_entries, new_entries, layout)
    write_text_if_changed(table_path, table_text)
    schema = union_schema(sizes)
    segment_ids = {name: schema[name][0] for name in layout}
    write_text_if_changed(asm_path, rewrite_scene_symbols(
        rewrite_asm(asm_path.read_text(), new_entries, layout, segment_ids), version))
    scene_refs = set(re.findall(r"OOT_PSP_(?:SCENE|ROOM)_ASSET\((\w+),",
                                (ROOT / "src/code/z_scene_table.c").read_text())) - {"name"}
    write_text_if_changed(build_dir / "oot_psp_scene_symbols.h",
        "/* Scene draw resources; addresses are linked for the selected ROM. */\n" +
        "".join(f"extern unsigned char {name}[];\n" for name in sorted(scene_refs)))
    declarations: set[str] = set()
    for segment, (_size, resources) in layout.items():
        base_size = base_sizes.get(segment, 0)
        declarations.update(
            resource.name for resource in resources.values() if resource.offset >= base_size
        )
    regional_header = (
        "#ifndef OOT_PSP_REGIONAL_ASSETS_H\n"
        "#define OOT_PSP_REGIONAL_ASSETS_H\n\n"
        "/* Generated declarations for revision-only textures in the union schema. */\n"
        + "".join(f"extern unsigned long long {name}[];\n" for name in sorted(declarations))
        + "\n#endif\n"
    )
    write_text_if_changed(build_dir / "oot_psp_regional_assets.h", regional_header)
    output_transform.parent.mkdir(parents=True, exist_ok=True)
    write_bytes_if_changed(
        output_transform,
        rewrite_transform(base_transform.read_bytes(), old_entries, new_entries, layout),
    )
    print(
        f"augmented {len(layout) - len(REGIONAL_XMLS)} PSP segments with "
        f"{len(declarations)} revision-only textures and added {', '.join(REGIONAL_XMLS)} "
        f"({len(old_entries)} -> {len(new_entries)} segments)"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Add all regional UI assets to the PSP canonical asset schema")
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("base_transform", type=Path)
    parser.add_argument("output_transform", type=Path)
    parser.add_argument("--version", default="ntsc-1.0")
    args = parser.parse_args()
    augment(args.build_dir, args.base_transform, args.output_transform, args.version)


if __name__ == "__main__":
    main()
