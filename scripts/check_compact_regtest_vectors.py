#!/usr/bin/env python3
"""Independent byte oracle for saved compact-regtest transactions.

Only the standard library and the existing independent Python Poseidon oracle
are used. This does not verify Spartan proofs or replace consensus validation.
Normal operation compares saved literals; --record is an explicit maintenance
operation, never run by CI. Proofs are randomized once and then kept unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import copy
import unittest

from generate_shielded_protocol_vectors import poseidon

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/vectors/compact_regtest_v1'
PROFILES = {
    4: (24610, (557, 1024, 570368), (563, 1024, 576512),
        'e6ff2a5c44171cd1065bf06b6ce35f1ca51a92dad2bef1c3d3d9bd227bbeec0c'),
    6: (39592, (1011, 1024, 1035264), (1021, 1024, 1045504),
        'a0a6b973fd134cdcb844fa8929ed1f4a6135c9ba8417cd2976055905b99c54c1'),
}
SHAPES = {'shield': (0, 1), 'transfer': (1, 2), 'unshield': (1, 0), 'maximum': (4, 2)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).digest()


def size(n):
    if n < 253:
        return bytes([n])
    if n <= 0xffff:
        return b'\xfd' + struct.pack('<H', n)
    if n <= 0xffffffff:
        return b'\xfe' + struct.pack('<I', n)
    return b'\xff' + struct.pack('<Q', n)


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        require(0 <= n <= len(self.data) - self.at, 'truncated fixture')
        out = self.data[self.at:self.at + n]
        self.at += n
        return out

    def uint(self, n):
        return int.from_bytes(self.take(n), 'little')

    def size(self):
        first = self.uint(1)
        if first < 253:
            return first
        n = self.uint({253: 2, 254: 4, 255: 8}[first])
        require(n >= {253: 253, 254: 65536, 255: 4294967296}[first],
                'nonminimal CompactSize')
        return n

    def blob(self):
        return self.take(self.size())

    def done(self):
        require(self.at == len(self.data), 'trailing bytes')


def expand(proof, profile):
    length, witness, error, circuit_hash = PROFILES[profile]
    require(len(proof) == length, 'wrong fixed proof length')
    require(proof[:5] == b'DZE1' + bytes([profile]), 'wrong compact profile')
    payload = proof[4:]
    require(struct.unpack('>QQQ', payload[1:25]) == witness, 'wrong witness dimensions')
    offset = 25 + 33 * witness[0]
    require(struct.unpack('>QQQ', payload[offset:offset + 24]) == error,
            'wrong error dimensions')
    offset += 24
    require(payload[offset:offset + 32].hex() == circuit_hash, 'wrong circuit hash')
    return payload[:offset] + bytes(33 * error[0]) + payload[offset:]


def bundle_fields(wire):
    r = Reader(wire)
    balance_bytes = r.take(8)
    expanded = bytearray(balance_bytes)
    spends, outputs, receipts = [], [], []
    for profile, fields in ((6, spends), (4, outputs)):
        count = r.size()
        require(count <= (4 if profile == 6 else 2), 'unsupported fixture shape')
        expanded += size(count)
        for index in range(count):
            item = {}
            if profile == 6:
                item['nullifier'], item['anchor'] = r.take(32), r.take(32)
                prefix = item['nullifier'] + item['anchor']
            else:
                item['commitment'] = r.take(32)
                prefix = item['commitment']
            item['cv'] = r.take(33)
            expanded += prefix + item['cv']
            if profile == 4:
                note = r.blob()
                expanded += size(len(note)) + note
            compact = r.blob()
            ordinary = expand(compact, profile)
            expanded += size(len(ordinary)) + ordinary
            fields.append(item)
            receipts.append({'profile': profile, 'index': index,
                             'compact_sha256': sha(compact).hex(),
                             'expanded_sha256': sha(ordinary).hex(),
                             'compact_bytes': len(compact), 'expanded_bytes': len(ordinary),
                             'circuit_hash': PROFILES[profile][3]})
    range_proof, bvk, signature = r.blob(), r.take(33), r.take(64)
    expanded += size(len(range_proof)) + range_proof + bvk + signature
    r.done()
    return (int.from_bytes(balance_bytes, 'little', signed=True), spends, outputs,
            bytes(expanded), receipts)


def tree_root(leaves):
    empty = [poseidon(bytes(32), bytes(32))]
    for _ in range(32):
        empty.append(poseidon(empty[-1], empty[-1]))
    frontier = [bytes(32) for _ in range(32)]
    for index, leaf in enumerate(leaves):
        current = leaf
        for depth in range(32):
            if not ((index >> depth) & 1):
                frontier[depth] = current
                break
            current = poseidon(frontier[depth], current)
    current = empty[0]
    for depth in range(32):
        current = (poseidon(frontier[depth], current) if (len(leaves) >> depth) & 1
                   else poseidon(current, empty[depth]))
    return current


def leaf(txid, index, amount, script, height):
    preimage = (b'DINERO-UTXO-LEAF-v2' + bytes.fromhex(txid)[::-1] +
                struct.pack('<IQ', index, amount) + size(len(script)) + script +
                struct.pack('<I', height) + b'\x00')
    return sha(preimage)


def inspect(raw, height, initial_commitments):
    r = Reader(raw)
    version = r.take(4)
    require(int.from_bytes(version, 'little') == 0x40000006, 'wrong transaction version')
    require(r.take(2) == b'\x00\x01', 'missing witness marker')
    start = r.at
    inputs = []
    for _ in range(r.size()):
        txid, index = r.take(32), r.take(4)
        r.blob()  # scriptSig is in txid, not the shielded signing preimage.
        sequence = r.take(4)
        inputs.append(txid + index + sequence)
    outputs = []
    for _ in range(r.size()):
        amount, script = r.uint(8), r.blob()
        require(amount > 0, 'fixture excludes ambiguous zero-value output encoding')
        outputs.append((amount, script))
    require(r.uint(1) == 1, 'missing explicit fee')
    fee = r.uint(8)
    envelope = raw[start:r.at]
    for _ in inputs:
        for _ in range(r.size()):
            r.blob()
    bundle, locktime = r.blob(), r.take(4)
    r.done()
    balance, spends, notes, expanded, proofs = bundle_fields(bundle)
    base = version + envelope + size(len(bundle)) + bundle + locktime
    expanded_base = version + envelope + size(len(expanded)) + expanded + locktime
    txid = sha(sha(base))[::-1].hex()
    sighash_preimage = (b'DIN/v7/shielded/tx-sighash/v1' + version +
                       struct.pack('<I', len(inputs)) + b''.join(inputs) +
                       struct.pack('<I', len(outputs)))
    for amount, script in outputs:
        sighash_preimage += struct.pack('<QI', amount, len(script)) + script
    sighash_preimage += locktime
    tx_sighash = sha(sighash_preimage)
    binding_preimage = (b'DIN/v7/shielded/binding/v1' + struct.pack('<q', balance) +
                        tx_sighash + struct.pack('<Q', len(spends)) +
                        b''.join(sorted(s['cv'] for s in spends)) +
                        struct.pack('<Q', len(notes)) + b''.join(sorted(o['cv'] for o in notes)))
    before = [bytes.fromhex(cm) for cm in initial_commitments]
    root_before = tree_root(before)
    require(all(s['anchor'] == root_before for s in spends), 'fixture anchor differs from prestate')
    weight = 3 * len(base) + len(raw)
    expected = {
        'txid': txid, 'wtxid': sha(sha(raw))[::-1].hex(), 'wire_sha256': sha(raw).hex(),
        'wire_bytes': len(raw), 'base_bytes': len(base), 'weight': weight,
        'vsize': (weight + 3) // 4, 'fee_una': fee, 'value_balance': balance,
        'spends': len(spends), 'outputs': len(notes), 'proofs': proofs,
        'expanded_view_txid': sha(sha(expanded_base))[::-1].hex(),
        'expanded_bundle_sha256': sha(expanded).hex(),
        'tx_sighash_preimage': sighash_preimage.hex(), 'tx_sighash': tx_sighash.hex(),
        'binding_sighash_preimage': binding_preimage.hex(),
        'binding_sighash': sha(binding_preimage).hex(),
        'root_before': root_before.hex(),
        'root_after': tree_root(before + [o['commitment'] for o in notes]).hex(),
        'nullifiers': [s['nullifier'].hex() for s in spends],
        'commitments': [o['commitment'].hex() for o in notes],
        'utreexo_leaves': [leaf(txid, i, amount, script, height).hex()
                           for i, (amount, script) in enumerate(outputs)],
    }
    require(expected['expanded_view_txid'] != txid, 'expanded view reused original identity')
    return expected, expanded


def verify_captured_leaf(case):
    if 'utreexo_proof' not in case:
        return
    proof = case['utreexo_proof']
    current = bytes.fromhex(case['expected']['utreexo_leaves'][0])
    position = proof['position']
    require(position < proof['num_leaves'], 'out-of-range position')
    for sibling in proof['siblings']:
        sibling = bytes.fromhex(sibling)
        current = sha(b'DINERO-UTREEXO-NODE-v1' +
                      (sibling + current if position & 1 else current + sibling))
        position >>= 1
    require(current.hex() in case['utreexo_roots'], 'captured leaf proof mismatch')


def check_literals(case, expected, expanded, saved_expanded):
    require((expected['spends'], expected['outputs']) == SHAPES[case['name']],
            'fixture shape changed')
    require(expected == case['expected'], case['name'] + ': literal mismatch')
    require(expanded == saved_expanded, case['name'] + ': expansion differs from fixed bytes')
    verify_captured_leaf(case)


def self_test(directory, manifest):
    class OracleNegativeControls(unittest.TestCase):
        def test_compact_size_requires_minimal_encoding(self):
            for encoded in (b'\xfd\xfc\x00', b'\xfe\xff\xff\x00\x00', b'\xff' + bytes(8)):
                with self.subTest(encoded=encoded.hex()), self.assertRaisesRegex(ValueError, 'nonminimal'):
                    Reader(encoded).size()

        def test_truncated_lengths_are_bounded(self):
            for encoded in (b'\xfd', b'\xfe\x00', b'\xff' + b'\xff'*8):
                with self.subTest(encoded=encoded.hex()), self.assertRaisesRegex(ValueError, 'truncated'):
                    Reader(encoded).blob()

        def test_profile_dimensions_and_lengths_fail_before_expansion(self):
            # Synthetic bytes exercise layout checks only; no claim of proof validity.
            for profile, (length, witness, error, circuit_hash) in PROFILES.items():
                proof = bytearray(length)
                proof[:5] = b'DZE1' + bytes([profile])
                proof[5:29] = struct.pack('>QQQ', *witness)
                offset = 4 + 25 + 33*witness[0]
                proof[offset:offset+24] = struct.pack('>QQQ', *error)
                proof[offset+24:offset+56] = bytes.fromhex(circuit_hash)
                self.assertEqual(len(expand(proof, profile)), length - 4 + 33*error[0])
                for malformed in (proof[:-1], proof + b'\x00'):
                    with self.assertRaisesRegex(ValueError, 'length'):
                        expand(malformed, profile)
                for at in (4, 5, offset, offset+24):
                    malformed = bytearray(proof); malformed[at] ^= 1
                    with self.assertRaises(ValueError):
                        expand(malformed, profile)

        def test_transaction_truncation_and_trailing_bytes(self):
            case = manifest['cases'][0]
            raw = bytes.fromhex((directory / case['wire_file']).read_text())
            for malformed in (raw[:-1], raw + b'\x00'):
                with self.assertRaises(ValueError):
                    inspect(malformed, case['height'], case['initial_commitments'])

        def test_changed_identity_expansion_and_shape_are_detected(self):
            case = manifest['cases'][0]
            expected = case['expected']
            expanded = bytes.fromhex((directory / case['expanded_bundle_file']).read_text())
            changed = copy.deepcopy(case); changed['expected']['txid'] = '00'*32
            with self.assertRaisesRegex(ValueError, 'literal mismatch'):
                check_literals(changed, expected, expanded, expanded)
            with self.assertRaisesRegex(ValueError, 'expansion differs'):
                check_literals(case, expected, expanded, expanded[:-1])
            wrong = copy.deepcopy(expected); wrong['outputs'] = 0
            with self.assertRaisesRegex(ValueError, 'shape changed'):
                check_literals(case, wrong, expanded, expanded)

        def test_changed_utreexo_leaf_is_detected(self):
            case = copy.deepcopy(next(c for c in manifest['cases'] if c['name'] == 'unshield'))
            case['expected']['utreexo_leaves'][0] = '00'*32
            with self.assertRaisesRegex(ValueError, 'leaf proof mismatch'):
                verify_captured_leaf(case)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(OracleNegativeControls))
    require(result.wasSuccessful(), 'oracle negative controls failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--directory', type=Path, default=FIXTURES)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    path = args.directory / 'manifest.json'
    manifest = json.loads(path.read_text())
    require(manifest['format'] == 'dinero-compact-regtest-fixed-v1', 'wrong manifest format')
    require(len(manifest['cases']) == 4 and {c['name'] for c in manifest['cases']} == set(SHAPES),
            'missing, duplicate or unexpected shape')
    for case in manifest['cases']:
        raw = bytes.fromhex((args.directory / case['wire_file']).read_text())
        expected, expanded = inspect(raw, case['height'], case['initial_commitments'])
        require((expected['spends'], expected['outputs']) == SHAPES[case['name']], 'fixture shape changed')
        expanded_path = args.directory / case['expanded_bundle_file']
        if args.record:
            case['expected'] = expected
            expanded_path.write_text(expanded.hex() + '\n')
        else:
            check_literals(case, expected, expanded, bytes.fromhex(expanded_path.read_text()))
        verify_captured_leaf(case)
        print(f"PASS {case['name']}: {expected['wire_bytes']} bytes, "
              f"{expected['spends']} spends/{expected['outputs']} outputs")
    if args.record:
        path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    if args.self_test:
        self_test(args.directory, manifest)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
