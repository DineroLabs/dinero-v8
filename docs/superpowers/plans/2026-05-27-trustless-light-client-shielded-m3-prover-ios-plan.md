# Trustless Light-Client Shielded Spend - Prover/iOS Follow-up Plan

**Status:** Draft
**Date:** 2026-05-27
**Scope:** post-witness M3 implementation: native prover kit, DineroDPI spend substrate, one-note unshield UX
**Primary engineering sites:** dinero-v8, DineroDPI
**Depends on:** M1 receive primitives, T8 block/bundle wiring, M2 output feed, M3 witness RPC

## Current State

The daemon witness lane is complete and deployed:

- M3 design/umbrella plan merged in dinero-v8 PR #163 at `e1c20692a53ab9ace6d810b3d251a56877285493`.
- M3 witness RPC merged in dinero-v8 PR #164 at `baa12e93d8478838e489f540ad20e3210884805a`.
- Fleet deploy completed for `baa12e93d8478838e489f540ad20e3210884805a` on LA, VA, MO, and CN.
- Deployed RPC contract is `shielded.witness.by_index` / `shieldedwitnessbyindex`.
- Response field for the 32-level Merkle path is `path`, not `auth_path`.

The next work is not another daemon witness branch unless latency/sanity testing finds an issue. The next work is to make iOS capable of locally proving and broadcasting a one-note unshield using the deployed public witness RPC.

## Goal

Ship one-note thin-client unshield from DineroDPI shared-RPC mode:

```text
stored owned note
  -> local anchor selection
  -> shielded.witness.by_index
  -> local witness verification
  -> ShieldedProverKit native bundle build
  -> v6 transaction serialization
  -> broadcast
  -> M2 nullifier feed marks note spent
```

One-note unshield remains the first user-visible spend path. Multi-note, change, and addressed shielded-to-shielded transfer are follow-ups.

## Review Split

Recommended split:

1. **dinero-v8 PR B: ShieldedProverKit native ABI + packaging + sanity.**
   This PR is the source-of-truth bridge. It must expose the exact daemon primitives iOS needs and prove that the native library can build a daemon-valid one-note unshield bundle.

2. **DineroDPI PR C1: spend substrate without UI.**
   Store migration, note repair, real nullifiers, witness client, witness verification, native wrapper, and transaction builder tests. No user-facing spend button yet.

3. **DineroDPI PR C2: one-note unshield UX.**
   Enable spend only after C1 has a valid end-to-end fixture and iPhone Release timing. This keeps UI review out of the cryptographic correctness review.

If schedule pressure demands fewer PRs, merge C1 and C2 only after C1's tests are already green as a separate commit group. Do not merge PR B into the iOS PR; the native ABI deserves its own review.

## Non-negotiable Corrections

### 1. Native unshield must carry real `d`

The M3 spec correctly requires iOS to persist each note's decrypted diversifier `d`. Source review found the current daemon pure helper still hard-codes a zero diversifier inside `BuildUnshieldBundleForTx`:

- `include/wallet/shielded_wallet_ops.h`: `UnshieldNoteInput` has no `d` field.
- `src/wallet/shielded_wallet_ops.cpp`: `BuildUnshieldBundleForTx` sets `sh::Hash diversifier{};` before building `SpendWitness`.

That is acceptable only for legacy zero-diversifier notes. Thin-client notes discovered from real shielded addresses need their actual `d`, because `NoteCommitment(d, pk_note, value, rcm)` is address-bound. PR B must extend the pure unshield input/ABI to carry `d` and pass it into `SpendWitness`.

Required test: build a spend for a non-zero `d` note whose commitment is `NoteCommitment(d, pk_note, value, rcm)`. The old hard-coded-zero path must fail validation or fail the commitment/path check; the corrected path must validate.

### 2. Swift must not hand-roll consensus byte layouts

Swift may own app orchestration, storage, and transaction assembly, but the following layouts must come from native helpers or daemon-pinned vectors:

- `DeriveNoteSpendKey(rcm)`
- `ComputeNullifier(DeriveNoteSpendKey(rcm), leaf_index)`
- `NoteCommitment(d, pk_note, value, rcm)`
- `ComputeShieldedTxSighash(tx)`
- serialized one-note unshield bundle bytes

The prior scalar-normalization and 96-byte feed issues came from trusting plausible local assumptions. M3 should make the daemon-native implementation the executable spec.

### 3. Spend enablement is capability-gated

Thin spend is enabled only when all are true:

