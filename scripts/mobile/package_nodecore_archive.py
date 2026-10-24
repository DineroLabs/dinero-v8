"""Merge exactly a successful NodeCore executable's archive closure, then relink.

Unlike a directory glob or hand-maintained list, the real link command includes
new Orchard, consensus-core, wallet-catalog and Rust archives automatically.
Never install the result into an app or force-load every duplicate definition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def link_tokens(command):
    # CMake's Ninja rule wraps a single linker invocation in ': && ... && :'.
    # Do not execute shell text or accept arbitrary pipelines/response files.
    candidates = []
    for line in command.splitlines():
        for segment in line.split(' && '):
            tokens = shlex.split(segment)
            if '-o' in tokens and '-c' not in tokens and any('nodecore_link_smoke.cpp.o' in token for token in tokens):
                candidates.append(tokens)
    if len(candidates) != 1:
        raise ValueError('expected exactly one NodeCore smoke executable link command')
    tokens = candidates[0]
    if any(token in (';', '|', '||', '>', '<') or token.startswith('@') for token in tokens):
        raise ValueError('unsupported shell/response-file link command')
    return tokens


def archives(tokens, build):
    result = []
    for token in tokens:
        if token.endswith('.a'):
            path = (build / token).resolve()
            if not path.is_file():
                raise ValueError(f'missing linked archive: {path}')
            if path not in result:
                result.append(path)
    if not any(path.name == 'libdinero_orchard_snapshot_store.a' for path in result):
        raise ValueError('v6 snapshot store is absent from NodeCore link closure')
    if not any(path.name == 'libdinero_orchard_snapshot.a' for path in result):
        raise ValueError('v6 snapshot codec is absent from NodeCore link closure')
    if not any(path.name == 'libdinero_orchard_backend.a' and 'cargo' in path.parts for path in result):
        raise ValueError('real Orchard Rust archive is absent from NodeCore link closure')
    return result


def relink(tokens, archive, output):
    result = []
    inserted = False
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token == '-o':
            result.extend(['-o', str(output)])
            i += 2
            continue
        if token.endswith('.a'):
            if not inserted:
                result.append(str(archive))
                inserted = True
        else:
            result.append(token)
        i += 1
    return result


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            result.update(chunk)
    return result.hexdigest()


def package(build, command, output):
    from sdk_consensus_manifest import build_source_receipt
    build, output = build.resolve(), output.resolve()
    provenance = build_source_receipt(build)
    tokens = link_tokens(command)
    inputs = archives(tokens, build)
    if output.exists():
        raise ValueError(f'refusing to replace existing SDK artifact: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['xcrun', 'libtool', '-static', '-o', str(output), *map(str, inputs)], check=True)
    # This second link uses ONLY the packaged archive, retaining the original
    # object, SDK/architecture flags and required system libraries/frameworks.
    smoke = output.with_suffix('.link-smoke')
    subprocess.run(relink(tokens, output, smoke), cwd=build, check=True)
    manifest = dict(qualification_only=True, **provenance,
                    archive=str(output), sha256=digest(output),
                    packaged_link_smoke=str(smoke),
                    inputs=[dict(path=str(path), sha256=digest(path)) for path in inputs])
    output.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    link_file = args.build / 'CMakeFiles/nodecore_link_smoke.dir/link.txt'
    if link_file.is_file():
        command = link_file.read_text()
    else:
        command = subprocess.check_output(['ninja', '-C', str(args.build), '-t', 'commands',
                                           'nodecore_link_smoke'], text=True)
    package(args.build, command, args.output)


if __name__ == '__main__':
    main()
