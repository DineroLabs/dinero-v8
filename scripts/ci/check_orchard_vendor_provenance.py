#!/usr/bin/env python3
"""Prove what the vendored Orchard crate is.

rust/orchard_backend builds against rust/orchard_backend/vendor/orchard, a
locally patched copy of the upstream `orchard` crate. This check makes the
statement "upstream plus one small reviewed patch" something the build proves
instead of something a reader has to take from the notes:

  1. the upstream crate archive has the recorded SHA-256;
  2. no upstream file is missing from the vendored copy;
  3. the only files added to it are the ones the manifest names;
  4. the difference between the two, as a unified diff, is byte for byte the
     committed, reviewed patch file.

Anything else in the vendored tree (one changed byte in any other file, a new
file, a removed file, a change to the patch that was not reviewed) fails.

It reads and compares; it never modifies the vendored tree. `--write-patch`
regenerates the patch file for a reviewer after an intended change.
"""
import argparse
import difflib
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import urllib.request

MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_MEMBER_BYTES = 16 * 1024 * 1024
MAX_MEMBERS = 5000


class ProvenanceError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ProvenanceError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_manifest(path):
    manifest = json.loads(Path(path).read_text())
    for key in ('crate', 'version', 'archive_url', 'archive_sha256', 'added_files', 'patch'):
        require(key in manifest, 'manifest lacks ' + key)
    require(isinstance(manifest['added_files'], list), 'manifest added_files must be a list')
    require(len(manifest['archive_sha256']) == 64, 'manifest archive_sha256 is not a SHA-256')
    return manifest


def read_archive(manifest, archive_path):
    if archive_path:
        data = Path(archive_path).read_bytes()
    else:
        with urllib.request.urlopen(manifest['archive_url'], timeout=120) as reply:
            data = reply.read(MAX_ARCHIVE_BYTES + 1)
    require(len(data) <= MAX_ARCHIVE_BYTES, 'upstream archive is larger than expected')
    found = sha256(data)
    require(found == manifest['archive_sha256'],
            'upstream archive SHA-256 is %s, the manifest records %s' % (found, manifest['archive_sha256']))
    return data


def upstream_files(manifest, data):
    """{relative posix path: bytes} for every regular file in the archive."""
    root = '%s-%s' % (manifest['crate'], manifest['version'])
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        members = archive.getmembers()
        require(len(members) <= MAX_MEMBERS, 'upstream archive has too many members')
        for member in members:
            path = PurePosixPath(member.name)
            require(not path.is_absolute() and '..' not in path.parts and path.parts and path.parts[0] == root,
                    'upstream archive member outside %s/: %s' % (root, member.name))
            if member.isdir():
                continue
            require(member.isfile(), 'upstream archive member is not a regular file: ' + member.name)
            require(member.size <= MAX_MEMBER_BYTES, 'upstream archive member is too large: ' + member.name)
            relative = PurePosixPath(*path.parts[1:]).as_posix()
            require(relative and relative not in files, 'upstream archive repeats ' + member.name)
            files[relative] = archive.extractfile(member).read()
    require(files, 'upstream archive holds no files')
    return files


def vendored_files(vendor):
    """{relative posix path: bytes}. Tracked files when the tree is in git
    (so a local build's leftovers do not count), every file otherwise."""
    vendor = Path(vendor).resolve()
    require(vendor.is_dir(), 'vendored crate directory not found: ' + str(vendor))
    names = None
    try:
        listed = subprocess.run(['git', '-C', str(vendor), 'ls-files', '-z', '--', '.'],
                                capture_output=True, check=True).stdout
        if listed:
            names = [n for n in listed.decode().split('\0') if n]
    except (OSError, subprocess.CalledProcessError):
        names = None
    if names is None:
        names = [p.relative_to(vendor).as_posix() for p in sorted(vendor.rglob('*')) if p.is_file()]
    files = {}
    for name in names:
        path = vendor / name
        require(path.is_file() and not path.is_symlink(), 'vendored entry is not a regular file: ' + name)
        files[PurePosixPath(name).as_posix()] = path.read_bytes()
    require(files, 'vendored crate directory is empty')
    return files


def unified_patch(manifest, upstream, vendored, changed):
    """The one canonical text form of the local changes."""
    root = '%s-%s' % (manifest['crate'], manifest['version'])
    out = []
    for name in sorted(changed):
        try:
            before = upstream[name].decode('utf-8').splitlines(keepends=True)
            after = vendored[name].decode('utf-8').splitlines(keepends=True)
        except UnicodeDecodeError:
            raise ProvenanceError('a non-text upstream file was changed: ' + name)
        out.extend(difflib.unified_diff(before, after, 'a/%s/%s' % (root, name), 'b/%s/%s' % (root, name), n=3))
        if out and not out[-1].endswith('\n'):
            out[-1] += '\n\\ No newline at end of file\n'
    return ''.join(out)


def audit(manifest_path, vendor, archive_path=None, write_patch=False):
    manifest_path = Path(manifest_path).resolve()
    manifest = load_manifest(manifest_path)
    upstream = upstream_files(manifest, read_archive(manifest, archive_path))
    vendored = vendored_files(vendor)

    missing = sorted(set(upstream) - set(vendored))
    require(not missing, 'upstream files missing from the vendored crate: ' + ', '.join(missing))
    added = sorted(set(vendored) - set(upstream))
    require(added == sorted(manifest['added_files']),
            'files added to the vendored crate are %s, the manifest allows %s' % (added, sorted(manifest['added_files'])))
    changed = sorted(name for name in upstream if upstream[name] != vendored[name])

    patch_path = manifest_path.parent / manifest['patch']
    current = unified_patch(manifest, upstream, vendored, changed)
    if write_patch:
        patch_path.write_text(current)
    require(patch_path.is_file(), 'reviewed patch file not found: ' + str(patch_path))
    reviewed = patch_path.read_text()
    require(current == reviewed,
            'the vendored crate differs from upstream in a way the reviewed patch does not describe; '
            'files that differ from upstream: %s. If the change is intended, regenerate the patch with '
            '--write-patch and have it reviewed.' % (', '.join(changed) or 'none'))
    return {'crate': manifest['crate'], 'version': manifest['version'],
            'archive_sha256': manifest['archive_sha256'], 'upstream_files': len(upstream),
            'added': added, 'changed': changed, 'patch_sha256': sha256(reviewed.encode())}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--manifest', default='rust/orchard_backend/vendor/orchard.provenance.json')
    parser.add_argument('--vendor', default='rust/orchard_backend/vendor/orchard')
    parser.add_argument('--archive', help='use this local copy of the upstream archive instead of downloading it')
    parser.add_argument('--write-patch', action='store_true',
                        help='regenerate the patch file from the current vendored tree (for review)')
    args = parser.parse_args()
    try:
        result = audit(args.manifest, args.vendor, args.archive, args.write_patch)
    except ProvenanceError as error:
        print('Orchard vendor provenance: FAIL: %s' % error, file=sys.stderr)
        return 1
    print('Orchard vendor provenance: PASS')
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
