# Trustless Light-Client Shielded Spend - M3 Design

**Status:** Draft
**Date:** 2026-05-27
**Scope:** dinero-v8 daemon RPC + DineroDPI thin-client spend construction
**Primary engineering sites:** dinero-v8, DineroDPI
**Depends on:** M1 receive primitives, T8 block/bundle wiring, M2 output feed, fleet deployment of `blockchain.shielded.outputs`

## Goal

Let DineroDPI in thin-client/shared-RPC mode spend a locally discovered shielded note without running an embedded node and without giving a remote daemon any viewing key, spending key, note plaintext, or wallet fingerprint.

M3 is the spend-side counterpart to M1/M2:

- M1 proved that iOS can derive shielded keys, trial-decrypt outputs, maintain a local commitment tree, and store owned notes.
- T8 wired real block/bundle parsing into receive scanning.
- M2 deployed a public shielded output/nullifier feed so shielded-only wallets see their notes.
- M3 adds the missing witness and proving path so a locally owned note can produce a valid shielded spend bundle and broadcast it through normal transaction relay.

The first user-visible M3 spend path is **one-note unshield**: spend one confirmed shielded note into a transparent output, with the fee paid by that shielded note. This is the smallest useful surface because it requires one spend proof and zero output proofs. The same foundation supports shielded-to-shielded transfer afterward, but addressed private transfer is intentionally not bundled into the first M3 PR.

## Non-goals

- **No hosted light-wallet service.** The daemon returns public witness data only. It never receives IVK, OVK, `rcm`, note plaintext, address index, or note ownership hints.
- **No outsourced proving.** The Spartan/rangeproof/binding work runs locally on iOS through a native `.xcframework`.
- **No all-wallet background spend support.** M3 spends from the active wallet only, matching M1/M2 receive scope.
- **No multi-note selection in the first PR.** M3 selects one confirmed unspent note. Multi-note consolidation and change outputs are a follow-up once the one-note spend path is empirically safe.
- **No shielded-to-shielded addressed transfer in the first PR.** Addressed transfer requires output proof generation, recipient encryption, change handling, and memo UX. The native proving surface should be designed for it, but the first app path remains one-note unshield.
- **No consensus change.** The chain already validates the bundle shape M3 will produce.

## Trust Model

The remote daemon can learn:

- that the client asked for a witness for a specific `leaf_index`;
- the anchor height/root the client wants to spend against;
- the final transaction bytes once the client broadcasts.

The remote daemon cannot learn:

- whether the requested witness belongs to this wallet or is a decoy query;
- the note value before broadcast, except what is revealed by the unshield transparent output and fee after broadcast;
- the note plaintext, `rcm`, per-note spend key, IVK/OVK, or mnemonic-derived wallet keys.

M3 does not make shielded spending perfectly metadata-private against an endpoint that sees every witness request and every broadcast. It preserves the core M1 privacy boundary: ownership and spend authority stay local. A later privacy hardening can batch/pad witness requests or fetch decoy witnesses, but M3 does not need that to be correct.

## Existing Source Truth

The daemon already has the spend-side primitives:

- `include/wallet/shielded_wallet_ops.h` / `src/wallet/shielded_wallet_ops.cpp`
  - `UnshieldNoteInput`
  - `BuildUnshieldBundleForTx`
  - `BuildTransferBundleForTx`
  - `BuildMultiTransferBundleForTx`
  - `BuildAddressedTransferBundleForTx`
- `include/consensus/shielded/bundle_builder.h` / `src/consensus/shielded/bundle_builder.cpp`
  - `PlannedSpend`, `PlannedOutput`
  - Pedersen value commitments
  - range proofs
  - blind-sum / `bvk_commitment`
  - binding signature
- `include/consensus/shielded/shielded_circuit.h` / `src/consensus/shielded/shielded_circuit.cpp`
  - `SpendWitness`, `SpendPublicInputs`, `ProveSpend`, `VerifySpend`
  - `OutputWitness`, `OutputPublicInputs`, `ProveOutput`, `VerifyOutput`
