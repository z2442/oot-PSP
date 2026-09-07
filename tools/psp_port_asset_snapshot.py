#!/usr/bin/env python3

from __future__ import annotations

import argparse
from collections import defaultdict
import mmap
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import version_config


GENERATED_FILES = (
    "oot_psp_asset_segments.S",
    "oot_psp_asset_segments.c",
    "oot_psp_asset_tables.c",
    "oot_psp_audio_tables.c",
    "oot_psp_rominfo.c",
)
RUNTIME_PREFIX = "runtime/"

STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')
CHAR_RE = re.compile(r"'(?:\\.|[^'\\])*'")
NUMBER_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:0[xX][0-9A-Fa-f]+|(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[fFlL]?|"
    r"[0-9]+(?:[eE][+-]?[0-9]+)?[uUlL]*)(?![A-Za-z0-9_])"
)

ASSET_ENTRY_RE = re.compile(
    r'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), '
    r'(0x[0-9A-F]+), (0x[0-9A-F]+), "([^"]+)" \}'
)

SCENE_LAYOUT_HEADER_COMMAND = 0xFF
SCENE_LAYOUT_CHILD_PAYLOAD = 0
SCENE_LAYOUT_CHILD_MESH_ENTRIES = 6
SCENE_LAYOUT_CHILD_MESH_DLIST = 7
SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS = 8
SCENE_LAYOUT_CHILD_IMAGE_SOURCE = 9
SCENE_LAYOUT_CHILD_IMAGE_TLUT = 10
SCENE_LAYOUT_CHILD_DLIST_VERTICES = 11
SCENE_LAYOUT_CHILD_DLIST_DATA = 12
SCENE_LAYOUT_COLLISION_FIELDS = (
    (1, 0x10, 0x0C, 6),
    (2, 0x18, 0x14, 0x10),
    (3, 0x1C, None, 8),
    (4, 0x20, None, 8),
    (5, 0x28, 0x24, 0x10),
)

ANIMATION_LAYOUT_HEADER = 0
ANIMATION_LAYOUT_FRAME_DATA = 1
ANIMATION_LAYOUT_JOINT_INDICES = 2

SKELETON_LAYOUT_HEADER = 0
SKELETON_LAYOUT_TABLE = 1
SKELETON_LAYOUT_LIMB = 2
SKELETON_LAYOUT_DLIST = 3
SKELETON_LAYOUT_SKIN_DATA = 4
SKELETON_LAYOUT_SKIN_MODIFS = 5
SKELETON_LAYOUT_SKIN_VERTICES = 6
SKELETON_LAYOUT_SKIN_TRANSFORMS = 7
SKELETON_LIMB_TYPES = {
    "Standard": (0, 12, (8,)),
    "LOD": (1, 16, (8, 12)),
    "Skin": (2, 16, ()),
    "Curve": (3, 12, (4, 8)),
}


def word_partitions(size: int) -> list[tuple[int, ...]]:
    result: list[tuple[int, ...]] = []

    def visit(remaining: int, parts: tuple[int, ...]) -> None:
        if remaining == 0:
            result.append(parts)
            return
        for width in (8, 4, 2, 1):
            if width <= remaining:
                visit(remaining - width, parts + (width,))

    visit(size, ())
    return result


def permutation(parts: tuple[int, ...]) -> bytes:
    result: list[int] = []
    offset = 0
    for width in parts:
        result.extend(reversed(range(offset, offset + width)))
        offset += width
    return bytes(result)


def parse_entries(table_path: Path) -> list[tuple[str, int, int, int]]:
    text = table_path.read_text()
    return [
        (name, int(vrom_end, 16) - int(vrom_start, 16), int(flags, 16), int(file_offset, 16))
        for vrom_start, vrom_end, _original_start, _original_end, flags, file_offset, name in ASSET_ENTRY_RE.findall(
            text
        )
    ]


def read_be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def scene_commands_size(data: bytes, offset: int) -> int | None:
    for cursor in range(offset, len(data) - 7, 8):
        if read_be32(data, cursor) == 0x14000000 and read_be32(data, cursor + 4) == 0:
            return cursor + 8 - offset
    return None


def cutscene_data_size(data: bytes, offset: int) -> int | None:
    word_count = (len(data) - offset) // 4
    if word_count < 2:
        return None
    command_count = read_be32(data, offset)
    cursor = 2
    for _ in range(command_count + 1):
        if cursor >= word_count:
            return None
        command = read_be32(data, offset + cursor * 4)
        if command == 0xFFFFFFFF:
            return (cursor + 1) * 4
        if command in (1, 2, 5, 6):
            cursor += 3
            while cursor + 4 <= word_count:
                continue_flag = data[offset + cursor * 4]
                cursor += 4
                if continue_flag == 0xFF:
                    break
                if continue_flag != 0:
                    return None
            continue
        if command in (7, 8):
            cursor += 7
            continue
        if command in (45, 1000):
            cursor += 4
            continue
        if cursor + 2 > word_count:
            return None
        item_count = read_be32(data, offset + (cursor + 1) * 4)
        cursor += 2 + item_count * (3 if command in (9, 19, 140) else 12)
    return None


