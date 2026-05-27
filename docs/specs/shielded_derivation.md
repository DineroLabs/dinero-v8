# Dinero Shielded Derivation Spec — v1 (DRAFT, not yet activated)

**Status:** DRAFT. This spec defines key derivation and address encoding for
the Dinero shielded pool (tx version 5). Consensus code in
`src/consensus/shielded/` already accepts v5 transactions; what remains
before any wallet ships shielded UX is locking the bytes in this document
and writing test vectors.

**Authority:** This is the canonical reference for `m/99'/1448'/...`.
Implementations that diverge from this spec are wrong. If a discrepancy
is found between this spec and the consensus code, the consensus code
wins; open a fix-the-spec PR.

**Retired coin type:** coin_type 1447 is permanently retired; any reference outside historical archives is a bug.

---

## 1. Scope

This spec defines:

- BIP32 derivation path for shielded keys
- Sapling-shape sub-derivation (sk → ask/nsk → ak/nk → ovk/ivk/dk → pk_d)
- Diversified address generation
- Bech32m address encoding and HRPs
- Encrypted note byte layout
- Domain-separation tags
- Test vectors

This spec does **not** define:

- The R1CS spend/output circuits (see `include/consensus/shielded/shielded_circuit.h`)
- Commitment tree structure (see `include/consensus/shielded/commitment_tree.h`)
- Nullifier set storage (see `include/consensus/shielded/nullifier_set.h`)
- Consensus validation rules (see `include/consensus/shielded/shielded_validation.h`)

---

## 2. Cryptographic Primitives

| Primitive | Concrete instantiation |
|---|---|
| Curve | secp256k1 (same curve as Taproot keys) |
| Hash-to-curve | `secp256k1_XMD:SHA-256_SSWU_RO_` per RFC 9380 §8.7 |
| Field-friendly hash | Poseidon-2 over secp256k1 scalar field (matches `commitment_tree.cpp`) |
| Symmetric AEAD | ChaCha20-Poly1305-IETF (RFC 8439), 12-byte nonce |
| Key-derivation function | HKDF-SHA256 (RFC 5869) |
| Point encoding | **x-only, even-y canonical** (BIP340 convention) — 32 bytes |
| ECDH shared secret | x-coordinate of `s · P` under even-y convention |

**Why x-only / even-y:** secp256k1 compressed encoding is 33 bytes (1-byte
prefix + 32-byte x), but Sapling-shape addresses are 43 bytes (11 + 32).
To stay 32 bytes per public key, we adopt BIP340's even-y canonical form:
every public key point is the unique `(x, y)` representative with `y` even.
ECDH then takes the x-coordinate of `s · P` and is well-defined for both
parties without sending y-parity. This is the same trick already used by
Taproot and is structurally identical to Sapling's Jubjub canonical
encoding.

---

## 3. Derivation Path

### 3.1 Account node (BIP32 hardened)

```
m / 99' / 1448' / account'
```

| Component | Value | Hardened |
|---|---|---|
| `purpose` | 99 | yes |
| `coin_type` | 1448 (Dinero v7) | yes |
| `account` | 0, 1, 2, … | yes |

**`m/99'/1448'/account'` is the deepest BIP32 hardened node.** Implementations
MUST NOT extend this path with `/0/index`, `/0'/index'`, or any other
BIP32 child below the account level. All receive addresses are produced
by Sapling-style diversifiers off the account-level spending key (§4).

### 3.2 Spending key extraction

From the BIP32-derived `(chain_code, key_bytes)` at `m/99'/1448'/account'`:

```
sk = key_bytes   // 32 bytes
```

`sk` is the master secret for this account. From `sk` everything else is
derived deterministically (no further BIP32 entropy is consumed).

---

## 4. Sapling-shape Sub-derivation

### 4.1 Domain-separation tags (constants)

These tags are ASCII bytes, no terminating NUL:

```
DST_ASK = "DIN/v7/shielded/ask"   // 19 bytes
DST_NSK = "DIN/v7/shielded/nsk"   // 19 bytes
DST_OVK = "DIN/v7/shielded/ovk"   // 19 bytes
DST_DK  = "DIN/v7/shielded/dk"    // 18 bytes
DST_DIV = "DIN/v7/shielded/div"   // 19 bytes
```