- endpoint supports `shielded.witness.by_index`;
- local shielded sync has a usable anchor at or after the selected note;
- note row has `d`, commitment, `rcm`, value, and leaf index;
- witness verifies against the local anchor root;
- `ShieldedProverKit` loads and passes startup self-test;
- one-note unshield build is under the hard performance gate on physical iPhone Release.

Rows missing `d` or commitment remain visible for balance/history but are spend-disabled until repair/rescan.

## PR B: ShieldedProverKit Native ABI

### Branch

Suggested branch: `codex/m3-shielded-proverkit`

### Files

Expected dinero-v8 files:

- Create `include/shielded_prover_kit/shielded_prover_kit.h`
- Create `src/shielded_prover_kit/`
- Modify CMake to build a static library target suitable for iOS device and simulator slices.
- Add C ABI tests under `src/test/`.
- Add packaging script under `scripts/`, for example `scripts/build-shielded-proverkit-xcframework.sh`.
- Add or update sanity log under `docs/superpowers/plans/`.

### ABI

Public C ABI should be narrow, stable, and C-compatible. No C++ exceptions, wallet DB types, daemon runtime types, or global wallet state cross this boundary.

Minimum ABI:

```c
typedef struct {
    uint8_t rcm[32];
    uint8_t d[32];
    uint64_t leaf_index;
    uint64_t value_una;
    uint8_t anchor[32];
    uint8_t merkle_path[32][32];
} dinero_shielded_spend_note;

typedef struct {
    uint8_t version;
    const uint8_t* serialized_unsigned_tx;
    size_t serialized_unsigned_tx_len;
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

int dinero_shielded_compute_note_commitment(
    const uint8_t d[32],
    const uint8_t rcm[32],
    uint64_t value_una,
    uint8_t out_commitment[32]);

int dinero_shielded_compute_nullifier(
    const uint8_t randomness[32],
    uint64_t leaf_index,
    uint8_t out_nullifier[32]);

void dinero_shielded_free_result(dinero_shielded_unshield_result* out);
```

Implementation notes:

- `dinero_shielded_compute_nullifier` accepts `randomness`/`rcm`, derives `sk_note = DeriveNoteSpendKey(rcm)`, then computes `ComputeNullifier(sk_note, leaf_index)`.
- `dinero_shielded_compute_note_commitment` derives `pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0)`, encodes `value_una` with the daemon's big-endian scalar layout, then calls `NoteCommitment(d, pk_note, value, rcm)`.
- `dinero_shielded_build_unshield_bundle` derives the per-note spend secret from `rcm` internally before filling the daemon's `UnshieldNoteInput`/`SpendWitness`; Swift should not persist or pass a separate note spend secret.
- `dinero_shielded_build_unshield_bundle` should parse the canonical unsigned transaction envelope and call the daemon's `ComputeShieldedTxSighash` internally. Swift must not reproduce the binding preimage.
- The returned `bundle_bytes` must be exactly the `SerializeShieldedBundle` output iOS attaches to the transaction.
- All returned memory is freed through `dinero_shielded_free_result`.
- Secret stack/storage buffers are cleansed on failure and success where practical.

The ABI can expose lower-level helpers for testing, but the Swift production path should call a single "build unshield bundle" function after witness verification.

### Native Tests

Required tests:

- C ABI computes nullifier matching `DeriveNoteSpendKey(rcm)` + `ComputeNullifier(sk_note, leaf_index)`.
- C ABI computes commitment matching `NoteCommitment(d, pk_note, value, rcm)` for non-zero `d`.
- One-note unshield bundle validates through the existing daemon validation path.
- Mutating recipient output value after proving fails binding-signature validation.
- Mutating recipient script after proving fails binding-signature validation.
- Mutating fee after proving fails validation.
- Wrong Merkle path or wrong anchor fails proof generation or validation.
- `fee_una >= value_una` returns an explicit error.
- ABI failure does not throw across C and leaves result free-safe.

### Packaging

Build artifacts:

```text
artifacts/ShieldedProverKit.xcframework
artifacts/ShieldedProverKit.xcframework.zip
artifacts/ShieldedProverKit.sha256
```

Required slices:

- iOS device arm64
- iOS simulator arm64

Validation:

```bash
find artifacts/ShieldedProverKit.xcframework -type f | sort
shasum -a 256 artifacts/ShieldedProverKit.xcframework.zip
lipo -info path/to/device/libShieldedProverKit.a
lipo -info path/to/simulator/libShieldedProverKit.a
```