def scene_header_offsets(data: bytes, segment: int) -> dict[int, tuple[int, int]]:
    primary_size = scene_commands_size(data, 0)
    if primary_size is None:
        return {}
    result = {0: (0, primary_size)}
    alternate_offset: int | None = None
    pointer_bounds: list[int] = []
    for cursor in range(0, primary_size, 8):
        command = data[cursor]
        pointer = read_be32(data, cursor + 4)
        if command == 0x18 and pointer >> 24 == segment:
            alternate_offset = pointer & 0xFFFFFF
        elif pointer >> 24 == segment:
            pointer_bounds.append(pointer & 0xFFFFFF)
    if alternate_offset is None or alternate_offset >= len(data):
        return result
    alternate_end = min(
        (offset for offset in pointer_bounds if offset > alternate_offset), default=len(data)
    )
    invalid_seen = False
    for slot in range(19):
        entry = alternate_offset + slot * 4
        if entry + 4 > alternate_end:
            break
        pointer = read_be32(data, entry)
        if pointer == 0:
            continue
        header_offset = pointer & 0xFFFFFF
        header_size = (
            scene_commands_size(data, header_offset)
            if pointer >> 24 == segment and header_offset < len(data)
            else None
        )
        if header_size is None:
            invalid_seen = True
            continue
        if invalid_seen:
            break
        result[slot + 1] = (header_offset, header_size)
    return result


def scene_header_commands(data: bytes, offset: int, size: int) -> list[tuple[int, int, int, int]]:
    occurrences: defaultdict[int, int] = defaultdict(int)
    result: list[tuple[int, int, int, int]] = []
    for cursor in range(offset, offset + size, 8):
        command = data[cursor]
        occurrence = occurrences[command]
        occurrences[command] += 1
        result.append((command, occurrence, data[cursor + 1], read_be32(data, cursor + 4)))
    return result


def scene_command_payload_size(
    data: bytes, command: int, count: int, pointer: int
) -> int | None:
    sizes = {
        0x00: count * 0x10,
        0x01: count * 0x10,
        0x02: 8,
        0x03: 0x2C,
        0x04: count * 8,
        0x06: 2,
        0x0B: count * 2,
        0x0C: count * 0x0E,
        0x0D: 8,
        0x0E: count * 0x10,
        0x0F: count * 0x16,
        0x13: 2,
        0x18: 4,
    }
    if command == 0x0A:
        offset = pointer & 0xFFFFFF
        if offset + 2 > len(data):
            return None
        if data[offset] in (0, 2):
            return 0x0C
        if data[offset] == 1:
            return {1: 0x20, 2: 0x10}.get(data[offset + 1])
        return None
    if command == 0x17 and pointer:
        return cutscene_data_size(data, pointer & 0xFFFFFF) or 4
    return sizes.get(command)


