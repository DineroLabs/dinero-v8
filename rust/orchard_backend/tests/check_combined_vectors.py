"""Check Rust-generated honest fixtures with independent intent framing.

The Orchard effect is an opaque input, not an independently computed effect
commitment. This script never loads the Rust or C++ library.
"""
from make_transparent_vector import ROOT, blob, scripts, sha, u32, u64


def intent(effect, spending):
    preimage = b'DIN/orchard-v2/tx-sighash/v1\x00\x02'
    preimage += bytes.fromhex('6fb72815ae47a082ff3b0f45246c928888c0d00ef43f232c7ef2ab361c000000')
    preimage += u32(0xa1b2c3d4) + u32(7) + u32(12345) + u32(2)
    first = 96 if spending else 32
    for start, vout, seq, amount, script in [
        (first, 3, 0xfffffffd, 12345, scripts[0]),
        (first + 32, 9, 0xfffffffe, 54321, scripts[1]),
    ]:
        preimage += bytes(range(start, start + 32)) + u32(vout) + u32(seq) + u64(amount) + blob(script)
    preimage += u32(2) + u64(10000) + blob(scripts[0])
    preimage += u64(56500 if spending else 51000) + blob(scripts[1])
    preimage += b'\x01' + u64(666) + b'\x01\x01\x01' + u32(2) + effect
    return sha(preimage)


def check():
    for spending in (False, True):
        prefix = 'combined-spend' if spending else 'combined-shield'
        effect = (ROOT / (prefix + '.effect')).read_bytes()
        saved = (ROOT / (prefix + '.digest')).read_bytes()
        if len(effect) != 32 or len(saved) != 32 or saved != intent(effect, spending):
            raise SystemExit('Combined intent mismatch: ' + prefix)
        bundle = (ROOT / (prefix + '.bundle')).read_bytes()
        balance = int.from_bytes(bundle[14:22], 'little', signed=True)
        if bundle[:9] != b'DNORCH01\x01' or int.from_bytes(bundle[10:14], 'little') != 2:
            raise SystemExit('Combined fixture framing mismatch: ' + prefix)
        if balance != (500 if spending else -5000):
            raise SystemExit('Combined balance mismatch: ' + prefix)
        if spending and bundle[22:54] != (ROOT / 'combined-spend.anchor').read_bytes():
            raise SystemExit('Combined spend anchor mismatch')
        print(prefix, saved.hex())


if __name__ == '__main__':
    check()
