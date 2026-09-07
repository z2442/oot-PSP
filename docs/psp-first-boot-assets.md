# PSP builds and first-boot assets

Run `./psp.sh` with PSPSDK on `PATH`. The build produces a launcher, a standalone
unpacker, and a secondary game PRX for each of the 16 configurations in
`baseroms/`. Building does not require a ROM or an existing `extracted/` tree.

The default bundle is `build/psp-port/ntsc-1.0/`. Copy these files together:

```text
PSP/GAME/OOT_PSP/
├── EBOOT.PBP
├── Modules/
│   ├── unpacker.prx
│   ├── ntsc-1.0.prx
│   ├── pal-1.1.prx
│   └── ... (all supported game modules)
├── Plugins/
│   └── dvemgr.prx
└── data/
    └── Your Ocarina of Time ROM.z64
```

Any `.z64` filename is accepted, including spaces and uppercase `.Z64`.
The historical `basrom` and `baserom` names are checked first, in `.z64`,
`.n64`, and `.v64` formats. Without one of those names, keep one `.z64` file
in `data/`; multiple candidates produce a clear error instead of selecting an
arbitrary ROM. File contents and checksums determine the revision, not the name.
Renaming an unsupported ROM does not make it supported.

The unpacker displays the original **Preparing Game Data** screen and progress
bar. Do not turn off the PSP during extraction. It saves the pack and identity
in `data/segments/`, shuts down its UI and heap, then returns to the launcher.
The launcher stops and unloads the unpacker before loading the matching game
PRX. A valid existing pack skips extraction on subsequent launches.

To change ROMs or rebuild assets, remove `data/segments/oot_psp_assets.id` and
put the desired ROM in `data/`. Packs without a current identity are rebuilt;
size alone is never treated as proof that a pack matches a ROM. Conversion
changes invalidate older identities automatically.

## Revision handling

ROM profiles cover N64 NTSC/PAL revisions, GameCube releases, Master Quest,
PAL debug releases, and iQue, including the checksum aliases in the main README.
Input byte order is normalized while reading. Yaz0 and iQue raw DEFLATE are
supported. Audio and message banks retain each ROM's lengths and table offsets.
Each game module is compiled with its revision's language and timing paths.

Objects use the canonical symbol layout so existing PSP fixes continue to refer
to the same resources. Animation frame counts come from the source ROM, which
allows PAL-resampled animations. Relocation follows typed pointer fields;
animation indices, vertices, and texture pixels are not guessed to be pointers.
Runtime patch relocations retain linked addends, including pointers into arrays.

For other revisions, scenes and rooms are parsed from their actual command
headers, collision records, room shapes, and display-list graphs. Their native
size and packed location are recorded in the identity. This preserves changes
such as PAL's cullable graveyard room and Master Quest's different room content.
Named scene symbols are linked at the selected revision's offsets. Scene
textures retain source byte order, separately from converted CPU/graphics
structures. NTSC 1.0 keeps its established native conversion.

## Incremental builds and packaging

`./psp.sh` builds incrementally. Use `CLEAN_BUILD=1 ./psp.sh` for an explicit clean
build. `JOBS=8` controls parallelism. Make overrides can be passed to the script,
including `PSP_PORT_INSTALL_DIR=build/psp-bundle`.

For a single module during development:

```sh
make VERSION=pal-1.1 PSP_PORT_NO_ASSET_REGEN=1 psp-port-game-module
```

Use `gmake` on macOS. `GPROF=1 ./psp.sh` selects the profiling build.

Create an installable ZIP from a completed bundle:

```sh
python3 tools/psp_port_package.py build/psp-port/ntsc-1.0 dist/oot-psp.zip
```

Packaging uses an explicit file list: EBOOT, unpacker, game modules, and DVE
plugin. It never copies the build's ROM, extracted assets, saves, or cache.
The ZIP includes supported versions and SHA-256 hashes in `build.json`.

## CI and nightlies

[PSP builds](../.github/workflows/psp.yml) builds the complete bundle on pushes,
pull requests, manual runs, and daily at 06:17 UTC. ZIPs are available as Actions
artifacts. Scheduled runs and manual runs on the default branch also publish a
dated nightly prerelease on the repository's Releases page. The workflow must
be on the default branch and Actions enabled for scheduled builds to run.

The container build uses the official PSPDEV image and a pinned Media Engine
library revision. No ROM or private extracted files are uploaded to CI.

## Asset checks

After generating build metadata, run the production unpacker with host file I/O
and address/undefined-behavior sanitizers:

```sh
python3 tools/psp_port_check_assets.py
python3 tools/psp_port_check_assets.py --rom /path/to/rom.z64 \
    --output build/asset-check/pal-1.1 --rebuild
```

The first command uses synthetic data to check byte orders, compression,
bounds, pointer-like animation values, variable room shapes, and ROM discovery.
The second performs full extraction into a separate test directory. `--rebuild`
invalidates only that test directory's identity.

To check NTSC 1.0 against the established conversion recipes:

```sh
.venv/bin/python3 tools/psp_port_compare_ntsc.py /path/to/ntsc-1.0.z64 \
    build/asset-check/ntsc-1.0/data/segments/oot_psp_assets.bin
```

Private-ROM extraction checks have covered NTSC 1.0, NTSC 1.2, PAL 1.1, and
GameCube Europe Master Quest. This is extraction validation, not a complete
playthrough or a substitute for testing the launcher on PSP hardware.
