#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


DEFINE_SCENE_RE = re.compile(
    r"DEFINE_SCENE\(\s*(?P<name>\w+)\s*,\s*(?P<title>\w+)\s*,\s*(?P<enum>\w+)\s*,\s*(?P<draw>\w+)\s*,\s*(?P<unk10>[^,]+)\s*,\s*(?P<unk12>[^)]+)\)"
)
DEFINE_OBJECT_RE = re.compile(r"DEFINE_OBJECT\(\s*(?P<name>\w+)\s*,\s*(?P<enum>\w+)\s*\)")
DEFINE_OBJECT_EMPTY_RE = re.compile(r"DEFINE_OBJECT_EMPTY\(\s*(?P<name>\w+)\s*,\s*(?P<enum>\w+)\s*\)")
DEFINE_OBJECT_UNSET_RE = re.compile(r"DEFINE_OBJECT_UNSET\(\s*(?P<enum>\w+)\s*\)")
EXTERN_SYMBOL_RE = re.compile(r"^\s*extern\b[^;()]*?\b(?P<name>[A-Za-z_]\w*)\s*(?:\[|;)", re.MULTILINE)
SPECIAL_OBJECT_SYMBOLS = {
    "gameplay_keep": "gArrow1_Anim",
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


def include_path(path: Path, version: str) -> str:
    extracted_root = ROOT / "extracted" / version
    try:
        return path.relative_to(extracted_root).as_posix()
    except ValueError:
        return path.relative_to(ROOT).as_posix()


def find_unique(root: Path, pattern: str) -> Path | None:
    matches = sorted(root.glob(pattern))
    return matches[0] if matches else None


def find_scene_headers(version: str) -> dict[str, Path]:
    root = ROOT / "extracted" / version / "assets/scenes"
    return {path.stem: path for path in sorted(root.glob("*/*/*.h"))}


def find_room_headers(version: str) -> dict[str, Path]:
    return {
        name: path
        for name, path in find_scene_headers(version).items()
        if re.search(r"_room_\d+$", name)
    }


def find_object_header(name: str, version: str) -> Path | None:
    candidates = [
        ROOT / "assets/objects" / name / f"{name}.h",
        ROOT / "extracted" / version / "assets/objects" / name / f"{name}.h",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def first_extern_symbol(header: Path | None) -> str | None:
    if header is None:
        return None

    text = header.read_text(errors="ignore")
    for match in EXTERN_SYMBOL_RE.finditer(text):
        name = match.group("name")
        if name not in {"NULL", "true", "false"}:
            return name
    return None


def emit(output: Path, version: str) -> None:
    scenes = parse_scenes()
    objects = parse_objects()
    scene_headers = find_scene_headers(version)
    room_headers = find_room_headers(version)

    object_headers: dict[str, Path] = {}
    object_symbols: dict[str, str] = {}
    for entry in objects:
        if entry["kind"] != "object":
            continue
        header = find_object_header(entry["name"], version)
        symbol = first_extern_symbol(header) or SPECIAL_OBJECT_SYMBOLS.get(entry["name"])
        if header is not None and symbol is not None:
            object_headers[entry["name"]] = header
            object_symbols[entry["name"]] = symbol

    includes: list[str] = [
        '#include "array_count.h"',
        '#include "gfx.h"',
        '#include "object.h"',
        '#include "romfile.h"',
        '#include "scene.h"',
        '#include "segment_symbols.h"',
        '#include "ultra64.h"',
        "",
    ]

    for path in sorted(set(scene_headers.values()) | set(object_headers.values()), key=lambda p: include_path(p, version)):
        includes.append(f'#include "{include_path(path, version)}"')

    lines: list[str] = []
    lines.extend(includes)
    lines.append("")
    lines.append("u8 gOotPspAssetFallback;")
    lines.append("s16 gLinkObjectIds[] = { OBJECT_LINK_BOY, OBJECT_LINK_CHILD };")
    lines.append("")
    lines.append("u32 gObjectTableSize = OBJECT_ID_MAX;")
    lines.append("RomFile gObjectTable[OBJECT_ID_MAX] = {")
    for entry in objects:
        enum = entry["enum"]
        if entry["kind"] == "object" and entry["name"] in object_symbols:
            symbol = object_symbols[entry["name"]]
            lines.append(f"    [{enum}] = {{ (uintptr_t)&{symbol}, (uintptr_t)&{symbol} }},")
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
        if scene_name in scene_headers:
            lines.append(
                f"    [{scene_enum}] = {{ {{ (uintptr_t){scene_name}, 0 }}, ROM_FILE_UNSET, {unk10}, {draw}, {unk12}, 0 }},"
            )
        else:
            lines.append(f"    [{scene_enum}] = {{ ROM_FILE_UNSET, ROM_FILE_UNSET, {unk10}, {draw}, {unk12}, 0 }},")
    lines.append("};")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uintptr_t romStart;")
    lines.append("    RomFile directFile;")
    lines.append("} OotPspRoomFileMap;")
    lines.append("")
    lines.append("static OotPspRoomFileMap sRoomFileMap[] = {")
    for room_name in sorted(room_headers):
        lines.append(
            f"    {{ (uintptr_t)_{room_name}SegmentRomStart, {{ (uintptr_t){room_name}, 0 }} }},"
        )
    lines.append("};")
    lines.append("")
    lines.append("void OotPsp_ResolveRoomList(RomFile* roomFiles, s32 count) {")
    lines.append("    s32 i;")
    lines.append("    size_t j;")
    lines.append("")
    lines.append("    if (roomFiles == NULL) {")
    lines.append("        return;")
    lines.append("    }")
    lines.append("")
    lines.append("    for (i = 0; i < count; i++) {")
    lines.append("        for (j = 0; j < ARRAY_COUNT(sRoomFileMap); j++) {")
    lines.append("            if (roomFiles[i].vromStart == sRoomFileMap[j].romStart) {")
    lines.append("                roomFiles[i] = sRoomFileMap[j].directFile;")
    lines.append("                break;")
    lines.append("            }")
    lines.append("        }")
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
