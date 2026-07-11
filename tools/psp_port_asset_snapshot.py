#!/usr/bin/env python3

from __future__ import annotations

import argparse
import mmap
import re
import struct
import zipfile
import zlib
from pathlib import Path


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
    root = output_dir.parents[2]
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
            target.write_bytes(archive.read(name))


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

    args = parser.parse_args()
    if args.command == "create":
        create_snapshot(args.build_dir, args.extracted_dir, args.snapshot, args.transform, args.packed_assets)
    elif args.command == "restore":
        restore_snapshot(args.snapshot, args.output_dir)
    elif args.command == "scrub-transform":
        args.output.write_bytes(scrub_transform(args.build_dir, args.extracted_dir, args.old_transform))
    else:
        sanitize_snapshot(args.source, args.output, args.generated_dir)


if __name__ == "__main__":
    main()
