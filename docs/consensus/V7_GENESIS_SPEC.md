# Dinero v7 Genesis Specification

**Status:** Draft proposal
**Version:** 0.1 (draft)
**Scope:** Fresh-genesis chain launch with mandatory post-quantum outputs from block 0.
**Supersedes:** [`V6_GENESIS_AND_UPGRADE_SURFACES.md`](./V6_GENESIS_AND_UPGRADE_SURFACES.md) (transparent Taproot genesis) and [`V7_P2MR_SPEC.md`](./V7_P2MR_SPEC.md) (additive P2MR on v5).
**Relation to v5:** v5 continues operating as the active Dinero chain until v7 genesis. The v5 freeze fork (`V5_FREEZE_FORK_SPEC.md`) stays armed. At a pre-announced v5 block height, the v5 UTXO set is snapshotted and committed in v7's genesis coinbase. v5 holders claim their balance on v7 via a signed migration proof.

## Executive Summary

Dinero `v7` is a fresh-genesis chain in which **every spendable output is post-quantum secure from block 0**. The single spendable output type is BIP-360-style Pay-to-Merkle-Root (P2MR) where the Merkle root commits to one or more ML-DSA-65 public keys.

- One signature scheme at genesis: **ML-DSA-65** (NIST FIPS 204, formerly Dilithium3).
- One spendable output type: **witness v3 P2MR**.
- One transaction family: `tx.version == 2`, transparent amounts, Taproot-tree-style sighash.
- No Schnorr/secp256k1 spend path on v7. No Taproot outputs. No confidential transactions. No ring signatures. No ZK lane.
- Utreexo is the sole consensus accumulator. Same 128-byte header as v5. Same transparent amount semantics. No `value=0` hack.
- v5 balance holders migrate via a deterministic snapshot + Schnorr proof committed in the v7 genesis coinbase.

The positioning claim is literal and defensible: **every UTXO on v7 is post-quantum safe. Always. There is no legacy Schnorr lane to drain.**

## Design Principle

Two things that turned out to be load-bearing in v5:

1. **Private-lane / transparent-lane coexistence broke Utreexo correctness** (the `value=0` hack for CT outputs caused the 3474-UTXO drift).
2. **Schnorr/secp256k1 signatures are quantum-fragile.** Any public key ever revealed on-chain is vulnerable to a cryptographically-relevant quantum computer (CRQC).

v7 removes both by construction. One lane. One signature scheme. The scheme is post-quantum. There is no backwards-compatible Schnorr spend path at any height, under any gate.

## Consensus Rules At Block 0

For every block at height `h >= 0`, every non-coinbase transaction `tx` MUST satisfy:

1. **Transaction version.** `tx.version == 2`. All other values rejected.
2. **No confidential outputs.** For every output, `output.is_confidential == false`. (Field retained for serialization compatibility with v5 code paths, always zero.)
3. **P2MR-only scriptPubKeys.** For every non-coinbase output in `tx.vout`, `output.scriptPubKey` MUST be one of:
   - **Witness v3 P2MR:** exactly 34 bytes, pattern `0x53 0x20 <32-byte Merkle root>`.
   - **OP_RETURN commitment:** `0x6a ...`, provably unspendable, never enters Utreexo. Used for coinbase commitments and DNRF filter commitments only.
4. **P2MR spend witness** is a canonical structured payload:
   ```
   <scheme_id:1 byte>
   <pubkey_len:varint> <pubkey_bytes>
   <signature_len:varint> <signature_bytes>
   <merkle_depth:1 byte>                   // 0..8 inclusive
   <sibling_hash_0:32> ... <sibling_hash_{d-1}:32>
   <leaf_index:varint>                     // < 2^merkle_depth
   ```
   Consensus validates, in order:
   - `scheme_id` is present in the `PQSchemeRegistry` with `state == ACCEPT` at the current height (see Signature Scheme Registry section). At v7 genesis this is `{0x01: ML-DSA-65}` only.
   - `merkle_depth <= 8`.
   - `leaf_index < 2^merkle_depth`.
   - The scheme-specific verifier validates the signature over the BIP341-style sighash with `pubkey_bytes`.
   - `H_leaf = SHA256(scheme_id || pubkey_bytes)`, hashed up the Merkle path using `sibling_hash_i` and the `i`-th bit of `leaf_index` for left/right ordering, equals the 32-byte Merkle root in the output's scriptPubKey.

