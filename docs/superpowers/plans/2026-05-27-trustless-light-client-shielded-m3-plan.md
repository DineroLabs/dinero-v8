# Trustless Light-Client Shielded Spend - M3 Implementation Plan

**Goal:** Implement the first thin-client shielded spend path described in `docs/superpowers/specs/2026-05-27-trustless-light-client-shielded-m3-spend-design.md`.

M3 delivers one-note unshield from DineroDPI shared-RPC mode:

```text
local owned note
  -> local anchor selection
  -> daemon public witness RPC
  -> local witness verification
  -> local ShieldedProverKit bundle build
  -> normal transaction broadcast
  -> M2 nullifier feed marks note spent
```

**Scope:**

- dinero-v8 daemon witness RPC
- dinero-v8 native `ShieldedProverKit.xcframework` C ABI
- DineroDPI witness client, note-store migration, nullifier fix, one-note unshield builder
- sanity log with daemon and physical-iPhone timings

**Post-witness follow-up:** Tasks 1-2 shipped in PR #164 and were deployed to the fleet at
`baa12e93d8478838e489f540ad20e3210884805a`. The active prover/iOS implementation
slice is now tracked in
`docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m3-prover-ios-plan.md`.

**Out of scope:**

- multi-note spends
- shielded-to-shielded addressed transfer
- memo sending
- hosted light-wallet services
- view-key sharing
- consensus changes

**Branch suggestion:**

- docs PR branch: `codex/m3-shielded-spend-plan`
- daemon implementation branch: `feature/m3-shielded-witness-rpc`
- iOS implementation branch: `feature/m3-shielded-thin-spend`

**Signing:** all commits signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File Map

Expected daemon files:

- Create: `include/consensus/shielded/shielded_witness.h`
- Create: `src/consensus/shielded/shielded_witness.cpp`
- Create: `src/test/shielded_witness_tests.cpp`
- Modify: `src/consensus/shielded/CMakeLists.txt`
- Modify: `src/rpc/methods_blockchain_context.cpp`
- Create: `src/shielded_prover_kit/` or equivalent C ABI target
- Create: `include/shielded_prover_kit/shielded_prover_kit.h`
- Create: `docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m3-sanity.md`

Expected DineroDPI files:

- Modify: `Core/Shielded/ShieldedNote.swift`
- Modify: `Core/Shielded/ShieldedNoteStore.swift`
- Modify: `Core/Shielded/ShieldedScanner.swift`
- Create: `Core/Shielded/ShieldedWitness.swift`
- Create: `Core/Shielded/ShieldedWitnessClient.swift`
- Create: `Core/Shielded/ShieldedSpendBuilder.swift`
- Create: `Core/Shielded/ShieldedPendingSpendStore.swift` or extend `ShieldedNoteStore`
- Create: `Core/Shielded/ShieldedProverKit.swift`
- Modify: wallet send UI to expose one-note unshield only when capability checks pass
- Add tests under `DineroDPITests/Shielded/`

---

## Pre-flight

1. Verify both repos are at the expected merged state.

```bash
git -C /Users/haydarevich/src/dinero-v8 fetch origin dinero-main
git -C /Users/haydarevich/src/dinero-v8 log -1 --oneline origin/dinero-main
git -C /Users/haydarevich/src/apps/DineroDPI fetch origin main
git -C /Users/haydarevich/src/apps/DineroDPI log -1 --oneline origin/main
```

Expected anchors:

- dinero-v8 includes `922c7e00` M2 output feed.
- DineroDPI includes `dcc63bec` shielded receive-note UI.

2. Confirm daemon full build/test baseline if the local machine is suitable:

```bash
cmake -S /Users/haydarevich/src/dinero-v8 -B /private/tmp/dinero-v8-m3-baseline-build
cmake --build /private/tmp/dinero-v8-m3-baseline-build --target dinerod dinero-cli -j
ctest --test-dir /private/tmp/dinero-v8-m3-baseline-build --output-on-failure -R 'shielded'
```

3. Confirm DineroDPI test baseline:

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj \
  -scheme DineroDPI \
  -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPad (A16)' \
  test -skip-testing:DineroDPIUITests