Versioned (`v7`) so a future shielded-v2 spec can coexist without key
collision.

### 4.2 PRF construction

Define `PRF(key, dst) → 32 bytes`:

```
PRF(key, dst) = Poseidon2_to_bytes(
                    F(key_bytes_le[0..32]),
                    F(dst_bytes_le, padded to 32))
```

where `F(·)` interprets the input as a little-endian secp256k1 scalar
mod q (curve order), and `Poseidon2_to_bytes(·)` returns the 32-byte
little-endian encoding of the field element.

**Why Poseidon-2 (not BLAKE2b):** ivk and ak are referenced inside the
spend circuit. BLAKE2b is cheap natively but expensive in R1CS;
Poseidon-2 keeps the circuit small. The native evaluator and R1CS
gadget MUST use the same Poseidon-2 parameters as `commitment_tree.cpp`
— if they diverge, off-chain proofs won't verify on-chain.

### 4.3 Spend authority and nullifier keys

```
ask = PRF(sk, DST_ASK) mod q     // spend authority key, scalar
nsk = PRF(sk, DST_NSK) mod q     // nullifier key, scalar
```

If `ask == 0` or `nsk == 0` (negligible probability), increment the input
to `PRF` and retry — but in practice, treat as a fatal generation error
since it means the seed is broken.

### 4.4 Public viewing components

```
ak = ask · G                     // 32 bytes (x-only, even-y)
nk = nsk · G                     // 32 bytes (x-only, even-y)
```

`G` is the secp256k1 base point. If `ak.y` is odd, negate `ask` so that
`ak.y` becomes even (BIP340 normalisation); same for `nsk`/`nk`.

### 4.5 Outgoing viewing key and diversifier key

```
ovk = PRF(sk, DST_OVK)           // 32 bytes (symmetric key, no mod q)
dk  = PRF(sk, DST_DK)            // 32 bytes (ChaCha20 key for diversifier)
```

`ovk` is the wallet's recovery key for outgoing notes (decrypts notes
*sent* by this wallet). `dk` keys the deterministic diversifier
generator (§5.1).

### 4.6 Incoming viewing key

```
ivk = Poseidon2(ak_bytes_le, nk_bytes_le) mod q
```

`ivk` is the master incoming viewing key. Disclosing `ivk` reveals all
incoming payments to every diversified address under this account but
does NOT permit spending. This is the property that makes diversified
addresses useful: one `ivk` shared with a watcher unlocks all receive
addresses without exposing `ask`.

### 4.7 Full viewing key (FVK)

```
fvk = (ak, nk, ovk)              // 96 bytes total
```

`fvk` is what gets shared with auditors/watch-only wallets: it permits
detection of incoming AND outgoing notes (via `ivk` derivable from
`ak`/`nk`, and `ovk` directly), but cannot spend.

---

## 5. Diversified Address Generation

### 5.1 Diversifier index

A diversifier index `j` is an 88-bit integer in `[0, 2^88)`. It is
**not** a BIP32 child index — it is fed through `dk` to produce the raw
diversifier `d`:

```
d = ChaCha20(key=dk, nonce=12-byte little-endian j padded with 0x00, counter=0)[0..11]
```

`d` is the first 11 bytes of the ChaCha20 keystream block. This makes
diversifiers indistinguishable from random bytes to anyone without `dk`,
preventing on-chain index correlation.

### 5.2 Hash-to-curve and validity check

```
P_d = HashToCurve(d, DST_DIV)
```

`HashToCurve` is `secp256k1_XMD:SHA-256_SSWU_RO_` per RFC 9380 §8.7,
with DST = `DST_DIV` (defined §4.1).

`HashToCurve` per RFC 9380 always returns a valid point — there is no
"50% failure" mode for SSWU_RO. **However**, we additionally require:

```
P_d != identity
```

which is overwhelmingly satisfied. If `P_d` is the identity (negligible
probability), increment `j` and retry.

This is a deliberate departure from Sapling, where Jubjub's
`group_hash` had a real ~50% failure rate. RFC 9380 SSWU_RO eliminates
that complication; spec-followers do not need a retry loop in practice
but MUST handle the identity case for correctness.

### 5.3 Diversified transmission key

```
pk_d = ivk · P_d
```

