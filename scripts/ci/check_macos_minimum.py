#!/usr/bin/env python3
"""Audit every bundled Mach-O slice's macOS minimum before signing."""
import argparse
import pathlib
import re
import subprocess

MAGICS = {bytes.fromhex(x) for x in ['feedface', 'cefaedfe', 'feedfacf', 'cffaedfe',
                                      'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca']}


def version(text):
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', text):
        raise ValueError(f'invalid macOS version: {text}')
    return tuple(int(x) for x in text.split('.')) + (0,) * (3 - len(text.split('.')))


def minimum(text):
    command = None
    values = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == 'cmd':
            command = fields[1]
        elif len(fields) == 2:
            if command == 'LC_BUILD_VERSION' and fields[0] == 'platform' and fields[1] not in ('1', 'MACOS', 'macos'):
                raise ValueError(f'not a macOS binary: {fields[1]}')
            if (command == 'LC_BUILD_VERSION' and fields[0] == 'minos') or (command == 'LC_VERSION_MIN_MACOSX' and fields[0] == 'version'):
                values.append(version(fields[1]))
    if len(values) != 1:
        raise ValueError(f'expected one macOS deployment minimum; found {len(values)}')
    return values[0]


def audit(root, arch, maximum):
    count = 0
    for path in sorted(root.rglob('*')):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open('rb') as stream:
            if stream.read(4) not in MAGICS:
                continue
        text = subprocess.check_output(['otool', '-arch', arch, '-l', str(path)], text=True)
        if minimum(text) > version(maximum):
            raise ValueError(f'{path} requires macOS {minimum(text)}, above {maximum}')
        count += 1
    if not count:
        raise ValueError('no Mach-O files audited')
    print(f'PASS: {count} Mach-O files support macOS {maximum} or earlier ({arch} slice)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=pathlib.Path)
    parser.add_argument('--arch', choices=['arm64', 'x86_64'], required=True)
    parser.add_argument('--maximum', required=True)
    args = parser.parse_args()
    try:
        audit(args.root, args.arch, args.maximum)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')