### Coinbase

A coinbase transaction has one input (null prevout) and one or more outputs.

**Coinbase input.**
- The coinbase `scriptSig` is free-form push-data only — never executed. It carries:
  - BIP34 height (serialized as a minimal push).
  - At block 0 only, the v7 genesis inscription bytes (see Genesis Inscription section). Inscription bytes are plain UTF-8 push data; there is no `OP_RETURN` in scriptSig (OP_RETURN is a script opcode, not an input payload).
  - Miner tag / extranonce at the miner's discretion.

**Coinbase outputs.** Every coinbase output's `scriptPubKey` MUST be exactly one of:

1. **P2MR reward output.** A 34-byte witness-v3 P2MR scriptPubKey (`0x53 0x20 <merkle_root>`) paying the block reward + fees to the miner's address.
2. **OP_RETURN commitment output.** A scriptPubKey beginning with `0x6a`, value zero, carrying a consensus-specified commitment:
   - **At block 0 only:** an output committing to the v5 snapshot Merkle root, format `0x6a 0x20 <snapshot_root_32>`. This is where the v5→v7 bridge is anchored — in a coinbase *output*, not in the input's scriptSig.
   - **At any height:** additional OP_RETURN outputs carrying witness commitments, DNRF filter commitments, or other consensus-specified block-level data, per the `header_feature_flags` upgrade surface.

A coinbase with any other output script shape (P2PKH, P2SH, Taproot, bare multisig, non-commitment OP_RETURN variants) is invalid.

The genesis inscription goes in the `scriptSig`; the snapshot-root commitment goes in a `scriptPubKey` OP_RETURN. These are separate locations with separate roles and must not be conflated.

### Signature Scheme Registry

The witness format carries `scheme_id` explicitly so that adding a new PQ scheme later does **not** change the witness wire format, does **not** require a wallet upgrade for senders, does **not** change the address format, and does **not** rewrite the consensus validation loop. Adding a scheme is "flip one row in a table, add one verifier wrapper."

At every height the consensus-accepted schemes are drawn from a `PQSchemeRegistry`:

```cpp
struct PQSchemeParams {
    uint8_t       scheme_id;
    const char*   name;
    SchemeState   state;                // ACCEPT | DARK_RESERVED | RESERVED
    uint32_t      pubkey_bytes_max;
    uint32_t      signature_bytes_max;
    uint32_t      witness_byte_weight;  // per-scheme multiplier on witness wire bytes
                                        // (replaces the single global "witness discount")
    uint32_t      verify_cost_weight;   // flat weight units added per spend-input,
                                        // independent of wire size; protects verify budget
    uint32_t      activation_height;    // height at which state becomes ACCEPT on mainnet
};
```

`state` semantics:

- `ACCEPT`  — `scheme_id` is valid at this height, signatures verify, witnesses accepted into blocks.
- `DARK_RESERVED` — `scheme_id` value is permanently bound to a named scheme; witnesses carrying it MUST be rejected at consensus today; a future activation-height fork flips this row to `ACCEPT`. Prevents adversarial standards-squatting.
- `RESERVED` — not yet bound to any scheme; witnesses carrying it MUST be rejected; the value is available for future binding.

Genesis registry:

| `scheme_id` | Name          | State           | `pubkey_max` | `sig_max` | `witness_byte_weight` | `verify_cost_weight` | Activation height |
|:-----------:|---------------|-----------------|:------------:|:---------:|:---------------------:|:--------------------:|:-----------------:|
| `0x01`      | ML-DSA-65     | `ACCEPT`        | 1952         | 3309      | 1                     | 25                   | 0 (genesis)       |
| `0x02`      | FALCON-512    | `DARK_RESERVED` | 897          | 666       | — (unused while dark) | — (unused while dark) | — (future fork)  |
| `0x03`      | SPHINCS+-128s | `DARK_RESERVED` | 32           | 7856      | — (unused while dark) | — (unused while dark) | — (future fork)  |
| `0x04`–`0xFE` | —           | `RESERVED`      | —            | —         | —                     | —                    | —                 |
| `0xFF`      | —             | `RESERVED`      | —            | —         | —                     | —                    | — (never assigned; reserved to keep the field one byte and leave a "reject-by-value" canary) |

