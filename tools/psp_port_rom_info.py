#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate a small C source blob with PSP port ROM metadata.")
    parser.add_argument("rom", type=Path, help="Path to baserom-decompressed.z64")
    parser.add_argument("output", type=Path, help="Output C source path")
    args = parser.parse_args()

    rom_data = args.rom.read_bytes()
    rom_md5 = hashlib.md5(rom_data).hexdigest()
    rom_header = ", ".join(f"0x{byte:02X}" for byte in rom_data[:16])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "\n".join(
            [
                '#include "ultra64.h"',
                "",
                f"const u32 gOotPspRomSize = {len(rom_data)}u;",
                f'const char gOotPspRomMd5[] = "{rom_md5}";',
                f"const u8 gOotPspRomHeader[16] = {{ {rom_header} }};",
                "",
            ]
        )
    )


if __name__ == "__main__":
    main()