def build_scene_layout_anchors(
    table_path: Path, extracted_dir: Path
) -> list[tuple[int, int, int, int, int, int, int, int, int, int, int, int, int]]:
    anchors: list[tuple[int, int, int, int, int, int, int, int, int, int, int, int, int]] = []
    for asset_index, (name, _size, flags, _file_offset) in enumerate(parse_entries(table_path)):
        room_match = re.search(r"_room_[0-9]+$", name)
        if not flags or (not name.endswith("_scene") and room_match is None):
            continue
        segment = 2 if name.endswith("_scene") else 3
        path = extracted_dir / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        next_graph_index = 0
        emitted_dlist_offsets: set[int] = set()
        emitted_dependencies: set[tuple[int, int]] = set()

        def append_dlist_graph(
            header_index: int,
            command: int,
            occurrence: int,
            pointer: int,
            child_index: int,
            child_slot: int,
            parent_graph_index: int = 0xFFFF,
            command_index: int = 0,
            stack: frozenset[int] = frozenset(),
        ) -> None:
            nonlocal next_graph_index
            dlist_offset = pointer & 0xFFFFFF
            if pointer == 0 or pointer >> 24 != segment:
                return
            if dlist_offset in emitted_dlist_offsets:
                return
            size = dlist_size(data, dlist_offset)
            if size is None:
                return
            emitted_dlist_offsets.add(dlist_offset)
            graph_index = next_graph_index
            next_graph_index += 1
            anchors.append(
                (asset_index, header_index, command, occurrence,
                 SCENE_LAYOUT_CHILD_MESH_DLIST, child_index, child_slot, segment,
                 graph_index, parent_graph_index, command_index, dlist_offset, size)
            )
            if dlist_offset in stack:
                return
            stack = stack | {dlist_offset}
            for index, cursor in enumerate(range(dlist_offset, dlist_offset + size, 8)):
                word0 = read_be32(data, cursor)
                word1 = read_be32(data, cursor + 4)
                opcode = word0 >> 24
                dependency_offset = word1 & 0xFFFFFF
                if word1 == 0 or word1 >> 24 != segment or dependency_offset >= len(data):
                    continue
                if opcode in (0xDE, 0x06):
                    append_dlist_graph(
                        header_index, command, occurrence, word1, 0, opcode,
                        graph_index, index, stack
                    )
                    continue
                dependency_size = 0
                child = SCENE_LAYOUT_CHILD_DLIST_DATA
                if opcode == 0x01:
                    dependency_size = ((word0 >> 12) & 0xFF) * 16
                    child = SCENE_LAYOUT_CHILD_DLIST_VERTICES
                elif opcode == 0x04:
                    dependency_size = ((word0 >> 10) & 0x3F) * 16
                    child = SCENE_LAYOUT_CHILD_DLIST_VERTICES
                elif opcode == 0xDA:
                    dependency_size = 0x40
                elif opcode in (0xDC, 0xFD):
                    # The command does not carry a complete resource length,
                    # but a one-byte anchor is sufficient to relocate its
                    # exact segmented base while the surrounding gap mapper
                    # moves the resource contents.
                    dependency_size = 1
                if dependency_size and dependency_offset + dependency_size <= len(data):
                    dependency_key = (child, dependency_offset)
                    if dependency_key in emitted_dependencies:
                        continue
                    emitted_dependencies.add(dependency_key)
                    anchors.append(
                        (asset_index, header_index, command, occurrence, child, 0, opcode,
                         segment, 0xFFFF, graph_index, index, dependency_offset,
                         dependency_size)
                    )

        for header_index, (header_offset, header_size) in scene_header_offsets(data, segment).items():
            anchors.append(
                (asset_index, header_index, SCENE_LAYOUT_HEADER_COMMAND, 0, 0, 0, 0, segment,
                 0xFFFF, 0xFFFF, 0,
                 header_offset, header_size)
            )
            for command, occurrence, count, pointer in scene_header_commands(
                data, header_offset, header_size
            ):
                if pointer >> 24 != segment:
                    continue
                payload_offset = pointer & 0xFFFFFF
                payload_size = scene_command_payload_size(data, command, count, pointer)
                if payload_size:
                    anchors.append(
                        (asset_index, header_index, command, occurrence,
                         SCENE_LAYOUT_CHILD_PAYLOAD, 0, 0, segment, 0xFFFF, 0xFFFF, 0,
                         payload_offset, payload_size)
                    )
                if command == 0x0A and payload_size is not None:
                    shape_type = data[payload_offset]
                    entry_count = 1 if shape_type == 1 else data[payload_offset + 1]
                    entry_size = 8 if shape_type in (0, 1) else 0x10
                    entry_pointer = read_be32(data, payload_offset + 4)
                    entry_offset = entry_pointer & 0xFFFFFF
                    if (
                        entry_count != 0
                        and entry_pointer >> 24 == segment
                        and entry_offset + entry_count * entry_size <= len(data)
                    ):
                        anchors.append(
                            (asset_index, header_index, command, occurrence,
                             SCENE_LAYOUT_CHILD_MESH_ENTRIES, 0, 0, segment,
                             0xFFFF, 0xFFFF, 0,
                             entry_offset, entry_count * entry_size)
                        )
                        dlist_fields = (0, 4) if shape_type in (0, 1) else (8, 12)
                        for entry_index in range(entry_count):
                            for slot, field in enumerate(dlist_fields):
                                dlist_pointer = read_be32(
                                    data, entry_offset + entry_index * entry_size + field
                                )
                                if dlist_pointer == 0 or dlist_pointer >> 24 != segment:
                                    continue
                                append_dlist_graph(
                                    header_index, command, occurrence, dlist_pointer,
                                    entry_index, slot
                                )
                    if shape_type == 1:
                        amount_type = data[payload_offset + 1]
                        backgrounds_offset = 0
                        background_count = 0
                        if amount_type == 1:
                            backgrounds_offset = payload_offset
                            background_count = 1
                        elif amount_type == 2:
                            background_count = data[payload_offset + 8]
                            backgrounds_pointer = read_be32(data, payload_offset + 0x0C)
                            backgrounds_offset = backgrounds_pointer & 0xFFFFFF
                            if (
                                background_count != 0
                                and backgrounds_pointer >> 24 == segment
                                and backgrounds_offset + background_count * 0x1C <= len(data)
                            ):
                                anchors.append(
                                    (asset_index, header_index, command, occurrence,
                                     SCENE_LAYOUT_CHILD_IMAGE_BACKGROUNDS, 0, 0, segment,
                                     0xFFFF, 0xFFFF, 0,
                                     backgrounds_offset, background_count * 0x1C)
                                )
                            else:
                                background_count = 0
                        for background_index in range(background_count):
                            background = (
                                backgrounds_offset
                                if amount_type == 1
                                else backgrounds_offset + background_index * 0x1C
                            )
                            field_base = 8 if amount_type == 1 else 4
                            source_pointer = read_be32(data, background + field_base)
                            tlut_pointer = read_be32(data, background + field_base + 8)
                            width, height = struct.unpack_from(">HH", data, background + field_base + 0x0C)
                            siz = data[background + field_base + 0x11]
                            texel_count = width * height
                            source_size = (texel_count * (4 << siz) + 7) // 8 if siz <= 3 else 0
                            tlut_count = struct.unpack_from(">H", data, background + field_base + 0x14)[0]
                            for child, child_pointer, child_size in (
                                (SCENE_LAYOUT_CHILD_IMAGE_SOURCE, source_pointer, source_size),
                                (SCENE_LAYOUT_CHILD_IMAGE_TLUT, tlut_pointer, tlut_count * 2),
                            ):
                                child_offset = child_pointer & 0xFFFFFF
                                if (
                                    child_size != 0
                                    and child_pointer >> 24 == segment
                                    and child_offset + child_size <= len(data)
                                ):
                                    anchors.append(
                                        (asset_index, header_index, command, occurrence,
                                         child, background_index, 0, segment,
                                         0xFFFF, 0xFFFF, 0,
                                         child_offset, child_size)
                                    )
                    continue
                if command != 0x03 or payload_offset + 0x2C > len(data):
                    continue
                for child, field, count_field, element_size in SCENE_LAYOUT_COLLISION_FIELDS:
                    child_pointer = read_be32(data, payload_offset + field)
                    if child_pointer >> 24 != segment:
                        continue
                    child_size = element_size
                    if count_field is not None:
                        child_size *= struct.unpack_from(">H", data, payload_offset + count_field)[0]
                    if child_size:
                        anchors.append(
                            (asset_index, header_index, command, occurrence, child, 0, 0, segment,
                             0xFFFF, 0xFFFF, 0,
                             child_pointer & 0xFFFFFF, child_size)
                        )
    return sorted(anchors, key=lambda anchor: anchor[0])


