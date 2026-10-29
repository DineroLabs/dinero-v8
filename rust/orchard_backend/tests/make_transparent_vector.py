"""Independent draft transparent signing vectors; public synthetic keys only.

Reads the existing Orchard effect fixture as opaque data. This is independent
framing/hash evidence, not an independent implementation of that effect hash.
"""
import argparse
import hashlib
from pathlib import Path
import struct

ROOT = Path(__file__).parent / 'fixtures'
u32 = lambda n: struct.pack('<I', n)
u64 = lambda n: struct.pack('<Q', n)
blob = lambda b: u32(len(b)) + b
sha = lambda b: hashlib.sha256(b).digest()
gx = bytes.fromhex('79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798')
key2 = bytes.fromhex('02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5')
scripts = [b'\x51\x20' + gx, b'\x00\x14' + hashlib.new('ripemd160', sha(key2)).digest()]

def intent():
    preimage = b'DIN/orchard-v2/tx-sighash/v1\x00\x02'
    preimage += bytes.fromhex('6fb72815ae47a082ff3b0f45246c928888c0d00ef43f232c7ef2ab361c000000')
    preimage += u32(0xa1b2c3d4) + u32(7) + u32(12345) + u32(2)
    for start, vout, seq, amount, script in [
        (32, 3, 0xfffffffd, 12345, scripts[0]), (64, 9, 0xfffffffe, 54321, scripts[1])
    ]:
        preimage += bytes(range(start, start + 32)) + u32(vout) + u32(seq) + u64(amount) + blob(script)
    preimage += u32(2) + u64(10000) + blob(bytes.fromhex('76a91411'))
    preimage += u64(56000) + blob(bytes.fromhex('51202233'))
    preimage += b'\x01' + u64(666) + b'\x01\x01\x01' + u32(2)
    preimage += (ROOT / 'candidate-spend.effect').read_bytes()
    return sha(preimage)

def vectors():
    d = intent()
    tag = sha(b'DIN/orchard-v2/transparent-sighash/v1')
    return {'candidate-transparent.intent': d,
            'candidate-transparent.taproot': sha(tag + tag + d + b'\x01\x01' + u32(0)),
            'candidate-transparent.p2wpkh': sha(tag + tag + d + b'\x01\x00' + u32(1))}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for name, data in vectors().items():
        path = ROOT / name
        if args.check:
            if path.read_bytes() != data:
                raise SystemExit('Vector mismatch: ' + name)
        else:
            path.write_bytes(data)
        print(name, data.hex())
