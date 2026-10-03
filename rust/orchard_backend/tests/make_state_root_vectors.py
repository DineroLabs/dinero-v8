"""Independent DNORST01 framing and logical-set digest vectors.
Uses public synthetic bundle nullifier fields and an opaque saved tree root.
Does not compute Orchard curve/tree hashes, load native libraries, or contact nodes.
"""
from pathlib import Path
import argparse
import hashlib
import struct
ROOT = Path(__file__).parent / 'fixtures'
u32 = lambda n: struct.pack('<I', n)
u64 = lambda n: struct.pack('<Q', n)
h = lambda n: bytes([n]) + bytes(31)
sha = lambda b: hashlib.sha256(b).digest()

def vectors():
    bundle = (ROOT / 'combined-shield.bundle').read_bytes()
    assert bundle[:9] == b'DNORCH01\x01' and int.from_bytes(bundle[10:14], 'little') == 2
    nullifiers = sorted(bundle[54 + 884*i + 32:54 + 884*i + 64] for i in range(2))
    assert len(set(nullifiers)) == 2 and all(len(n) == 32 for n in nullifiers)
    nf = sha(b'ONF1\x01' + b''.join(nullifiers) + u64(2))
    anchor = (ROOT / 'combined-spend.anchor').read_bytes()
    assert len(anchor) == 32
    result = {'state-set.nullifiers': nf, 'state-empty.nullifiers': sha(b'ONF1\x01' + u64(0))}
    for height, parent, refs, name in [(20001, 1, 1, 'funded'), (20002, 2, 2, 'empty-child')]:
        anchors = sha(b'OAN1\x01' + anchor + u64(refs) + u64(1) + u64(refs))
        pre = b'DNORST01' + u32(7) + b'\x01\x01\x01' + u32(5)
        pre += b'\x02' + bytes.fromhex('6fb72815ae47a082ff3b0f45246c928888c0d00ef43f232c7ef2ab361c000000')
        pre += u32(0xa1b2c3d4) + u32(20001) + u32(3) + h(1) + h(55) + u64(37)
        pre += h(90) + u64(2) + u64(2)
        pre += u32(height) + h(parent) + anchor + u64(2) + u64(5000)
        pre += nf + u64(2) + anchors + u64(1) + u64(refs)
        result['state-'+name+'.preimage'] = pre
        result['state-'+name+'.root'] = sha(pre)
        result['state-'+name+'.anchors'] = anchors
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for name, data in vectors().items():
        path = ROOT / name
        if args.check:
            if path.read_bytes() != data: raise SystemExit('State vector mismatch: '+name)
        else: path.write_bytes(data)
        print(name, len(data), sha(data).hex())