def write_scene_layout_header(table_path: Path, extracted_dir: Path, output: Path) -> None:
    anchors = build_scene_layout_anchors(table_path, extracted_dir)
    lines = [
        "#ifndef OOT_PSP_SCENE_LAYOUTS_H\n",
        "#define OOT_PSP_SCENE_LAYOUTS_H\n\n",
        "typedef struct OotPspSceneLayoutAnchor {\n",
        "    u16 assetIndex;\n",
        "    u8 headerIndex;\n",
        "    u8 commandId;\n",
        "    u8 occurrence;\n",
        "    u8 child;\n",
        "    u16 childIndex;\n",
        "    u8 childSlot;\n",
        "    u8 segmentId;\n",
        "    u16 graphIndex;\n",
        "    u16 parentGraphIndex;\n",
        "    u16 commandIndex;\n",
        "    u16 padding;\n",
        "    u32 targetOffset;\n",
        "    u32 targetSize;\n",
        "} OotPspSceneLayoutAnchor;\n\n",
        "static const OotPspSceneLayoutAnchor sOotPspSceneLayoutAnchors[] = {\n",
    ]
    lines.extend(
        f"    {{ 0x{asset_index:04X}, 0x{header_index:02X}, 0x{command:02X}, "
        f"0x{occurrence:02X}, 0x{child:02X}, 0x{child_index:02X}, "
        f"0x{child_slot:02X}, 0x{segment:02X}, "
        f"0x{graph_index:04X}, 0x{parent_graph_index:04X}, "
        f"0x{command_index:04X}, 0, "
        f"0x{offset:08X}, 0x{size:08X} }},\n"
        for asset_index, header_index, command, occurrence, child, child_index,
        child_slot, segment, graph_index, parent_graph_index, command_index,
        offset, size in anchors
    )
    lines.extend(
        [
            "};\n\n",
            f"#define OOT_PSP_SCENE_LAYOUT_ANCHOR_COUNT {len(anchors)}U\n\n",
            "#endif\n",
        ]
    )
    output.write_text("".join(lines))
    print(f"wrote {len(anchors)} PSP scene layout anchors to {output}")


def dlist_size(data: bytes, offset: int) -> int | None:
    for cursor in range(offset, len(data) - 7, 8):
        if data[cursor] == 0xDF:
            return cursor + 8 - offset
    return None


def canonical_xml_files(
) -> dict[str, tuple[int, int, list[ET.Element], list[ET.Element]]]:
    files: dict[str, tuple[int, int, list[ET.Element], list[ET.Element]]] = {}
    config = version_config.load_version_config("ntsc-1.0")

    for asset in config.assets:
        base = asset.start_offset or 0
        root = ET.parse(Path(__file__).resolve().parent.parent / asset.xml_path).getroot()
        for file_elem in root.iter("File"):
            name = file_elem.attrib.get("Name")
            if not name:
                continue
            files[name] = (
                base,
                int(file_elem.attrib.get("Segment", "0"), 0),
                list(file_elem.iter("Skeleton")),
                list(file_elem.iter("Animation")),
            )
    return files


def build_animation_layout_anchors(
    table_path: Path, extracted_dir: Path
) -> list[tuple[int, int, int, int, int, int, int, int]]:
    anchors: list[tuple[int, int, int, int, int, int, int, int]] = []
    xml_files = canonical_xml_files()

    for asset_index, (name, _size, flags, _file_offset) in enumerate(parse_entries(table_path)):
        schema = xml_files.get(name)
        path = extracted_dir / name
        if not flags or schema is None or not path.is_file():
            continue
        base, xml_segment, _skeletons, animations = schema
        data = path.read_bytes()
        for animation_index, animation in enumerate(animations):
            header_offset = base + int(animation.attrib["Offset"], 0)
            if header_offset + 0x10 > len(data):
                continue
            frame_count = struct.unpack_from(">h", data, header_offset)[0]
            frame_pointer = read_be32(data, header_offset + 4)
            joint_pointer = read_be32(data, header_offset + 8)
            static_index_max = struct.unpack_from(">H", data, header_offset + 0x0C)[0]
            segment = frame_pointer >> 24
            frame_offset = frame_pointer & 0xFFFFFF
            joint_offset = joint_pointer & 0xFFFFFF
            if (
                frame_count <= 0
                or segment == 0
                or segment >= 0x10
                or joint_pointer >> 24 != segment
                or (xml_segment != 0 and segment != xml_segment)
                or not (frame_offset < joint_offset < header_offset)
                or (joint_offset - frame_offset) % 2 != 0
                or header_offset - joint_offset < 6
            ):
                continue
            anchors.extend(
                (
                    (asset_index, animation_index, ANIMATION_LAYOUT_HEADER, segment,
                     frame_count, static_index_max, header_offset, 0x10),
                    (asset_index, animation_index, ANIMATION_LAYOUT_FRAME_DATA, segment,
                     frame_count, static_index_max, frame_offset, joint_offset - frame_offset),
                    (asset_index, animation_index, ANIMATION_LAYOUT_JOINT_INDICES, segment,
                     frame_count, static_index_max, joint_offset, header_offset - joint_offset),
                )
            )
    return sorted(anchors)


