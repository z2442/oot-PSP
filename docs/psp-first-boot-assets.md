# PSP first-boot assets

The PSP build does not read a ROM or require a pre-existing `extracted/` tree.
Run `./psp.sh` to produce `build/psp-port/ntsc-1.0/EBOOT.PBP` using only the
checked-in NTSC 1.0 metadata and zero-filled compiler placeholders.

Install the files on the memory stick with this layout:

```text
OOT_PSP/
├── EBOOT.PBP
└── data/
    └── basrom.z64
```

`data/baserom.z64` is also accepted as a compatibility spelling. The ROM must
be the big-endian, compressed NTSC 1.0 ROM (32 MiB, CRC32 `CD16C529`).

On the first launch, the port validates the ROM and atomically creates
`data/segments/oot_psp_assets.bin`. It also fills the executable's zero
placeholders from that user-supplied ROM. A failed or interrupted conversion
removes the temporary output. A home-menu-styled progress screen covers ROM
validation and asset conversion, and shows a readable error panel if setup
cannot finish. Later launches use the completed file directly, so the ROM is
not scanned or converted again.

Delete `data/segments/oot_psp_assets.bin` to force a rebuild from the ROM.

The conversion metadata snapshot is maintained from a private reference build
with:

```sh
python3 tools/psp_port_asset_snapshot.py create \
    build/psp-port/ntsc-1.0 \
    extracted/ntsc-1.0/baserom \
    assets/psp/ntsc-1.0/generated.zip \
    assets/psp/ntsc-1.0/asset_transform.z \
    --packed-assets build/psp-port/ntsc-1.0/data/segments/oot_psp_assets.bin
```

The statically linked placeholder refill recipes are maintained with:

```sh
python3 tools/psp_port_runtime_patches.py create \
    ntsc-1.0 \
    build/psp-port/ntsc-1.0/oot-psp-port.reference.elf \
    build/psp-port/ntsc-1.0/oot_psp_asset_segments.c \
    extracted/ntsc-1.0/baserom \
    assets/psp/ntsc-1.0/runtime_patches.z
```

These maintainer-only commands require an extracted ROM and a reference build,
but their checked-in outputs contain only declarations, zero placeholders,
addresses, byte-order selectors, and source-offset/delta recipes. They do not
store the packed asset file, message text, textures, audio, or literal
reconstructed bytes. Never commit a ROM, an extraction tree, the reference ELF,
or `oot_psp_assets.bin`. Normal PSP compilation only restores and resolves the
checked-in payload-free metadata.
