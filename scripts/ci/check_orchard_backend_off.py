#!/usr/bin/env python3
"""Check a configured backend-OFF graph without building or modifying it."""
import argparse
import json
from pathlib import Path
import re
import shlex


class AuditError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def audit(build, source):
    build, source = Path(build).resolve(), Path(source).resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    require(re.search(r'^DINERO_BUILD_ORCHARD_BACKEND:BOOL=OFF$', cache, re.M),
            'Orchard backend must be explicitly OFF')
    commands = json.loads((build / 'compile_commands.json').read_text())
    require(isinstance(commands, list) and commands, 'Empty compile graph')
    selected = {str(source / n): [] for n in (
        'src/daemon/services/chainstate_service.cpp',
        'tests/daemon/test_assumeutxo_replay.cpp')}
    backend = source / 'rust/orchard_backend'
    allowed_include = (backend / 'include').resolve()
    for entry in commands:
        directory = Path(entry['directory'])
        file = Path(entry['file'])
        if not file.is_absolute():
            file = directory / file
        file = file.resolve()
        require(not file.is_relative_to(backend), 'Compiled Orchard backend source: ' + str(file))
        if str(file) not in selected:
            continue
        tokens = entry.get('arguments')
        if tokens is None:
            tokens = shlex.split(entry['command'])
        require(isinstance(tokens, list) and all(isinstance(x, str) for x in tokens),
                'Invalid compile arguments')
        for macro in ('DINERO_HAS_ORCHARD_RUNTIME_READER', 'DINERO_TEST_ORCHARD_ORIGIN'):
            require(not any(macro in x for x in tokens), 'Enabled backend macro: ' + macro)
        for token in tokens:
            if '/rust/orchard_backend' not in token:
                continue
            require(str(file) == str(source / 'tests/daemon/test_assumeutxo_replay.cpp')
                    and token.startswith('-I') and Path(token[2:]).is_absolute()
                    and Path(token[2:]).resolve() == allowed_include,
                    'Unexpected backend compiler dependency: ' + token)
        selected[str(file)].append(entry)
    require(all(selected.values()), 'Missing required daemon/replay compile entry')

    # Check the generated link/build graph, not merely the compile include path.
    # A source-only audit would miss a prebuilt Rust/static backend dependency.
    graph_files = []
    if (build / 'build.ninja').is_file():
        graph_files.append(build / 'build.ninja')
        graph_files.extend(build.glob('CMakeFiles/*.ninja'))
    else:
        targets = build / 'CMakeFiles/TargetDirectories.txt'
        require(targets.is_file(), 'Missing generated target inventory')
        graph_files.append(targets)
        links = list(build.rglob('link.txt'))
        require(links, 'Missing generated link commands')
        graph_files.extend(links)
    forbidden = re.compile(r'libdinero_orchard[^\s/\\:]*\.(?:a|so(?:\.[0-9]+)*|dylib|lib)'
                           r'|(?:^|\s)-ldinero_orchard[^\s]*'
                           r'|dinero_orchard_rust_build'
                           r'|CMakeFiles/dinero_orchard[^/\s]*\.dir')
    for file in graph_files:
        require(not forbidden.search(file.read_text()), 'Backend target or link dependency: ' + str(file))
    return {'compile_entries': len(commands), 'graph_files': len(graph_files),
            'required_sources': len(selected), 'backend': 'OFF'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        result = audit(args.build, args.source)
    except (AuditError, OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, 'Orchard backend OFF audit failed: ' + str(error) + '\n')
    print('Orchard backend OFF graph verified: ' + json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
