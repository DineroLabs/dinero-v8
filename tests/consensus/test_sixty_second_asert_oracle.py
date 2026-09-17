#!/usr/bin/env python3
"""Independent arbitrary-precision oracle for both eras of Dinero ASERT.

Historical rows preserve Dinero's deployed polynomial scaling and shift bounds.
Activated rows use the full 16-bit cubic terms, overflow-safe arithmetic and
byte-normalized compact encoding. Anchor/reference-time policy remains Dinero's.
"""
import random
import subprocess
import sys

DISABLED = 2**32 - 1
MASK = 2**256 - 1


def decode(bits):
    size, mantissa = bits >> 24, bits & 0x7fffff
    return (mantissa >> (8 * (3 - size)) if size <= 3
            else mantissa << (8 * (size - 3)))


def encode(value, legacy=False):
    size = (value.bit_length() + 7) // 8
    shift = (value.bit_length() if legacy else size * 8) - 24
    mantissa = value >> shift if shift >= 0 else value << -shift
    if mantissa & 0x800000:
        mantissa >>= 8
        size += 1
    return (size << 24) | mantissa


def oracle(row):
    activation, height, anchor_height, anchor_time, bits, limit_bits, now, spacing, half_life = row
    active = activation != DISABLED and height >= activation
    elapsed = (height - anchor_height) * spacing
    if activation != DISABLED:
        # Count each era's intervals explicitly, independent of the C++ helper.
        old_end = min(height, activation - 1)
        old_count = max(0, old_end - anchor_height)
        new_count = height - anchor_height - old_count
        elapsed = old_count * spacing + new_count * 60
    excess = now - anchor_time - elapsed
    if height == anchor_height or (excess == 0 and not active):
        return bits
    k, r = divmod(excess, half_life)
    k = min(32, max(-32, k))
    target = decode(bits)
    target = target << k if k >= 0 else target >> -k
    if not active:
        target &= MASK
    if r:
        fraction = r * 65536 // half_life
        squared = fraction * fraction if active else fraction * fraction // 65536
        cubed = squared * fraction if active else squared * fraction // 65536
        factor = 65536 + ((195766423245049 * fraction + 971821376 * squared
                          + 5127 * cubed + 2**47) >> 48)
        target *= factor
        if not active:
            target &= MASK
        target >>= 16
    return encode(max(1, min(target, decode(limit_bits))), legacy=not active)


def main():
    rows = []
    rng = random.Random(0x60D1)
    for activation in (1, 1000, DISABLED):
        for height in (1, 999, 1000, 1001, 2000):
            for bits in (0x0900ffff, 0x10123456, 0x1c100000, 0x1d200000, 0x1d31ffce, 0x207fffff):
                for excess in (-1_500_001, -86_400, -43_201, -43_200, -43_199,
                               -1, 0, 1, 43_199, 43_200, 43_201, 1_500_001):
                    ideal = min(height, activation - 1) * 120 + max(0, height - activation + 1) * 60
                    rows.append((activation, height, 0, 1_000_000, bits, 0x207fffff,
                                 1_000_000 + ideal + excess, 120, 43_200))
    for _ in range(5000):
        activation = rng.choice((1, 1000, DISABLED))
        height = rng.randint(1, 2_000_000)
        anchor = rng.randint(0, height - 1)
        bits = encode(rng.getrandbits(rng.randint(64, 255)) or 1)
        limit = rng.choice((0x1d31ffce, 0x207fffff))
        if decode(bits) > decode(limit):
            bits = limit
        rows.append((activation, height, anchor, 1_000_000, bits, limit,
                     1_000_000 + rng.randint(0, 240_000_000), 120, 43_200))
    payload = ''.join(' '.join(map(str, row)) + '\n' for row in rows)
    run = subprocess.run([sys.argv[1]], input=payload, text=True, capture_output=True, check=True)
    actual = [int(line) for line in run.stdout.splitlines()]
    assert len(actual) == len(rows), (len(actual), len(rows))
    for row, got in zip(rows, actual):
        want = oracle(row)
        assert got == want, f"input={row}: actual={got:#x}, independent={want:#x}"
    print(f"PASS {len(rows)} independent big-integer ASERT vectors (historical and activated)")


if __name__ == "__main__":
    main()