**Phase 1 benchmarks (PQClean `clean` variant, pinned upstream commit `3730b32a`):**

| Host                             | Role                  | Date       | verify/s | ms/verify | ms/sign | ms/keygen |
|----------------------------------|-----------------------|------------|---------:|----------:|--------:|----------:|
| Apple Silicon M-series (local)   | dev / fast host       | 2026-04-16 | 13,968   | 0.072     | 0.283   | 0.095     |
| **AMD EPYC-Rome @ 2.0 GHz (va)** | **weakest fleet node**| 2026-04-16 | **4,931** | **0.203** | 0.833   | 0.381     |

- EPYC-Rome is ~2.83× slower than Apple Silicon on ML-DSA verify, close to the 3× assumption the spec was originally sized against.
- The dinerova result is the load-bearing number for mainnet sizing: the slowest fleet node sets the consensus budget.
- At the 25%-of-120s block-verify budget (30 s wall), dinerova fits **~147,950** ML-DSA verifications. That is the ceiling `verify_cost_weight` must respect.

**ML-DSA-65 row values (locked after EPYC-Rome run and Phase 2 policy call):**

- `verify_cost_weight = 25 WU`. Tight-fit from the EPYC benchmark would be 2,000,000 / 147,950 ≈ **13.5 WU**; the spec carries 25 WU (+85% margin) so that a merely noisy run-to-run variation, a future kernel upgrade, or a slightly slower successor node does not require a consensus change. Ample headroom without affecting the throughput profile.
- `witness_byte_weight = 1`. **Same weight-per-byte as base tx bytes (i.e., the classic 4× witness discount vs. base is preserved at its Bitcoin value).** The original draft proposed a halved discount (`= 2`); the Phase 2 decision restored the full Bitcoin-style discount on the grounds that PQ witness bytes still do not enter the UTXO set and are still transient from a storage perspective, so they deserve the same discount Schnorr witnesses have historically received. `MAX_BLOCK_WEIGHT` stays at 8 MB WU at launch; if testnet exposes propagation pain we have `MAX_BLOCK_WEIGHT` as the next lever.
- **This is the knob that sets throughput, not `verify_cost_weight`** — see Per-input weight below.

**These values are the spec's locked starting point for mainnet genesis.** Any later benchmark on a slower node (hardware degradation, new node added to the minimum-spec tier) or propagation stress on testnet is a valid reason to revise; a faster node on its own is not.

Per-input P2MR weight with the locked values:

- Witness wire bytes: ~5,400 × `witness_byte_weight=1` = 5,400 WU
- Plus `verify_cost_weight=25` WU
- **Total per P2MR spend-input: ~5,425 WU**
- At 8,000,000 WU max block weight: **~1,470 P2MR spend-inputs per block** = **~735 typical 2-input txs per block** = **~6 tx/sec steady-state.**

This is the honest cost of mandatory PQ: roughly half the throughput of a Bitcoin-equivalent Taproot-only chain at the same block weight, driven entirely by the ~5.4 KB ML-DSA witness. The verify CPU cost is a rounding error in this equation — the EPYC benchmark has over 100× more verify headroom than this policy consumes. If testnet later exposes propagation pain, the next lever is `MAX_BLOCK_WEIGHT`, not the registry values.

Consensus rule at v7 genesis: any witness with `scheme_id != 0x01` MUST be rejected. Unreserved values (`0x04..0xFE`) are permanently future-reserved and MUST continue to be rejected until a specific activation-height fork binds them.

`verify_cost_weight` is a per-scheme parameter consulted by:

- **Block-weight accounting.** Each P2MR spend-input adds `verify_cost_weight` weight units to the block, independent of witness byte size. This is what protects the block-validation budget from DoS regardless of which PQ scheme is used.
- **Mempool fee-rate calculation.** Fees are rated against weight units inclusive of `verify_cost_weight` — so a 3309-byte ML-DSA sig and a 666-byte FALCON sig pay proportional to their actual verifier cost, not their byte size. FALCON is physically smaller on the wire but not proportionally cheaper to verify; the weight table reflects that.

The concrete `verify_cost_weight` number for ML-DSA is filled in after Phase 1 benchmarks. Future FALCON / SPHINCS+ activation forks populate their own rows without touching ML-DSA's.

## What Is Not In v7 Genesis

