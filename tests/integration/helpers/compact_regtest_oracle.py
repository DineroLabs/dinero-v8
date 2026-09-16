#!/usr/bin/env python3
"""Independent byte/leaf oracle for locally generated compact-regtest fixtures.

No daemon or crypto-library imports. This is an assertion tool for trusted test
outputs, not an untrusted-proof decoder or a second consensus implementation.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path


def sha(data):
    return hashlib.sha256(data).digest()


def compact_size(n):
    if n < 253:
        return bytes([n])
    if n <= 0xFFFF:
        return b'\xfd' + struct.pack('<H', n)
    if n <= 0xFFFFFFFF:
        return b'\xfe' + struct.pack('<I', n)
    return b'\xff' + struct.pack('<Q', n)


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        assert 0 <= n <= len(self.data) - self.at, 'truncated fixture'
        result = self.data[self.at:self.at + n]
        self.at += n
        return result

    def size(self):
        first = self.take(1)[0]
        if first < 253:
            return first
        n = int.from_bytes(self.take({253: 2, 254: 4, 255: 8}[first]), 'little')
        assert n >= {253: 253, 254: 65536, 255: 4294967296}[first]
        return n

    def blob(self):
        return self.take(self.size())


def expand_fixture(proof, profile):
    assert proof[:5] == b'DZE1' + bytes([profile])
    payload = proof[4:]
    witness_rows = int.from_bytes(payload[1:9], 'big')
    assert 0 < witness_rows <= 4096
    dims = 1 + 24 + 33 * witness_rows
    error_rows = int.from_bytes(payload[dims:dims + 8], 'big')
    assert 0 < error_rows <= 4096
    offset = dims + 24
    assert offset < len(payload)
    return payload[:offset] + bytes(33 * error_rows) + payload[offset:]


def expanded_bundle(wire):
    r = Reader(wire)
    out = bytearray(r.take(8))
    for profile, prefix in ((6, 97), (4, 65)):
        count = r.size()
        assert count <= (4 if profile == 6 else 2)
        out += compact_size(count)
        for _ in range(count):
            out += r.take(prefix)
            if profile == 4:
                note = r.blob()
                out += compact_size(len(note)) + note
            proof = expand_fixture(r.blob(), profile)
            out += compact_size(len(proof)) + proof
    # Range proof, binding key and signature retain their exact bytes.
    out += r.take(len(wire) - r.at)
    return bytes(out)


def inspect(raw, expected_txid):
    r = Reader(raw)
    version = r.take(4)
    assert int.from_bytes(version, 'little') == 0x40000006
    assert r.take(2) == b'\x00\x01'
    start = r.at
    inputs = r.size()
    for _ in range(inputs):
        r.take(36)
        r.blob()
        r.take(4)
    outputs = []
    for _ in range(r.size()):
        amount = int.from_bytes(r.take(8), 'little')
        script = r.blob()
        assert amount > 0, 'fixture only creates nonzero transparent outputs'
        outputs.append({'amount_una': amount, 'script': script.hex()})
    assert r.take(1) == b'\x01'
    fee = int.from_bytes(r.take(8), 'little')
    envelope = raw[start:r.at]
    for _ in range(inputs):
        for _ in range(r.size()):
            r.blob()
    bundle = r.blob()
    locktime = r.take(4)
    assert r.at == len(raw)
    base = version + envelope + compact_size(len(bundle)) + bundle + locktime
    txid = sha(sha(base))[::-1].hex()
    assert txid == expected_txid, (txid, expected_txid)
    expanded = expanded_bundle(bundle)
    expanded_base = version + envelope + compact_size(len(expanded)) + expanded + locktime
    expanded_id = sha(sha(expanded_base))[::-1].hex()
    assert expanded_id != txid
    weight = 3 * len(base) + len(raw)
    return {'txid': txid, 'expanded_view_txid': expanded_id,
            'wtxid': sha(sha(raw))[::-1].hex(), 'wire_bytes': len(raw),
            'base_bytes': len(base), 'vsize': (weight + 3) // 4,
            'fee_una': fee, 'outputs': outputs}


def leaf(txid, vout, amount, script, height):
    payload = (b'DINERO-UTXO-LEAF-v2' + bytes.fromhex(txid)[::-1]
               + struct.pack('<IQ', vout, amount) + compact_size(len(script))
               + script + struct.pack('<I', height) + b'\x00')
    return sha(payload)


def verifies(value, proof, roots):
    position = proof['position']
    assert position < proof['num_leaves']
    for encoded in proof['siblings']:
        sibling = bytes.fromhex(encoded)
        pair = sibling + value if position % 2 else value + sibling
        value = sha(b'DINERO-UTREEXO-NODE-v1' + pair)
        position //= 2
    return value.hex() in roots


def prove(record, proof_response, roots_response, height):
    proof = proof_response['result']['proofs'][0]['proof']
    roots = roots_response['result']['roots']
    output = record['outputs'][0]
    amount, script = output['amount_una'], bytes.fromhex(output['script'])
    original = leaf(record['txid'], 0, amount, script, height)
    assert verifies(original, proof, roots), 'compact leaf does not match captured roots'
    alternatives = [
        leaf(record['expanded_view_txid'], 0, amount, script, height),
        leaf(record['txid'], 1, amount, script, height),
        leaf(record['txid'], 0, amount + 1, script, height),
        leaf(record['txid'], 0, amount, script + b'\x00', height),
        leaf(record['txid'], 0, amount, script, height + 1),
    ]
    assert all(not verifies(value, proof, roots) for value in alternatives)
    return {'leaf': original.hex(), 'height': height, 'historical_proof_valid': True,
            'wrong_txid_vout_amount_script_height_rejected': True}


if __name__ == '__main__':
    if sys.argv[1] == 'inspect':
        result = inspect(bytes.fromhex(Path(sys.argv[2]).read_text().strip()), sys.argv[3])
    elif sys.argv[1] == 'prove':
        result = prove(*(json.loads(Path(p).read_text()) for p in sys.argv[2:5]), int(sys.argv[5]))
    else:
        raise SystemExit('expected inspect or prove')
    print(json.dumps(result, sort_keys=True))
