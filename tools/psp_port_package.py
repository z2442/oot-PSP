#!/usr/bin/env python3
"""Package only redistributable PSP build outputs, never the build's data tree."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from psp_port_check_module import check_module

ROOT = Path(__file__).resolve().parents[1]


def package(bundle: Path, output: Path, revision: str) -> None:
    versions = sorted(p.name for p in (ROOT / 'baseroms').iterdir()
                      if (p / 'config.yml').is_file() and (p / 'segments.csv').is_file())
    paths = ['EBOOT.PBP', 'Modules/unpacker.prx', 'Plugins/dvemgr.prx']
    paths += [f'Modules/{version}.prx' for version in versions]
    contents = {}
    for relative in paths:
        source = bundle / relative
        if source.is_symlink():
            raise ValueError(f'refusing linked bundle file: {source}')
        data = source.read_bytes()
        expected = (b'\0PBP',) if relative.endswith('.PBP') else (b'\x7fELF', b'~PSP')
        if data[:4] not in expected:
            raise ValueError(f'invalid build output: {source}')
        if relative != "Plugins/dvemgr.prx":
            check_module(data, relative == "Modules/unpacker.prx")
        contents[relative] = data
    manifest = {'revision': revision, 'versions': versions,
                'sha256': {path: hashlib.sha256(data).hexdigest() for path, data in contents.items()}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path, data in contents.items():
            archive.writestr('OOT_PSP/' + path, data)
        archive.writestr('OOT_PSP/build.json', json.dumps(manifest, indent=2) + '\n')
        archive.writestr('OOT_PSP/data/README.txt',
            'Place one supported Ocarina of Time .z64 ROM here. Any filename is accepted.\n'
            'Do not turn off the PSP during first-boot asset extraction.\n')
        archive.writestr('OOT_PSP/README.txt',
            'Copy OOT_PSP into PSP/GAME. Keep EBOOT.PBP, Modules and Plugins together.\n'
            'Place your ROM in data and launch the game. Extraction runs once.\n'
            'To change ROMs, remove data/segments/oot_psp_assets.id before launching.\n'
            'No ROM or extracted assets are included. See build.json for the source revision.\n')
    print(f'Packaged {len(versions)} game modules: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--revision', default='local')
    args = parser.parse_args()
    package(args.bundle, args.output, args.revision)