```

---

## Task 1: Daemon Pure Witness Builder

**Files:**

- Create `include/consensus/shielded/shielded_witness.h`
- Create `src/consensus/shielded/shielded_witness.cpp`
- Modify `src/consensus/shielded/CMakeLists.txt`
- Create `src/test/shielded_witness_tests.cpp`

API shape:

```cpp
namespace dinero::consensus::shielded {

struct ShieldedWitnessRequest {
    uint64_t leaf_index = 0;
    uint32_t anchor_height = 0;
    Hash anchor_root{};
    uint32_t shielded_activation_height = 0;
};

struct ShieldedWitness {
    uint64_t leaf_index = 0;
    uint32_t anchor_height = 0;
    uint64_t tree_size = 0;
    Hash anchor_root{};
    Hash commitment{};
    CommitmentTree::AuthPath auth_path{};
};

enum class ShieldedWitnessError : uint8_t {
    Ok = 0,
    MissingBlock = 1,
    BundleDecodeFailed = 2,
    LeafOutOfRange = 3,
    AnchorMismatch = 4,
};

using BlockByHeightLookup = std::function<std::optional<dinero::Block>(uint32_t)>;

ShieldedWitnessError BuildWitnessByIndex(
    const ShieldedWitnessRequest& req,
    BlockByHeightLookup lookup,
    ShieldedWitness* out);

} // namespace dinero::consensus::shielded
```

Implementation:

- Replay blocks from `shielded_activation_height` through `anchor_height`.
- Use `ExtractShieldedOutputFeed` per block.
- Append output commitments to a fresh full `CommitmentTree`.
- Capture the requested commitment when `next_leaf_index == leaf_index`.
- After replay, compare `tree.Root()` to `req.anchor_root`.
- Return `LeafOutOfRange` if `leaf_index >= tree.Size()`.
- Return `AnchorMismatch` if the derived root does not match the client-supplied root.
- Return `BundleDecodeFailed` on malformed historical bundle.

Tests:

- 0-leaf tree rejects leaf 0.
- synthetic 5-leaf tree returns a path that verifies to the expected root.
- wrong anchor root returns `AnchorMismatch`.
- requested leaf past tree size returns `LeafOutOfRange`.
- a malformed bundle in replay returns `BundleDecodeFailed`.
- build after frontier-only state is simulated by using only block replay, not live chainstate `GetAuthPath`.

Commit:

```bash
git add include/consensus/shielded/shielded_witness.h \
        src/consensus/shielded/shielded_witness.cpp \
        src/consensus/shielded/CMakeLists.txt \
        src/test/shielded_witness_tests.cpp
git commit -S -m "feat(shielded): build witnesses by leaf index"
```

---

## Task 2: `shielded.witness.by_index` RPC

**Files:**

- Modify `src/rpc/methods_blockchain_context.cpp`

RPC params:

```json
{
  "leaf_index": 912,
  "anchor_height": 30123,
  "anchor_root": "32-byte-hex"
}
```

Response:

```json
{
  "leaf_index": 912,
  "anchor_height": 30123,
  "anchor_root": "32-byte-hex",
  "tree_size": 1831,
  "commitment": "32-byte-hex",
  "path": ["32-byte-hex", "... 32 entries ..."]
}
```

Implementation notes:

- Register `shielded.witness.by_index`.
- Optional alias: `shieldedwitnessbyindex`.
- Reuse `ReadRpcBlock` and `chain_db->getBlockHashByHeight`.
- Reject `anchor_height > tip_height`.
- Require exactly 32-byte hex for `anchor_root`.
- Surface structured errors:
  - `missing_block`
  - `bundle_decode_failed`
  - `leaf_out_of_range`
  - `anchor_mismatch`
- Do not return wallet data. This RPC is public chain data only.

Tests:

- Direct handler test with fixture blocks, if existing RPC mock shape is light enough.
- Otherwise shell regtest after Task 5's fixture scaffolding.

Commit:

```bash
git add src/rpc/methods_blockchain_context.cpp
git commit -S -m "feat(rpc): add shielded witness-by-index RPC"
```

---

## Task 3: ShieldedProverKit Native ABI

**Files:**

- Create native target folder, for example `src/shielded_prover_kit/`
- Create public C header `include/shielded_prover_kit/shielded_prover_kit.h`
- Modify CMake to build a static library suitable for `xcodebuild -create-xcframework`
- Add C++ unit tests for the C ABI

Expose a narrow C ABI that wraps existing C++ primitives:

- `DeriveNoteSpendKey(rcm)`
- `ComputeNullifier(secret_key, leaf_index)`
- `ComputeShieldedTxSighash` from a canonical transparent envelope or serialized transaction
- `BuildUnshieldBundleForTx` equivalent using:
  - note secret key
  - randomness
  - `d`
  - value
  - leaf index
  - anchor
  - 32-level auth path
  - fee
  - tx sighash

Rules:

- ABI owns and frees returned `bundle_bytes` through a matching free function.
- Error strings are heap-allocated by the kit and freed by the kit.
- No C++ exceptions cross the C ABI.
- No wallet DB or daemon globals cross the ABI.
- Secret material is cleansed before returning on failure.

Tests:

- C ABI builds an unshield bundle that `DeserializeShieldedBundle` accepts.
- `ValidateShieldedBundle` accepts the returned bundle for the same transparent envelope.
- Mutating the transparent output after bundle build fails binding-sig validation.
- Wrong auth path fails proof generation or validation.
- fee >= note value returns an explicit error.

Commit:

```bash
git add include/shielded_prover_kit/shielded_prover_kit.h \
        src/shielded_prover_kit \
        <cmake files> \
        <tests>
