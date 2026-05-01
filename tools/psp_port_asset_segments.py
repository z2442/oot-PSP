#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import version_config

EXTERNAL_VROM_BASE = 0x20000000
EXTERNAL_VROM_ALIGN = 16
RUNTIME_SEGMENT_DIR = Path("data/segments")

DECLARE_RE = re.compile(r"\bDECLARE_(?:ROM_)?SEGMENT\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)")
DECLARE_OVERLAY_RE = re.compile(r"\bDECLARE_OVERLAY_SEGMENT\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)")
SKIP_NAMES = {
    "boot",
    "code",
    "dmadata",
    "n64dd",
}
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
    baserom_dir = ROOT / "extracted" / version / "baserom"
    cursor = EXTERNAL_VROM_BASE
    entries: list[dict[str, object]] = []

    for name in declared_segment_names():
        if not should_emit_raw_segment(name, baserom_dir):
            continue

        source = baserom_dir / name
        size = source.stat().st_size
        start = align(cursor, EXTERNAL_VROM_ALIGN)
        end = start + size
        cursor = align(end, EXTERNAL_VROM_ALIGN)
        entries.append(
            {
                "name": name,
                "source": source,
                "size": size,
                "vrom_start": start,
                "vrom_end": end,
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


def copy_data_files(entries: list[dict[str, object]], data_dir: Path) -> None:
    data_dir.mkdir(parents=True, exist_ok=True)
    expected: set[str] = set()

    for entry in entries:
        source = entry["source"]
        assert isinstance(source, Path)
        filename = f"{entry['name']}.bin"
        target = data_dir / filename
        expected.add(filename)
        shutil.copy2(source, target)

    for stale in data_dir.glob("*.bin"):
        if stale.name not in expected:
            stale.unlink()


def emit_asm(output: Path, entries: list[dict[str, object]]) -> None:
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


def emit_table(output: Path, entries: list[dict[str, object]], message_entries: dict[str, list[dict[str, int]]]) -> None:
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

    emit_message_entries(lines, "gOotPspJpnMessageEntries", message_entries["jpn"])
    emit_message_entries(lines, "gOotPspNesMessageEntries", message_entries["nes"])
    emit_message_entries(lines, "gOotPspGerMessageEntries", message_entries["ger"])
    emit_message_entries(lines, "gOotPspFraMessageEntries", message_entries["fra"])
    emit_message_entries(lines, "gOotPspStaffMessageEntries", message_entries["staff"])

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))


def emit(version: str, asm_output: Path, table_output: Path, data_dir: Path) -> None:
    entries = build_entries(version)
    message_entries = build_message_entries(version, entries)
    emit_asm(asm_output, entries)
    emit_table(table_output, entries, message_entries)
    copy_data_files(entries, data_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate PSP external raw asset segment symbols and data files.")
    parser.add_argument("version")
    parser.add_argument("asm_output", type=Path)
    parser.add_argument("table_output", type=Path)
    parser.add_argument("data_dir", type=Path)
    args = parser.parse_args()
    emit(args.version, args.asm_output, args.table_output, args.data_dir)


if __name__ == "__main__":
    main()