- `include/consensus/shielded/commitment_tree.h`
  - `CommitmentTree::GetAuthPath`
  - `ComputeNullifier(secret_key, leaf_index)`
  - `NoteCommitment(d, pk_note, value, rcm)`
- `include/wallet/shielded_derivation.h`
  - `DeriveNoteSpendKey(rcm)`
  - `TryDecryptNoteForViewer(ivk, encrypted_note)`
  - `EncryptNoteForRecipient(d, pk_d, note)`

Critical correction: iOS does not need "a Spartan wrapper" alone. A valid shielded bundle also requires Pedersen commitments, range proofs, blind-sum arithmetic, and binding signatures. M3 therefore ships a narrow **ShieldedProverKit** native library, not a raw Spartan-only bridge.

## Spend Inputs

To spend a note, iOS needs:

| Item | Source | M3 requirement |
|---|---|---|
| `value_una` | decrypted M1 note plaintext | already stored |
| `rcm` | decrypted M1 note plaintext | already stored |
| `d` | decrypted M1 note plaintext | **not currently stored; add column** |
| `commitment` | M2 output feed | **not currently stored; add column** |
| `leaf_index` | M2 output feed | already stored |
| `nullifier` | `ComputeNullifier(DeriveNoteSpendKey(rcm), leaf_index)` | current iOS placeholder must be replaced |
| `sk_note` | `DeriveNoteSpendKey(rcm)` | derive locally at spend time; do not persist if avoidable |
| `pk_note` | `Poseidon(sk_note, 0)` | derive locally |
| `auth_path` | new daemon `shielded.witness.by_index` RPC | verify locally against M1 snapshot/root |
| `anchor` | local commitment tree snapshot/current root | client source of truth |

Two M1/M2 receive gaps are load-bearing for M3:

1. **Store `d`.** The spend circuit commits to `d` via the address-binding formula. A note discovered by trial-decrypt has `d` in plaintext, but `ShieldedNoteStore` currently persists only `value`, `rcm`, `memo`, and placeholder `nullifier`.
2. **Store or recompute `commitment`.** Witness verification needs the leaf commitment. The client can recompute it from `(d, pk_note, value, rcm)`, but persisting the block-supplied commitment makes audit and repair simpler.

M3 must migrate the iOS store before enabling spend. Existing rows can be repaired by replaying M2 output feed entries for their `(block_hash, leaf_index)` and re-trial-decrypting the encrypted note to fill `d` and `commitment`. If repair cannot fill a row, that note remains visible for balance/history but is marked `spend_disabled_until_rescan`.

## Daemon RPC: `shielded.witness.by_index`

### Request

Named form:

```json
{
  "leaf_index": 912,
  "anchor_height": 30123,
  "anchor_root": "32-byte-hex"
}
```

Positional form:

```json
[912, 30123, "32-byte-hex"]
```

`anchor_root` is required. The client chooses the anchor from its own locally derived tree/snapshot lineage; the daemon must prove against that exact root or fail.

### Response

```json
{
  "leaf_index": 912,
  "anchor_height": 30123,
  "anchor_root": "32-byte-hex",
  "tree_size": 1831,
  "commitment": "32-byte-hex",
  "path": [
    "level-0-sibling-32-byte-hex",
    "... exactly 32 entries ..."
  ]
}
```

### Semantics

- `leaf_index` is the global shielded output index from M2.
- `anchor_height` is a connected block height at or below the daemon tip.
- `anchor_root` is the commitment-tree root after applying all shielded outputs through `anchor_height`.
- `path` is ordered leaf-to-root and has `TREE_DEPTH = 32` entries.
- The daemon returns `not_found` if `leaf_index >= tree_size` at `anchor_height`.
- The daemon returns `anchor_mismatch` if its derived root at `anchor_height` differs from the supplied `anchor_root`.