If `pk_d.y` is odd, negate `ivk · P_d` (i.e., output `(x, -y)` then
re-normalise) so that the encoded point is even-y canonical. The
encoded `pk_d_bytes` is the 32-byte big-endian x-coordinate.

**Note on negation:** because we always emit even-y, the implicit y is
recoverable — the recipient and sender both know to use the even-y
representative when computing ECDH.

### 5.4 Address payload

```
address_payload = d || pk_d_bytes      // 11 + 32 = 43 bytes
```

### 5.5 Bech32m encoding

```
address = bech32m_encode(HRP, convertbits(address_payload, 8, 5, pad=true))
```

| Network | HRP |
|---|---|
| mainnet | `dins` |
| testnet | `tdins` |
| regtest | `rdins` |

**No witness-version byte is prepended.** These addresses are NOT
BIP173/BIP350 witness programs — they do not appear in scriptPubKey.
The full payload is the 43 raw bytes from §5.4, bech32m-encoded
directly. Length-disambiguation from `din1p`/`din1r` (which are 33-byte
witness programs) is by encoded-string length: shielded addresses are
deterministically longer.

Example shape: `dins1q...` (about 76 characters total for mainnet).

---

## 6. Note Format

### 6.1 Plaintext note

```
struct Note {
    uint8   d[11];               // diversifier (matches address)
    uint64  value;               // una (little-endian)
    uint8   rcm[32];             // randomness for commitment
    uint8   memo[512];           // recipient memo, zero-padded
}                                // total: 563 bytes
```

`memo` is fixed-length (512 bytes) and zero-padded to prevent length
leakage. UTF-8 application data with trailing zeros stripped on
decryption.

### 6.2 Note commitment (consensus-fixed)

```
ADDR_TAG  = "DIN/v7/shielded/addr/v1"            // 23-byte ASCII DST

addr_bind = Poseidon2(
                F(ADDR_TAG, padded to 32),
                Poseidon2(F(d, padded to 32), F(pk_d_bytes)))

commitment = Poseidon2(
                 Poseidon2(addr_bind, F(value)),
                 F(rcm))
```

`F(·)` interprets bytes as a little-endian secp256k1 scalar mod q
(matches §4.2). `d` is the 11-byte diversifier (the same `d` carried
in the encrypted note plaintext at §6.1). `pk_d_bytes` is the 32-byte
x-only even-y encoding of pk_d.

**Why the address-binding tag:** consensus cannot tell a Taproot
public key from a shielded `pk_d` by bytes alone — both are 32-byte
x-only secp256k1 points. The tag forces every valid commitment to
include `Poseidon2(d, pk_d)` under the canonical `ADDR_TAG`. A sender
who splices a raw Taproot pk into the output (bypassing the bech32m
address codec) produces bytes that pass low-level type checks but
cannot be reconstructed by anyone running the canonical wallet, since
the canonical wallet only accepts a parsed `dins`/`tdins`/`rdins`
address as input. Combined with the daemon-RPC enforcement (§7,
new row), this turns "shield to a transparent address" from a wallet
UX bug into a deliberate, custom-fork-only attack — and even then,
the attacker only loses their own funds.

This formula MUST match the R1CS gadget in `shielded_circuit.cpp`. Any
divergence breaks proof verification. **This is the consensus boundary**
— the spec's authority over this formula is subordinate to the circuit;
if the circuit changes, this spec must follow.

### 6.3 Encrypted note — wire format

For each shielded output, the bundle carries `encrypted_note` (variable
length) computed as:

```
esk     = random scalar (32 bytes, ephemeral)
epk     = esk · G                              // 32 bytes (x-only, even-y)
shared  = (esk · pk_d).x                       // 32 bytes ECDH
key     = HKDF-SHA256(salt=epk, ikm=shared, info="DIN/v7/shielded/note")[0..32]
nonce   = 12 zero bytes  (epk provides freshness)
ct      = ChaCha20-Poly1305(key, nonce, aad=epk, plaintext=Note)

encrypted_note = epk || ct                     // 32 + 563 + 16 = 611 bytes
```

Recipient detection: walk the wallet's note candidates; for each shielded
output, compute `shared = (ivk · epk).x` and attempt decryption. Successful
AEAD authentication (correct Poly1305 tag) confirms the note belongs to
this wallet — no probabilistic guessing.

