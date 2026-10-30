#!/usr/bin/env python3
"""Check Mac product tools at their actual app/DMG install locations."""
import argparse
import os
import pathlib
import subprocess

APP_TOOLS = ('dinerod', 'dinero-cli', 'dinero-swap-tower', 'dinero-qt',
             'dinero-miner', 'dinero-solo-miner', 'dinero-gpu-miner',
             'dinero-stratum-worker', 'dinero-sv2-miner', 'dinero-sv2-gpu-miner',
             'dinero-seeder')


def check(stage, arch):
    paths = [stage / 'dinero-qt.app/Contents/MacOS' / tool for tool in APP_TOOLS]
    # Wallet CLI is an operator sidecar; Qt does not launch it from the app.
    paths.append(stage / 'dinero-wallet-cli')
    for path in paths:
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f'missing executable product tool: {path}')
        actual = subprocess.check_output(['lipo', '-archs', str(path)], text=True).strip()
        if actual != arch:
            raise ValueError(f'{path}: expected native {arch}, got {actual}')
    print(f'PASS: all {len(paths)} product tools present and native {arch}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', type=pathlib.Path)
    p.add_argument('--arch', choices=['arm64', 'x86_64'], required=True)
    args = p.parse_args()
    try:
        check(args.stage, args.arch)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        p.exit(1, f'{exc}\n')
