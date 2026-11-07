#!/usr/bin/env python3
"""Check ME cache ownership in the linked PSP image, including section tails."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CACHE_LINE = 64


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', nargs='?', type=Path,
                        default=ROOT / 'build/psp-port/ntsc-1.0/oot-psp-port.base.elf')
    args = parser.parse_args()
    output = subprocess.check_output(
        [os.environ.get('PSP_NM', 'psp-nm'), '-C', '-n', '-S', str(args.elf)], text=True)
    objects = []
    for line in output.splitlines():
        match = re.match(r'^([0-9a-fA-F]+) ([0-9a-fA-F]+) [bBdDgGsS] (.+)$', line)
        if match:
            address, size = (int(match.group(i), 16) for i in (1, 2))
            if size:
                objects.append((match.group(3), address, size))
    by_name = {name: (address, size) for name, address, size in objects}
    # The first two objects are accessed through uncached aliases. The others
    # can publish entire dirty ME cache lines, including their trailing bytes.
    owners = ('gOotPspAudioMeSharedStorage', 'sWakeGeneration', 'sMeScalarStorage',
              'sMixerExecutionState', 'sMixerMeState', 'sMixerCounters',
              'sOotPspAudioMeLocalFlags', 'sOotPspAudioReverbDownsampleCmds',
              'sOotPspAudioMeAssetInvalidations')
    errors = []
    for owner in owners:
        if owner not in by_name:
            errors.append(f'Missing cache owner: {owner}')
            continue
        address, size = by_name[owner]
        begin = address & -CACHE_LINE
        end = (address + size + CACHE_LINE - 1) & -CACHE_LINE
        if address != begin:
            errors.append(f'{owner} is not cache-line aligned')
        for other, other_address, other_size in objects:
            if other != owner and other_address < end and other_address + other_size > begin:
                errors.append(f'{owner} shares a cache line with {other} '
                              f'at 0x{other_address:08X}')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'ME cache layout: {len(owners)} owners isolated from neighboring globals')

    entry = subprocess.check_output(
        [os.environ.get('PSP_OBJDUMP', 'psp-objdump'), '-d', '--disassemble=meLibOnProcess',
         str(args.elf)], text=True)
    operations = {int(value, 16) for value in re.findall(r'\bcache\s+0x([0-9a-f]+),', entry)}
    if operations != {0x11, 0x13}:
        raise SystemExit(f'ME entry must discard both cache tags without writeback: {operations}')
    if re.search(r'\b(?:sp|jal|jalr|bal)\b', entry):
        raise SystemExit('ME cache reset must precede all C stack accesses and function calls')
    if '4080e000' not in entry.lower() or '4080e800' not in entry.lower():
        raise SystemExit('ME entry must zero both cache tag registers before invalidation')
    print('ME entry: both tag ways invalidated; no dirty writeback, stack access or C calls')


if __name__ == '__main__':
    main()