def write_animation_layout_header(table_path: Path, extracted_dir: Path, output: Path) -> None:
    anchors = build_animation_layout_anchors(table_path, extracted_dir)
    lines = [
        "#ifndef OOT_PSP_ANIMATION_LAYOUTS_H\n",
        "#define OOT_PSP_ANIMATION_LAYOUTS_H\n\n",
        "typedef struct OotPspAnimationLayoutAnchor {\n",
        "    u16 assetIndex;\n",
        "    u16 animationIndex;\n",
        "    u8 kind;\n",
        "    u8 segmentId;\n",
        "    u16 frameCount;\n",
        "    u16 staticIndexMax;\n",
        "    u16 padding;\n",
        "    u32 targetOffset;\n",
        "    u32 targetSize;\n",
        "} OotPspAnimationLayoutAnchor;\n\n",
        "static const OotPspAnimationLayoutAnchor sOotPspAnimationLayoutAnchors[] = {\n",
    ]
    lines.extend(
        f"    {{ 0x{asset_index:04X}, 0x{animation_index:04X}, 0x{kind:02X}, "
        f"0x{segment:02X}, 0x{frame_count:04X}, 0x{static_index_max:04X}, 0, "
        f"0x{offset:08X}, 0x{size:08X} }},\n"
        for asset_index, animation_index, kind, segment, frame_count,
        static_index_max, offset, size in anchors
    )
    lines.extend(
        [
            "};\n\n",
            f"#define OOT_PSP_ANIMATION_LAYOUT_ANCHOR_COUNT {len(anchors)}U\n\n",
            "#endif\n",
        ]
    )
    output.write_text("".join(lines))
    print(f"wrote {len(anchors)} PSP animation layout anchors to {output}")


def build_skeleton_layout_anchors(
    table_path: Path, extracted_dir: Path
) -> list[tuple[int, int, int, int, int, int, int, int, int, int, int]]:
    anchors: list[tuple[int, int, int, int, int, int, int, int, int, int, int]] = []
    xml_files = canonical_xml_files()

    for asset_index, (name, _size, flags, _file_offset) in enumerate(parse_entries(table_path)):
        schema = xml_files.get(name)
        path = extracted_dir / name
        if not flags or schema is None or not path.is_file():
            continue
        base, xml_segment, skeletons, _animations = schema
        data = path.read_bytes()
        for skeleton_index, skeleton in enumerate(skeletons):
            anchor_start = len(anchors)
            limb_description = SKELETON_LIMB_TYPES.get(skeleton.attrib.get("LimbType", ""))
            if limb_description is None:
                continue
            limb_type, limb_size, dlist_fields = limb_description
            target_offset = base + int(skeleton.attrib["Offset"], 0)
            skeleton_type = skeleton.attrib.get("Type", "Normal")
            header_size = 12 if skeleton_type == "Flex" else 8
            if target_offset + header_size > len(data):
                continue
            table_pointer = read_be32(data, target_offset)
            segment = table_pointer >> 24
            table_offset = table_pointer & 0xFFFFFF
            limb_count = data[target_offset + 4]
            dlist_count = data[target_offset + 8] if skeleton_type == "Flex" else 0
            table_size = limb_count * 4
            if (
                segment == 0
                or segment >= 0x10
                or (xml_segment != 0 and segment != xml_segment)
                or table_offset + table_size > len(data)
                or limb_count == 0
            ):
                continue
            anchors.append(
                (
                    asset_index,
                    skeleton_index,
                    SKELETON_LAYOUT_HEADER,
                    0,
                    0,
                    segment,
                    limb_type,
                    limb_count,
                    dlist_count,
                    target_offset,
                    header_size,
                )
            )
            anchors.append(
                (
                    asset_index,
                    skeleton_index,
                    SKELETON_LAYOUT_TABLE,
                    0,
                    0,
                    segment,
                    limb_type,
                    limb_count,
                    dlist_count,
                    table_offset,
                    table_size,
                )
            )
            for limb_index in range(limb_count):
                limb_pointer = read_be32(data, table_offset + limb_index * 4)
                limb_offset = limb_pointer & 0xFFFFFF
                if limb_pointer >> 24 != segment or limb_offset + limb_size > len(data):
                    break
                anchors.append(
                    (
                        asset_index,
                        skeleton_index,
                        SKELETON_LAYOUT_LIMB,
                        limb_index,
                        0,
                        segment,
                        limb_type,
                        limb_count,
                        dlist_count,
                        limb_offset,
                        limb_size,
                    )
                )

                def append_dlist(dlist_pointer: int, dlist_index: int) -> bool:
                    dlist_offset = dlist_pointer & 0xFFFFFF
                    if dlist_pointer == 0:
                        return True
                    if dlist_pointer >> 24 != segment:
                        return False
                    size = dlist_size(data, dlist_offset)
                    if size is None:
                        return False
                    anchors.append(
                        (
                            asset_index,
                            skeleton_index,
                            SKELETON_LAYOUT_DLIST,
                            limb_index,
                            dlist_index,
                            segment,
                            limb_type,
                            limb_count,
                            dlist_count,
                            dlist_offset,
                            size,
                        )
                    )
                    return True

                valid_limb = True
                if limb_type == SKELETON_LIMB_TYPES["Skin"][0]:
                    segment_type = read_be32(data, limb_offset + 8)
                    skin_pointer = read_be32(data, limb_offset + 12)
                    skin_offset = skin_pointer & 0xFFFFFF

                    if skin_pointer == 0:
                        continue
                    if skin_pointer >> 24 != segment:
                        break
                    if segment_type == 11:
                        valid_limb = append_dlist(skin_pointer, 0)
                    elif segment_type == 4:
                        if skin_offset + 12 > len(data):
                            break
                        anchors.append(
                            (
                                asset_index,
                                skeleton_index,
                                SKELETON_LAYOUT_SKIN_DATA,
                                limb_index,
                                0,
                                segment,
                                limb_type,
                                limb_count,
                                dlist_count,
                                skin_offset,
                                12,
                            )
                        )
                        modif_count = struct.unpack_from(">H", data, skin_offset + 2)[0]
                        modifs_pointer = read_be32(data, skin_offset + 4)
                        modifs_offset = modifs_pointer & 0xFFFFFF
                        if modif_count != 0:
                            if (
                                modifs_pointer >> 24 != segment
                                or modifs_offset + modif_count * 16 > len(data)
                            ):
                                break
                            anchors.append(
                                (
                                    asset_index,
                                    skeleton_index,
                                    SKELETON_LAYOUT_SKIN_MODIFS,
                                    limb_index,
                                    0,
                                    segment,
                                    limb_type,
                                    limb_count,
                                    dlist_count,
                                    modifs_offset,
                                    modif_count * 16,
                                )
                            )
                        dlist_pointer = read_be32(data, skin_offset + 8)
                        dlist_offset = dlist_pointer & 0xFFFFFF
                        if dlist_pointer != 0:
                            if dlist_pointer >> 24 != segment:
                                break
                            size = dlist_size(data, dlist_offset)
                            if size is None:
                                break
                            anchors.append(
                                (
                                    asset_index,
                                    skeleton_index,
                                    SKELETON_LAYOUT_DLIST,
                                    limb_index,
                                    0,
                                    segment,
                                    limb_type,
                                    limb_count,
                                    dlist_count,
                                    dlist_offset,
                                    size,
                                )
                            )
                        for modif_index in range(modif_count):
                            modif_offset = modifs_offset + modif_index * 16
                            vtx_count, transform_count = struct.unpack_from(">HH", data, modif_offset)
                            for kind, count, pointer_field, element_size in (
                                (SKELETON_LAYOUT_SKIN_VERTICES, vtx_count, 8, 10),
                                (SKELETON_LAYOUT_SKIN_TRANSFORMS, transform_count, 12, 10),
                            ):
                                if count == 0:
                                    continue
                                pointer = read_be32(data, modif_offset + pointer_field)
                                offset = pointer & 0xFFFFFF
                                size = count * element_size
                                if pointer >> 24 != segment or offset + size > len(data):
                                    valid_limb = False
                                    break
                                anchors.append(
                                    (
                                        asset_index,
                                        skeleton_index,
                                        kind,
                                        limb_index,
                                        modif_index,
                                        segment,
                                        limb_type,
                                        limb_count,
                                        dlist_count,
                                        offset,
                                        size,
                                    )
                                )
                            if not valid_limb:
                                break
                    elif skin_pointer != 0:
                        valid_limb = False
                else:
                    for dlist_index, field in enumerate(dlist_fields):
                        if not append_dlist(read_be32(data, limb_offset + field), dlist_index):
                            valid_limb = False
                            break
                if not valid_limb:
                    break
            else:
                continue
            # A malformed canonical graph should never emit a partial record.
            del anchors[anchor_start:]
    return sorted(anchors)