Out of scope for v7 block 0. Reserved for future additive forks.

- **FALCON / SPHINCS+ / hybrid signatures.** New `scheme_id` via future fork. The witness format already supports them — only the consensus-permitted `scheme_id` whitelist needs widening.
- **Confidential transactions, ring signatures, shielded pool.** Not in v7, ever, by design. If a shielded lane is added in the future, it uses a separate upgrade surface (`header_feature_flags` + coinbase OP_RETURN commitment), exactly as sketched in the archived V6 spec. Not a genesis concern.
- **Silent Payments over P2MR.** Requires a PQ-compatible stealth scheme. Research-level. v7 receives are explicit-address only at genesis.
- **Taproot key-path spends.** No Schnorr spend path exists on v7. Period.

## Utreexo And The 128-Byte Header

- Leaf hashing is unchanged: `HashUTXO(txid, vout, amount, scriptPubKey)`. Amounts are plaintext. Same SHA-256 domain as v5.
- The 128-byte header from v5 is reused verbatim. The 12 reserved bytes at offset 116 are interpreted per V6 spec: `header_feature_flags` (4 bytes) + `reserved_strict_zero` (8 bytes). At genesis and through the chain, `header_feature_flags == 0` and `reserved_strict_zero == 0` until a specific future fork lights a flag bit.
- No `utreexo_root` drift can occur on v7. Every output is transparent P2MR; every UTXO hashes the same way on insertion and on rebuild. Test F (`UTXOPositionIndex::Rebuild` returns `missing == 0 && malformed == 0`) is a structural invariant of v7, not just a test.

## Block Weight And Fee Policy

ML-DSA-65 witnesses are large. Indicative per-spend wire size for scheme `0x01`:

| Component | Size (bytes) |
|---|---|
| `scheme_id` | 1 |
| pubkey | 1952 |
| signature | 3309 |
| `merkle_depth` byte | 1 |
| Merkle path (d=4, typical) | 128 |
| `leaf_index` varint | 1–2 |
| **Per-input witness (typical, d=4)** | **~5,400** |
| **Per-input witness (max, d=8)** | **~5,540** |

Weight accounting rule:

```
tx_weight = base_tx_bytes * 4
          + sum over inputs of (witness_bytes * witness_byte_weight(scheme_id)
                                + PQSchemeRegistry[scheme_id].verify_cost_weight)
```

- `witness_byte_weight(scheme_id)` — per-scheme multiplier on wire bytes. Stored in `PQSchemeRegistry`. Replaces v5/Bitcoin's single global witness discount. Keeps different PQ schemes honestly priced against their actual relative verifier cost per byte.
- `verify_cost_weight` — flat weight added per spend-input *independent of wire size*. Protects the block verification budget directly: no matter how clever a witness-packing trick is, the verifier cost is charged. Same field, per-scheme, from the registry.

Genesis policy:

