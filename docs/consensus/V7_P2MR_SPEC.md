# Dinero v7 P2MR (BIP-360-style Pay-to-Merkle-Root) Specification

> **⚠️ Status: SUPERSEDED — retained for historical reference.**
>
> This document describes the **additive** P2MR-on-v5 approach (middle path). The chosen forward direction is **[V7_GENESIS_SPEC.md](./V7_GENESIS_SPEC.md)** — a fresh-genesis v7 chain with mandatory P2MR from block 0 and the `PQSchemeRegistry` pattern. This doc is kept for reference because the registry design and witness layout were refined here first.
>
> Do **not** implement from this document. Use V7_GENESIS_SPEC.md.

**Status:** Superseded draft (pre-pivot)
**Version:** 0.2 (superseded; aligned with registry pattern for consistency only)
**Scope (as written, now obsolete):** Activation-height fork on v5 that **relaxes Gate 3** of the freeze fork to permit a new post-quantum output family alongside Taproot.
**Predecessor:** [`V5_FREEZE_FORK_SPEC.md`](./V5_FREEZE_FORK_SPEC.md).

## Executive Summary

BIP-360 Pay-to-Merkle-Root (P2MR) gives Dinero post-quantum output protection **without touching Utreexo correctness, transparent-amount semantics, or tx.version**. It adds a new `witness_v3` scriptPubKey shape that commits to a Merkle root over multiple PQ public keys; at spend time the owner reveals one chosen leaf + Merkle path + PQ signature.

Key properties:

- **Additive, not destructive.** Taproot outputs remain first-class. P2MR is a new sibling, not a replacement.
- **No value hack.** Amounts are transparent, same as Taproot. `HashUTXO(txid, vout, amount, scriptPubKey)` domain unchanged.
- **Tx format unchanged.** `tx.version == 2`. Gate 2 of the freeze fork stays as-is — no v3/v4 revival.
- **Quantum hiding until spend.** The 32-byte commitment on-chain reveals no PQ public key; a quantum attacker harvesting the chain sees only Merkle roots, not the keys themselves.

## Consensus Rules At `P2MR_ACTIVATION_HEIGHT`

For every block at height `h >= P2MR_ACTIVATION_HEIGHT`, every non-coinbase transaction `tx`:

1. **Gate 3 of the freeze fork is relaxed** to also accept the P2MR script shape.
   Gate 1 (no confidential outputs) and Gate 2 (no v3/v4 tx formats) remain unchanged.

2. **P2MR output script shape.** A P2MR scriptPubKey is exactly 34 bytes:
   ```
   0x53 0x20 <32-byte Merkle root>
   ```
   where `0x53` is `OP_3` (witness version 3) and `0x20` is `PUSH32`. Any other byte pattern remains subject to the original Gate 3.

3. **P2MR spend (witness)** is a structured payload:
   ```
   <scheme_id:1>
   <pubkey_bytes>
   <signature_bytes>
   <merkle_path_depth:1>
   <sibling_hash_0:32> ... <sibling_hash_{d-1}:32>
   <leaf_index:varint>
   ```
   Consensus validates: (a) scheme_id is a permitted PQ scheme; (b) the pubkey verifies the signature over the BIP341-style sighash; (c) `H_leaf(scheme_id || pubkey)` hashed up the Merkle path with the provided siblings and leaf_index reproduces the 32-byte Merkle root in the scriptPubKey.

4. **Permitted signature schemes (aligned with `PQSchemeRegistry` per `V7_GENESIS_SPEC.md`):**

   At activation of this (superseded) fork, the registry would have been:

   | `scheme_id` | Name          | State             |
   |:-----------:|---------------|-------------------|
   | `0x01`      | ML-DSA-65     | `ACCEPT`          |
   | `0x02`      | FALCON-512    | `DARK_RESERVED`   |
   | `0x03`      | SPHINCS+-128s | `DARK_RESERVED`   |
   | `0x04`–`0xFF` | —           | `RESERVED`        |

   This was revised from the original draft of this doc, which activated ML-DSA + FALCON simultaneously. Lesson from the V7_GENESIS spec: activate one at a time, populate `witness_byte_weight` + `verify_cost_weight` per scheme from live benchmarks before flipping `DARK_RESERVED` → `ACCEPT`. SPHINCS+ stays dark until block-weight policy is re-tuned.

