#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import version_config
from psp_port_asset_union import texture_size, union_schema


ROOT = Path(__file__).resolve().parent.parent
MESSAGE_ASSETS = (
    ("jpn_message_data_static", "sJpnMessageEntryTable"),
    ("nes_message_data_static", "sNesMessageEntryTable"),
    ("ger_message_data_static", "sGerMessageEntryTable"),
    ("fra_message_data_static", "sFraMessageEntryTable"),
    ("staff_message_data_static", "sStaffMessageEntryTable"),
)
TEXT_LANGUAGES = {"NTSC": 0, "PAL": 1, "CN": 2}
ASSET_ENTRY_RE = re.compile(
    r'\{ (0x[0-9A-F]+), (0x[0-9A-F]+), 0x[0-9A-F]+, 0x[0-9A-F]+, '
    r'0x[0-9A-F]+, 0x[0-9A-F]+, "([^"]+)" \}'
)
README_VERSION_RE = re.compile(r"^\|\s*([a-z0-9.-]+)\s*\|.*?\|\s*(.*?)\s*\|$")
MD5_RE = re.compile(r"[0-9a-fA-F]{32}")
ASSET_SPLICE_COPY_SIZE_MASK = 0x0FFFFFFF
ASSET_SPLICE_COPY_EXACT = 0
ASSET_SPLICE_COPY_DLIST = 1
ASSET_SPLICE_COPY_SCENE_COMMANDS = 2
ASSET_SPLICE_COPY_CUTSCENE = 3