def write_skeleton_layout_header(table_path: Path, extracted_dir: Path, output: Path) -> None:
    anchors = build_skeleton_layout_anchors(table_path, extracted_dir)
    lines = [
        "#ifndef OOT_PSP_SKELETON_LAYOUTS_H\n",
        "#define OOT_PSP_SKELETON_LAYOUTS_H\n\n",
        "typedef struct OotPspSkeletonLayoutAnchor {\n",
        "    u16 assetIndex;\n",
        "    u8 skeletonIndex;\n",
        "    u8 kind;\n",
        "    u8 limbIndex;\n",
        "    u8 childIndex;\n",
        "    u8 segmentId;\n",
        "    u8 limbType;\n",
        "    u8 limbCount;\n",
        "    u8 dListCount;\n",
        "    u16 padding;\n",
        "    u32 targetOffset;\n",
        "    u32 targetSize;\n",
        "} OotPspSkeletonLayoutAnchor;\n\n",
        "static const OotPspSkeletonLayoutAnchor sOotPspSkeletonLayoutAnchors[] = {\n",
    ]
    lines.extend(
        f"    {{ 0x{asset_index:04X}, 0x{skeleton_index:02X}, 0x{kind:02X}, "
        f"0x{limb_index:02X}, 0x{child_index:02X}, 0x{segment:02X}, "
        f"0x{limb_type:02X}, 0x{limb_count:02X}, 0x{dlist_count:02X}, 0, "
        f"0x{offset:08X}, 0x{size:08X} }},\n"
        for (
            asset_index,
            skeleton_index,
            kind,
            limb_index,
            child_index,
            segment,
            limb_type,
            limb_count,
            dlist_count,
            offset,
            size,
        ) in anchors
    )
    lines.extend(
        [
            "};\n\n",
            f"#define OOT_PSP_SKELETON_LAYOUT_ANCHOR_COUNT {len(anchors)}U\n\n",
            "#endif\n",
        ]
    )
    output.write_text("".join(lines))
    print(f"wrote {len(anchors)} PSP skeleton layout anchors to {output}")