---

## 7. Consensus vs. Wallet Boundary

| Item | Where enforced | Spec's authority |
|---|---|---|
| Path `m/99'/1448'/account'` | Wallet only | Canonical |
| Sapling-shape sub-derivation | Wallet only | Canonical |
| HRP / bech32m address encoding | Wallet only | Canonical |
| `ivk = Poseidon2(ak, nk)` | Wallet only | Canonical |
| Diversifier validity (P_d != identity) | Wallet only | Canonical |
| Note plaintext layout | Wallet only | Canonical |
| Encrypted note wire format | Wallet ↔ Wallet | Canonical |
| Destination HRP validation (refuse non-shielded HRPs) | **Daemon RPC** + Wallet | Canonical |
| Address-binding tag inside commitment (§6.2 `addr_bind`) | **Consensus** (R1CS gadget) | Subordinate to `shielded_circuit.cpp` |
| Note commitment formula | **Consensus** (R1CS gadget) | Subordinate to `shielded_circuit.cpp` |
| Nullifier formula | **Consensus** (R1CS gadget) | Subordinate to `shielded_circuit.cpp` |
| Anchor freshness window | **Consensus** | Subordinate to `shielded_validation.cpp` |
| Value balance check | **Consensus** | Subordinate to `shielded_validation.cpp` |

If you change a "wallet-only" item, you ship a new wallet. If you change
a "consensus" item, you fork the chain.

### 7.1 Daemon RPC contract — non-shielded destinations are unreachable

Every `dinerod` RPC method that constructs a shielded output (working
names: `shieldsend`, `shieldedtransfer`, `unshield`) MUST take its
shielded destination(s) as a **bech32m address string**, not as raw
bytes, raw scalars, or descriptor hex. The daemon's RPC handler:

1. Rejects with `RPC_INVALID_ADDRESS` if the destination string fails
   bech32m decoding.
2. Rejects with `RPC_INVALID_ADDRESS` if the decoded HRP is not one of
   `dins` / `tdins` / `rdins` matching the active network. In
   particular, `din1p`, `din1r`, `bc1*`, and any non-Dinero-shielded
   HRP MUST be rejected at parse time, before any cryptographic work.
3. Decodes the 43-byte payload (11-byte `d` ∥ 32-byte `pk_d`) and only
   then constructs the output, computing `addr_bind` per §6.2 with the
   exact `(d, pk_d)` parsed from the address string.

There is **no raw-bytes destination path**. Custom clients (Python
scripts, hand-built JSON-RPC requests) cannot bypass this check
without forking and recompiling `dinerod`. At that point they are no
longer running canonical Dinero — and even then, the only loss is
their own funds, since the chain still rejects malformed proofs.

Wallet UIs (DineroDPI, dinero-qt) re-validate the same HRP rule
client-side before sending the RPC, so the user sees a clear UI error
instead of a generic RPC rejection.

---

## 8. Test Vectors

**STATUS: COMPLETE for Vectors 1–3** as of 2026-04-27. Phase 5 Waves
1, 2, and 3a (key derivation, diversified address, encrypted-note
ECDH/AEAD) are all pinned in
`src/test/shielded_derivation_tests.cpp` (`ShieldedDerivation` ctest).
Vector 4 (cross-language parity) remains an open invitation for
independent implementations.

### 8.1 Vector 1 — account 0, diversifier index 0

Inputs:

```
seed (64 bytes) = "DIN/v7/shielded/derivation/v1" (29 ASCII bytes,
                  zero-padded to 32) || (first 32 bytes XOR 0xFF, all 32 bytes)
                = 44494e2f76372f736869656c6465642f64657269766174696f6e2f7631000000
                  bbb6b1d089c8d08c9697b29b9a9bdab2bb9d9e9b8b8b8e8b9bd089cdffffff
account = 0
```

(The seed pin is verified deterministically by `CanonicalSeed()` in
`src/test/shielded_derivation_tests.cpp`.)

Wave 1 outputs (PINNED, originally captured 2026-04-27 against
`src/wallet/shielded_derivation.cpp`; `ask` re-pinned 2026-05-26
after the `NormalizeScalarToEvenY` fix that actually negates the
scalar on odd-y inputs — the old `ask` was `n - new_ask` and did
not pair with the stored `ak`):