git commit -S -m "feat(shielded): expose native prover kit ABI"
```

---

## Task 4: Build `ShieldedProverKit.xcframework`

**Files:**

- Add or extend release/build script under `scripts/`
- Add DineroDPI vendored library update in the iOS repo, not dinero-v8, after this task has a built artifact

Artifacts:

- iOS device slice
- iOS simulator arm64 slice
- public module/header map

Suggested script output:

```text
artifacts/ShieldedProverKit.xcframework
artifacts/ShieldedProverKit.sha256
```

Validation:

```bash
xcodebuild -create-xcframework ...
find artifacts/ShieldedProverKit.xcframework -type f | sort
shasum -a 256 artifacts/ShieldedProverKit.xcframework.zip
```

Commit in dinero-v8:

```bash
git add scripts/<proverkit-build-script> docs/<artifact-note-if-any>
git commit -S -m "build(shielded): add iOS prover kit packaging"
```

DineroDPI vendoring happens in Task 7.

---

## Task 5: Daemon Integration Fixture + Sanity Seed

**Files:**

- Add shell regtest or extend existing shielded RPC fixture
- Add initial M3 sanity log file

Fixture flow:

1. Start regtest `dinerod`.
2. Mine spendable transparent funds.
3. Create a shielded output that the fixture can identify.
4. Call `blockchain.shielded.outputs` and record the output leaf index.
5. Call `shielded.witness.by_index` with correct anchor root.
6. Verify response has 32 siblings and the returned commitment.
7. Call with wrong anchor root and assert structured failure.

Sanity log records:

- witness RPC latency at current regtest size;
- witness replay path behavior after daemon restart;
- response byte size;
- any skipped performance numbers to be filled after iPhone.

Commit:

```bash
git add tests/<fixture> docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m3-sanity.md
git commit -S -m "test(shielded): cover witness RPC fixture"
```

At this point daemon PR can be opened and reviewed independently. Do not start iOS spend UI before the witness RPC shape is review-stable.

---

## Task 6: DineroDPI Store Migration + Real Nullifiers

**Files:**

- Modify `Core/Shielded/ShieldedNote.swift`
- Modify `Core/Shielded/ShieldedNoteStore.swift`
- Modify `Core/Shielded/ShieldedScanner.swift`
- Add tests

Schema additions:

- `diversifier BLOB(11)`
- `commitment BLOB(32)`
- `pending_spend_txid BLOB(32)`
- `pending_spend_at INTEGER`

Scanner changes:

- Store plaintext `d`.
- Store block-supplied `commitment`.
- Replace placeholder nullifier with:

```swift
skNote = Poseidon.hash(plaintext.rcm, DstToHash("DIN/v7/shielded/sk_note/v1"))
nullifier = Poseidon.hash(skNote, scalarBytes(leafIndex))
```

Migration:

- Add nullable columns.
- For existing rows missing `diversifier` or `commitment`, run a repair scan over M2 output feed for the row's block/leaf.
- Until repaired, mark note not spendable but keep it in balance/history.

Tests:

- migration preserves existing rows;
- new scans persist `d` and `commitment`;
- nullifier vector matches daemon `DeriveNoteSpendKey(rcm)` + `ComputeNullifier`;
- unrepaired legacy row is excluded from spend selection but not balance.

Commit:

```bash
git add DineroDPI/DineroDPI/Core/Shielded/ShieldedNote.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedNoteStore.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedScanner.swift \
        DineroDPI/DineroDPITests/Shielded/<tests>
git commit -S -m "fix(shielded): persist spend material and real nullifiers"
```

---

## Task 7: DineroDPI Witness Client + Verification

**Files:**

- Create `Core/Shielded/ShieldedWitness.swift`
- Create `Core/Shielded/ShieldedWitnessClient.swift`
- Extend `CommitmentTree` helpers if needed
- Add tests

Implementation:

- Call `shielded.witness.by_index`.
- Parse and validate exactly 32 path elements.
- Verify:
  - RPC commitment equals local recomputed commitment;
  - path root equals locally selected anchor root;
  - anchor height is not ahead of local shielded sync;
  - note is not spent/pending.

Anchor selection:

- Prefer the current local tree root if sync is caught up and no endpoint divergence is active.
- Otherwise choose the newest M1 snapshot lineage root that is recent enough for daemon anchor history.
- Never use a daemon-reported root as the client's source of truth.

Tests:

- happy witness verifies;
- wrong sibling rejects;
- wrong commitment rejects;
- wrong root rejects;
- malformed path length rejects.

Commit:

```bash
git add DineroDPI/DineroDPI/Core/Shielded/ShieldedWitness.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedWitnessClient.swift \
        DineroDPI/DineroDPITests/Shielded/<tests>
