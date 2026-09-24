"""Independent draft wire/identity vector from public synthetic bundle bytes.
Does not generate proofs, access wallets, or contact a node.
"""
from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent / 'fixtures'
u32 = lambda n: struct.pack('<I', n)
u64 = lambda n: struct.pack('<Q', n)
def blob(data):
    return u32(len(data)) + data

def envelope(with_witness):
    payload = u32(2)
    for start, vout, seq in [(32, 3, 0xfffffffd), (64, 9, 0xfffffffe)]:
        payload += bytes(range(start, start + 32)) + u32(vout) + blob(b'\x51') + u32(seq)
        payload += (u32(2) + blob(b'\x12\x34') + blob(b'')) if with_witness else u32(0)
    payload += u32(2) + u64(10000) + blob(bytes.fromhex('76a91411'))
    payload += u64(56000) + blob(bytes.fromhex('51202233'))
    payload += b'\x01' + u64(666) + blob((ROOT / 'candidate-spend.bundle').read_bytes()) + u32(12345)
    return u32(7) + b'\0\0DNORCHTX\x01' + u32(len(payload)) + payload

def identity(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    vectors = {'candidate-envelope.bin': envelope(True),
               'candidate-envelope.txid': identity(envelope(False)),
               'candidate-envelope.wtxid': identity(envelope(True))}
    for name, data in vectors.items():
        path = ROOT / name
        if args.check:
            if path.read_bytes() != data:
                raise SystemExit(f'Vector mismatch: {name}')
        else:
            path.write_bytes(data)
        print(name, len(data), hashlib.sha256(data).hexdigest())
