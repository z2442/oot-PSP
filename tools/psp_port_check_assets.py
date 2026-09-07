#!/usr/bin/env python3
"""Run the production PSP converter on the host, optionally using a private ROM.

No game code is executed. The host shims only replace PSP file I/O, MD5 and UI.
Requires a C compiler and zlib (plus OpenSSL on non-Apple hosts).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/psp-port/ntsc-1.0")
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--rebuild", action="store_true", help="rebuild the test output even if its identity is valid")
    parser.add_argument("--output", type=Path, help="separate test output directory (required with --rom)")
    args = parser.parse_args()
    if bool(args.rom) != bool(args.output):
        parser.error("--rom and --output must be supplied together")
    build = args.build_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="oot-psp-check-") as temp:
        work = Path(temp)
        transform = (build / "oot_psp_asset_transform.z").resolve()
        prefix = "_" if sys.platform == "darwin" else ""
        assembly = work / "transform.s"
        assembly.write_text(
            f'.data\n.global {prefix}gOotPspAssetTransformCompressed\n'
            f'{prefix}gOotPspAssetTransformCompressed:\n.incbin "{transform}"\n'
            f'.global {prefix}gOotPspAssetTransformCompressedEnd\n'
            f'{prefix}gOotPspAssetTransformCompressedEnd:\n'
        )
        executable = work / "asset-check"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wno-deprecated-declarations",
            "-DOOT_PSP_UNPACKER_MODULE=1", "-I" + str(ROOT / "tools/tests/psp_host"),
            "-I" + str(ROOT / "src/port/psp"), "-I" + str(ROOT / "include"),
            str(ROOT / "tools/tests/psp_host/asset_builder.c"),
            str(ROOT / "src/port/psp/oot_psp_asset_root.c"),
            str(ROOT / "src/port/psp/oot_psp_rom_profiles.c"),
            str(build / "oot_psp_asset_segments.c"), str(build / "oot_psp_rom_profiles_data.c"),
            str(assembly), "-lz", *([] if sys.platform == "darwin" else ["-lcrypto"]),
            "-o", str(executable),
        ], check=True)
        command = [str(executable)]
        if args.rom:
            args.output.mkdir(parents=True, exist_ok=True)
            if args.rebuild:
                (args.output / "data/segments/oot_psp_assets.id").unlink(missing_ok=True)
            command.extend([str(args.rom.resolve()), str(args.output.resolve() / "EBOOT.PBP")])
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
