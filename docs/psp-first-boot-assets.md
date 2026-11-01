# PSP first-boot assets

The PSP build does not read a ROM or require a pre-existing `extracted/` tree.
Run `./psp.sh` to produce `build/psp-port/ntsc-1.0/EBOOT.PBP` using only the
checked-in NTSC 1.0 metadata and zero-filled compiler placeholders.

Install the files on the memory stick with this layout:

```text
OOT_PSP/
├── EBOOT.PBP
├── Plugins/
│   └── dvemgr.prx
└── data/
    └── basrom.z64
```

`data/baserom.z64` is also accepted as a compatibility spelling. The ROM must
be the big-endian, compressed NTSC 1.0 ROM (32 MiB, CRC32 `CD16C529`).

`dvemgr.prx` enables the system's DVE video-output controls on PSP Slim and
later models. The port checks the hardware model through kuBridge before it
loads the module, so a PSP-1000 does not load or call the DVE bridge. On a Slim
or later system the module stays loaded for the session, even when no cable was
present during startup.

Press HOME and open **Video Settings** to choose LCD or TV output, 480i or
480p, and a 4:3 or 16:9 viewport. Composite cables are limited to 480i;
component cables can use either TV resolution. **Apply video mode** changes the
current session, while **Save video.ini** writes the selection beside
`EBOOT.PBP` for later launches. TV output remains unavailable on a PSP-1000, if
no cable is detected, or if `Plugins/dvemgr.prx` is missing.

**VSync** defaults to **On** and can be toggled with Left/Right or Cross in
**Video Settings**. It takes effect immediately for gameplay and the custom HOME
menu. **Save video.ini** persists the choice (`vsync = on` or `vsync = off`),
and **Reset defaults** turns VSync back on. Older configuration files without
a `vsync` entry also default to on.

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
