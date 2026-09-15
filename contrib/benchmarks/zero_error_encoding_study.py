"""Offline zero-error encoding study; NOT a transaction format or verifier.

Default tests use synthetic structural fixtures (not valid cryptographic proofs).
With --fixtures, tests and the report use the AuthResourceMeasurements dumps.
Only use local test data. DZE1 is an experiment tag, not an assigned wire version.
"""
import argparse
import hashlib
import json
from pathlib import Path
import unittest

FIXTURE_DIR = None
NAMES = ("unshield.spend-0.bin", "shield.output-0.bin")
MAGIC = b'DZE1'
LIMIT = 1_000_000


def layout(blob, omitted=False):
    if not 0 < len(blob) <= LIMIT:
        raise ValueError('proof length')
    at = 0
    parts = {}
    dims = {}

    def take(n):
        nonlocal at
        if n < 0 or n > len(blob) - at:
            raise ValueError('truncated field')
        value = blob[at:at+n]
        at += n
        return value

    def number(n=8):
        return int.from_bytes(take(n), 'big')

    version = number(1)
    if version not in (4, 6):
        raise ValueError('only current cv output / Auth spend fixtures')
    parts['version'] = 1
    e_start = e_end = e_header = e_count = 0
    for label in ('witness_commitment', 'error_commitment'):
        start = at
        rows, cols, total = number(), number(), number()
        if not (0 < rows <= 4096 and 0 < cols <= 4096 and
                cols & (cols-1) == 0 and total == rows * cols):
            raise ValueError('matrix dimensions')
        dims[label] = {'rows': rows, 'columns': cols, 'total': total}
        if label == 'error_commitment':
            e_header, e_start, e_count = start, at, rows * 33
        if label != 'error_commitment' or not omitted:
            take(rows * 33)
        if label == 'error_commitment':
            e_end = at
        parts[label] = at - start
    take(32)
    parts['circuit_hash'] = 32
    rounds = number()
    if not 0 < rounds <= 32:
        raise ValueError('outer rounds')
    take(rounds * 128)
    parts['outer_sumcheck'] = 8 + rounds * 128
    take(128)
    parts['claims'] = 128
    rounds = number()
    if not 0 < rounds <= 32:
        raise ValueError('inner rounds')
    take(rounds * 96)
    parts['inner_sumcheck'] = 8 + rounds * 96
    for label in ('witness_evaluation', 'error_evaluation'):
        start = at
        take(32)
        rounds = number(4)
        if not 0 < rounds <= 32:
            raise ValueError('IPA rounds')
        take(rounds * 66 + 64)
        parts[label] = at - start
    if at != len(blob):
        raise ValueError('trailing data')
    return {'version': version, 'parts': parts, 'dimensions': dims,
            'error_header': e_header, 'error_start': e_start,
            'error_end': e_end, 'error_point_bytes': e_count}


def pack(blob):
    info = layout(blob)
    start, end = info['error_start'], info['error_end']
    if any(blob[start:end]):
        raise ValueError('nonzero error commitment')
    return MAGIC + blob[:start] + blob[end:]


def unpack(blob):
    if not blob.startswith(MAGIC):
        raise ValueError('experiment tag')
    payload = blob[len(MAGIC):]
    info = layout(payload, omitted=True)
    count, start = info['error_point_bytes'], info['error_start']
    if len(payload) + count > LIMIT:
        raise ValueError('expanded proof length')
    expanded = payload[:start] + bytes(count) + payload[start:]
    layout(expanded)
    return expanded


def fixture_names(directory):
    # Require both standalone measurements; additionally check any transfer
    # fixture dumps from the same opt-in test. Never consume transaction files.
    extra = sorted(path.name for pattern in ('transfer_*.spend-*.bin', 'transfer_*.output-*.bin')
                   for path in directory.glob(pattern))
    return [*NAMES, *extra]


def synthetic_fixture(version):
    """Structurally shaped bytes only; no cryptographic validity claim."""
    def u64(n):
        return n.to_bytes(8, 'big')
    header = u64(2) + u64(2) + u64(4)
    ipa = bytes(32) + (1).to_bytes(4, 'big') + bytes(66 + 64)
    return (bytes([version]) + header + bytes([2]) * 66 + header + bytes(66)
            + bytes(32) + u64(1) + bytes(128) + bytes(128)
            + u64(1) + bytes(96) + ipa + ipa)


class EncodingTests(unittest.TestCase):
    def fixtures(self):
        if FIXTURE_DIR is not None:
            return [(FIXTURE_DIR / name).read_bytes() for name in fixture_names(FIXTURE_DIR)]
        return [synthetic_fixture(v) for v in (4, 6)]

    def test_exact_roundtrip_and_size(self):
        for original in self.fixtures():
            info = layout(original)
            packed = pack(original)
            self.assertEqual(len(packed), len(original) - info['error_point_bytes'] + len(MAGIC))
            self.assertEqual(unpack(packed), original)
            self.assertEqual(pack(unpack(packed)), packed)

    def test_nonzero_error_commitment_cannot_be_discarded(self):
        for original in self.fixtures():
            bad = bytearray(original)
            bad[layout(original)['error_start']] = 2
            with self.assertRaises(ValueError):
                pack(bytes(bad))

    def test_malformed_encoding_rejected(self):
        for original in self.fixtures():
            encoded = pack(original)
            for bad in (b'', encoded[1:], encoded[:-1], encoded+b'\0'):
                with self.assertRaises(ValueError):
                    unpack(bad)

    def test_expansion_dimensions_bounded(self):
        for original in self.fixtures():
            encoded = bytearray(pack(original))
            at = len(MAGIC) + layout(original)['error_header']
            encoded[at:at+8] = (2**64-1).to_bytes(8, 'big')
            with self.assertRaises(ValueError):
                unpack(bytes(encoded))

    def test_unsupported_proof_versions_rejected(self):
        for original in self.fixtures():
            for version in (0, 3, 5, 7, 255):
                with self.assertRaises(ValueError):
                    pack(bytes([version]) + original[1:])

    def test_retained_data_is_never_rewritten(self):
        # This is a byte codec, not cryptographic validation. Altered witness
        # commitments must reach the real verifier, never be silently repaired.
        for original in self.fixtures():
            changed = bytearray(original)
            changed[25] ^= 1  # First witness commitment byte, outside comm_E.
            self.assertEqual(unpack(pack(bytes(changed))), changed)


def report(directory):
    results = []
    for name in fixture_names(directory):
        original = (directory/name).read_bytes()
        info = layout(original)
        encoded = pack(original)
        expanded = unpack(encoded)
        assert expanded == original
        (directory/(name+'.dzer')).write_bytes(encoded)
        results.append({'fixture':name, 'bytes':len(original), 'encoded_bytes':len(encoded),
            'saved_bytes':len(original)-len(encoded),
            'saved_percent':round(100*(len(original)-len(encoded))/len(original),2),
            'sha256':hashlib.sha256(original).hexdigest(),
            'expanded_sha256':hashlib.sha256(expanded).hexdigest(), **info})
    (directory/'zero-error-results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path,
                        help='directory containing real Auth resource proof dumps')
    parser.add_argument('--report', action='store_true',
                        help='write measured JSON and experimental .dzer files into fixture directory')
    args = parser.parse_args()
    FIXTURE_DIR = args.fixtures
    if args.report and FIXTURE_DIR is None:
        parser.error('--report requires --fixtures; synthetic fixtures are not measurements')
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(EncodingTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.report:
        report(FIXTURE_DIR)