### Implementation Shape

Do not depend on the daemon's live `CommitmentTree` having full leaves. After restart, chainstate may deserialize only the compact frontier, and `CommitmentTree::GetAuthPath` explicitly returns null when full leaves are not present.

The first M3 implementation should build witnesses by deterministic replay:

1. Walk blocks from `shielded_activation_height` through `anchor_height`.
2. Extract shielded outputs using the M2 feed helper.
3. Append each commitment to a fresh full `CommitmentTree`.
4. When the requested `leaf_index` is reached, retain its commitment.
5. After replay, compare `tree.Root()` with request `anchor_root`.
6. Return `tree.GetAuthPath(leaf_index)`.

This is O(outputs up to anchor). At current mainnet size the M2 sanity log measured the output-feed walk in the low tens of milliseconds, and shielded output counts are small. A sidecar Merkle-node cache is allowed later if witness latency grows, but it is not required for the first correct M3.

## Client Verification

iOS must treat the daemon witness as untrusted public data.

Before proving:

1. Recompute `sk_note = DeriveNoteSpendKey(rcm)`.
2. Recompute `pk_note = Poseidon(sk_note, 0)`.
3. Recompute `commitment = NoteCommitment(d, pk_note, value, rcm)`.
4. Check the recomputed commitment equals both the stored commitment and the RPC `commitment`.
5. Verify the 32-level auth path with the note's `leaf_index`.
6. Check the path root equals the local anchor root selected from the M1 snapshot/current tree lineage.
7. Check that local receive sync has reached `anchor_height` and is not in endpoint-divergence pause state.

Only then may iOS hand note material to the native prover.

## ShieldedProverKit

M3 introduces `ShieldedProverKit.xcframework`, built from dinero-v8 source and linked into DineroDPI.

The public ABI should be narrow and C-compatible. It should not expose C++ `Transaction`, wallet DB, or daemon runtime types to Swift.

Minimum ABI for one-note unshield:

```c
typedef struct {
    uint8_t secret_key[32];
    uint8_t randomness[32];
    uint8_t d[32];
    uint64_t leaf_index;
    uint64_t value_una;
    uint8_t anchor[32];
    uint8_t merkle_path[32][32];
} dinero_shielded_spend_note;

typedef struct {
    const uint8_t* tx_sighash;
    size_t tx_sighash_len;      // must be 32
    uint64_t fee_una;
    const dinero_shielded_spend_note* note;
} dinero_shielded_unshield_request;

typedef struct {
    uint8_t nullifier[32];
    uint8_t anchor[32];
    uint8_t* bundle_bytes;
    size_t bundle_len;
    char* error;
} dinero_shielded_unshield_result;

int dinero_shielded_build_unshield_bundle(
    const dinero_shielded_unshield_request* req,
    dinero_shielded_unshield_result* out);

void dinero_shielded_free_result(dinero_shielded_unshield_result* out);
```

Swift remains responsible for:

- building the transparent transaction envelope;
- computing the same shielded transaction sighash the daemon uses, or calling a second native helper that computes it from a canonical serialized envelope;
- attaching the returned bundle bytes to a v6 transaction;
- broadcasting through existing RPC;
- marking the note pending-spent locally.

Recommended ABI hardening: expose `dinero_shielded_compute_tx_sighash(serialized_envelope)` as a native helper in the same kit, so Swift does not reimplement the binding-sig preimage differently from C++.

## Transaction Shape

M3 one-note unshield transaction:

- `tx.version = 6` if DineroDPI's parser/broadcaster supports bundle-committing v6; otherwise v5 remains acceptable if daemon consensus still accepts it.
- `vin = []`
- `vout = [recipient transparent output]`
- `explicit_fee = fee_una`
- `shielded_bundle_bytes = SerializeShieldedBundle({one spend}, {})`
- `bundle.value_balance = -note.value_una`

Transparent value equation:

