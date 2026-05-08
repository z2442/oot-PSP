#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import version_config


DEFINE_SCENE_RE = re.compile(
    r"DEFINE_SCENE\(\s*(?P<name>\w+)\s*,\s*(?P<title>\w+)\s*,\s*(?P<enum>\w+)\s*,\s*(?P<draw>\w+)\s*,\s*(?P<unk10>[^,]+)\s*,\s*(?P<unk12>[^)]+)\)"
)
DEFINE_OBJECT_RE = re.compile(r"DEFINE_OBJECT\(\s*(?P<name>\w+)\s*,\s*(?P<enum>\w+)\s*\)")
DEFINE_OBJECT_EMPTY_RE = re.compile(r"DEFINE_OBJECT_EMPTY\(\s*(?P<name>\w+)\s*,\s*(?P<enum>\w+)\s*\)")
DEFINE_OBJECT_UNSET_RE = re.compile(r"DEFINE_OBJECT_UNSET\(\s*(?P<enum>\w+)\s*\)")
SKIP_SEGMENTS = {
    "makerom",
    "boot",
    "code",
    "dmadata",
    "n64dd",
}


def parse_scenes() -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    in_debug_assets_block = False
    for line in (ROOT / "include/tables/scene_table.h").read_text().splitlines():
        stripped = line.strip()
        if stripped == "#if DEBUG_ASSETS":
            in_debug_assets_block = True
            continue
        if in_debug_assets_block and stripped == "#endif":
            in_debug_assets_block = False
            continue
        if in_debug_assets_block:
            continue
        match = DEFINE_SCENE_RE.search(line)
        if match:
            entries.append(match.groupdict())
    return entries


def parse_objects() -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    for line in (ROOT / "include/tables/object_table.h").read_text().splitlines():
        if match := DEFINE_OBJECT_RE.search(line):
            entry = match.groupdict()
            entry["kind"] = "object"
            entries.append(entry)
        elif match := DEFINE_OBJECT_EMPTY_RE.search(line):
            entry = match.groupdict()
            entry["kind"] = "empty"
            entries.append(entry)
        elif match := DEFINE_OBJECT_UNSET_RE.search(line):
            entries.append({"kind": "unset", "name": "", "enum": match.group("enum")})
    return entries


def external_segment_names(version: str) -> set[str]:
    config = version_config.load_version_config(version)
    baserom_dir = ROOT / "extracted" / version / "baserom"
    names: set[str] = set()

    for name in config.dmadata_segments.keys():
        if name in SKIP_SEGMENTS:
            continue
        if (baserom_dir / name).is_file():
            names.add(name)

    return names


def emit_rom_segment_externs(lines: list[str], names: set[str]) -> None:
    for name in sorted(names):
        lines.append(f"extern u8 _{name}SegmentRomStart[];")
        lines.append(f"extern u8 _{name}SegmentRomEnd[];")


def emit(output: Path, version: str) -> None:
    scenes = parse_scenes()
    objects = parse_objects()
    external_segments = external_segment_names(version)

    includes: list[str] = [
        '#include "object.h"',
        '#include "oot_psp_asset_loader.h"',
        '#include "romfile.h"',
        '#include "scene.h"',
        '#include "ultra64.h"',
        "",
    ]

    lines: list[str] = []
    lines.extend(includes)
    lines.append("")
    table_segments = {entry["name"] for entry in scenes if entry["name"] in external_segments}
    table_segments.update(
        entry["name"] for entry in objects if entry["kind"] in {"object", "empty"} and entry["name"] in external_segments
    )
    emit_rom_segment_externs(lines, table_segments)
    lines.append("")
    lines.append("u8 gOotPspAssetFallback;")
    lines.append("s16 gLinkObjectIds[] = { OBJECT_LINK_BOY, OBJECT_LINK_CHILD };")
    lines.append("")
    lines.append("u32 gObjectTableSize = OBJECT_ID_MAX;")
    lines.append("RomFile gObjectTable[OBJECT_ID_MAX] = {")
    for entry in objects:
        enum = entry["enum"]
        name = entry["name"]
        if entry["kind"] == "object" and name in external_segments:
            lines.append(f"    [{enum}] = ROM_FILE({name}),")
        elif entry["kind"] == "empty" and name in external_segments:
            lines.append(f"    [{enum}] = ROM_FILE_EMPTY({name}),")
        else:
            lines.append(f"    [{enum}] = ROM_FILE_UNSET,")
    lines.append("};")
    lines.append("")
    lines.append("SceneTableEntry gSceneTable[SCENE_ID_MAX] = {")
    for entry in scenes:
        scene_name = entry["name"]
        scene_enum = entry["enum"]
        draw = entry["draw"]
        unk10 = entry["unk10"].strip()
        unk12 = entry["unk12"].strip()
        if scene_name in external_segments:
            lines.append(f"    [{scene_enum}] = {{ ROM_FILE({scene_name}), ROM_FILE_UNSET, {unk10}, {draw}, {unk12}, 0 }},")
        else:
            lines.append(f"    [{scene_enum}] = {{ ROM_FILE_UNSET, ROM_FILE_UNSET, {unk10}, {draw}, {unk12}, 0 }},")
    lines.append("};")
    lines.append("")
    lines.append("void OotPsp_ResolveRoomList(RomFile* roomFiles, s32 count) {")
    lines.append("    s32 i;")
    lines.append("")
    lines.append("    if (roomFiles == NULL) {")
    lines.append("        return;")
    lines.append("    }")
    lines.append("")
    lines.append("    for (i = 0; i < count; i++) {")
    lines.append("        OotPsp_NormalizeRomFile(&roomFiles[i]);")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate PSP-native scene, room, and object asset tables.")
    parser.add_argument("version")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    emit(args.output, args.version)


if __name__ == "__main__":
    main()
