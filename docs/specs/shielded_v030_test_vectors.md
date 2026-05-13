# Shielded Pool — v0.3.0 Wire-Format Canonical Vectors

**Status:** **LIVE** as of 2026-04-27. Pins live in
`src/test/v030_wire_vectors_tests.cpp`. Suite: `ShieldedV030Vectors`
(`ctest -R ShieldedV030Vectors`).

**Scope:** the v0.3.0 bundle wire format introduced when Path C
(Pedersen + Schnorr binding sig) shipped. Specifically pins:

- `kPedersenVDST` and the derived V generator x-coordinate
- Per-scenario `Nullifier(sk, leaf_index)` hex
- Per-scenario `NoteCommitment(d=0, pk, value_hash, randomness)` hex
- Bundle-level `value_balance` integer
- Round-trip parity through `SerializeShieldedBundle` ↔
  `DeserializeShieldedBundle`
- `ValidateShieldedBundle == Ok` against the full validator
  (including `pedersen_verify_tally` + BIP340 Schnorr verify)

**Out of scope** (intentionally NOT pinned):

- Spartan ZK proof bytes (zero-knowledge requires fresh randomness)
- `cv` / `bvk_commitment` x-coords — these depend on `rcv` values
  which the test supplies deterministically, but compressed-point
  parity is a libsecp implementation detail not worth chain-splitting
  on. The validator's `pedersen_verify_tally` exercises the algebra.

A second implementation that reproduces the deterministic public
outputs in §2 below is byte-compatible at the consensus layer.

---

## 1. Test inputs

All scenarios use diversifier `d = Hash{}` (32 zero bytes), reflecting
the Phase 5-parked diversifier scheme. Hash inputs use the helper
`MakeHash(seed, tail)`: `h[0] = seed`, `h[31] = tail`, all other
bytes 0. Values are encoded big-endian into the low 8 bytes of the
hash via `ValueAsHash(v)`.

Per-scenario input table:

| Scenario | sk seed/tail | randomness | value (una) | tx_sighash |
|---|---|---|---|---|
| Shield      | 0x10/0xF0 | 0x11/0xF0 | 100_000_000 | 0x12/0xF0 |
| Unshield    | 0x20/0xF1 | 0x21/0xF1 | 100_000_000 | 0x22/0xF1 |
| Transfer-3d in  | 0x30/0xF2 | 0x31/0xF2 | 100_000_000 | 0x34/0xF2 |
| Transfer-3d out | 0x32/0xF2 | 0x33/0xF2 | 99_990_000  | (same)    |
| Transfer-3e A in    | 0x40/0xF3 | 0x41/0xF3 | 50_000_000 | 0x50/0xF3 |
| Transfer-3e B in    | 0x42/0xF3 | 0x43/0xF3 | 50_000_000 | (same)    |
| Transfer-3e recipient | 0x48/0xF3 | 0x49/0xF3 | 70_000_000 | (same) |
| Transfer-3e change    | 0x4C/0xF3 | 0x4D/0xF3 | 29_980_000 | (same) |

`fee_una`: Transfer-3d uses 10_000; Transfer-3e uses 20_000.
`leaf_index` per spend is assigned by `tree.Append(commitment)` in
the test (0 for the first spend in each scenario).

---

## 2. Pinned outputs

### 2.1 Pedersen generator V

```
kPedersenVDST   = "DIN/v7/shielded/cv/V/v1"
PedersenGeneratorV() x-coord (hex):
    347ff26d5f650e9f4b3af12a66dba9ebeee1996ffac6a964f98a24dab522b1d4
```

ANY divergence between independent implementations here ships a
chain split. Both the DST string and the derived 32 bytes are
constants frozen at activation.

### 2.2 Shield (0 spends, 1 output, value_balance = +100_000_000)

```
commitment = NoteCommitment(0, Poseidon(sk,0), ValueAsHash(1e8), randomness)
           = 1805599d42fa07a9e96becb98a5a2627a7bd92a49445114cd2b8a611a883a752
value_balance = +100_000_000
```

### 2.3 Unshield (1 spend, 0 outputs, value_balance = -100_000_000)

```
commitment (in-tree) =
    20c697258800b3f1048837dbba83efb539eeebd4bd5798c33923316066beb372
nullifier = ComputeNullifier(sk, leaf_idx=0)
          = cf2b8adc7b53b371094aab1dfa02c5edbd12476a4085184ddba1cd801a72ec33
value_balance = -100_000_000
```

### 2.4 Transfer-3d (1 spend, 1 output, value_balance = -10_000)

```
spend commitment =
    55065d9e5fe5e7f9a63bb0f092a0e14914d92524143e0c36ee0771b2130961cb
nullifier =
    bfd22264f5ddddd16160586823d8fc54abfa49d8cc12e563688929f06f3b907d
output commitment =
    607ceaf56e7bd4cdd8e177fa5c67a57aeaf9b6c74d0cc691139f3caec1b8eed1
value_balance = -10_000  (= -fee)
```

### 2.5 Transfer-3e (2 spends, 2 outputs, value_balance = -20_000)

```
spend A commitment =
    9bd9f2d07c65265e350b2ef96e1e6651c32c1475b898d4cbca2717bfa1490e70
spend A nullifier =
    02350fc32a565e2e5fac9894efbbee47f4146ad69b250a02023752c3de4ac5c7

spend B commitment =
    f87b8857bcfc0382af9876f6dfcffecd80cea2eb67599cd23dc52d83b5cfe06e
spend B nullifier =
    d6126c0a59e51448882c3b0fb5cfce70ad2fe0590e76144a9849c149fdb2151c

output recipient commitment =
    9568bfe75016b5bf7a6fe107bdd63a82dc1b1d1f17825a33bb284aec3c2582da
output change commitment =
    f318afecf0ae4cec0564be3d243eed47c71b24e78145111912bbba9cee584e6d

value_balance = -20_000  (= -fee)
```

---

## 3. How to verify in another implementation

1. Set diversifier to all-zero 32 bytes.
2. Implement `Poseidon(sk, 0)` for `pk` derivation (Poseidon-2 over
   the secp256k1 scalar field).
3. Implement `NoteCommitment(d, pk, value_hash, randomness) =
   Poseidon(Poseidon(addr_bind(d, pk), value_hash), randomness)`
   where `addr_bind = Poseidon(ADDR_TAG, Poseidon(d, pk))` and
   `ADDR_TAG = "DIN/v7/shielded/addr/v1"` right-padded to 32 bytes.
4. Implement `ComputeNullifier(sk, leaf_index) = Poseidon(sk,
   ValueAsHash(leaf_index))`.
5. Run `MakeHash(seed, tail)` against the inputs in §1 and verify
   the hex outputs in §2 match byte-for-byte.

If any value disagrees, a consensus rule has been broken.

---

## 4. Forward compatibility

This document covers v0.3.0. Future wire-format changes (e.g. real
diversifier derivation lifting `d = 0` to per-recipient values) MUST
introduce new vectors for the new schema, not edit these. The
existing pins should keep passing under the post-activation v0.3.0
validator forever — they only fail if a consensus rule changes,
which IS a chain split by definition.

When activation height is set on testnet/mainnet, this document
also needs to record the canonical genesis-of-shielded test
vectors covering activation behaviour. That is Wave 3 calendar
work, not pre-activation.
