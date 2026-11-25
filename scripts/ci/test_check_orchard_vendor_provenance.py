#!/usr/bin/env python3
"""The provenance check must refuse every way the vendored crate can drift."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from check_orchard_vendor_provenance import ProvenanceError, audit

UPSTREAM = {
    'Cargo.toml': b'[package]\nname = "orchard"\n',
    'src/lib.rs': b'pub mod builder;\n',
    'src/builder.rs': b'fn build() {}\n',
    'tests/data.bin': bytes(range(256)),
}


def archive(files, root='orchard-9.9.9', extra=None):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w:gz') as tar:
        for name, content in sorted(files.items()):
            info = tarfile.TarInfo('%s/%s' % (root, name))
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
        for info in extra or []:
            tar.addfile(info)
    return data.getvalue()


class Fixture:
    """An upstream archive, a vendored tree with one reviewed change and one
    added note, and the manifest and patch that describe exactly that."""

    def __init__(self, directory):
        self.root = Path(directory)
        self.vendor = self.root / 'vendor' / 'orchard'
        self.archive = self.root / 'orchard-9.9.9.crate'
        self.manifest = self.root / 'vendor' / 'orchard.provenance.json'
        self.patch = self.root / 'vendor' / 'orchard.local.patch'
        data = archive(UPSTREAM)
        self.archive.write_bytes(data)
        for name, content in UPSTREAM.items():
            self.write(name, content)
        self.write('src/builder.rs', b'fn build() {}\nfn build_saved() {}\n')
        self.write('UPSTREAM.md', b'notes\n')
        self.manifest.write_text(json.dumps({
            'crate': 'orchard', 'version': '9.9.9', 'archive_url': 'file:///not-used',
            'archive_sha256': hashlib.sha256(data).hexdigest(),
            'added_files': ['UPSTREAM.md'], 'patch': 'orchard.local.patch'}))
        audit(self.manifest, self.vendor, self.archive, write_patch=True)

    def write(self, name, content):
        path = self.vendor / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def check(self):
        return audit(self.manifest, self.vendor, self.archive)


class OrchardVendorProvenance(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.f = Fixture(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def refused(self, needle):
        with self.assertRaises(ProvenanceError) as caught:
            self.f.check()
        self.assertIn(needle, str(caught.exception))

    def test_upstream_plus_the_reviewed_patch_passes(self):
        result = self.f.check()
        self.assertEqual(result['changed'], ['src/builder.rs'])
        self.assertEqual(result['added'], ['UPSTREAM.md'])
        self.assertEqual(result['upstream_files'], len(UPSTREAM))
        patch = self.f.patch.read_text()
        self.assertIn('+fn build_saved() {}', patch)
        self.assertNotIn('lib.rs', patch)

    def test_one_changed_byte_in_any_other_file_is_refused(self):
        self.f.write('src/lib.rs', b'pub mod builder; \n')
        self.refused('src/lib.rs')

    def test_a_further_change_to_the_patched_file_is_refused(self):
        self.f.write('src/builder.rs', b'fn build() {}\nfn build_saved() {}\nfn extra() {}\n')
        self.refused('reviewed patch does not describe')

    def test_reverting_the_patched_file_without_updating_the_patch_is_refused(self):
        self.f.write('src/builder.rs', UPSTREAM['src/builder.rs'])
        self.refused('reviewed patch does not describe')

    def test_an_added_file_the_manifest_does_not_name_is_refused(self):
        self.f.write('src/backdoor.rs', b'fn x() {}\n')
        self.refused('src/backdoor.rs')

    def test_a_missing_upstream_file_is_refused(self):
        (self.f.vendor / 'src' / 'lib.rs').unlink()
        self.refused('missing from the vendored crate: src/lib.rs')

    def test_a_missing_allowed_addition_is_refused(self):
        (self.f.vendor / 'UPSTREAM.md').unlink()
        self.refused('the manifest allows')

    def test_a_changed_binary_file_is_refused(self):
        self.f.write('tests/data.bin', bytes(reversed(range(256))))
        self.refused('non-text upstream file was changed: tests/data.bin')

    def test_another_archive_is_refused(self):
        other = dict(UPSTREAM)
        other['src/lib.rs'] = b'pub mod builder; // not what was reviewed\n'
        self.f.archive.write_bytes(archive(other))
        self.refused('upstream archive SHA-256')

    def test_an_edited_patch_file_is_refused(self):
        self.f.patch.write_text(self.f.patch.read_text().replace('build_saved', 'build_other'))
        self.refused('reviewed patch does not describe')

    def test_an_archive_that_escapes_its_directory_is_refused(self):
        info = tarfile.TarInfo('orchard-9.9.9/../outside.rs')
        info.size = 0
        data = archive(UPSTREAM, extra=[info])
        self.f.archive.write_bytes(data)
        manifest = json.loads(self.f.manifest.read_text())
        manifest['archive_sha256'] = hashlib.sha256(data).hexdigest()
        self.f.manifest.write_text(json.dumps(manifest))
        self.refused('outside orchard-9.9.9/')

    def test_a_link_in_the_archive_is_refused(self):
        info = tarfile.TarInfo('orchard-9.9.9/src/link.rs')
        info.type = tarfile.SYMTYPE
        info.linkname = '/etc/passwd'
        data = archive(UPSTREAM, extra=[info])
        self.f.archive.write_bytes(data)
        manifest = json.loads(self.f.manifest.read_text())
        manifest['archive_sha256'] = hashlib.sha256(data).hexdigest()
        self.f.manifest.write_text(json.dumps(manifest))
        self.refused('not a regular file')


if __name__ == '__main__':
    unittest.main()