def build_transform(build_dir: Path, extracted_dir: Path, packed_path: Path | None = None) -> bytes:
    entries = parse_entries(build_dir / "oot_psp_asset_segments.c")
    if packed_path is None:
        packed_path = build_dir / "data/segments/oot_psp_assets.bin"
    permutations = [permutation(parts) for parts in word_partitions(8)]
    native_count = sum(flags != 0 for _name, _size, flags, _file_offset in entries)
    output = bytearray(b"OPZ4" + struct.pack("<II", native_count, len(permutations)))

    for mapping in permutations:
        output.extend(mapping)

    with packed_path.open("rb") as packed_file:
        packed = mmap.mmap(packed_file.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            for asset_index, (name, size, flags, file_offset) in enumerate(entries):
                if flags == 0:
                    continue

                source = (extracted_dir / name).read_bytes()
                selectors = bytearray()
                mappings = bytearray()
                source_positions: dict[int, int] = {}

                for source_offset, value in enumerate(source):
                    source_positions.setdefault(value, source_offset)

                if len(source) != size:
                    raise ValueError(f"unexpected source size for {name}: {len(source)} != {size}")

                for offset in range(0, size, 8):
                    raw_block = source[offset : offset + 8]
                    native_block = packed[file_offset + offset : file_offset + min(offset + 8, size)]
                    selector = len(permutations) + 1  # mapped from the user's ROM

                    if len(raw_block) == 8 and len(native_block) == 8:
                        for index, mapping in enumerate(permutations):
                            if bytes(raw_block[source_index] for source_index in mapping) == native_block:
                                selector = index
                                break
                        else:
                            if native_block == bytes(8):
                                selector = len(permutations)  # zero fill

                    if selector == len(permutations) + 1:
                        for byte_offset, value in enumerate(native_block):
                            original_offset = offset + byte_offset
                            source_offset = source_positions.get(value, original_offset)
                            delta = (value - source[source_offset]) & 0xFF

                            # A mapping is an offset into the user's own extracted
                            # segment plus a byte delta. It never stores an output
                            # byte from the game.
                            mappings.extend(struct.pack("<IB", source_offset, delta))
                    selectors.append(selector)

                payload = selectors + mappings
                compressed = zlib.compress(payload, level=9)
                output.extend(struct.pack("<IIII", asset_index, size, len(payload), len(compressed)))
                output.extend(compressed)
        finally:
            packed.close()

    return bytes(output)


def scrub_transform(build_dir: Path, extracted_dir: Path, old_transform: Path) -> bytes:
    """Convert the old literal-bearing OPZ3 format into payload-free OPZ4.

    This migration helper is intentionally maintainer-only. New snapshots are
    created as OPZ4 directly by :func:`build_transform`.
    """
    entries = parse_entries(build_dir / "oot_psp_asset_segments.c")
    permutations = [permutation(parts) for parts in word_partitions(8)]
    data = old_transform.read_bytes()

    if data[:4] != b"OPZ3":
        raise ValueError("expected an OPZ3 transform to scrub")

    entry_count, permutation_count = struct.unpack_from("<II", data, 4)
    if permutation_count != len(permutations):
        raise ValueError("unexpected OPZ3 permutation table")

    cursor = 12 + (permutation_count * 8)
    output = bytearray(b"OPZ4" + data[4:cursor])

    for _ in range(entry_count):
        asset_index, size, payload_size, compressed_size = struct.unpack_from("<IIII", data, cursor)
        cursor += 16
        payload = zlib.decompress(data[cursor : cursor + compressed_size])
        cursor += compressed_size

        if len(payload) != payload_size:
            raise ValueError("damaged OPZ3 payload")

        name, expected_size, flags, _file_offset = entries[asset_index]
        if (flags == 0) or (size != expected_size):
            raise ValueError(f"OPZ3 entry does not match {name}")

        source = (extracted_dir / name).read_bytes()
        if len(source) != size:
            raise ValueError(f"unexpected source size for {name}")

        selector_count = (size + 7) // 8
        selectors = payload[:selector_count]
        literals = memoryview(payload)[selector_count:]
        literal_cursor = 0
        mappings = bytearray()
        source_positions: dict[int, int] = {}

        for source_offset, value in enumerate(source):
            source_positions.setdefault(value, source_offset)

        for block_index, selector in enumerate(selectors):
            if selector != permutation_count + 1:
                continue

            block_size = min(8, size - (block_index * 8))
            native_block = literals[literal_cursor : literal_cursor + block_size]
            literal_cursor += block_size
            for byte_offset, value in enumerate(native_block):
                original_offset = (block_index * 8) + byte_offset
                source_offset = source_positions.get(value, original_offset)
                delta = (value - source[source_offset]) & 0xFF
                mappings.extend(struct.pack("<IB", source_offset, delta))

        if literal_cursor != len(literals):
            raise ValueError(f"damaged OPZ3 literals for {name}")

        clean_payload = selectors + mappings
        clean_compressed = zlib.compress(clean_payload, level=9)
        output.extend(struct.pack("<IIII", asset_index, size, len(clean_payload), len(clean_compressed)))
        output.extend(clean_compressed)

    if cursor != len(data):
        raise ValueError("trailing data in OPZ3 transform")
    return bytes(output)


def runtime_dependencies(build_dir: Path, root: Path, version: str) -> list[Path]:
    prefixes = (f"extracted/{version}/", f"build/{version}/")
    dependencies: set[Path] = set()

    for dep_file in (build_dir / "src").rglob("*.d"):
        text = dep_file.read_text().replace("\\\n", " ")
        for token in text.split():
            relative = token.rstrip(":")
            path = root / relative
            if relative.startswith(prefixes) and path.is_file():
                dependencies.add(path)

    return sorted(dependencies)


def sanitize_initializer_source(data: bytes) -> bytes:
    """Keep C declaration shape while removing extracted values.

    Array dimensions, preprocessor directives, and symbol references are build
    metadata. Initializer numbers, character constants, and strings are replaced
    so the compiler can determine the same types without embedding game content.
    """
    text = data.decode("utf-8")
    output: list[str] = []

    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("#"):
            output.append(line)
            continue

        protected: list[str] = []

        def protect_brackets(match: re.Match[str]) -> str:
            protected.append(match.group(0))
            return f"__OOT_PSP_DIM_{len(protected) - 1}__"

        line = re.sub(r"\[[^\]\n]*\]", protect_brackets, line)
        line = STRING_RE.sub('""', line)
        line = CHAR_RE.sub("0", line)
        line = NUMBER_RE.sub("0", line)
        for index, value in enumerate(protected):
            line = line.replace(f"__OOT_PSP_DIM_{index}__", value)
        output.append(line)

    return "".join(output).encode("utf-8")


def clean_runtime_data(relative: str, data: bytes) -> bytes:
    if relative.endswith(("/text/message_data.h", "/text/message_data_staff.h")):
        return b"/* PSP message text is loaded from the user's ROM. */\n"
    if relative.endswith((".inc.c", ".c")):
        return sanitize_initializer_source(data)
    return data


def clean_runtime_dependency(path: Path) -> bytes:
    return clean_runtime_data(path.as_posix(), path.read_bytes())


def write_snapshot_member(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def create_snapshot(build_dir: Path, extracted_dir: Path, snapshot: Path, transform: Path,
                    packed_path: Path | None = None) -> None:
    build_dir = build_dir.resolve()
    extracted_dir = extracted_dir.resolve()
    root = build_dir.parents[2]
    version = build_dir.name
    dependencies = runtime_dependencies(build_dir, root, version)

    snapshot.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(snapshot, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in GENERATED_FILES:
            write_snapshot_member(archive, name, (build_dir / name).read_bytes())
        for path in dependencies:
            write_snapshot_member(
                archive,
                RUNTIME_PREFIX + path.relative_to(root).as_posix(),
                clean_runtime_dependency(path),
            )

    transform_data = build_transform(build_dir, extracted_dir, packed_path)
    transform.write_bytes(transform_data)
    print(
        f"wrote {snapshot} and {transform} "
        f"({len(dependencies)} runtime inputs; {len(transform_data)} bytes of per-asset transforms)"
    )


def restore_snapshot(snapshot: Path, output_dir: Path) -> None:
    output_dir = output_dir.resolve()
    root = Path(__file__).resolve().parents[1]
    output_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(snapshot, "r") as archive:
        names = set(archive.namelist())
        if not set(GENERATED_FILES).issubset(names):
            raise ValueError("PSP snapshot is missing generated metadata")
        if any(name not in GENERATED_FILES and not name.startswith(RUNTIME_PREFIX) for name in names):
            raise ValueError("PSP snapshot contains an unexpected member")
        for name in GENERATED_FILES:
            (output_dir / name).write_bytes(archive.read(name))
        for name in sorted(names - set(GENERATED_FILES)):
            relative = Path(name[len(RUNTIME_PREFIX) :])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"unsafe PSP snapshot member: {name}")
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            data = archive.read(name)
            # Shared compiler placeholders must not invalidate every other
            # revision when another module restores the same snapshot.
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)


def sanitize_snapshot(source: Path, output: Path, generated_dir: Path | None = None) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source, "r") as archive, zipfile.ZipFile(
        output, "w", zipfile.ZIP_DEFLATED, compresslevel=9
    ) as cleaned:
        names = archive.namelist()
        if not set(GENERATED_FILES).issubset(names):
            raise ValueError("PSP snapshot is missing generated metadata")

        for name in names:
            if (generated_dir is not None) and (name in GENERATED_FILES):
                data = (generated_dir / name).read_bytes()
            else:
                data = archive.read(name)
            if name.startswith(RUNTIME_PREFIX):
                data = clean_runtime_data(name[len(RUNTIME_PREFIX) :], data)
            write_snapshot_member(cleaned, name, data)