The artifact hash goes in the PR description and the M3 sanity log. Do not paste shortened hashes in copyable docs.

### PR B Verification

Suggested local verification:

```bash
cmake -S /private/tmp/dinero-v8-m3-proverkit -B /private/tmp/dinero-v8-m3-proverkit-build
cmake --build /private/tmp/dinero-v8-m3-proverkit-build --target dinerod dinero-cli -j
ctest --test-dir /private/tmp/dinero-v8-m3-proverkit-build --output-on-failure -R 'ShieldedWitness|ShieldedProver|Shielded.*Circuit|Shielded.*Validation'
scripts/build-shielded-proverkit-xcframework.sh
```

Sanity log must record:

- valid witness RPC latency against a real non-empty fixture, not only the empty-anchor smoke;
- bundle build wall-clock on the build host;
- artifact hash;
- whether non-zero `d` fixture passed.

## PR C1: DineroDPI Spend Substrate

### Branch

Suggested branch: `codex/m3-shielded-spend-substrate`

### Files

Expected DineroDPI files:

- Modify `DineroDPI/DineroDPI/Core/Shielded/ShieldedNote.swift`
- Modify `DineroDPI/DineroDPI/Core/Shielded/ShieldedNoteStore.swift`
- Modify `DineroDPI/DineroDPI/Core/Shielded/ShieldedScanner.swift`
- Create `DineroDPI/DineroDPI/Core/Shielded/ShieldedWitness.swift`
- Create `DineroDPI/DineroDPI/Core/Shielded/ShieldedWitnessClient.swift`
- Create `DineroDPI/DineroDPI/Core/Shielded/ShieldedWitnessVerifier.swift`
- Create `DineroDPI/DineroDPI/Core/Shielded/ShieldedProverKit.swift`
- Create `DineroDPI/DineroDPI/Core/Shielded/ShieldedSpendBuilder.swift`
- Add or extend pending-spend storage in `ShieldedNoteStore`.
- Add tests under `DineroDPITests/Shielded/`.

### Store Migration

Add persisted fields:

- `d` as 11 raw diversifier bytes or 32-byte packed form; choose one canonical storage shape and document it.
- `commitment` as 32-byte block-supplied commitment.
- `real_nullifier` as 32-byte daemon-equivalent nullifier.
- `pending_spend_txid`.
- `pending_spend_created_at`.
- `spent_in_block_hash`.
- `spent_height`.
- `spend_disabled_reason` for repair failures.

Migration rules:

- New rows from receive scan store `d` immediately from decrypted note plaintext.
- New rows store the block-supplied commitment from M2 feed/T8 parser, not a recomputed placeholder.
- Existing rows are repaired by replaying M2 output feed around the note's height/leaf index and re-trial-decrypting candidate encrypted notes.
- If repair cannot recover `d` and commitment, keep note visible but spend-disabled.

### Witness Client

Request:

```json
{
  "leaf_index": 0,
  "anchor_height": 0,
  "anchor_root": "64 lowercase hex characters"
}
```

Parser rules:

- Accept only 32-byte `commitment`.
- Accept only 32 `path` entries.
- Reject `auth_path` as a response field in production code; the deployed RPC uses `path`.
- Treat structured daemon errors as unavailable witness, not as proof material.
- Do not query archived wallets in M3; active wallet only.

### Witness Verification

Before calling `ShieldedProverKit`, iOS must:

- recompute commitment with native helper using stored `d`, value, and `rcm`;
- compare recomputed commitment to stored commitment;
- compare stored commitment to RPC commitment;
- verify the 32-sibling path from the commitment and leaf index to the selected local anchor root;
- confirm local shielded sync has reached `anchor_height`;
- refuse while endpoint divergence is active.

### Native Wrapper

`ShieldedProverKit.swift` should:

- load and call the C ABI;
- convert Swift `Data` fields to fixed 32-byte buffers with exact length checks;
- expose typed errors rather than raw integer status codes;
- free native result memory on every path;
- run a startup self-test against a bundled tiny vector before enabling spend controls.

### Transaction Builder

`ShieldedSpendBuilder` builds a one-note unshield transaction:

- version 6 unless daemon validation proves v5 is required;
- no transparent inputs;
- one transparent output to the selected recipient;
- explicit fee set;
- shielded bundle bytes from native kit;
- txid computed from the final transaction serialization;
- broadcast through the existing NodeKit/NodeWalletRPC path.