```
sk:    0afa9463b4d5f06c7d4e9cf14f9d261eaf6c7a0ba243453f5d4308ddc415d9e0
ask:   5fc6bdf8e5b1c3d47de4b249a086cfd95c6894a35a924a034136770e64cce9ad
nsk:   4d548e2eabaab49cb2e5877bfaad6e456c4033bcc2bfb1882c1f991d1aeb8e3e
ovk:   5eff91d8d132177c83f2302494d879ad01e064846a767617170406d48627ecc9
dk:    7ca608cc6062bfebd3d1a6f7128cdbe9befc60edbb4fc29060fc6219e782c3f2
ak:    864ba7ec6376210f1568f972d907b003723ff985e65305e620effb56789bcdff
nk:    dcfcd14d2739a61201e4d870751c3592bddf8f4b591b9c2abefbf859fd5e19ff
ivk:   51c856061f52ffa07c1f2f05cd0f1b0ded8e94428809044c1c486aba27e492c5
fvk = (ak || nk || ovk):
       864ba7ec6376210f1568f972d907b003723ff985e65305e620effb56789bcdff
       dcfcd14d2739a61201e4d870751c3592bddf8f4b591b9c2abefbf859fd5e19ff
       5eff91d8d132177c83f2302494d879ad01e064846a767617170406d48627ecc9
```

Wave 2 outputs (PINNED, captured 2026-04-27):

```
j = 0
d (after ChaCha20, 11 bytes):
       6b92d6a2de35177cada44c

pk_d  (x-only, even-y, 32 bytes):
       981db4b85ce150d7e74768cd6d9147148cba846857289d5c585b0681f9a469f9

address_payload (43 bytes) = d || pk_d:
       6b92d6a2de35177cada44c
       981db4b85ce150d7e74768cd6d9147148cba846857289d5c585b0681f9a469f9

address (mainnet, dins):
       dins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535ljkd4ggr
address (testnet, tdins):
       tdins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535lj5qhwc6
address (regtest, rdins):
       rdins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535lj0pxq49
```

**Spec divergence notes** (resolved in favor of code per §0
"consensus code wins"):

- **§4.2 endianness:** the spec text describes `F(·)` as
  little-endian scalar interpretation, but the on-chain Poseidon
  evaluator (commitment_tree.cpp) reads bytes big-endian. Wave 1
  implementation follows the on-chain big-endian convention.
- **§5.2 hash-to-curve:** the spec calls for RFC 9380 SSWU_RO. Wave 2
  implementation uses libsecp's `secp256k1_generator_generate`
  (try-and-increment over `SHA256(DST_DIV || d)`), matching the
  V-generator pattern in `pedersen_generators.cpp`. Same nothing-up-
  my-sleeve property; same negligible collision probability across
  the 88-bit diversifier index space; no separate SSWU implementation
  required. **Canonical:** any wallet implementing §5.2 differently
  will produce different `pk_d` from the same `(ivk, d)` and recipients
  will fail to decrypt.
- **§6.3 epk derivation:** the spec text says `epk = esk · G`, but
  combined with §5.3's `pk_d = ivk · P_d` this is internally
  inconsistent — sender's `shared = esk · pk_d = esk · ivk · P_d` and
  receiver's `shared = ivk · epk = ivk · esk · G` are NOT equal unless
  `P_d == G`. The implementation follows Sapling's actual ECDH
  pattern: `epk = esk · P_d`, then `shared = esk · pk_d` for the
  sender and `shared = ivk · epk` for the receiver, both equal to
  `esk · ivk · P_d`. The receiver then uses `d` from the decrypted
  plaintext to sanity-check `pk_d = ivk · HashToPoint(d)`. BIP340
  even-y normalisation applied to `epk` and `esk` is symmetric to
  Sapling's keypair normalisation.

A follow-up spec PR will align §4.2, §5.2, and §6.3 with the
implementation.

### 8.2 Vector 2 — encrypted note round-trip (PINNED)

Pinned in `src/test/shielded_derivation_tests.cpp::PinnedHexVector2EncryptedNote`.
Independent implementations that agree on the round trip MUST emit
exactly these 611 bytes given the same inputs.

