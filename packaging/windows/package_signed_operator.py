#!/usr/bin/env python3
"""Package signed operator bytes and their required NVRTC redistributables."""
import argparse
import hashlib
from pathlib import Path
import re
import zipfile

TOOLS = ('dinerod.exe', 'dinero-cli.exe', 'dinero-miner.exe',
         'dinero-stratum-worker.exe', 'dinero-gpu-miner.exe',
         'dinero-wallet-cli.exe', 'dinero-seeder.exe',
         'dinero-sv2-miner.exe', 'dinero-sv2-gpu-miner.exe')


def ordinary(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('required ordinary payload file absent: ' + path.name)
    return path.read_bytes()


def assemble(stage, license_file, output, version):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('expected numeric release version')
    if stage.is_symlink() or not stage.is_dir():
        raise ValueError('signed stage must be an ordinary directory')
    entries = {'bin/' + name: ordinary(stage / name) for name in TOOLS}
    # The GUI build's daemon imports NVRTC even when mining is not requested.
    # The compiler also dynamically loads its matching builtins library.
    for pattern in ('nvrtc64_*.dll', 'nvrtc-builtins64_*.dll'):
        matches = list(stage.glob(pattern))
        if len(matches) != 1:
            raise ValueError('expected one required NVRTC runtime: ' + pattern)
        entries['bin/' + matches[0].name] = ordinary(matches[0])
    entries['LICENSE'] = ordinary(license_file)
    entries['README.txt'] = (
        'Dinero ' + version + ' signed Windows operator archive.\n'
        'Keep every file in bin together.\n'
        'For mainnet, configure lightning.oracles.enable=0; this disables the optional legacy Lightning IPC oracles, not block relay.\n'
        'Requires the Microsoft Visual C++ 2015-2022 x64 Redistributable.\n'
        'The NVIDIA NVRTC compiler runtime is included; CUDA mining also '
        'requires a compatible NVIDIA GPU and display driver.\n').encode('ascii')
    entries['SHA256SUMS.txt'] = ''.join(
        hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
        for name, data in sorted(entries.items())).encode('ascii')
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as stream:
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(entries.items()):
                archive.writestr(name, data)
    return {name: hashlib.sha256(data).hexdigest() for name, data in entries.items()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--license', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--version', required=True)
    a = p.parse_args()
    entries = assemble(a.stage, a.license, a.output, a.version)
    print('SIGNED_OPERATOR_PACKAGE_PASS entries=' + str(len(entries)))


if __name__ == '__main__':
    main()