The transaction must not mutate after bundle build. If UI changes amount, fee, recipient, locktime, or version, discard the bundle and rebuild.

### Pending Spend State

States:

- confirmed unspent;
- pending spent;
- confirmed spent;
- spend-disabled repair failure.

Rules:

- On successful build plus accepted broadcast, set `pending_spend_txid`.
- Pending-spent notes are excluded from selection.
- When M2 feed reports the note nullifier in a mined block, clear pending and set confirmed-spent fields.
- If broadcast fails, clear pending immediately.
- On restart, check pending txids. If tx is unknown and no mined nullifier exists, clear pending.
- On reorg, if the mined nullifier disappears, clear confirmed-spent state; if the tx is still in mempool, keep pending.

### PR C1 Verification

Suggested simulator verification:

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj \
  -scheme DineroDPI \
  -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPad (A16)' \
  test -skip-testing:DineroDPIUITests
```

Required tests:

- migration adds `d`, commitment, nullifier, pending fields;
- repair succeeds for a daemon-generated non-zero `d` note;
- repair failure leaves note visible but spend-disabled;
- nullifier matches native helper and daemon vector;
- witness parser rejects wrong path count;
- witness verifier rejects wrong root, wrong commitment, wrong leaf index, and endpoint divergence;
- wrapper frees native results on success and failure;
- spend builder refuses to build when note is pending, spent, unrepaired, or fee exceeds value;
- transaction mutation after bundle build is caught by validation fixture.

## PR C2: One-note Unshield UX

### Branch

Suggested branch: `codex/m3-one-note-unshield-ux`

### Files

Expected DineroDPI files:

- Modify `DineroDPI/DineroDPI/Views/WalletView.swift` or the current shielded view after UX reconciliation.
- Modify send/unshield flow only enough to expose one-note unshield.
- Add focused UI/model tests where existing project patterns support it.

### UX Rules

- Show unshield only for active wallet rows with spend-capable note state.
- Keep embedded-node shielded RPC UX unchanged.
- In shared-RPC mode, explain receive-only vs spend-capable state through concise status text.
- During proving, show a blocking progress sheet; spend is explicit user action.
- On success, mark pending immediately and show txid.
- On failure, leave funds spendable unless native kit or broadcast has returned an accepted txid.

No multi-note selection, no shielded-to-shielded addressed transfer, no memo sending.

### Physical iPhone Gate

Hard gate:

- one-note unshield build under 3 seconds on physical iPhone Release.

Preferred command shape:

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj \
  -scheme DineroDPI \
  -configuration Release \
  -destination 'id=00008130-0008384C20FA8D3A' \
  test \
  -only-testing:DineroDPITests/ShieldedProverKitTests/test_perf_oneNoteUnshieldBuild \
  -skip-testing:DineroDPIUITests \
  ENABLE_TESTABILITY=YES \
  -allowProvisioningUpdates
```

The measured number goes into the M3 sanity log before C2 leaves draft.

## End-to-End Acceptance

M3 is complete only when a regtest or controlled mainnet-like fixture proves:

1. DineroDPI receives a shielded note through M2 output feed.
2. Store row contains value, `rcm`, `d`, commitment, leaf index, and real nullifier.
3. DineroDPI selects a local anchor and requests `shielded.witness.by_index`.
4. DineroDPI verifies the witness path to its local anchor.
5. Native kit builds an unshield bundle.
6. DineroDPI broadcasts the final transaction.
7. Daemon accepts the transaction.
8. After mining, M2 nullifier feed marks the note spent.
9. Restart preserves pending/confirmed-spent state.
10. Reorg of the spend block restores spendability unless the tx remains in mempool.

## Stop/Go Gates

Stop and fix before UI if any are true:

- PR B cannot prove a non-zero `d` note.
- iOS needs to duplicate binding-sighash logic in Swift.
- witness response cannot be verified against a local anchor.
- store repair leaves spendable rows without `d` or commitment.
- physical iPhone Release one-note proving exceeds 3 seconds.
- transaction accepted by tests only because daemon validation is bypassed.

Go to C2 only after PR B and C1 have green tests and a written sanity line for the native bundle build.

## Roadmap After One-note Unshield

After C2 ships:

- multi-note unshield;
- shielded self-transfer with change;
- addressed shielded-to-shielded transfer;
- decoy/padded witness fetches for endpoint metadata hardening;
- witness replay sidecar cache if production latency requires it.