Inputs:

```
recipient (Vector 1, j=0, regtest HRP):
    d         = 6b92d6a2de35177cada44c
    pk_d      = 981db4b85ce150d7e74768cd6d9147148cba846857289d5c585b0681f9a469f9
    ivk       = 51c856061f52ffa07c1f2f05cd0f1b0ded8e94428809044c1c486aba27e492c5

note plaintext (563 bytes):
    d         = 6b92d6a2de35177cada44c          (matches recipient)
    value     = 100_000_000  (1 DIN, little-endian into bytes 11..18)
    rcm       = ASCII "DIN/v7/shielded/note/rcm" (24 bytes) zero-padded
                to 32 = 44494e2f76372f736869656c6465642f6e6f74652f72636d
                       0000000000000000
    memo[0..21] = ASCII "DIN test note plain v1"
    memo[22..511] = 0x00

ephemeral esk (sender):
    24 ASCII bytes "DIN/v7/shielded/note/esk" zero-padded to 32:
        44494e2f76372f736869656c6465642f6e6f74652f65736b
        0000000000000000
```

ECDH inner steps (deviation from spec §6.3 — see notes after §8):

```
P_d  = HashToPoint(d, "DIN/v7/shielded/div")  -- libsecp generator_generate
epk  = (esk · P_d) with BIP340 even-y normalisation
shared = (esk_normalised · pk_d).x
key  = HKDF-SHA256(salt=epk, ikm=shared,
                   info="DIN/v7/shielded/note")[0..32]
nonce = 12 zero bytes  (epk provides freshness)
aad  = epk
ct   = ChaCha20-Poly1305(key, nonce, aad, plaintext)
```

Outputs (PINNED):

```
epk:
    94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f

encrypted_note  (32 epk || 563 ct || 16 tag = 611 bytes):
    94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f
    bf48bb6d359922610ff31ceadd6872092a20e35c9aa1e82e666ef880af7450e8
    26e2385e8685a7638ddaa5ee0ccdce6414b825b9233dacb3bb87cd3463b4e40e
    246772fd6e2a9bab03834141fbca84782a3476ffb4b708d3d32b0e18b4094eb5
    7b473fd7b401ef1dd4af9c35c14426b7006fb6a29db599ce12594458dd233b30
    157988ea3d95499d4a838143ee43d22f0af54173735c919eeaed19a320c72d13
    22caba24b161f872851ea6db9e5e46b23fde0ccde181ff31863dbd04bbe72c67
    a3292b55934d48db77137afe3cdf5db3bc7ea9705939f1163b2920a2d3a917a0
    b87364b68ffcaaeda17a0b34fe6e15637a457029b54c7f0a4ea2f2d019d217c1
    384f74d76e86cfd903a7878256b1d01c20cc4be94354a4d6d2bc2c361491492c
    64e6b047e92587e3b4ada8aedfcc88e87e70b1d7d35b2c1c0a2539384450e04a
    ac15eebf7a6f3f7606bb1e74a479eabe17696db5af14bc5c775eb71a14e13ea7
    7bc914815e04368ce7cee67acc27e40131b2bd002dfb99880797914547b0b92b
    a88cb19b68b465514cdecab53de8e15e9b9bbe6877fc1c8f5d629d99f553acab
    2948febe585a8163ec05a9a2b2d46d2f90bdb802985577248abc5560354aadea
    e52a7ad584eb15e4b5329306205b213fd9c366f69cc7380716dcffadf7677cf6
    ca12dcab99be4ab07b7e4284031a4e639c4ceb9e092e8bec187c6ff3b37828c6
    e4a0bf59761d8ef12d79279bfff1be6c1db607f34524a9ad63291be18604c802
    7524a02de17fdfecbcedcfbab2e83e6028c8a4d9be01b2f8b9dc98b92cbd2d8c
    14b14a
```

Decryption with the recipient's `ivk` (Vector 1) recovers the
plaintext byte-identically.

### 8.3 Vector 3 — diversifier retry (DEAD-CODE DOCUMENTATION)

**Status:** dead-code path. The implementation uses libsecp's
`secp256k1_generator_generate` (try-and-increment) instead of the
spec's RFC 9380 SSWU_RO (see §5.2 deviation note). Both constructions
produce a valid curve point for any 11-byte input; neither hits the
identity for any practical `j`.

