#!/usr/bin/env python3
"""Bind artifact-only desktop builds to an exact source SHA and source version."""
import argparse
import pathlib
import re
import subprocess


def validate(candidate, source=None):
    if not re.fullmatch(r'[0-9a-f]{40}', candidate):
        raise ValueError('candidate source must be a full lowercase commit SHA')
    if source is None:
        return None
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != candidate:
        raise ValueError(f'checkout {head} does not match candidate {candidate}')
    versions = re.findall(r'^project\(dinero VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES',
                          (source / 'CMakeLists.txt').read_text(), re.MULTILINE)
    if len(versions) != 1:
        raise ValueError('expected exactly one Dinero source version')
    return versions[0]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate')
    parser.add_argument('--source', type=pathlib.Path)
    args = parser.parse_args()
    try:
        version = validate(args.candidate, args.source)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')
    if version:
        print(version)
