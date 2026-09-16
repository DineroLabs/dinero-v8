#!/usr/bin/env python3
"""Reject shielded sanitizer builds whose tested source files lack instrumentation."""
import argparse
import json
import shlex
from pathlib import Path


def enabled_sanitizers(arguments):
    enabled = set()
    for argument in arguments:
        if argument.startswith('-fsanitize='):
            enabled.update(argument.split('=', 1)[1].split(','))
        elif argument.startswith('-fno-sanitize='):
            disabled = set(argument.split('=', 1)[1].split(','))
            if 'all' in disabled:
                enabled.clear()
            else:
                enabled.difference_update(disabled)
    return enabled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('sanitizers', help='Required comma-separated sanitizer flags')
    parser.add_argument('--fuzz', action='store_true')
    args = parser.parse_args()
    sources = [
        'contrib/benchmarks/compact_spartan_codec.cpp',
        'src/zk/zkvm/scalar.cpp',
        'src/zk/zkvm/r1cs_spartan.cpp',
        'src/consensus/shielded/shielded_serialization.cpp',
        'src/wallet/shielded_derivation.cpp',
    ]
    sources += (['fuzz/fuzz_compact_spartan.cpp', 'fuzz/fuzz_shielded_surfaces.cpp']
                if args.fuzz else ['tests/zk/test_compact_spartan.cpp',
                                      'tests/zk/test_spartan_soundness.cpp'])
    entries = json.loads((args.build_dir / 'compile_commands.json').read_text())
    required = set(args.sanitizers.split(','))
    failures, checked = [], []
    for source in sources:
        matches = [entry for entry in entries
                   if Path(entry['file']).as_posix().endswith('/' + source)]
        if not matches:
            failures.append(f'{source}: absent from compile commands')
        for entry in matches:
            arguments = entry.get('arguments') or shlex.split(entry['command'])
            enabled = enabled_sanitizers(arguments)
            checked.append({'source': source, 'enabled': sorted(enabled)})
            if not required <= enabled:
                failures.append(f'{source}: missing {sorted(required - enabled)}')
    print(json.dumps({'required': sorted(required), 'checked': checked,
                      'failures': failures}, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
