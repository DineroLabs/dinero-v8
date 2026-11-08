#!/usr/bin/env python3
"""Prepare pinned primary/recovery pairs locally, without qualifying or publishing them."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts/ci'))
from check_snapshot_freshness import validate as check_freshness


def verify(data, manifest, expected, primary=False):
    if manifest.get('network') != 'mainnet' or not isinstance(manifest.get('snapshot'), dict):
        raise ValueError('not a mainnet snapshot manifest')
    identity = manifest['snapshot']
    for field in ('height', 'block_hash', 'bytes', 'sha256'):
        if identity.get(field) != expected[field]:
            raise ValueError('manifest differs from pinned ' + field)
    if data.stat().st_size != expected['bytes']:
        raise ValueError('payload length differs from pin')
    with data.open('rb') as stream:
        header = stream.read(68)
        if len(header) != 68 or header[:4] != b'UXTO':
            raise ValueError('invalid payload header')
        if (struct.unpack_from('<I', header, 4)[0] != expected['format_version'] or
                struct.unpack_from('<I', header, 40)[0] != expected['height'] or
                header[8:40][::-1].hex() != expected['block_hash']):
            raise ValueError('payload header differs from pinned identity')
        stream.seek(0)
        if hashlib.file_digest(stream, 'sha256').hexdigest() != expected['sha256']:
            raise ValueError('payload checksum differs from pin')
        stream.seek(0)
        checksum = hashlib.sha256()
        remaining = expected['bytes'] - 32
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError('truncated payload')
            checksum.update(chunk)
            remaining -= len(chunk)
        if stream.read(32) != checksum.digest():
            raise ValueError('internal payload checksum mismatch')
    if not primary and identity.get('snapshot_file') != expected['filename']:
        raise ValueError('fallback manifest filename differs from its retained payload')


def prepare(catalog, candidate_dir, fallback_dir, destination, now=None):
    destination = Path(destination)
    if destination.exists():
        raise ValueError('destination already exists; retained inputs are never overwritten')
    if catalog.get('schema') != 'dinero.desktop-snapshot-inputs.v1':
        raise ValueError('unsupported input catalog')
    entries = [catalog['primary'], *catalog['fallbacks']]
    names = [entry['filename'] for entry in entries]
    if len(set(names)) != len(names) or any(Path(n).name != n or not n.endswith('.dat') for n in names):
        raise ValueError('duplicate or unsafe snapshot filename')
    if len({entry['height'] for entry in entries}) != len(entries):
        raise ValueError('duplicate snapshot base')
    verified = []
    for index, expected in enumerate(entries):
        primary = index == 0
        root = Path(candidate_dir if primary else fallback_dir)
        data = root / ('snapshot.dat' if primary else expected['filename'])
        metadata = root / ('manifest.json' if primary else expected['filename'] + '.manifest.json')
        manifest = json.loads(metadata.read_text())
        verify(data, manifest, expected, primary)
        if primary:
            check_freshness(manifest, now=now)  # Age-only; no claim of burial qualification.
        verified.append((data, metadata, manifest, expected))
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.desktop-inputs-', dir=destination.parent))
    try:
        for index, (data, metadata, manifest, expected) in enumerate(verified):
            filename = expected['filename']
            shutil.copyfile(data, stage / filename)
            if index == 0:
                normalized = copy.deepcopy(manifest)
                normalized['snapshot']['snapshot_file'] = filename
                (stage / (filename + '.manifest.json')).write_text(json.dumps(normalized, indent=2) + '\n')
            else:
                shutil.copyfile(metadata, stage / (filename + '.manifest.json'))
            # Bind the copied bytes too, so changing a source during staging
            # cannot leave a success receipt for another payload.
            copied_manifest = json.loads((stage / (filename + '.manifest.json')).read_text())
            verify(stage / filename, copied_manifest, expected, index == 0)
        receipt = {'status': 'INPUTS_PREPARED_NOT_RELEASE_ADMITTED',
                   'primary_height': entries[0]['height'], 'fallback_heights': [e['height'] for e in entries[1:]],
                   'files': names, 'native_qualified': False, 'signatures_verified': False, 'published': False}
        (stage / 'preparation-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        stage.rename(destination)
        return receipt
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, default=Path(__file__).with_name('desktop-inputs-candidate.json'))
    parser.add_argument('--candidate-dir', type=Path, required=True)
    parser.add_argument('--fallback-dir', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    try:
        receipt = prepare(json.loads(args.catalog.read_text()), args.candidate_dir,
                          args.fallback_dir, args.destination)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f'desktop input preparation refused: {exc}\n')
    print(json.dumps(receipt, sort_keys=True))