def main() -> None:
    parser = argparse.ArgumentParser(description="Create or restore ROM-independent PSP asset metadata")
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create")
    create.add_argument("build_dir", type=Path)
    create.add_argument("extracted_dir", type=Path)
    create.add_argument("snapshot", type=Path)
    create.add_argument("transform", type=Path)
    create.add_argument("--packed-assets", type=Path, default=None)

    restore = subparsers.add_parser("restore")
    restore.add_argument("snapshot", type=Path)
    restore.add_argument("output_dir", type=Path)

    scrub = subparsers.add_parser("scrub-transform")
    scrub.add_argument("build_dir", type=Path)
    scrub.add_argument("extracted_dir", type=Path)
    scrub.add_argument("old_transform", type=Path)
    scrub.add_argument("output", type=Path)

    sanitize = subparsers.add_parser("sanitize-snapshot")
    sanitize.add_argument("source", type=Path)
    sanitize.add_argument("output", type=Path)
    sanitize.add_argument("--generated-dir", type=Path, default=None)

    scene_layouts = subparsers.add_parser("scene-layouts")
    scene_layouts.add_argument("asset_table", type=Path)
    scene_layouts.add_argument("extracted_dir", type=Path)
    scene_layouts.add_argument("output", type=Path)

    skeleton_layouts = subparsers.add_parser("skeleton-layouts")
    skeleton_layouts.add_argument("asset_table", type=Path)
    skeleton_layouts.add_argument("extracted_dir", type=Path)
    skeleton_layouts.add_argument("output", type=Path)

    animation_layouts = subparsers.add_parser("animation-layouts")
    animation_layouts.add_argument("asset_table", type=Path)
    animation_layouts.add_argument("extracted_dir", type=Path)
    animation_layouts.add_argument("output", type=Path)

    args = parser.parse_args()
    if args.command == "create":
        create_snapshot(args.build_dir, args.extracted_dir, args.snapshot, args.transform, args.packed_assets)
    elif args.command == "restore":
        restore_snapshot(args.snapshot, args.output_dir)
    elif args.command == "scrub-transform":
        args.output.write_bytes(scrub_transform(args.build_dir, args.extracted_dir, args.old_transform))
    elif args.command == "sanitize-snapshot":
        sanitize_snapshot(args.source, args.output, args.generated_dir)
    elif args.command == "scene-layouts":
        write_scene_layout_header(args.asset_table, args.extracted_dir, args.output)
    elif args.command == "skeleton-layouts":
        write_skeleton_layout_header(args.asset_table, args.extracted_dir, args.output)
    else:
        write_animation_layout_header(args.asset_table, args.extracted_dir, args.output)


if __name__ == "__main__":
    main()