5. **Leaf hash.** `H_leaf = SHA256(scheme_id || pubkey_bytes)`. Node hash: `H_node = SHA256(left || right)` with canonical left-right ordering by the `leaf_index` bit at that depth.

6. **Max Merkle depth.** `<= 8`. Caps pubkey-set cardinality at 256 — enough for multi-sig and rotation schemes, prevents DoS via deep proofs.

## What Remains Unchanged

- **Utreexo semantics.** Same SHA-256 leaf hashing, same `HashUTXO`, same 128-byte header. P2MR amounts are transparent, so the drift class that caused the v5 freeze does **not** re-emerge here.
- **Tx format and version.** `tx.version == 2`. No new tx family.
- **Freeze fork gates 1 and 2.** CT outputs and v3/v4 tx formats remain consensus-rejected.
- **Taproot (witness v1) outputs.** Continue to work unchanged. Pre-P2MR Taproot UTXOs remain spendable under their original rules.
- **Silent Payments, CoinJoin.** Wallet-layer. P2MR does not break them for Taproot flows. P2MR receive is explicit-address only (see Open Items).

## Activation Parameters

Following the template at `include/consensus/freeze_fork_activation.h`:

```cpp
struct P2MRActivationParams {
    static constexpr uint32_t MAINNET_ACTIVATION_HEIGHT = UINT32_MAX;  // set after fleet soak
    static constexpr uint32_t TESTNET_ACTIVATION_HEIGHT = UINT32_MAX;  // set after regtest soak
    static constexpr uint32_t REGTEST_ACTIVATION_HEIGHT = 300;         // regtest harness

    static bool IsP2MRActive(uint32_t height, Chain chain);
    static uint32_t GetActivationHeight(Chain chain);
};
```

P2MR activation MUST be strictly greater than `FREEZE_FORK_ACTIVATION_HEIGHT` on every chain.

## Block Weight Impact

P2MR witnesses are much larger than Schnorr-over-secp256k1 (~64 bytes). Rough per-spend sizes:

| Scheme         | Pubkey | Sig   | + Merkle path (d=4) | Witness bytes |
|----------------|--------|-------|----------------------|---------------|
| Schnorr (TR)   | 32     | 64    | n/a                  | ~96           |
| ML-DSA-65      | 1952   | 3293  | 128                  | ~5,400        |
| FALCON-512     | 897    | 666   | 128                  | ~1,800        |

A P2MR spend is **20–60×** the witness weight of a Taproot spend. **The original draft kept the 4× global witness discount and called for re-tuning max block weight.** The revised model (per `V7_GENESIS_SPEC.md`) replaces the single global discount with per-scheme `witness_byte_weight` and `verify_cost_weight` fields in the `PQSchemeRegistry`. Taproot spends continue to use the 4× witness discount under their legacy accounting (unchanged); P2MR spends use the per-scheme registry values. Block-weight maximum still needs re-tuning from benchmarks before activation.

## Implementation Files

Additive changes only. No existing consensus function is modified by value; gates are widened.

- **New file:** `include/consensus/p2mr_activation.h` — activation params, `IsP2MRAllowedScript(script)` helper.
- **Modified:** `include/consensus/freeze_fork_activation.h` — `IsFreezeForkAllowedScript` gains a P2MR clause gated by height.
- **New file:** `src/consensus/p2mr_verify.cpp` — Merkle-path reconstruction, scheme dispatch.
- **New file:** `src/consensus/pq/ml_dsa_65.cpp`, `src/consensus/pq/falcon_512.cpp` — signature verification wrappers. Library choice TBD (liboqs or vetted single-scheme refs).
- **Modified:** `src/consensus/script_validation.cpp` — recognize witness-v3 spends and route to P2MR verifier.
- **Modified:** `src/daemon/validation_mempool.cpp`, `src/consensus/block_validation.cpp`, `src/consensus/parallel_block_validator.cpp`, `src/consensus/validation_queue.cpp` — the four freeze-fork gate sites gain the P2MR relaxation.
- **New file:** `tests/regtest/test_p2mr.sh` — see Test Plan.
- **Wallet (dinero-wallet):** new `getnewp2mraddress` RPC, sign path, address codec.
- **Qt (dinero-qt):** new address type in receive tab, new send-mode option, warning if PQ fee is significantly higher than Taproot.

