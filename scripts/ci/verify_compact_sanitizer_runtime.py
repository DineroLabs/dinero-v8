#!/usr/bin/env python3
"""Prove sanitizer runtimes start, report a real fault, and preserve log_path output."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    probes = {
        'asan': ('#include <stdlib.h>\nint main(void) { volatile int *p = (int*)malloc(sizeof(int)); '
                 'free((void*)p); *p = 3; return 0; }\n', 'heap-use-after-free'),
        'ubsan': ('#include <limits.h>\nint main(int argc, char** argv) { volatile int n = INT_MAX; '
                  'return n + argc; }\n', 'signed integer overflow'),
    }
    receipts = []
    for name, (code, diagnostic) in probes.items():
        source, binary = directory / (name + '.c'), directory / name
        source.write_text(code)
        subprocess.run(['clang', '-O0', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(source), '-o', str(binary)],
                       check=True, timeout=60)
        env = dict(os.environ,
                   ASAN_OPTIONS=f'detect_leaks=0:halt_on_error=1:abort_on_error=0:log_path={directory}/{name}-asan',
                   UBSAN_OPTIONS=f'halt_on_error=1:print_stacktrace=1:log_path={directory}/{name}-ubsan')
        try:
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            raise SystemExit(f'{name}: sanitizer runtime failed to start/finish within 30 seconds; no qualification result')
        reports = list(directory.glob(name + '-asan.*')) + list(directory.glob(name + '-ubsan.*'))
        text = '\n'.join(p.read_text(errors='replace') for p in reports)
        (directory / (name + '-stderr.txt')).write_text(result.stderr)
        if result.returncode == 0 or diagnostic not in text:
            raise SystemExit(f'{name}: expected failing exit and retained {diagnostic} report')
        receipts.append({'probe': name, 'exit': result.returncode, 'diagnostic': diagnostic,
                         'reports': [p.name for p in reports]})
    print(json.dumps({'architecture': platform.machine(), 'probes': receipts}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