The retry loop exists only for spec correctness. Implementations
SHOULD detect the identity case and increment `j`, but in practice
the loop never executes more than one iteration. No test vector is
required because the failure case is unreachable on secp256k1 with
the chosen seed-construction.

A hostile environment could construct a `dk` such that some `j`
produces a problematic `d` byte string; the implementation MUST still
detect identity and retry. This is testable only via mocked inputs;
not part of the canonical Vector set.

### 8.4 Vector 4 — cross-language parity

The same seed must produce the same `address` from independent
implementations in:

- C++ (the daemon / dinero-qt)
- Swift (DineroDPI)
- Rust (any future SV2-side wallet)

Vector 1 + Vector 2 hex must be reproduced bit-identically by all three.

---

## 9. Implementation Touch List

When a wallet implementer goes to wire this up, these are the files
they touch:

- `include/wallet/shielded_derivation.h` — new (peer of `pq_derivation.h`)
- `src/wallet/shielded_derivation.cpp` — new
- `src/wallet/utxo_index.cpp:43` — extend path-prefix allowlist with `m/99'/`
- `src/wallet/key_origin.cpp` — extend parser to accept purpose 99
- `include/wallet/address_validator.h` + impl — add `dins`/`tdins`/`rdins` HRPs and length checks for 43-byte payload
- `src/wallet/dinero_wallet_api.h` — `GetNetworkHRP()` callers do not change; shielded HRPs are queried via a new accessor
- `src/test/shielded_derivation_tests.cpp` — new, must include all vectors from §8

CMakeLists.txt entries are required for the two new source files and
the test (recurring CMake gotcha — test entries are near the end of the
9000-line file).

---

## 10. Open Items (deliberately unspecified, escalate before locking)

1. **`account` allocation policy.** Single account (`account=0`) for v1, or
   multi-account from the start? Multi-account complicates HD wallet
   backups; single-account is restrictive. Recommendation: single account
   for v1 ship, document upgrade path to multi-account.

2. **xprv / xpub serialization for the shielded subtree.** BIP32 standard
   serialization assumes secp256k1 child derivation continues below the
   serialized node. Since we terminate at `m/99'/1448'/account'`, the
   serialized xprv is simply the BIP32-standard form at that node — no
   custom serialization needed. Document explicitly to prevent confusion.

3. **Memo encryption to OVK as well.** Sapling encrypts the note twice:
   once to `ivk` (recipient) and once to `ovk` (sender's own recovery).
   Spec currently defines only the ivk-encrypted ciphertext in §6.3 —
   add an OVK-keyed envelope before lock-in, or explicitly defer "wallet
   cannot recover sent notes after re-import" as a known v1 limitation.

4. **Address checksum vs. bech32m checksum.** Bech32m provides a
   built-in 6-character checksum; this spec relies on it exclusively. No
   additional checksum is added. Document in §5.5 to forestall the
   "should we add HMAC?" question.

5. **Activation height.** This spec defines key derivation, not
   activation. Activation of shielded UI surface in DineroDPI / dinero-qt
   is a separate operator decision tracked elsewhere (project memory:
   "shielded pool parked").

---

## 11. Change Control

Spec is versioned by the `v7` tag in domain-separation strings. Any
change that affects derived bytes (new DST, new HashToCurve suite, new
encoding) MUST bump to `v8` and define a migration. Cosmetic edits
(typos, clarifications) bump only the document revision below.

| Revision | Date | Change |
|---|---|---|
| 0.1.0-draft | 2026-04-26 | Initial draft. Test vectors §8 not yet generated. |
| 0.2.0-draft | 2026-04-26 | §6.2 commitment formula amended to bind `(d, pk_d)` under `ADDR_TAG = "DIN/v7/shielded/addr/v1"` (`addr_bind`). Added §7.1 daemon-RPC contract: shielded-output RPCs accept bech32m address strings only, reject non-`dins`/`tdins`/`rdins` HRPs at parse time. §7 boundary table gained two rows (HRP validation, address-binding tag). Test vectors §8 must regenerate against the amended commitment formula. **Bytes-affecting change**: any earlier prototype using the unbound commitment formula is incompatible. |