## Test Plan

All regtest, activation at `P2MR_ACTIVATION_HEIGHT = 300`, post-freeze-fork.

- **Test A.** Pre-P2MR-activation: any `witness_v3` output rejected by Gate 3 (freeze-fork only, no P2MR yet).
- **Test B.** Post-activation: construct a 2-leaf P2MR with two ML-DSA keys; spend via leaf 0 with correct signature + Merkle path. MUST succeed.
- **Test C.** Post-activation: spend via leaf 0 with a **wrong** Merkle path. MUST be rejected with `p2mr-merkle-path-mismatch`.
- **Test D.** Post-activation: spend with a valid ML-DSA signature but a **different** pubkey than the leaf commits to. MUST be rejected.
- **Test E.** Post-activation: spend with `scheme_id = 0xFF` (reserved). MUST be rejected.
- **Test F — Utreexo correctness proof.** Same shape as Test F of the freeze fork: mine 100 post-P2MR-activation blocks using a mix of Taproot and P2MR outputs; assert `UTXOPositionIndex::Rebuild` reports `missing == 0` and `malformed == 0`. This is the load-bearing claim that P2MR does not re-introduce drift.
- **Test G.** Max-depth Merkle path (d = 8, 256-leaf set). Construct, spend leaf at index 127. MUST succeed.
- **Test H.** Max-depth + 1 (d = 9). MUST be rejected at script validation.

## Non-Goals

- **Revising tx.version.** No v5 shielded lane, no v3/v4 revival. If a shielded lane ever ships, it gets its own activation fork and its own tx.version byte, per the two-surfaces principle in `V6_GENESIS_AND_UPGRADE_SURFACES.md`.
- **Making Taproot quantum-safe.** Existing Taproot UTXOs remain vulnerable to a hypothetical CRQC attacker who sees revealed pubkeys. P2MR is the opt-in PQ path; Taproot holders migrate by sending to a P2MR address.
- **Silent Payments over P2MR.** Requires a PQ-friendly stealth scheme (research open). Out of scope for this spec; P2MR receive is explicit-address only at v7 genesis.
- **SPHINCS+ at v7 genesis.** Defer to a later additive fork that bumps block-weight policy first.
- **Hybrid signatures (PQ + Schnorr).** Out of scope; P2MR leaves are single-scheme.

## Deployment Sequence

1. Land this spec; fleet review.
2. Implement the P2MR verifier, script recognition, wallet support, Qt UI. Ship with `MAINNET_ACTIVATION_HEIGHT = UINT32_MAX`.
3. Run Tests A through H on regtest. All MUST pass.
4. Benchmark P2MR verification throughput on the weakest fleet node. Re-derive block-weight policy if needed.
5. Fleet soak at `UINT32_MAX` on mainnet.
6. Set `MAINNET_ACTIVATION_HEIGHT` to `current_tip + 2000` in a later release. Coordinate fleet upgrade.
7. Monitor post-activation for rejected P2MR spends (should only be malformed witnesses).

## Open Items

- **PQ library choice.** liboqs (broad coverage, larger surface) vs. single-scheme vetted reference implementations (smaller, audited, more work to integrate). Leaning toward pq-crystals `ml_dsa` ref + PQClean `falcon` ref for consensus use.
- **Exact witness serialization encoding.** TLV vs. length-prefixed concatenation. Needs a single canonical choice so signatures are deterministic.
- **Address format.** Propose `din1m...` bech32m HRP with a new witness-version char. Needs wallet / Qt / explorer coordination.
- **Silent-Payments-equivalent for P2MR.** Research-level; defer.
- **Interaction with future DNRF filter commitments.** P2MR outputs are distinguishable script-pattern-wise; filter indexing should treat them as a separate family.
- **Fee-rate discount for P2MR.** Does the 4× witness discount still make sense when a P2MR witness is 5 KB? Possibly tune to 2× for witness-v3 only, policy-layer.