def c_identifier(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def read_asset_layouts(path: Path) -> list[tuple[str, int]]:
    entries = [
        (name, int(vrom_end, 0) - int(vrom_start, 0))
        for vrom_start, vrom_end, name in ASSET_ENTRY_RE.findall(path.read_text())
    ]
    if not entries:
        raise ValueError(f"no PSP external assets found in {path}")
    if len(entries) >= 0xFFFF:
        raise ValueError("too many PSP external assets for 16-bit DMA maps")
    return entries


def read_readme_digests() -> dict[str, set[bytes]]:
    result: dict[str, set[bytes]] = {}

    for line in (ROOT / "README.md").read_text().splitlines():
        match = README_VERSION_RE.match(line)
        if match is None:
            continue
        version, digest_text = match.groups()
        for digest in MD5_RE.findall(digest_text):
            result.setdefault(version, set()).add(bytes.fromhex(digest))
    return result


def read_checksum_digests(version_dir: Path) -> set[bytes]:
    result: set[bytes] = set()

    for checksum in sorted(version_dir.glob("checksum*-compressed.md5")):
        digest = checksum.read_text().split()[0]
        if MD5_RE.fullmatch(digest) is None:
            raise ValueError(f"invalid MD5 in {checksum}")
        result.add(bytes.fromhex(digest))
    return result


def digest_region(version: str, digest: bytes) -> str:
    if version.startswith("pal-") or version.startswith("gc-eu"):
        return "REGION_EU"
    if version.startswith("gc-jp"):
        return "REGION_JP"
    japanese_checksum = ROOT / "baseroms" / version / "checksum-JP-compressed.md5"
    if japanese_checksum.is_file() and digest.hex() == japanese_checksum.read_text().split()[0]:
        return "REGION_JP"
    return "REGION_US"


def read_segment_names(path: Path) -> list[str]:
    with path.open(newline="") as stream:
        return [row["Name"] for row in csv.DictReader(stream)]


def format_u16_array(values: list[int]) -> list[str]:
    lines: list[str] = []
    for offset in range(0, len(values), 16):
        line = ", ".join(f"0x{value:04X}" for value in values[offset : offset + 16])
        lines.append(f"    {line},")
    return lines


def format_digest(digest: bytes) -> str:
    return ", ".join(f"0x{value:02X}" for value in digest)


def read_asset_schema(config: version_config.VersionConfig) -> dict[str, tuple[int, dict[str, int]]]:
    result: dict[str, tuple[int, dict[str, int]]] = {}

    for asset in config.assets:
        base = asset.start_offset or 0
        root = ET.parse(ROOT / asset.xml_path).getroot()
        for file_elem in root.iter("File"):
            name = file_elem.attrib.get("Name")
            if not name:
                continue
            segment_id = int(file_elem.attrib.get("Segment", "0"), 0)
            offsets: dict[str, int] = {}
            for elem in file_elem.iter():
                symbol = elem.attrib.get("Name")
                offset = elem.attrib.get("Offset")
                if symbol is not None and offset is not None:
                    offsets.setdefault(symbol, base + int(offset, 0))
            result[name] = (segment_id, offsets)
    return result


def asset_resource_copy_spec(elem: ET.Element) -> tuple[int | None, int] | None:
    size: int | None = None
    mode = ASSET_SPLICE_COPY_EXACT

    if elem.tag == "Texture":
        size = texture_size(elem)
    elif elem.tag == "DList":
        mode = ASSET_SPLICE_COPY_DLIST
    elif elem.tag == "Array":
        if len(elem) != 1:
            raise ValueError(f"unsupported Array description for {elem.attrib.get('Name')}")
        child = elem[0]
        if child.tag == "Vtx":
            element_size = 16
        elif (child.tag == "Vector" and child.attrib.get("Type") == "s16" and
              int(child.attrib.get("Dimensions", "0"), 0) == 3):
            element_size = 6
        elif child.tag == "Scalar" and child.attrib.get("Type") == "s16":
            element_size = 2
        else:
            raise ValueError(f"unsupported Array element {child.tag} for {elem.attrib.get('Name')}")
        size = int(elem.attrib["Count"], 0) * element_size
    elif elem.tag == "Limb":
        size = {
            "standard": 12,
            "lod": 16,
            "skin": 16,
            "curve": 12,
            "legacy": 32,
        }[elem.attrib["LimbType"].lower()]
    elif elem.tag == "LimbTable":
        size = int(elem.attrib["Count"], 0) * 4
    elif elem.tag == "Skeleton":
        skeleton_type = elem.attrib["Type"].lower()
        limb_type = elem.attrib["LimbType"].lower()
        if skeleton_type == "flex":
            size = 12
        elif skeleton_type == "curve" or limb_type in ("standard", "lod", "skin"):
            size = 8
        else:
            raise ValueError(f"unsupported Skeleton layout for {elem.attrib.get('Name')}")
    elif elem.tag == "Animation":
        size = 16
    elif elem.tag == "PlayerAnimation":
        size = 8
    elif elem.tag == "PlayerAnimationData":
        size = int(elem.attrib["FrameCount"], 0) * (22 * 3 + 1) * 2
    elif elem.tag == "LegacyAnimation":
        size = 12
    elif elem.tag == "CurveAnimation":
        size = 16
    elif elem.tag == "Collision":
        size = 44
    elif elem.tag == "Path":
        size = int(elem.attrib["NumPaths"], 0) * 8
    elif elem.tag in ("Scene", "Room"):
        mode = ASSET_SPLICE_COPY_SCENE_COMMANDS
    elif elem.tag == "Cutscene":
        mode = ASSET_SPLICE_COPY_CUTSCENE
    elif elem.tag == "Blob":
        size = int(elem.attrib["Size"], 0)
    elif elem.tag == "Mtx":
        size = 64
    else:
        return None
    pointer_kinds = {
        "Animation": 4, "PlayerAnimation": 4, "LegacyAnimation": 14,
        "Skeleton": 5, "LimbTable": 6, "Collision": 11, "Path": 12, "CurveAnimation": 13,
    }
    if elem.tag == "Limb":
        mode = {"standard": 7, "lod": 8, "skin": 9, "curve": 10, "legacy": 0}[elem.attrib["LimbType"].lower()]
    else:
        mode = pointer_kinds.get(elem.tag, mode)
    return size, mode


def read_asset_copy_specs(
    config: version_config.VersionConfig,
) -> dict[str, dict[str, tuple[int | None, int]]]:
    result: dict[str, dict[str, tuple[int | None, int]]] = {}

    for asset in config.assets:
        root = ET.parse(ROOT / asset.xml_path).getroot()
        for file_elem in root.iter("File"):
            file_name = file_elem.attrib.get("Name")
            if not file_name:
                continue
            sizes = result.setdefault(file_name, {})
            for elem in file_elem.iter():
                name = elem.attrib.get("Name")

                if name is None or "Offset" not in elem.attrib:
                    continue
                spec = asset_resource_copy_spec(elem)
                if spec is None:
                    raise ValueError(f"unsupported resource type {elem.tag} for {file_name}:{name}")
                previous = sizes.get(name)
                if (previous is not None) and (previous != spec):
                    raise ValueError(f"conflicting resource layouts for {file_name}:{name}")
                sizes[name] = spec
    return result


def build_asset_splices(
    asset_names: list[str],
    target_sizes: dict[str, int],
    canonical_schema: dict[str, tuple[int, dict[str, int]]],
    source_schema: dict[str, tuple[int, dict[str, int]]],
    canonical_copy_specs: dict[str, dict[str, tuple[int | None, int]]],
    source_copy_specs: dict[str, dict[str, tuple[int | None, int]]],
) -> list[tuple[int, int, int, int, int, int]]:
    asset_indices = {name: index for index, name in enumerate(asset_names)}
    result: list[tuple[int, int, int, int, int, int]] = []

    for name, asset_index in asset_indices.items():
        # File select has a dedicated revision-aware runtime loader because
        # its regional layouts contain non-canonical data sets. Code must also
        # retain its native layout: the audio and message metadata loaders use
        # offsets from the active ROM profile. Runtime patch sources embedded
        # in code carry their own per-profile offsets in the OPB4 blob.
        if name in ("code", "ovl_file_choose", "nintendo_rogo_static"):
            continue
        canonical = canonical_schema.get(name)
        source = source_schema.get(name)
        if canonical is None or source is None:
            continue
        target_size = target_sizes[name]
        pairs: dict[int, set[int]] = {}
        copy_specs: dict[tuple[int, int], tuple[int | None, int]] = {}
        for symbol in canonical[1].keys() & source[1].keys():
            target_offset = canonical[1][symbol]
            source_offset = source[1][symbol]
            if target_offset < target_size:
                pairs.setdefault(target_offset, set()).add(source_offset)
                specs = [
                    copy_map[symbol]
                    for copy_map in (canonical_copy_specs.get(name, {}), source_copy_specs.get(name, {}))
                    if symbol in copy_map
                ]
                if specs:
                    modes = {mode for _size, mode in specs}
                    if len(modes) != 1:
                        raise ValueError(f"conflicting copy modes for {name}:{symbol}")
                    limits = [size for size, _mode in specs if size is not None]
                    spec = (min(limits) if limits else None, modes.pop())
                    key = (target_offset, source_offset)
                    previous = copy_specs.get(key)
                    if previous is not None:
                        if previous[1] != spec[1]:
                            raise ValueError(f"conflicting aliased copy modes for {name}:{symbol}")
                        previous_limits = [size for size in (previous[0], spec[0]) if size is not None]
                        spec = (min(previous_limits) if previous_limits else None, spec[1])
                    copy_specs[key] = spec

        if not any(target_offset != source_offset for target_offset, sources in pairs.items()
                   for source_offset in sources) and canonical[0] == source[0] and not (
                       name.endswith("_scene") or "_room_" in name):
            continue

        target_boundaries = sorted(set(canonical[1].values()) | {0, target_size})
        source_boundaries = sorted(set(source[1].values()) | {0})
        records: dict[int, tuple[int, int]] = {}
        for target_offset, source_offsets in pairs.items():
            if len(source_offsets) != 1:
                raise ValueError(f"ambiguous source offsets for {name} at 0x{target_offset:X}")
            target_end = next(
                (offset for offset in target_boundaries if offset > target_offset), target_size
            )
            for source_offset in source_offsets:
                source_end = next(
                    (offset for offset in source_boundaries if offset > source_offset),
                    source_offset + target_end - target_offset,
                )
                copy_size = min(target_end - target_offset, source_end - source_offset)
                explicit_size, copy_mode = copy_specs.get(
                    (target_offset, source_offset), (None, ASSET_SPLICE_COPY_EXACT)
                )
                if explicit_size is not None:
                    if copy_size < explicit_size:
                        raise ValueError(
                            f"resource overlaps its next XML anchor in {name} at 0x{target_offset:X}"
                        )
                    copy_size = explicit_size
                if copy_size <= 0:
                    continue
                if copy_size > ASSET_SPLICE_COPY_SIZE_MASK:
                    raise ValueError(f"copy record too large for {name}:{target_offset:X}")
                records[target_offset] = (
                    source_offset, copy_size | (copy_mode << 28)
                )

        # Source order lets the PSP relocate exact XML-named segmented
        # pointers with a binary search, including arbitrarily reordered
        # regional texture collections.
        for target_offset, (source_offset, copy_size) in sorted(
            records.items(), key=lambda item: (item[1][0], item[0])
        ):
            result.append(
                (asset_index, source[0], canonical[0], target_offset, source_offset, copy_size)
            )
    return result


def generate(asset_table: Path, output: Path) -> None:
    asset_layouts = read_asset_layouts(asset_table)
    asset_names = [name for name, _size in asset_layouts]
    target_sizes = dict(asset_layouts)
    readme_digests = read_readme_digests()
    canonical_schema = union_schema(target_sizes)
    canonical_copy_specs = read_asset_copy_specs(version_config.load_version_config("ntsc-1.0"))
    profiles: list[dict[str, object]] = []

    for version_dir in sorted((ROOT / "baseroms").iterdir()):
        config_path = version_dir / "config.yml"
        segments_path = version_dir / "segments.csv"
        if not config_path.is_file() or not segments_path.is_file():
            continue

        version = version_dir.name
        config = version_config.load_version_config(version)
        asset_splices = build_asset_splices(
            asset_names,
            target_sizes,
            canonical_schema,
            read_asset_schema(config),
            canonical_copy_specs,
            read_asset_copy_specs(config),
        )
        if len(asset_splices) >= 0xFFFF:
            raise ValueError(f"too many asset layout records for {version}")
        segment_names = read_segment_names(segments_path)
        segment_indices = {name: index for index, name in enumerate(segment_names)}
        if len(segment_names) >= 0xFFFF:
            raise ValueError(f"too many DMA entries for {version}")

        code = config.dmadata_segments.get("code")
        if code is None or code.vram is None:
            raise ValueError(f"{version} does not define a code VRAM address")
        incbins = {incbin.name: incbin for incbin in config.incbins}
        try:
            njpg_text = incbins["njpgdspMainText"]
            njpg_data = incbins["njpgdspMainData"]
        except KeyError as error:
            raise ValueError(f"{version} does not define {error.args[0]}") from error

        digests = read_checksum_digests(version_dir)
        digests.update(readme_digests.get(version, set()))
        if not digests:
            raise ValueError(f"{version} has no compressed input-ROM digest")

        audio_offsets = {
            symbol: config.variables[symbol] - code.vram
            for symbol in (
                "gSequenceFontTable",
                "gSequenceTable",
                "gSoundFontTable",
                "gSampleBankTable",
            )
        }
        sequence_font_size = audio_offsets["gSequenceTable"] - audio_offsets["gSequenceFontTable"]
        sound_font_table_size = audio_offsets["gSequenceFontTable"] - audio_offsets["gSoundFontTable"]
        sequence_table_size = audio_offsets["gSampleBankTable"] - audio_offsets["gSequenceTable"]
        if sequence_font_size != 0x1C0:
            raise ValueError(f"{version} has an unsupported sequence-font table size")
        if sound_font_table_size != 0x270:
            raise ValueError(f"{version} does not have 38 sound-font entries")
        if sequence_table_size != 0x6F0:
            raise ValueError(f"{version} does not have 110 sequence entries")

        logo_asset = next(
            (asset for asset in config.assets if asset.name == "textures/nintendo_rogo_static"),
            None,
        )
        if logo_asset is None:
            raise ValueError(f"{version} does not define the Nintendo logo asset")
        logo_xml = ET.parse(ROOT / logo_asset.xml_path).getroot()
        logo_dlist = next(
            (
                elem
                for elem in logo_xml.iter("DList")
                if elem.attrib.get("Name") == "gNintendo64LogoDL"
            ),
            None,
        )
        if logo_dlist is None or "Offset" not in logo_dlist.attrib:
            raise ValueError(f"{version} does not define the Nintendo logo display-list offset")
        logo_dlist_offset = int(logo_dlist.attrib["Offset"], 0)
        file_choose_asset = next(
            (asset for asset in config.assets if asset.name == "overlays/ovl_file_choose"),
            None,
        )
        if file_choose_asset is None or file_choose_asset.start_offset is None:
            raise ValueError(f"{version} does not define the file-select data offset")

        profiles.append(
            {
                "name": version,
                "identifier": c_identifier(version),
                "dmadata_offset": config.dmadata_start,
                "dmadata_count": len(segment_names),
                "flags": 1 if version.startswith("ique-") else 0,
                "njpg_text_offset": njpg_text.vram - code.vram,
                "njpg_text_size": njpg_text.size,
                "njpg_data_offset": njpg_data.vram - code.vram,
                "njpg_data_size": njpg_data.size,
                "sequence_font_offset": audio_offsets["gSequenceFontTable"],
                "sequence_font_size": sequence_font_size,
                "sequence_table_offset": audio_offsets["gSequenceTable"],
                "sound_font_table_offset": audio_offsets["gSoundFontTable"],
                "sample_bank_table_offset": audio_offsets["gSampleBankTable"],
                "text_language": TEXT_LANGUAGES[config.text_lang],
                "message_table_offsets": [
                    config.variables.get(symbol, code.vram) - code.vram
                    if symbol in config.variables
                    else 0
                    for _segment, symbol in MESSAGE_ASSETS
                ],
                "message_dma_indices": [
                    segment_indices.get(segment, 0xFFFF)
                    for segment, _symbol in MESSAGE_ASSETS
                ],
                "nintendo_logo_dlist_offset": logo_dlist_offset,
                "file_choose_data_offset": file_choose_asset.start_offset,
                "asset_splices": asset_splices,
                "mapping": [segment_indices.get(name, 0xFFFF) for name in asset_names],
                "digests": sorted(digests),
            }
        )

    if not profiles:
        raise ValueError("no ROM profiles found")

    lines = [
        "/* Generated by tools/psp_port_rom_profiles.py. */",
        '#include "oot_psp_rom_profiles.h"',
        '#include "region.h"',
        "",
    ]
    for profile in profiles:
        lines.append(f"static const u16 sOotPspAssetDmaIndices_{profile['identifier']}[] = {{")
        lines.extend(format_u16_array(profile["mapping"]))
        lines.extend(["};", ""])
        splices = profile["asset_splices"]
        if splices:
            lines.append(f"static const OotPspRomAssetSplice sOotPspAssetSplices_{profile['identifier']}[] = {{")
            for asset_index, source_segment_id, target_segment_id, target_offset, source_offset, copy_size in splices:
                lines.append(
                    f"    {{ 0x{asset_index:04X}, 0x{source_segment_id:02X}, "
                    f"0x{target_segment_id:02X}, 0x{target_offset:08X}, "
                    f"0x{source_offset:08X}, 0x{copy_size:08X} }},"
                )
            lines.extend(["};", ""])

    lines.append("const OotPspRomProfile gOotPspRomProfiles[] = {")
    for profile in profiles:
        splice_symbol = "sOotPspAssetSplices_" + profile["identifier"] if profile["asset_splices"] else "NULL"
        lines.append(
            f'    {{ "{profile["name"]}", 0x{profile["dmadata_offset"]:08X}, '
            f'{profile["dmadata_count"]}, 0x{profile["flags"]:04X}, '
            f'0x{profile["njpg_text_offset"]:08X}, 0x{profile["njpg_text_size"]:08X}, '
            f'0x{profile["njpg_data_offset"]:08X}, 0x{profile["njpg_data_size"]:08X}, '
            f'0x{profile["sequence_font_offset"]:08X}, 0x{profile["sequence_font_size"]:08X}, '
            f'0x{profile["sequence_table_offset"]:08X}, 0x{profile["sound_font_table_offset"]:08X}, '
            f'0x{profile["sample_bank_table_offset"]:08X}, '
            f'{profile["text_language"]}, 0, '
            f'{{ {", ".join(f"0x{value:08X}" for value in profile["message_table_offsets"])} }}, '
            f'{{ {", ".join(f"0x{value:04X}" for value in profile["message_dma_indices"])} }}, 0, '
            f'0x{profile["nintendo_logo_dlist_offset"]:08X}, '
            f'0x{profile["file_choose_data_offset"]:08X}, '
            f'{len(profile["asset_splices"])}, 0, '
            f'{splice_symbol}, '
            f'sOotPspAssetDmaIndices_{profile["identifier"]} }},'
        )
    lines.extend(
        [
            "};",
            "const size_t gOotPspRomProfileCount =",
            "    sizeof(gOotPspRomProfiles) / sizeof(gOotPspRomProfiles[0]);",
            "",
            "const OotPspRomDigest gOotPspRomDigests[] = {",
        ]
    )
    for profile_index, profile in enumerate(profiles):
        for digest in profile["digests"]:
            region = digest_region(profile["name"], digest)
            lines.append(f"    {{ {{ {format_digest(digest)} }}, {profile_index}, {region} }},")
    lines.extend(
        [
            "};",
            "const size_t gOotPspRomDigestCount =",
            "    sizeof(gOotPspRomDigests) / sizeof(gOotPspRomDigests[0]);",
            "",
        ]
    )

    generated = "\n".join(lines)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != generated:
        output.write_text(generated)

    source_hash = hashlib.sha256(generated.encode()).hexdigest()[:12]
    print(
        f"wrote {len(profiles)} PSP ROM profiles and "
        f"{sum(len(profile['digests']) for profile in profiles)} digests to {output} ({source_hash})"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate build-independent PSP ROM profile metadata")
    parser.add_argument("asset_table", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.asset_table, args.output)


if __name__ == "__main__":
    main()