```text
transparent_in(0) - transparent_out(note_value - fee) - fee
  = -note_value
  = bundle.value_balance
```

The binding signature signs the transparent envelope sighash. Any mutation of recipient, amount, fee, locktime, version, or explicit-fee field after proving invalidates the bundle.

## Pending Spend State

iOS needs the same third state the daemon wallet uses:

| State | Store shape |
|---|---|
| Confirmed unspent | `spent_in_block_hash IS NULL`, `pending_spend_txid IS NULL` |
| Pending spent | `pending_spend_txid != NULL`, no mined spend block yet |
| Confirmed spent | `spent_in_block_hash != NULL` |

Rules:

- On successful local build + broadcast, set `pending_spend_txid`.
- Exclude pending-spent notes from coin selection.
- When M2 feed later reports the note's nullifier in a mined block, clear pending and set `spent_in_block_hash`.
- If broadcast fails, clear pending immediately.
- On app restart, ask normal transaction/mempool status for pending txids. If the tx is unknown and no mined nullifier exists, clear pending and make the note spendable again.
- On reorg, if the mined nullifier disappears, clear `spent_in_block_hash`; if the original tx is still in mempool, retain pending.

Default toward recovery over stuck funds: if the app cannot prove a pending tx still exists, clear pending. A duplicate spend attempt will be rejected by mempool nullifier conflict if the old tx is still live.

## Security Checks

M3 must fail closed on:

- witness path root not equal to the local anchor;
- daemon `anchor_root` mismatch;
- note commitment mismatch;
- note already spent or pending-spent;
- local shielded sync not current enough for the selected anchor;
- endpoint divergence pause;
- fee >= note value;
- transparent output below dust threshold;
- `.xcframework` returns proof/bundle error;
- broadcast rejected with nullifier conflict.

## Performance

Expected costs:

- `shielded.witness.by_index`: tens of milliseconds at current chain size via replay; measure in sanity log.
- One spend proof: benchmark on physical iPhone Release. Historical daemon comments put spend proof around hundreds of milliseconds, but the iOS native bridge must be measured, not assumed.
- Bundle build includes range proof + binding signature in addition to Spartan spend proof.

Hard gate:

- one-note unshield build should complete in under 3 seconds on the physical iPhone used for M1 perf (`00008130-0008384C20FA8D3A`), Release build.

Soft target:

- under 1 second on-device. If it misses, M3 can still ship with a blocking progress sheet because spend is explicit user action, unlike background receive scanning.

## Rollout

1. Ship daemon witness RPC to fleet first.
2. Ship DineroDPI support gated on endpoint capability:
   - If `shielded.witness.by_index` is absent, show receive-only shielded state.
   - If present and `ShieldedProverKit` loads, enable one-note unshield.
3. Keep embedded-node shielded wallet paths unchanged.
4. Do not enable addressed shielded-to-shielded thin-client transfer until the one-note unshield path has a sanity log and review.

## Test Plan

Daemon:

- pure witness builder returns 32 siblings and root for a synthetic tree;
- witness builder rejects `leaf_index >= tree_size`;
- RPC returns `anchor_mismatch` for wrong client root;
- RPC response path verifies back to root;
- replay implementation works after daemon restart from frontier-only state;
- malformed historical bundle returns structured error, not a bogus witness.

DineroDPI:

- note-store migration adds `d`, `commitment`, pending-spend fields;
- legacy rows are repaired by output-feed replay;
- real nullifier matches daemon vector for `DeriveNoteSpendKey(rcm), leaf_index`;
- witness JSON parser validates exactly 32 siblings;
- witness verification rejects wrong root, wrong commitment, wrong leaf index;
- native prover builds a bundle accepted by daemon `ValidateShieldedBundle`;
- one-note unshield regtest: shield to iOS-recognizable note, sync, witness, prove, broadcast, mine, note marked spent;
- restart with pending spend clears or confirms state correctly;
- reorg of mined spend restores spendability if the nullifier disappears.