git commit -S -m "feat(shielded): verify spend witnesses locally"
```

---

## Task 8: DineroDPI ProverKit Integration

**Files:**

- Vendor `ShieldedProverKit.xcframework`
- Create `Core/Shielded/ShieldedProverKit.swift`
- Add tests

Implementation:

- Thin Swift wrapper around C ABI.
- Convert `StoredShieldedNote + ShieldedWitness + tx_sighash + fee` into native request.
- Own/free native result buffers.
- Convert native errors into typed Swift errors.
- Cleanse local secret buffers after native call.

Tests:

- loads framework in simulator;
- computes nullifier matching daemon vector;
- builds unshield bundle for a daemon fixture;
- rejects fee >= note value.

Commit:

```bash
git add DineroDPI/Libraries/ShieldedProverKit.xcframework \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedProverKit.swift \
        DineroDPI/DineroDPITests/Shielded/<tests>
git commit -S -m "feat(shielded): integrate native prover kit"
```

---

## Task 9: One-note Unshield Builder + Pending State

**Files:**

- Create `Core/Shielded/ShieldedSpendBuilder.swift`
- Extend `ShieldedNoteStore`
- Modify wallet send UI minimally
- Add tests

Flow:

1. Select smallest unspent confirmed spendable note with `value_una >= amount + fee + dust`.
2. Build transparent recipient output.
3. Compute shielded transaction sighash through ProverKit helper.
4. Fetch witness and verify locally.
5. Build native bundle.
6. Attach bundle bytes.
7. Broadcast transaction through existing RPC client.
8. Mark note pending-spent with txid.
9. M2 output feed later marks confirmed spent by nullifier.

UI:

- Enable only when endpoint reports `shielded.witness.by_index` and ProverKit loads.
- Use a blocking progress sheet while proving.
- On success, show txid and pending state.
- On capability missing, keep receive-only Shielded Notes screen.

Tests:

- coin selection excludes spent and pending notes;
- pending marker is written after successful broadcast;
- broadcast failure clears pending;
- M2 nullifier feed confirms spend;
- app restart recovery clears stale pending tx.

Commit:

```bash
git add DineroDPI/DineroDPI/Core/Shielded/ShieldedSpendBuilder.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedNoteStore.swift \
        DineroDPI/DineroDPI/Views/WalletView.swift \
        DineroDPI/DineroDPITests/Shielded/<tests>
git commit -S -m "feat(shielded): build one-note thin-client unshield"
```

---

## Task 10: End-to-End Regtest + iPhone Sanity

**Files:**

- Add DineroDPI integration fixture if existing test harness can drive local daemon RPC
- Update M3 sanity log in dinero-v8
- Update PR descriptions with measured numbers

Required verification:

Daemon:

```bash
cmake --build <build> --target dinerod dinero-cli <shielded witness/prover tests> -j
ctest --test-dir <build> --output-on-failure -R 'shielded|witness|prover'
```

DineroDPI simulator:

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj \
  -scheme DineroDPI \
  -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPad (A16)' \
  test -skip-testing:DineroDPIUITests
```

Physical iPhone Release:

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

Sanity log must record:

- witness RPC latency;
- witness response size;
- native one-note unshield build time on iPhone Release;
- simulator full test count;
- daemon ctest status;
- known limitations:
  - one-note only;
  - unshield only;
  - no decoy witness requests;
  - no addressed private transfer yet.

Commit sanity update:

```bash
git add docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m3-sanity.md
git commit -S -m "docs(shielded): M3 spend sanity log"
```

---

## PR Split Recommendation

Use three PRs, not one large mixed PR:

1. **dinero-v8 PR A:** witness RPC + tests.
2. **dinero-v8 PR B:** ShieldedProverKit ABI + packaging + sanity.
3. **DineroDPI PR C:** store migration, witness client, prover kit integration, one-note unshield UI.

Reason: witness RPC is reviewable as public chain data; native proving is build/security-sensitive; app spend UX has different risks. Splitting keeps review focused and allows daemon fleet deployment before the app enables spend.
