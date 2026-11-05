#!/usr/bin/env python3
"""Package the PSP runtime files for installation under PSP/GAME."""

from __future__ import annotations

import argparse
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME_FILES = ("EBOOT.PBP", "Plugins/dvemgr.prx")


def package(build_dir: Path, output: Path) -> None:
    files = [(build_dir / name, f"OOT_PSP/{name}") for name in RUNTIME_FILES]
    files.append((ROOT / "docs/psp-install.txt", "OOT_PSP/README.txt"))
    files.append((ROOT / "docs/PSPSDK-LICENSE.txt", "OOT_PSP/PSPSDK-LICENSE.txt"))
    for source, _ in files:
        if not source.is_file() or source.stat().st_size == 0:
            raise ValueError(f"required package file is missing or empty: {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("OOT_PSP/data/", b"")
        for source, destination in files:
            archive.write(source, destination)
    print(f"Created {output}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="directory containing EBOOT.PBP and Plugins")
    parser.add_argument("output", type=Path, help="destination ZIP archive")
    args = parser.parse_args()
    try:
        package(args.build_dir, args.output)
    except ValueError as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