- **Max block weight:** `8,000,000` WU (doubled from v5's 4M). Policy decision revisitable post-benchmark.
- **ML-DSA-65 row values:** `witness_byte_weight` and `verify_cost_weight` **filled in from Phase 1 benchmark results**. Starting hypotheses: `witness_byte_weight = 2` and `verify_cost_weight` set so that the 25%-of-120s block-verification budget equals `≥ 500` ML-DSA verifies on the weakest fleet node. Actual numbers come from the benchmark, not from this spec.
- **Minimum fee rate:** tuned after benchmark. Indicative starting point: 5 µDIN / weight-unit.

When FALCON (`0x02`) eventually flips to `ACCEPT`, its row gets its own `witness_byte_weight` and `verify_cost_weight` from benchmarks at that time. ML-DSA's row is not touched. Senders do not need to know these numbers to construct a P2MR address; the address commits to a 32-byte Merkle root and nothing else.

## Address Format

v7 addresses are [bech32m (BIP-350)](https://github.com/bitcoin/bips/blob/master/bip-0350.mediawiki) with HRP `din`. The first data char after the `1` separator encodes the witness version as a 5-bit value in the standard bech32 alphabet.

- bech32 alphabet (position → char): `qpzry9x8gf2tvdw0s3jn54khce6mua7l`.
- Witness **v0** (SegWit) → position 0 → `q` → `din1q...` (not used in v7; historical only).
- Witness **v1** (Taproot) → position 1 → `p` → `din1p...` (not used in v7; no Taproot on v7).
- **Witness v3 (P2MR) → position 3 → `r` → `din1r<merkle_root_bech32>...`.**

Example shape (payload is illustrative, not a real key):
```
din1rqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqxxxx
^^^  ^                                                        ^^^^
HRP  witness version char 'r' (= 3)                           bech32m checksum
     ↓
     then 52 chars encoding the 32-byte Merkle root via base32 (5-bit groups)
```

Total on-wire address length: 3 (HRP) + 1 (`1` separator) + 1 (version char) + 52 (Merkle root) + 6 (checksum) = **63 characters**.

**The address commits to the 32-byte Merkle root and nothing else.** The `scheme_id` of the key inside that root is **not** in the address. This is deliberate:

- The sender does not know, and does not need to know, which PQ scheme the recipient has committed at a given Merkle leaf. The address is scheme-agnostic.
- When FALCON or SPHINCS+ activation flips their registry rows to `ACCEPT`, an existing `din1r...` address still works unchanged — the recipient can add a new leaf committing to a FALCON pubkey under the same root (if they planned for it at key-generation time) or rotate to a new root address.
- Wallets never parse the address to decide what signature to produce; they sign with whatever key they hold, and the witness carries the `scheme_id` explicitly.

This means the address format is **permanent**. Future PQ scheme additions change consensus + verifier + registry, not the address codec.

## Genesis Inscription

The v7 genesis inscription is pinned in the coinbase `scriptSig`, verbatim. Encoding: UTF-8, no BOM, no trailing newline.

```
Dinero v7 - Real Money For Free People | Post-Quantum from block 0 | <DATE>
```

Exact byte sequence is fixed at genesis cut and included here as part of consensus once the launch date is set. Until then, this spec treats the `<DATE>` token as a placeholder.

The v5 snapshot-root commitment is placed in a separate coinbase **output** (`OP_RETURN`-style scriptPubKey), not in the scriptSig. See the Coinbase section above.

## v5 → v7 Migration

v7 launch does not destroy v5 transparent value. CT and ring-covenant UTXOs, already frozen by the v5 freeze fork, are **not** migrated — see "What Is Not Migrated" below. The mechanism for transparent UTXOs:

1. **Snapshot height.** At a pre-announced v5 block height `H_snap`, take a deterministic snapshot of the **transparent** v5 UTXO set only. The snapshot includes `(txid, vout, amount, scriptPubKey)` for every spendable v5 UTXO whose `scriptPubKey` is Taproot, P2WPKH, P2WSH, P2PKH, or P2SH (i.e., has an identifiable Schnorr or ECDSA signing owner).
2. **Snapshot root.** A canonical Merkle root over the sorted snapshot entries.
3. **v7 genesis coinbase.** Commits the snapshot root in an `OP_RETURN` **output** (separate output in the coinbase tx; see Coinbase section). This ties v7 block 0 to a specific frozen v5 state; there is no disputable "which snapshot" question.
4. **Claim transaction.** On v7, a v5 holder constructs a claim by:
   - Signing their v7 P2MR destination address with the v5 Schnorr (Taproot) or ECDSA (pre-SegWit-ish) key that controls their v5 UTXO.
   - Submitting a special v7 "genesis claim" transaction that proves inclusion in the snapshot root, verifies the signature against the scriptPubKey from the snapshot entry, and mints the claimed amount at the P2MR destination.
5. **Claim window.** A pre-announced number of blocks (proposal: ~180 days worth of v7 blocks). After the window closes, unclaimed value is burned. The claim window is a consensus rule, not policy.

### What Is Not Migrated

**CT (confidential-transaction) outputs and ring-covenant outputs are explicitly excluded from the v5→v7 snapshot and have no v7 claim path.** This is not an oversight; it is a direct consequence of two prior decisions:

- The v5 freeze fork (`V5_FREEZE_FORK_SPEC.md`) already declared these UTXOs stuck. Gate 2 of the freeze rejects v3 and v4 transactions, which are the only spend paths that can unlock a CT output or a ring-covenant output. The freeze-fork spec is explicit: "pre-activation confidential UTXOs are intentionally stuck."
- CT outputs commit to Pedersen amount commitments owned by a blinding factor, and ring outputs use one-time stealth keys validated by CLSAG ring signatures — **neither has a canonical transparent-Schnorr owner that the v7 claim path can verify**. A "CT-aware claim" would require re-introducing the CT/ring validators on v7, which is the exact complexity v7 exists to eliminate. That path is rejected.

Concretely: at `H_snap`, the snapshot generator iterates the v5 UTXO set and **skips every entry where `is_confidential == true` or the owning transaction's `tx.version` is 3 or 4**. These outputs remain on v5 forever. v7 consensus has no knowledge of them.

This is consistent with the freeze-fork's stated "known consequence" and does not change the v5 holders' position — they were already stuck before v7 genesis.

### Genesis Claim Signature Verification

Genesis claim validation uses the signature scheme of the v5 output being claimed — Schnorr (Taproot), Schnorr/ECDSA (SegWit v0), or ECDSA (legacy). This is the only place in v7 consensus that legacy signature verification is wired in, and it is valid **only** for claim transactions against the genesis-committed snapshot root within the claim window. Normal v7 spends are registry-gated (ML-DSA-only at genesis).

**The CRQC window during the claim period** is real but bounded: an attacker who breaks Schnorr/ECDSA during the 180-day window could steal unclaimed v5 balances from the v7 perspective. This risk is identical to the risk Bitcoin holders carry today, bounded by the claim window, and goes to zero after the window closes. Acceptable tradeoff.

## Honest Positioning

- *Every spendable output on v7 is post-quantum safe from block 0.* True. No Schnorr UTXOs exist on v7; the only Schnorr verification is gated on genesis-committed snapshot claims within a bounded window.
- *Bitcoin-lineage PoW, Utreexo-backed stateless validation, mandatory post-quantum signatures.* True.
- *First live chain with PQ-safe-by-construction economics.* True at time of launch.
- *The existing wallet-layer privacy stack (Silent Payments, CoinJoin) ships as a v7 wallet feature.* Design-pending; not a genesis consensus concern.

What we do NOT claim:

- Not a shielded chain. Amounts are transparent.
- Not a privacy-by-default chain at the consensus layer. Privacy is wallet-layer.
- Not a "drop-in Bitcoin replacement". v7 is its own chain with its own address format and its own signature scheme.

## Implementation Sequencing

Phased plan. Each phase gates the next.

**Phase 0. Spec consolidation.** This document.

**Phase 1. ML-DSA library selection + benchmark.** ✅ **Complete.**
- Library: **PQClean** chosen. See `docs/consensus/V7_PQ_LIBRARY_SELECTION.md`. Vendored as `third_party/pqclean/` (ML-DSA-65 clean variant only) pinned to upstream commit `3730b32aa50ba9e712592c1476bdd048f5f6ed7e`.
- Benchmark harness: `tools/pq_bench/ml_dsa_bench.cpp`. Builds as target `ml_dsa_bench`.
- Apple Silicon M-series: 13,968 verify/s, 0.072 ms/verify.
- **Weakest fleet node (dinerova, AMD EPYC-Rome @ 2.0 GHz): 4,931 verify/s, 0.203 ms/verify.** EPYC is ~2.83× slower than Apple Silicon, matching the 3× assumption.
- ML-DSA-65 registry row confirmed by the EPYC run with +85% margin on `verify_cost_weight`. Registry values (`witness_byte_weight=2`, `verify_cost_weight=25`) are the starting point for mainnet genesis.

**Phase 2. Block weight + fee policy.** ✅ **Complete.**
- `MAX_BLOCK_WEIGHT = 8,000,000 WU` (unchanged from provisional).
- ML-DSA-65 `witness_byte_weight = 1` (revised down from provisional 2; full Bitcoin-style witness discount).
- ML-DSA-65 `verify_cost_weight = 25` (+85% margin over tight-fit against EPYC-Rome).
- Launch throughput target: ~6 tx/s steady-state with typical 2-input P2MR txs.
- Next lever if testnet exposes propagation pain: raise `MAX_BLOCK_WEIGHT`.

**Phase 3. PQ primitives + registry.**
- Add `src/consensus/pq/scheme_registry.{h,cpp}` — `PQSchemeParams` struct and the genesis `PQSchemeRegistry` with one active row (`0x01` ML-DSA-65) and reserved-dark rows (`0x02`, `0x03`). Lookups by `scheme_id` return `(state, verify_cost_weight, witness_byte_weight, ...)`.
- Add `src/consensus/pq/ml_dsa_65.{h,cpp}` wrapping the chosen library. No consensus integration yet — just keygen / sign / verify with round-trip tests.
- Registry interface is the ONE thing the consensus P2MR verifier and the mempool fee-rate code consult. Adding FALCON later means one new row, one new verifier wrapper file, zero changes to the consumers.

**Phase 4. Address codec + wallet.**
- New bech32m HRP char for P2MR. Proposal: `din1m...` (same bech32 `din` prefix, `m` witness-version char for v3).
- Wallet derivation path for ML-DSA keys. BIP-44-style. Needs a new SLIP-44 registration eventually; use a provisional coin type for development.
- `wallet.getnewp2mraddress`, `wallet.signp2mr` RPCs.

**Phase 5. Consensus integration.**
- New file: `include/consensus/p2mr_consensus.h` — witness deserializer, Merkle path verifier, script recognition.
- New file: `src/consensus/p2mr_verify.cpp`.
- Modified: script validation routes `0x53 0x20 ...` scripts to P2MR verifier.
- Modified: `BlockValidator::ValidateTransaction` and mempool enforce output shape + claim-tx-only Schnorr exception.
- Coinbase creation in `BlockAssembler` switched to P2MR.

**Phase 6. v5 → v7 migration infrastructure.**
- Snapshot tool: reads v5 ChainDB, emits canonical snapshot + Merkle root.
- Genesis coinbase builder consumes the snapshot root.
- Claim transaction verifier (Schnorr + Merkle inclusion + double-claim prevention).
- Claim window expiry at consensus height.

**Phase 7. Qt + wallet UX.**
- Receive tab: P2MR address type only.
- Send tab: ML-DSA signing.
- Migration tab: "claim my v5 balance" flow that takes a v5 private key (or hardware signer signing surface) and derives a v7 P2MR destination.

**Phase 8. Regtest / testnet / fleet soak.**
- Regtest harness for P2MR (analog of `test_freeze_fork.sh`).
- Public testnet with its own genesis.
- Fleet dry-run of v5→v7 migration end-to-end using a testnet snapshot.

**Phase 9. Public launch.**
- Announce v5 snapshot height and v7 genesis date.
- v5 fleet runs both v5 (frozen post-freeze-fork) and v7 in parallel during the claim window.
- Post-claim-window: v5 is end-of-life; v7 is the active chain.

## Non-Goals

- Shielded lane at genesis (see V6 for why this is out of scope).
- Silent Payments / CoinJoin at genesis (wallet layer, ships when ready).
- Multiple PQ schemes at genesis (intentional — ML-DSA only).
- Backwards compatibility with v5 at the consensus level (there is none; only the claim path bridges).
- Dynamic block weight adjustment (fixed at genesis, change via fork if needed).

## Open Items

Everything below is flagged for resolution before Phase 3.

- **PQ library choice:** PQClean vs. liboqs vs. pq-crystals reference. Leaning PQClean for audited single-scheme footprint; liboqs is convenient but broader-surface.
- **Exact witness serialization:** varint vs. fixed-width length prefixes. TLV vs. sequential. Needs ONE canonical choice for deterministic signing. Serialization must be scheme-agnostic so the format stays stable when FALCON / SPHINCS+ flip to `ACCEPT`.
- **bech32m HRP char:** `m` for witness v3 is a proposal, not nailed down. Need to check for collisions with existing registered addr-types.
- **`verify_cost_weight` and `witness_byte_weight` for ML-DSA-65:** populated from Phase 1 benchmark. Until then the registry row for `0x01` is a placeholder.
- **SLIP-44 coin type:** provisional number for development; permanent number requires registration with BIP community.
- **Snapshot height and claim window length:** numbers go here after fleet/ops discussion.
- **Genesis inscription date token:** `<DATE>` resolves to an absolute calendar date at launch time. Must be pinned before genesis.
- **Claim-tx Schnorr verification surface:** keep or reject CRQC risk argument on claim window — revisit if CRQC timeline compresses.
- **Block weight numbers:** `8,000,000` WU and `2×` witness discount are starting points. Final numbers from Phase 1 benchmark.
- **Mempool admission policy for ultra-large claim txs:** claim txs may be larger than normal P2MR spends due to snapshot Merkle proofs. Needs its own size/fee-rate policy.
