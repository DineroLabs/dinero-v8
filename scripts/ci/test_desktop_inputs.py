import copy
import datetime as dt
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'packaging/snapshot'))
from prepare_desktop_inputs import prepare


class DesktopInputPreparation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.candidate = self.root / 'candidate'; self.candidate.mkdir()
        self.fallbacks = self.root / 'fallbacks'; self.fallbacks.mkdir()
        self.destination = self.root / 'prepared'
        self.now = dt.datetime(2026, 11, 8, tzinfo=dt.timezone.utc)
        entries = []
        for index, height in enumerate((148375, 99677, 65300)):
            version = 5 if index == 0 else 4
            name = f'snapshot-{height}-v{version}.dat'
            header = struct.pack('<II32sIQQQ', 0x4f545855, version, bytes([index + 1]) * 32, height, 0, 0, 0)
            payload = header + b'synthetic identity fixture'
            payload += hashlib.sha256(payload).digest()
            entry = {'filename': name, 'height': height, 'format_version': version,
                     'block_hash': header[8:40][::-1].hex(), 'bytes': len(payload),
                     'sha256': hashlib.sha256(payload).hexdigest()}
            manifest = {'network': 'mainnet', 'snapshot': {k: entry[k] for k in ('height', 'block_hash', 'bytes', 'sha256')}}
            manifest['snapshot']['snapshot_file'] = 'snapshot.dat' if index == 0 else name
            if index == 0:
                manifest['generated_at'] = '20261107T194900Z'
                data, meta = self.candidate / 'snapshot.dat', self.candidate / 'manifest.json'
            else:
                data, meta = self.fallbacks / name, self.fallbacks / (name + '.manifest.json')
            data.write_bytes(payload); meta.write_text(json.dumps(manifest) + '\n')
            entries.append(entry)
        self.catalog = {'schema': 'dinero.desktop-snapshot-inputs.v1', 'primary': entries[0], 'fallbacks': entries[1:]}

    def prepare(self, catalog=None):
        return prepare(self.catalog if catalog is None else catalog, self.candidate,
                       self.fallbacks, self.destination, self.now)

    def test_primary_named_correctly_and_both_old_pairs_preserved(self):
        receipt = self.prepare()
        name = self.catalog['primary']['filename']
        manifest = json.loads((self.destination / (name + '.manifest.json')).read_text())
        self.assertEqual(manifest['snapshot']['snapshot_file'], name)
        self.assertEqual((self.destination / name).read_bytes(), (self.candidate / 'snapshot.dat').read_bytes())
        for entry in self.catalog['fallbacks']:
            for suffix in ('', '.manifest.json'):
                old = entry['filename'] + suffix
                self.assertEqual((self.destination / old).read_bytes(), (self.fallbacks / old).read_bytes())
        self.assertEqual(receipt['fallback_heights'], [99677, 65300])
        self.assertFalse(receipt['native_qualified'])
        self.assertFalse(receipt['published'])

    def test_corrupt_candidate_refused_without_output(self):
        p = self.candidate / 'snapshot.dat'
        data = bytearray(p.read_bytes()); data[69] ^= 1; p.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.prepare()
        self.assertFalse(self.destination.exists())

    def test_missing_recovery_payload_refused(self):
        (self.fallbacks / self.catalog['fallbacks'][1]['filename']).unlink()
        with self.assertRaises(OSError):
            self.prepare()
        self.assertFalse(self.destination.exists())

    def test_wrong_fallback_name_refused(self):
        p = self.fallbacks / (self.catalog['fallbacks'][0]['filename'] + '.manifest.json')
        manifest = json.loads(p.read_text()); manifest['snapshot']['snapshot_file'] = 'other.dat'
        p.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'filename'):
            self.prepare()

    def test_unsafe_or_duplicate_output_names_refused(self):
        for filename in ('../escape.dat', self.catalog['fallbacks'][0]['filename']):
            catalog = copy.deepcopy(self.catalog); catalog['primary']['filename'] = filename
            with self.subTest(filename=filename), self.assertRaisesRegex(ValueError, 'filename'):
                self.prepare(catalog)

    def test_existing_destination_is_retained(self):
        self.destination.mkdir(); marker = self.destination / 'keep'
        marker.write_text('retained')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.prepare()
        self.assertEqual(marker.read_text(), 'retained')

    def test_old_primary_refused_but_undated_fallbacks_allowed(self):
        p = self.candidate / 'manifest.json'
        manifest = json.loads(p.read_text()); manifest['generated_at'] = '20261012T043837Z'
        p.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'seven days'):
            self.prepare()

    def test_internal_corruption_refused_even_when_outer_hash_matches(self):
        p = self.candidate / 'snapshot.dat'
        data = bytearray(p.read_bytes()); data[-1] ^= 1; p.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        self.catalog['primary']['sha256'] = digest
        m = self.candidate / 'manifest.json'; manifest = json.loads(m.read_text())
        manifest['snapshot']['sha256'] = digest; m.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'internal'):
            self.prepare()


if __name__ == '__main__':
    unittest.main()
