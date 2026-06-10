# AssumeUTXO Replay Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the availability+count placeholder in `BackgroundValidationWorker` with a real genesis→base replay through `BlockValidator::ConnectBlock` against an isolated UTXO set, recompute the snapshot's content commitment, and flip `replay_performed=true` — making `fully_validated` reachable and the spec's Release Gate satisfiable.

**Architecture:** A shared canonical UTXO-record serializer (extracted from `ExportSnapshot`) defines the commitment: SHA256 over the sorted per-UTXO records. `LoadSnapshot` accumulates that digest while importing and persists it as the *expected commitment* (plus the v3 utreexo root). A new self-contained `AssumeUtxoReplayEngine` owns a fresh `ConsensusUTXOSet` + `BlockValidator` and replays blocks; the existing worker drives it and feeds `AssumeUtxoLifecycle::OnReplayComplete(replay_performed=true, …)`. Integration tests run on regtest (instant `generate` mining, `dumptxoutset`/`loadtxoutset`, no compiled-in anchor for regtest heights — which is exactly the spec's "compromised binary" simulation for the poisoned-snapshot test).

**Tech Stack:** C++17, CMake, GoogleTest (vendored), regtest shell e2e (pattern: `tests/abuse/test_full_assumeutxo_lifecycle.sh`), sqlite `UTXOIndex` metadata.

**Branch:** create `feature/assumeutxo-replay-engine` off `feature/assumeutxo-fatal-lifecycle` (PR #269 — this work depends on the lifecycle; if #269 has merged, branch off `dinero-main` instead). Use superpowers:using-git-worktrees at execution time. First commit: this plan file.

---

## Scope

**In scope:** the replay engine, the commitment plumbing, the four latent fixes parked from PR #269's reviews, the `assumeutxo_bg_stall_timeout` config knob, and regtest e2e tests for spec Required Tests 1 and 4 at integration level.

**Explicit non-goals (do not creep):**
- Wallet-send safe-mode gating (spec Fatal items 3/4 completion) — separate plan; the `fatal_scope_note` honesty disclosure stays until then.
- Flipping the server/installer default to fast bootstrap — a release decision gated on the spec's Release Gate, not code in this plan.
- Background-validation scheduling/throttling — spec non-goal.
- A ChainstateService unit-test fixture — e2e regtest coverage substitutes here; the fixture remains a known gap.

## Ground truth (verified 2026-06-10, branch feature/assumeutxo-fatal-lifecycle)

- Snapshot record layout (deterministic, sorted by `OutPoint`): txid(32) ‖ vout(4) ‖ value.v(8) ‖ script_len(4) ‖ script ‖ height(4) ‖ is_coinbase(1) — `src/daemon/services/chainstate_service.cpp:7775-7818` inside `ExportSnapshot` (7670-7893). File checksum = SHA256 over header+records+v3 section, trailing 32 bytes.
- v3 section carries `utreexo_root` (32B) + serialized forest — `include/consensus/utxo_snapshot.h:72-125`.
- `BlockValidator(IConsensusUTXOSet*)` — `include/consensus/block_validation.h:50`; `ConnectBlock(const Block&, uint32_t height, const uint256& hash, BlockUndo&, std::string& error, CPUBudgetMonitor* = nullptr)` — `src/consensus/block_validation.cpp:611`. Pure state machine over the passed set: **isolated replay is supported by construction**.
- `ConsensusUTXOSet` default-constructible, in-memory, owns the utreexo forest; `GetUTXOs()` → `const std::unordered_map<OutPoint, UTXOEntry>&` (`include/consensus/consensus_utxo_set.h:73, 266`); `GetForest().getCommitment()` → 32-byte root.
- Lifecycle entry point: `AssumeUtxoLifecycle::OnReplayComplete(bool replay_performed, bool commitment_match, const std::string& expected, const std::string& recomputed, uint32_t missing_body_count, TimePoint now)` — `include/daemon/services/assumeutxo_lifecycle.h:77-80`. Production call sites passing `replay_performed=false`: `src/daemon/services/chainstate_service.cpp:11811` (worker) and `:12033` (failure-convergence fallback — stays false there).
- Worker: `BackgroundValidationWorker` `chainstate_service.cpp:11722+` with first-time-only progress (`height_validated` vector) and missing-bodies rescan loop; `VerifyUTXOSetMatch` `:11831-11871` (count + spot-check only).
- Regtest: `mine_blocks_on_demand=true` (`src/consensus/chainparams_impl.cpp:319-427`); `generate` RPC (`src/rpc/methods_mining_extras_vnext.cpp:250-294`); `AssumeUTXORegistry` has only mainnet heights, so regtest snapshots pass load with file-checksum-only verification (`chainstate_service.cpp:8083-8106` falls through on `nullopt`).
- e2e daemon harness patterns: `tests/abuse/test_full_assumeutxo_lifecycle.sh` (dump→load→validate), ctest env-var registration pattern `tests/integration/CMakeLists.txt:419` (`"DINEROD=$<TARGET_FILE:dinerod>"`). **Do NOT copy `cold_start_test.cpp`'s relative `./dinerod` exec** (known harness defect).
- Blocks+undo on disk: flatfiles via `BlockStorage` (`include/storage/block_storage.h`), worker reads via `chain_db_->getBlockHashByHeight(h)` + `getBlockByHash(hash)`.

## File map

| File | Action | Responsibility |
|---|---|---|
| `include/consensus/utxo_set_digest.h` | Create | Canonical record serializer + streaming digest + whole-set digest |
| `src/consensus/utxo_set_digest.cpp` | Create | Implementation |
| `tests/consensus/test_utxo_set_digest.cpp` | Create | Golden bytes, order-independence, field sensitivity |
| `src/daemon/services/chainstate_service.cpp` | Modify | ExportSnapshot uses shared serializer (7775-7818); LoadSnapshot accumulates+persists expected commitment; worker drives replay; loadtxoutset re-entry tighten; pruning-safe restore check |
| `include/daemon/services/assumeutxo_lifecycle.h` / `.cpp` | Modify | Latent fixes (stalled-promotion, stoul guards); new metadata keys |
| `include/daemon/services/assumeutxo_replay.h` | Create | `AssumeUtxoReplayEngine` |
| `src/daemon/services/assumeutxo_replay.cpp` | Create | Implementation |
| `tests/daemon/test_assumeutxo_replay.cpp` | Create | Engine over a deterministic synthetic chain |
| `tests/daemon/test_assumeutxo_lifecycle.cpp` | Modify | Tests for latent fixes |
| `tests/integration/test_assumeutxo_replay_e2e.sh` | Create | Regtest e2e: retirement + poisoned-fatal (spec Required Tests 4 and 1 at integration level) |
| `tests/integration/CMakeLists.txt` | Modify | Register e2e with `DINEROD=$<TARGET_FILE:dinerod>` |
| `tests/CMakeLists.txt` | Modify | Register the two new unit test binaries |
| `CMakeLists.txt` | Modify | Add `src/consensus/utxo_set_digest.cpp` + `src/daemon/services/assumeutxo_replay.cpp` to the libs that own their directories (match how `assumeutxo_lifecycle.cpp` was added to `dinero_core` at ~line 870) |

Build/test commands for every task:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release       # once; submodules: git submodule update --init --recursive
cmake --build build -j8 --target <test-target>
ctest --test-dir build -R <TestName> --output-on-failure
```

---

### Task 1: Canonical UTXO records digest

**Files:**
- Create: `include/consensus/utxo_set_digest.h`
- Create: `src/consensus/utxo_set_digest.cpp`
- Create: `tests/consensus/test_utxo_set_digest.cpp`
- Modify: `tests/CMakeLists.txt` (append a registration block; copy the `test_assumeutxo_lifecycle` block at ~3049 as the pattern, target `test_utxo_set_digest`, sources `tests/consensus/test_utxo_set_digest.cpp src/consensus/utxo_set_digest.cpp`, test name `UtxoSetDigest`, labels `consensus;assumeutxo;digest`)

- [ ] **Step 1: Write the failing test**

Create `tests/consensus/test_utxo_set_digest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <unordered_map>

#include "consensus/utxo_set_digest.h"
#include "consensus/consensus_utxo_set.h"   // OutPoint, UTXOEntry
#include "primitives/uint256.h"

namespace dinero::consensus {

namespace {
UTXOEntry MakeEntry(uint64_t value, uint32_t height, bool coinbase,
                    std::initializer_list<uint8_t> script) {
    UTXOEntry e{};
    e.value.v = value;
    e.height = height;
    e.isCoinbase = coinbase;
    e.scriptPubKey.assign(script);
    return e;
}
OutPoint MakeOutpoint(uint8_t txid_byte, uint32_t vout) {
    OutPoint op{};
    uint8_t raw[32] = {0};
    raw[0] = txid_byte;
    // Adapt to the real OutPoint txid setter — ExportSnapshot reads
    // outpoint.txid.AsUint256().data, so construct via the same type.
    op.txid = decltype(op.txid)(uint256(std::vector<uint8_t>(raw, raw + 32)));
    op.vout = vout;
    return op;
}
}  // namespace

// The digest must be a pure function of CONTENT, independent of map order.
TEST(UtxoSetDigest, OrderIndependent) {
    std::unordered_map<OutPoint, UTXOEntry> a;
    a.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));
    a.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));

    std::unordered_map<OutPoint, UTXOEntry> b;
    b.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));
    b.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));

    EXPECT_EQ(ComputeUtxoRecordsDigest(a).GetHex(),
              ComputeUtxoRecordsDigest(b).GetHex());
}

// Any field change must change the digest.
TEST(UtxoSetDigest, FieldSensitive) {
    std::unordered_map<OutPoint, UTXOEntry> base;
    base.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true, {0x51}));
    const std::string d0 = ComputeUtxoRecordsDigest(base).GetHex();

    auto flip = [&](UTXOEntry e) {
        std::unordered_map<OutPoint, UTXOEntry> m;
        m.emplace(MakeOutpoint(0x01, 0), std::move(e));
        return ComputeUtxoRecordsDigest(m).GetHex();
    };
    EXPECT_NE(d0, flip(MakeEntry(5001, 10, true,  {0x51})));   // value
    EXPECT_NE(d0, flip(MakeEntry(5000, 11, true,  {0x51})));   // height
    EXPECT_NE(d0, flip(MakeEntry(5000, 10, false, {0x51})));   // coinbase
    EXPECT_NE(d0, flip(MakeEntry(5000, 10, true,  {0x52})));   // script
}

// The streaming accumulator over per-record bytes must equal the whole-set
// digest when fed the same records in sorted order (this is what LoadSnapshot
// uses while importing).
TEST(UtxoSetDigest, StreamingMatchesWholeSet) {
    std::unordered_map<OutPoint, UTXOEntry> m;
    m.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));
    m.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));
    m.emplace(MakeOutpoint(0x02, 0), MakeEntry(7500, 12, false, {}));

    StreamingUtxoDigest stream;
    // Feed in sorted-outpoint order, same as snapshot file order.
    std::vector<OutPoint> sorted;
    for (const auto& [op, _] : m) sorted.push_back(op);
    std::sort(sorted.begin(), sorted.end());
    for (const auto& op : sorted) {
        stream.AddRecord(op, m.at(op));
    }
    EXPECT_EQ(stream.Finalize().GetHex(), ComputeUtxoRecordsDigest(m).GetHex());
}

// Golden byte layout: the serializer must produce EXACTLY the snapshot-file
// record encoding (txid32 ‖ vout4 ‖ value8 ‖ script_len4 ‖ script ‖ height4 ‖
// coinbase1, little-endian integers). If this test breaks, the snapshot file
// format changed and the commitment design must be revisited.
TEST(UtxoSetDigest, GoldenRecordBytes) {
    const auto op = MakeOutpoint(0xAB, 7);
    const auto e  = MakeEntry(0x1122334455667788ULL, 0x000000FF, true, {0xDE, 0xAD});
    const std::vector<uint8_t> bytes = SerializeUtxoRecord(op, e);
    ASSERT_EQ(bytes.size(), 32u + 4 + 8 + 4 + 2 + 4 + 1);
    EXPECT_EQ(bytes[0], 0xAB);                       // txid[0]
    EXPECT_EQ(bytes[32], 0x07);                      // vout LE
    EXPECT_EQ(bytes[36], 0x88);                      // value LE low byte
    EXPECT_EQ(bytes[44], 0x02);                      // script_len LE
    EXPECT_EQ(bytes[48], 0xDE);                      // script[0]
    EXPECT_EQ(bytes[50], 0xFF);                      // height LE low byte
    EXPECT_EQ(bytes[54], 0x01);                      // coinbase
}

}  // namespace dinero::consensus

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

(Adapt `MakeOutpoint`'s txid construction to the real `OutPoint`/txid types — read `include/consensus/consensus_utxo_set.h` and how `chainstate_service.cpp:7787` reads `outpoint.txid.AsUint256().data`. Adapt linker libs in the CMake block if `uint256`/sha256 need `dinero_chainstate`.)

- [ ] **Step 2: Run to verify it fails** — `cmake --build build -j8 --target test_utxo_set_digest` → FAIL: `utxo_set_digest.h: No such file`.

- [ ] **Step 3: Implement**

`include/consensus/utxo_set_digest.h`:

```cpp
#pragma once

#include "consensus/consensus_utxo_set.h"
#include "crypto/sha256.h"
#include "primitives/uint256.h"

#include <unordered_map>
#include <vector>

namespace dinero::consensus {

// Canonical per-UTXO record encoding — MUST stay byte-identical to the
// snapshot file's record section (ExportSnapshot): txid(32) ‖ vout(4) ‖
// value.v(8) ‖ script_len(4) ‖ script ‖ height(4) ‖ is_coinbase(1), LE ints.
// This is the unit of the AssumeUTXO content commitment
// (docs/design/assumeutxo-fatal-state-machine.md, Anchor Binding).
std::vector<uint8_t> SerializeUtxoRecord(const OutPoint& outpoint, const UTXOEntry& entry);

// SHA256 over all records in sorted-outpoint order. Pure content function:
// independent of container iteration order.
uint256 ComputeUtxoRecordsDigest(const std::unordered_map<OutPoint, UTXOEntry>& utxos);

// Streaming variant for LoadSnapshot: records arrive already in the file's
// sorted order; feed each and Finalize() once.
class StreamingUtxoDigest {
public:
    void AddRecord(const OutPoint& outpoint, const UTXOEntry& entry);
    void AddRecordBytes(const uint8_t* data, size_t len);  // pre-serialized
    uint256 Finalize();

private:
    crypto::CSHA256 sha_;
};

}  // namespace dinero::consensus
```

`src/consensus/utxo_set_digest.cpp`:

```cpp
#include "consensus/utxo_set_digest.h"

#include <algorithm>
#include <cstring>

namespace dinero::consensus {

std::vector<uint8_t> SerializeUtxoRecord(const OutPoint& outpoint, const UTXOEntry& entry) {
    std::vector<uint8_t> out;
    out.reserve(32 + 4 + 8 + 4 + entry.scriptPubKey.size() + 4 + 1);
    const auto& txid_data = outpoint.txid.AsUint256().data;
    out.insert(out.end(), txid_data, txid_data + 32);
    auto append_le = [&out](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    append_le(&outpoint.vout, sizeof(outpoint.vout));
    append_le(&entry.value.v, sizeof(entry.value.v));
    const uint32_t script_len = static_cast<uint32_t>(entry.scriptPubKey.size());
    append_le(&script_len, sizeof(script_len));
    out.insert(out.end(), entry.scriptPubKey.begin(), entry.scriptPubKey.end());
    append_le(&entry.height, sizeof(entry.height));
    const uint8_t is_coinbase = entry.isCoinbase ? 1 : 0;
    out.push_back(is_coinbase);
    return out;
}

uint256 ComputeUtxoRecordsDigest(const std::unordered_map<OutPoint, UTXOEntry>& utxos) {
    std::vector<OutPoint> sorted;
    sorted.reserve(utxos.size());
    for (const auto& [op, _] : utxos) sorted.push_back(op);
    std::sort(sorted.begin(), sorted.end());

    StreamingUtxoDigest stream;
    for (const auto& op : sorted) {
        stream.AddRecord(op, utxos.at(op));
    }
    return stream.Finalize();
}

void StreamingUtxoDigest::AddRecord(const OutPoint& outpoint, const UTXOEntry& entry) {
    const auto bytes = SerializeUtxoRecord(outpoint, entry);
    sha_.Write(bytes.data(), bytes.size());
}

void StreamingUtxoDigest::AddRecordBytes(const uint8_t* data, size_t len) {
    sha_.Write(data, len);
}

uint256 StreamingUtxoDigest::Finalize() {
    uint256 out;
    sha_.Finalize(out.data);
    return out;
}

}  // namespace dinero::consensus
```

(Match the real `crypto::CSHA256` API — `ExportSnapshot` uses `sha256.Write(ptr, n)` and a Finalize; copy its include + finalize idiom from `chainstate_service.cpp:7748` region. Match `uint256`'s mutable-data accessor.)

- [ ] **Step 4: Run to verify pass** — rebuild + `ctest --test-dir build -R UtxoSetDigest --output-on-failure` → PASS (4 tests).

- [ ] **Step 5: Commit** — `git add include/consensus/utxo_set_digest.h src/consensus/utxo_set_digest.cpp tests/consensus/test_utxo_set_digest.cpp tests/CMakeLists.txt CMakeLists.txt && git commit -m "consensus: canonical UTXO records digest (AssumeUTXO content commitment)"`

---

### Task 2: ExportSnapshot uses the shared serializer

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp:7785-7818` (the per-UTXO write loop)

Byte-identity is the whole point: the file format MUST NOT change.

- [ ] **Step 1: Capture a golden snapshot digest BEFORE the change.** Build dinerod, start a throwaway regtest node (pattern below in Task 8's script — datadir in mktemp, `--regtest`), `generate 20`, `dumptxoutset /tmp/golden_a.dat`, stop node. Record `shasum -a 256 /tmp/golden_a.dat`.

- [ ] **Step 2: Refactor the loop.** Replace the body of the `for (const auto& outpoint : sorted_outpoints)` loop (the seven write/sha pairs at 7785-7818) with:

```cpp
        for (const auto& outpoint : sorted_outpoints) {
            const auto& entry = all_utxos.at(outpoint);
            const std::vector<uint8_t> record =
                consensus::SerializeUtxoRecord(outpoint, entry);
            file.write(reinterpret_cast<const char*>(record.data()),
                       static_cast<std::streamsize>(record.size()));
            sha256.Write(record.data(), record.size());

            exported++;
            if (exported % 10000 == 0) {
                logger_->info("[ExportSnapshot] Exported " + std::to_string(exported) + " UTXOs...");
            }
        }
```

Add `#include "consensus/utxo_set_digest.h"` to the file's include block.

- [ ] **Step 3: Verify byte-identity.** Rebuild dinerod. Repeat Step 1's node with the SAME deterministic chain (regtest from fresh datadir, same `generate 20` — confirm determinism by exporting twice from the same running node first; if exports differ run-to-run due to the header timestamp field, compare only `tail -c +69` of the files, i.e. skip the 68-byte header, and ALSO skip the trailing 32-byte checksum: `tail -c +69 file | ghead -c -32` or python). Expected: record sections byte-identical pre/post refactor. If they differ: STOP, the serializer diverged — fix `SerializeUtxoRecord`, do not adapt the test.

- [ ] **Step 4: Existing snapshot tests still pass.** `ctest --test-dir build -R "AssumeUtxoLifecycle|AssumeUTXOMetadataLifecycle" --output-on-failure` plus run `tests/abuse/test_full_assumeutxo_lifecycle.sh` if it is ctest-registered (check `grep -rn full_assumeutxo tests/`); otherwise run it manually with `DINEROD=$PWD/build/dinerod bash tests/abuse/test_full_assumeutxo_lifecycle.sh`.

- [ ] **Step 5: Commit** — `git add src/daemon/services/chainstate_service.cpp && git commit -m "daemon: ExportSnapshot serializes records via shared canonical serializer (byte-identical)"`

---

### Task 3: LoadSnapshot persists the expected commitment

**Files:**
- Modify: `include/daemon/services/assumeutxo_lifecycle.h` (two new metadata keys next to the existing key family, ~line 17-25):

```cpp
inline constexpr const char* kExpectedCommitmentKey  = "assumeutxo_expected_commitment";
inline constexpr const char* kExpectedUtreexoRootKey = "assumeutxo_expected_utreexo_root";
```

- Modify: `src/daemon/services/chainstate_service.cpp` LoadSnapshot import pass (~7895-8144; find the per-record read loop of the verified second pass — the "Two-Pass Import" where UTXOs are added after checksum verification).

- [ ] **Step 1: Accumulate the records digest during import.** In the pass that reads each UTXO record (after checksum verification — locate the loop that constructs `OutPoint`/`UTXOEntry` from file bytes and inserts into `consensus_utxo_set_`), declare before the loop:

```cpp
    consensus::StreamingUtxoDigest records_digest;
```

and inside the loop, after the record's fields are parsed into `outpoint`/`entry` (and before/independent of insertion):

```cpp
    records_digest.AddRecord(outpoint, entry);
```

(Records are stored in sorted order in the file, so streaming order == canonical order. If the import loop only has raw bytes conveniently, use `AddRecordBytes` over the exact record byte span instead — equivalent by construction.)

- [ ] **Step 2: Persist after successful import**, adjacent to where `SetAssumeUTXOState(..., /*persist_metadata=*/true)` runs on the success path (~8290 region on this branch — grep `SetAssumeUTXOState(header.block_hash`):

```cpp
        // Expected content commitment for the replay engine (spec: Completion
        // Criteria item 3). Recomputed from genesis replay and compared before
        // fully_validated can be entered.
        if (utxo_index_) {
            utxo_index_->SetMetadata(assumeutxo::kExpectedCommitmentKey,
                                     records_digest.Finalize().GetHex());
            utxo_index_->SetMetadata(assumeutxo::kExpectedUtreexoRootKey,
                                     /* v3 utreexo_root hex — from the parsed
                                        SnapshotUtreexoSection; grep how the v3
                                        section is read in this function and
                                        take its utreexo_root field */
                                     snapshot_utreexo_root_hex);
        }
```

Read the v3-section parse in LoadSnapshot to bind `snapshot_utreexo_root_hex` (the 32-byte root as hex). If a v2 (legacy) snapshot has no v3 section, store only the records commitment and skip the root key.

Also: `OperatorReset` (lifecycle) and `Disable()` delete the metadata key family — add the two new keys to BOTH deletion lists in `src/daemon/services/assumeutxo_lifecycle.cpp` (grep `DeleteMetadata(kLcBaseHeightKey)` — two sites) so reset/disable can't strand a stale expected commitment.

- [ ] **Step 3: Unit-test the deletion** (append to `tests/daemon/test_assumeutxo_lifecycle.cpp`):

```cpp
// Reset/disable must not strand a stale expected commitment.
TEST_F(AssumeUtxoLifecycleTest, ResetClearsExpectedCommitmentKeys) {
    utxo_index_->SetMetadata(assumeutxo::kExpectedCommitmentKey, "aa");
    utxo_index_->SetMetadata(assumeutxo::kExpectedUtreexoRootKey, "bb");
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnReplayComplete(true, false, "x", "y", 0, t0_ + 10s);
    ASSERT_TRUE(lc->OperatorReset(assumeutxo::kResetToken));
    EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kExpectedCommitmentKey).has_value());
    EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kExpectedUtreexoRootKey).has_value());
}
```

RED first (keys survive reset before the deletion-list change), then GREEN.

- [ ] **Step 4: Build dinerod + suites green; commit** — `git add -A && git commit -m "daemon: LoadSnapshot persists expected content commitment + utreexo root for replay"` (LoadSnapshot wiring itself is exercised end-to-end in Task 8's e2e — note that in the commit body).

---

### Task 4: Latent lifecycle fixes (stalled-promotion guard, stoul hardening)

**Files:**
- Modify: `src/daemon/services/assumeutxo_lifecycle.cpp` (OnReplayComplete state guard; RestoreFromPersistence stoul sites ~168/173)
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (two new tests)

- [ ] **Step 1: Write the failing tests:**

```cpp
// Spec Allowed Transitions: validation_stalled -> fully_validated is forbidden
// without resumed progress. Promotion requires ValidatingHistory; fatal stays
// reachable from Stalled.
TEST_F(AssumeUtxoLifecycleTest, StalledCannotPromoteDirectlyToFullyValidated) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(5, t0_ + 5s);
    lc->Tick(t0_ + 5s + 1800s);
    ASSERT_EQ(lc->GetState(), State::ValidationStalled);

    // Direct completion from Stalled: refused, state unchanged.
    EXPECT_FALSE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 4000s));
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);

    // Fatal must STILL be reachable from Stalled (mismatch discovered late).
    lc->OnReplayComplete(true, /*commitment_match=*/false, "aa", "bb", 0, t0_ + 4100s);
    EXPECT_EQ(lc->GetState(), State::FatalMismatch);
}

// Hand-corrupted numeric metadata must fail safe (fatal), not crash startup.
TEST_F(AssumeUtxoLifecycleTest, CorruptHeightMetadataGoesFatalNotThrow) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
    }
    utxo_index_->SetMetadata(assumeutxo::kLcBaseHeightKey, "not-a-number");
    auto lc2 = MakeLifecycle();
    EXPECT_NO_THROW(lc2->RestoreFromPersistence(true));
    EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
    EXPECT_NE(lc2->GetStatus(t0_).fatal_reason.find("corrupt"), std::string::npos);
}
```

- [ ] **Step 2: RED** — first test fails (Stalled currently promotes); second test crashes/throws. Quote both.

- [ ] **Step 3: Implement.**

(a) In `OnReplayComplete`, the success promotion path only (AFTER the mismatch check, so fatal stays reachable from Stalled):

```cpp
    if (!commitment_match) {
        EnterFatal(/* unchanged */ ...);
        return false;
    }
    if (missing_body_count > 0) { return false; }
    if (!replay_performed) { return false; }
    if (state_ != State::ValidatingHistory) {
        // Spec: leaving validation_stalled requires actual progress
        // (OnBlockValidated) before completion can be claimed.
        return false;
    }
    state_ = State::FullyValidated;
```

(b) Wrap BOTH `std::stoul` sites in `RestoreFromPersistence` (base height ~173, progress height) in a helper:

```cpp
namespace {
bool ParseU32(const std::string& s, uint32_t& out) {
    try {
        size_t pos = 0;
        unsigned long v = std::stoul(s, &pos);
        if (pos != s.size() || v > UINT32_MAX) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) { return false; }
}
}  // namespace
```

On parse failure: `EnterFatal("corrupt lifecycle metadata: non-numeric height value", TimePoint{}); return;` — consistent with the marker-tamper philosophy (never trust, never crash). NOTE: also harden the matching `std::stoul` in `chainstate_service.cpp`'s restore else-branch (grep `std::stoul` near `kLcBaseHeightKey` usage ~3046) with the same pattern → on failure treat as `chainstate_matches=false`.

- [ ] **Step 4: GREEN** — all lifecycle tests pass (now 19+). Neuter check on (a): revert the new state guard → first test FAILS → restore.

- [ ] **Step 5: Commit** — `git add -A && git commit -m "daemon(assumeutxo): forbid stalled->fully_validated without progress; corrupt metadata fails fatal not crash"`

---

### Task 5: loadtxoutset re-entry tighten + stall-timeout config knob

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` (LoadSnapshot post-mutation OnSnapshotLoaded site ~8362-8377; EnsureAssumeUtxoLifecycle ~1613)
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (one knob test via ctor — already covered by DefaultStallTimeoutIsThirtyMinutes; the knob itself is config plumbing)

- [ ] **Step 1: Re-entry tighten.** At the LoadSnapshot site where `OnSnapshotLoaded` returning false is currently treated as the benign rehydrate case, distinguish: rehydrate (same base hash+height as the lifecycle's current base) stays benign; a DIFFERENT base while lifecycle is in SnapshotLoaded/ValidatingHistory/ValidationStalled must FAIL the load:

```cpp
        EnsureAssumeUtxoLifecycle();
        if (!assumeutxo_lifecycle_->OnSnapshotLoaded(header.block_hash, header.block_height)) {
            const auto st = assumeutxo_lifecycle_->GetStatus(std::chrono::steady_clock::now());
            const bool same_base = (st.snapshot_base_block == header.block_hash &&
                                    st.snapshot_base_height == header.block_height);
            if (!same_base) {
                result.error_message =
                    "another snapshot lifecycle is active (base height " +
                    std::to_string(st.snapshot_base_height) +
                    "); reset or let validation finish before loading a different snapshot";
                logger_->error("[LoadSnapshot] " + result.error_message);
                return result;
            }
            // same base: benign rehydrate, lifecycle state preserved
        }
```

(Adapt `result.error_message`/return idiom to the surrounding `SnapshotImportResult` returns in that function — copy a neighbor.)

- [ ] **Step 2: Config knob.** In `EnsureAssumeUtxoLifecycle()` construct with the spec-named option:

```cpp
        const int stall_timeout_s = config_
            ? config_->GetInt("assumeutxo_bg_stall_timeout", 1800)
            : 1800;
        assumeutxo_lifecycle_ = std::make_unique<assumeutxo::AssumeUtxoLifecycle>(
            utxo_index_.get(), /* logger ptr as on this branch */,
            std::chrono::seconds(std::max(1, stall_timeout_s)));
```

(Check how `config_` is null-guarded + `GetInt` signature against the nearby `assumeutxo_snapshot_max_mb` usage in `methods_blockchain_context.cpp:381` / LoadSnapshot.)

- [ ] **Step 3: Verify** — full dinerod build clean; lifecycle suites green (the 30-min default test pins the fallback). The re-entry tighten is exercised end-to-end in Task 8's e2e (scenario C). Commit: `git add -A && git commit -m "daemon(assumeutxo): refuse loading a different snapshot mid-lifecycle; assumeutxo_bg_stall_timeout config"`

---

### Task 6: AssumeUtxoReplayEngine

**Files:**
- Create: `include/daemon/services/assumeutxo_replay.h`
- Create: `src/daemon/services/assumeutxo_replay.cpp`
- Create: `tests/daemon/test_assumeutxo_replay.cpp`
- Modify: `tests/CMakeLists.txt` (register `test_assumeutxo_replay`, sources include `src/daemon/services/assumeutxo_replay.cpp src/consensus/utxo_set_digest.cpp`, link `dinero_chainstate dinero_wallet gtest sqlite3`, labels `daemon;assumeutxo;replay`)
- Modify: root `CMakeLists.txt` (`src/daemon/services/assumeutxo_replay.cpp` into `dinero_core`, next to `assumeutxo_lifecycle.cpp` ~line 870)

- [ ] **Step 1: Find the deterministic chain builder.** The ConsensusFuzzer ("Phase 2.1 Deterministic Consensus Fuzzer — pure consensus fuzzing, no database") builds valid chains programmatically: locate its source (`grep -rln "Deterministic Consensus Fuzzer" tests/ src/`) and identify the helper that produces consensus-valid `Block`s over a `ConsensusUTXOSet` from genesis (coinbase construction, height progression). Reuse it (include its header or lift the minimal builder into the test). If the fuzzer's builder proves unusable for `BlockValidator::ConnectBlock` (e.g. it bypasses full validation), STOP and report BLOCKED with what you found — do not hand-roll a block builder beyond ~50 lines and do not weaken the test to mocks; Task 8's e2e then becomes the sole replay-correctness gate and this unit test is dropped WITH a written justification in the commit message.

- [ ] **Step 2: Write the failing test** (signatures below assume a `BuildDeterministicChain(uint32_t n)` adapter you write over the fuzzer's builder, returning `std::vector<Block>` valid from genesis):

```cpp
#include <gtest/gtest.h>

#include "daemon/services/assumeutxo_replay.h"
#include "consensus/utxo_set_digest.h"
// + the fuzzer chain-builder header located in Step 1

namespace dinero {

// Replaying N valid blocks must reproduce exactly the digest of a directly
// applied set (same blocks, same order, independent instance).
TEST(AssumeUtxoReplay, ReplayReproducesDirectDigest) {
    const auto chain = BuildDeterministicChain(20);

    // Direct application — the "snapshot creator's" view.
    consensus::ConsensusUTXOSet direct;
    consensus::BlockValidator direct_validator(&direct);
    for (uint32_t h = 0; h < chain.size(); ++h) {
        BlockUndo undo; std::string err;
        ASSERT_TRUE(direct_validator.ConnectBlock(chain[h], h, chain[h].GetHash(), undo, err)) << err;
    }
    const std::string expected = consensus::ComputeUtxoRecordsDigest(direct.GetUTXOs()).GetHex();

    // Replay engine — the verifier's view.
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    for (uint32_t h = 0; h < chain.size(); ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h], h, chain[h].GetHash(), err)) << err;
    }
    EXPECT_EQ(engine.RecordsDigestHex(), expected);
    EXPECT_EQ(engine.Height(), chain.size() - 1);
}

// A tampered block must fail ConnectBlock with a non-empty error, and the
// engine must report the failure height.
TEST(AssumeUtxoReplay, TamperedBlockFailsValidation) {
    auto chain = BuildDeterministicChain(10);
    // Corrupt block 5's first transaction output value (adapt the field path
    // to the real Block/Transaction layout).
    chain[5].transactions[0].outputs[0].value.v += 1;

    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    for (uint32_t h = 0; h < 5; ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h], h, chain[h].GetHash(), err)) << err;
    }
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[5], 5, chain[5].GetHash(), err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(engine.Height(), 4u);
}

}  // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

(Tampering block 5 invalidates its hash/PoW-or-commitments — if regtest-style blocks need re-mining after mutation, instead corrupt at the UTXO-effect level: e.g. duplicate-spend by replaying block 4 twice and assert failure. The point is: ConnectBlock must refuse SOMETHING the test can construct; pick the cheapest genuine refusal and name it in a comment.)

- [ ] **Step 3: RED** (header missing), then implement:

`include/daemon/services/assumeutxo_replay.h`:

```cpp
#pragma once

#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utxo_set_digest.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <memory>
#include <string>

namespace dinero::assumeutxo {

// Isolated genesis->base replay for AssumeUTXO background validation
// (docs/design/assumeutxo-fatal-state-machine.md — Completion Criteria).
// Owns a fresh ConsensusUTXOSet + BlockValidator; never touches the live
// chainstate. Single-threaded use by BackgroundValidationWorker.
class AssumeUtxoReplayEngine {
public:
    AssumeUtxoReplayEngine();
    ~AssumeUtxoReplayEngine();

    // Validate + apply one block through the normal connection path.
    // Heights must be fed strictly ascending from 0 (genesis).
    // Returns false with `error` set on validation failure (the spec's
    // "hard validation failure proving the snapshot cannot be trusted"
    // when the block came from the canonical chain below the base).
    bool ConnectAndAdvance(const Block& block, uint32_t height,
                           const uint256& block_hash, std::string& error);

    uint32_t Height() const { return last_height_; }
    uint64_t UtxoCount() const;
    // Canonical content commitment of the replayed set (Task 1 digest).
    std::string RecordsDigestHex() const;
    // Utreexo root of the replayed forest, hex (compare vs v3 snapshot root).
    std::string UtreexoRootHex() const;

private:
    std::unique_ptr<consensus::ConsensusUTXOSet> set_;
    std::unique_ptr<consensus::BlockValidator> validator_;
    uint32_t last_height_ = 0;
    bool any_connected_ = false;
};

}  // namespace dinero::assumeutxo
```

`src/daemon/services/assumeutxo_replay.cpp`:

```cpp
#include "daemon/services/assumeutxo_replay.h"

namespace dinero::assumeutxo {

AssumeUtxoReplayEngine::AssumeUtxoReplayEngine()
    : set_(std::make_unique<consensus::ConsensusUTXOSet>()),
      validator_(std::make_unique<consensus::BlockValidator>(set_.get())) {}

AssumeUtxoReplayEngine::~AssumeUtxoReplayEngine() = default;

bool AssumeUtxoReplayEngine::ConnectAndAdvance(const Block& block, uint32_t height,
                                               const uint256& block_hash,
                                               std::string& error) {
    if (any_connected_ && height != last_height_ + 1) {
        error = "replay heights must be strictly ascending (got " +
                std::to_string(height) + " after " + std::to_string(last_height_) + ")";
        return false;
    }
    BlockUndo undo;  // replay does not persist undo
    if (!validator_->ConnectBlock(block, height, block_hash, undo, error)) {
        return false;
    }
    last_height_ = height;
    any_connected_ = true;
    return true;
}

uint64_t AssumeUtxoReplayEngine::UtxoCount() const { return set_->GetUTXOs().size(); }

std::string AssumeUtxoReplayEngine::RecordsDigestHex() const {
    return consensus::ComputeUtxoRecordsDigest(set_->GetUTXOs()).GetHex();
}

std::string AssumeUtxoReplayEngine::UtreexoRootHex() const {
    const consensus::UtreexoHash root = set_->GetForest().getCommitment();
    uint256 h;
    std::memcpy(h.data, root.data(), 32);
    return h.GetHex();
}

}  // namespace dinero::assumeutxo
```

(Adapt: `BlockUndo` include; `uint256.data` mutability; whether genesis (height 0) needs special handling in ConnectBlock — read how the fuzzer/ConnectTip treat genesis and mirror it; if genesis is pre-applied rather than connected, document it in the header comment and start replay at height 1 in the worker.)

- [ ] **Step 4: GREEN** — `ctest --test-dir build -R AssumeUtxoReplay --output-on-failure` → 2 tests pass.

- [ ] **Step 5: Commit** — `git add -A && git commit -m "daemon: isolated AssumeUTXO replay engine (real ConnectBlock over fresh consensus set)"`

---

### Task 7: Wire the worker to real replay (replay_performed=true)

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` BackgroundValidationWorker (~11722-11825) and its `OnReplayComplete` call (~11811)

This is the plan's payoff commit. The worker's structure (first-time-only progress vector, missing-bodies rescan loop, Tick during waits, should_stop responsiveness) is KEPT; the per-height body changes from "availability scan" to "replay", and the completion changes from count-check to commitment comparison.

- [ ] **Step 1: Per-height: replay instead of scan.** The current loop body retrieves the block and counts it; change the retrieved-block branch to ALSO connect it into the engine. Engine is created at worker start (after EnsureAssumeUtxoLifecycle):

```cpp
        assumeutxo::AssumeUtxoReplayEngine replay;
        bool replay_poisoned = false;        // set when ConnectBlock refuses
        std::string replay_poison_reason;
```

In the per-height body, replace the availability-only handling of a successfully read block:

```cpp
                const Block& blk = block_result.value();
                if (!height_validated[height]) {
                    std::string connect_err;
                    if (!replay.ConnectAndAdvance(blk, height, hash_result.value(), connect_err)) {
                        // A canonical-chain block below the snapshot base failed
                        // real validation: the snapshot's chain is invalid —
                        // spec: hard validation failure => fatal.
                        replay_poisoned = true;
                        replay_poison_reason = "block " + std::to_string(height) +
                                               " failed validation during replay: " + connect_err;
                        break;  // out of the height loop; handled below
                    }
                    height_validated[height] = true;
                    {
                        std::lock_guard<std::mutex> lock(bg_validation_mutex_);
                        bg_validation_blocks_validated_++;
                    }
                    assumeutxo_lifecycle_->OnBlockValidated(height, std::chrono::steady_clock::now());
                }
```

IMPORTANT — restart interaction: the replay engine cannot resume mid-set across daemon restarts (its UTXO set is in-memory only). On worker start, if the durable progress marker (`resume_height` from `GetStatus().current_validation_height`) is > 0, the engine must still replay from genesis — so do NOT pre-mark heights ≤ resume_height as validated this run for REPLAY purposes; pre-marking only suppresses lifecycle progress signals. Restructure: keep the pre-mark vector for *lifecycle progress dedup* (as on this branch), but the replay loop connects EVERY height 0..target in order regardless. Concretely: split the `!height_validated[height]` guard — `ConnectAndAdvance` runs unconditionally for every retrieved height; only the `OnBlockValidated` lifecycle signal stays behind the first-time guard. Document with a comment: "replay state is in-memory; restart replays from genesis, but only genuinely new heights feed the stall clock."

Missing-body handling stays the rescan loop — but with replay, a rescan after missing bodies must RESTART the engine from genesis (heights must ascend): re-create `replay = assumeutxo::AssumeUtxoReplayEngine{};` at the top of each pass of the `while (true)` rescan loop, and reset `replay_poisoned`. (Replaying from 0 each pass is acceptable: passes beyond the first only happen while bodies are missing, and the final pass is the one that completes.)

- [ ] **Step 2: Poison + completion handling after the loop.** After the rescan loop exits with `blocks_skipped == 0` (and before the legacy `VerifyUTXOSetMatch` call):

```cpp
        if (replay_poisoned) {
            assumeutxo_lifecycle_->OnReplayComplete(
                /*replay_performed=*/true, /*commitment_match=*/false,
                "(canonical chain below base)", replay_poison_reason,
                0, std::chrono::steady_clock::now());
            OnBackgroundValidationComplete(false, replay_poison_reason);
            return;
        }

        // Fast pre-check (legacy): UTXO count vs snapshot metadata.
        const bool count_ok = VerifyUTXOSetMatch();

        // Real commitment comparison (spec Completion Criteria items 2-3).
        const auto expected_commitment =
            utxo_index_ ? utxo_index_->GetMetadata(assumeutxo::kExpectedCommitmentKey)
                        : std::nullopt;
        if (!expected_commitment) {
            // Legacy load (pre-commitment binary) — replay ran, but there is
            // nothing trustworthy to compare against. Stay in
            // validating_history; never claim fully_validated. Operator remedy:
            // reload the snapshot with a current binary, or full resync.
            logger_->warning("[BackgroundValidation] no expected commitment persisted "
                             "(snapshot loaded by a pre-replay binary) — cannot retire "
                             "trust assumption; reload snapshot or resync to proceed");
            OnBackgroundValidationComplete(true, "");
            return;
        }

        const std::string recomputed = replay.RecordsDigestHex();
        const bool commitment_match = count_ok &&
            (recomputed == expected_commitment.value());
        // Defense-in-depth: utreexo root comparison when the v3 root was stored.
        bool root_match = true;
        if (auto expected_root = utxo_index_->GetMetadata(assumeutxo::kExpectedUtreexoRootKey)) {
            root_match = (replay.UtreexoRootHex() == expected_root.value());
        }

        assumeutxo_lifecycle_->OnReplayComplete(
            /*replay_performed=*/true,
            commitment_match && root_match,
            expected_commitment.value(),
            recomputed + (root_match ? "" : " (utreexo root mismatch)"),
            /*missing_body_count=*/0, std::chrono::steady_clock::now());

        if (!(commitment_match && root_match)) {
            OnBackgroundValidationComplete(false,
                "replay commitment mismatch at base height " + std::to_string(target_height));
            return;
        }
        OnBackgroundValidationComplete(true, "");
```

Delete the old `OnReplayComplete(/*replay_performed=*/false, …)` call at ~11811 (this block replaces it). The failure-convergence fallback at ~12033 KEEPS `replay_performed=false` (it reports operational failures, not replay verdicts).

- [ ] **Step 3: Memory note + guard.** The replay set holds all UTXOs in memory a second time. At worker start log the expectation and add a soft guard:

```cpp
        logger_->info("[BackgroundValidation] replay engine active: in-memory replay set "
                      "(expected ~" + std::to_string(assumeutxo_base_height_) +
                      " blocks; UTXO count bounded by snapshot)");
```

(No hard cap — YAGNI at current mainnet scale 98k UTXOs; the spec's resource scheduling is a separate doc.)

- [ ] **Step 4: Verify.** Full build clean. ALL existing suites: `ctest --test-dir build -R "AssumeUtxoLifecycle|AssumeUTXOMetadataLifecycle|UtxoSetDigest|AssumeUtxoReplay" --output-on-failure` green. The end-to-end proof is Task 8.

- [ ] **Step 5: Commit:**

```bash
git add src/daemon/services/chainstate_service.cpp
git commit -m "daemon: background validation performs real genesis->base replay

The worker now connects every historical block through BlockValidator
against an isolated consensus set and compares the recomputed records
digest (+ utreexo root) against the commitment persisted at snapshot
load. replay_performed=true: fully_validated is now reachable, and a
commitment mismatch or a block failing real validation goes fatal.
Legacy loads without a persisted commitment remain validating_history
forever (honest: nothing to compare against)."
```

---

### Task 8: Regtest e2e — spec Required Tests 1 and 4 at integration level

**Files:**
- Create: `tests/integration/test_assumeutxo_replay_e2e.sh`
- Modify: `tests/integration/CMakeLists.txt` (register with the `DINEROD=$<TARGET_FILE:dinerod>` env pattern from line ~419; name `AssumeUtxoReplayE2E`, labels `assumeutxo;replay;e2e;integration`, TIMEOUT 300)

- [ ] **Step 1: Write the script.** Model startup/RPC plumbing on `tests/abuse/test_full_assumeutxo_lifecycle.sh` (read it first; reuse its helper functions if it sources a common lib — grep `source ` in it). Structure:

```bash
#!/usr/bin/env bash
# AssumeUTXO replay engine e2e (regtest).
# Spec docs/design/assumeutxo-fatal-state-machine.md Required Tests:
#   Scenario A = Test 4 (good snapshot retires trust marker, survives restart)
#   Scenario B = Test 1 (poisoned snapshot is fatal: load-time gates pass —
#                regtest has no compiled-in anchor, simulating a compromised
#                binary — but genesis replay mismatches)
#   Scenario C = re-entry: loading a DIFFERENT snapshot mid-lifecycle refused
set -euo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
CLI_PORT_A=24101; P2P_PORT_A=24102
CLI_PORT_B=24103; P2P_PORT_B=24104
WORK="$(mktemp -d -t dinero_replay_e2e_XXXXXX)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$WORK"' EXIT

rpc() {  # <port> <method> [params-json]
    local port="$1" method="$2" params="${3:-[]}"
    curl -fsS --max-time 10 \
      -H 'Content-Type: application/json' \
      --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
      "http://127.0.0.1:${port}/" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["result"]))'
}
# NOTE: adapt auth — if the daemon requires the .cookie file (see
# cold_start_test.cpp readCookie), add: -u "$(cat $datadir/.cookie)".

start_node() {  # <datadir> <rpcport> <p2pport> [extra args...]
    local datadir="$1" rpcport="$2" p2pport="$3"; shift 3
    mkdir -p "$datadir"
    "$DINEROD" --regtest --datadir="$datadir" --rpcport="$rpcport" \
               --port="$p2pport" --listen=1 "$@" \
               > "$datadir/daemon.log" 2>&1 &
    # readiness: poll getblockcount
    for i in $(seq 1 60); do
        if rpc "$rpcport" getblockcount >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    echo "FAIL: node on rpc port $rpcport did not become ready" >&2
    tail -30 "$datadir/daemon.log" >&2
    return 1
}

wait_status() {  # <rpcport> <python-predicate-on-dict-s> <timeout_s> <desc>
    local port="$1" pred="$2" timeout="$3" desc="$4"
    for i in $(seq 1 "$timeout"); do
        local st
        st=$(rpc "$port" getsnapshotbootstrapstatus 2>/dev/null || echo '{}')
        if python3 -c "import json,sys; s=json.loads(sys.argv[1]); sys.exit(0 if ($pred) else 1)" "$st"; then
            echo "PASS: $desc"
            return 0
        fi
        sleep 1
    done
    echo "FAIL: timeout waiting for: $desc" >&2
    rpc "$port" getsnapshotbootstrapstatus >&2 || true
    return 1
}

echo "=== Setup: source node mines 60 blocks, exports snapshot at tip ==="
start_node "$WORK/source" "$CLI_PORT_A" "$P2P_PORT_A"
rpc "$CLI_PORT_A" generate '[60]' > /dev/null
SNAP="$WORK/utxo-snapshot.dat"
rpc "$CLI_PORT_A" dumptxoutset "[\"$SNAP\"]" > /dev/null
[ -s "$SNAP" ] || { echo "FAIL: snapshot not written"; exit 1; }

echo "=== Scenario A: good snapshot -> fully_validated -> survives restart ==="
start_node "$WORK/nodeA" "$CLI_PORT_B" "$P2P_PORT_B" \
           "--connect=127.0.0.1:${P2P_PORT_A}" \
           "--assumeutxo_bg_stall_timeout=120"
rpc "$CLI_PORT_B" loadtxoutset "[\"$SNAP\"]" > /dev/null
# Background validation needs bodies genesis->base from the source peer,
# replays them, compares commitments, retires the trust marker.
wait_status "$CLI_PORT_B" \
  's.get("history_fully_validated") == True and s.get("assumeutxo_active") == False and s.get("fatal") == False' \
  180 "history_fully_validated after real replay"

# Restart preserves fully_validated (spec Persistence + Required Test 4).
pkill -f "datadir=$WORK/nodeA" ; sleep 3
start_node "$WORK/nodeA" "$CLI_PORT_B" "$P2P_PORT_B" "--connect=127.0.0.1:${P2P_PORT_A}"
wait_status "$CLI_PORT_B" \
  's.get("history_fully_validated") == True and s.get("fatal") == False' \
  30 "fully_validated survives restart"
pkill -f "datadir=$WORK/nodeA"; sleep 2

echo "=== Scenario B: poisoned snapshot (valid file checksum, wrong content) -> fatal ==="
POISON="$WORK/poisoned.dat"
python3 - "$SNAP" "$POISON" <<'PYEOF'
import hashlib, sys
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
body, _old_checksum = data[:-32], data[-32:]
# Flip one byte inside the first UTXO record's value field
# (header is 68 bytes; record begins at 68; value starts at 68+32+4).
poke = 68 + 32 + 4
body[poke] ^= 0xFF
# Recompute the trailing SHA256 so load-time integrity passes — this is the
# spec's compromised-binary simulation: load gates green, content is wrong.
checksum = hashlib.sha256(bytes(body)).digest()
open(dst, "wb").write(bytes(body) + checksum)
PYEOF

start_node "$WORK/nodeB" "$CLI_PORT_B" "$P2P_PORT_B" \
           "--connect=127.0.0.1:${P2P_PORT_A}" \
           "--assumeutxo_bg_stall_timeout=120"
rpc "$CLI_PORT_B" loadtxoutset "[\"$POISON\"]" > /dev/null  # MUST succeed (gates pass)
wait_status "$CLI_PORT_B" \
  's.get("fatal") == True and s.get("history_fully_validated") == False and "mismatch" in s.get("fatal_reason","")' \
  180 "poisoned snapshot drives fatal_mismatch after replay"

# Restart preserves fatal (spec Required Test 1).
pkill -f "datadir=$WORK/nodeB"; sleep 3
start_node "$WORK/nodeB" "$CLI_PORT_B" "$P2P_PORT_B"
wait_status "$CLI_PORT_B" 's.get("fatal") == True' 30 "fatal survives restart"

echo "=== Scenario B2: explicit operator reset recovers ==="
RES=$(rpc "$CLI_PORT_B" resetassumeutxofatal '[{"confirm":"RESET-ASSUMEUTXO-FATAL"}]')
python3 -c "import json,sys; r=json.loads(sys.argv[1]); assert r.get('reset') is True, r" "$RES"
wait_status "$CLI_PORT_B" 's.get("fatal") == False' 15 "reset clears fatal over RPC"
pkill -f "datadir=$WORK/nodeB"; sleep 2

echo "=== Scenario C: different snapshot refused mid-lifecycle ==="
start_node "$WORK/nodeC" "$CLI_PORT_B" "$P2P_PORT_B" "--assumeutxo_bg_stall_timeout=3600"
# no peer => validation will sit in validating/stalled with bodies missing
rpc "$CLI_PORT_B" loadtxoutset "[\"$SNAP\"]" > /dev/null
rpc "$CLI_PORT_A" generate '[5]' > /dev/null
SNAP2="$WORK/utxo-snapshot-2.dat"
rpc "$CLI_PORT_A" dumptxoutset "[\"$SNAP2\"]" > /dev/null
if rpc "$CLI_PORT_B" loadtxoutset "[\"$SNAP2\"]" 2>/dev/null | grep -qv error; then
    # adapt: the RPC surfaces result["error"]; assert the call REPORTS an error
    echo "FAIL: second loadtxoutset with a different base was not refused" >&2
    exit 1
fi
echo "PASS: different snapshot refused mid-lifecycle"

echo "ALL SCENARIOS PASSED"
```

**Adaptation duties for the implementer (read before running):** (1) RPC auth — copy the working curl/cookie idiom from `tests/abuse/test_full_assumeutxo_lifecycle.sh` verbatim; (2) `loadtxoutset`/`dumptxoutset` exact param shape (positional path vs object) — grep their handlers; (3) error-shape of a refused loadtxoutset for scenario C (the handler returns `result["error_message"]`/`result["error"]` — assert on the actual field); (4) the 68-byte header offset in the poison script — verify against `SnapshotMetadata` serialization order in ExportSnapshot (magic4+version4+hash32+height4+count8+timestamp8+reserved8 = 68; recount the actual field sizes and fix the offset if different); (5) confirm `--assumeutxo_bg_stall_timeout` config name matches Task 5's knob and that `--key=value` CLI form reaches `config_->GetInt` (check how other config flags are passed in existing .sh tests — config file in datadir may be required instead of CLI; `dinero.conf` in the datadir is the known-good pattern from memory: no CLI addnode flag existed).

- [ ] **Step 2: Register in ctest** (`tests/integration/CMakeLists.txt`, copy the env-pattern block at ~419):

```cmake
add_test(NAME AssumeUtxoReplayE2E
         COMMAND ${CMAKE_COMMAND} -E env
                 "DINEROD=$<TARGET_FILE:dinerod>"
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_replay_e2e.sh)
set_tests_properties(AssumeUtxoReplayE2E PROPERTIES
    LABELS "assumeutxo;replay;e2e;integration"
    TIMEOUT 600
)
```

- [ ] **Step 3: Run it** — `ctest --test-dir build -R AssumeUtxoReplayE2E --output-on-failure`. Expected: ALL SCENARIOS PASSED. This is the plan's end-to-end gate: scenario A proves retirement through REAL replay; scenario B proves fatal through real mismatch; B2 proves recovery; C proves the re-entry tighten.

- [ ] **Step 4: NEUTER CHECK (mandatory, this is the keystone):** temporarily revert Task 7's commitment comparison (force `commitment_match = true` regardless) → scenario B MUST FAIL (poisoned snapshot would reach fully_validated). Quote the failure. Revert the neuter via git checkout, rebuild, re-run, ALL SCENARIOS PASSED, git status clean.

- [ ] **Step 5: Commit** — `git add tests/integration/test_assumeutxo_replay_e2e.sh tests/integration/CMakeLists.txt && git commit -m "test(assumeutxo): regtest e2e — replay retires trust marker; poisoned snapshot fatal (spec Required Tests 1+4)"`

---

### Task 9: Pruning-safe fully_validated restore

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` restore else-branch (~3037-3052, the `chain_db_->getBlockHashByHeight` marker check)

Context (PR #269 known gap c): a `fully_validated` marker is re-verified at startup against `getBlockHashByHeight(base)`; if that lookup fails for a NON-tamper reason (pruned height index, cold chaindb), the node would false-positive into fatal. Now that `fully_validated` is reachable, this is live.

- [ ] **Step 1: Determine the actual failure modes.** Read `getBlockHashByHeight`'s implementation (grep in `src/storage/` / chaindb): does it read the `height` column family (survives body pruning) or block files? Does this codebase even support pruning today (`grep -rn "prune" include/ src/ --include=*.h | grep -i mode` — `PruningMode` exists in chainstate_service.h:384-390)? Report findings in the commit body.

- [ ] **Step 2: Make the check fail-safe on lookup ERROR vs fail-FATAL on mismatch:**

```cpp
        bool chainstate_matches = true;   // benign default: lookup unavailable ≠ tamper
        bool lookup_ok = false;
        if (chain_db_ && h > 0) {
            auto hash_result = chain_db_->getBlockHashByHeight(h);
            if (hash_result.ok()) {
                lookup_ok = true;
                chainstate_matches =
                    (hash_result.value() == uint256::FromHexUnsafe(bb.value()));
            } else {
                // Index unavailable (pruned/cold) is NOT evidence of tampering.
                // Keep the marker, log loudly, and let normal startup integrity
                // checks (IsCanonicalStateAligned) catch real divergence.
                logger_->warning("[AssumeUTXO restore] cannot verify fully_validated "
                                 "marker (height index unavailable at " +
                                 std::to_string(h) + "); retaining marker");
            }
        }
        assumeutxo_lifecycle_->RestoreFromPersistence(chainstate_matches);
        (void)lookup_ok;
```

Apply the same hash-COMPARISON-mismatch-is-fatal / lookup-FAILURE-is-benign split to the active-branch check (~2897-2916) if it shares the pattern. Keep the spec rule intact: a SUCCESSFUL lookup that MISMATCHES is still fatal.

- [ ] **Step 3: Verify** — full build; lifecycle suites green; e2e (Task 8) still green (its restart scenarios exercise this path with a healthy index). Commit: `git add -A && git commit -m "daemon(assumeutxo): marker verification distinguishes index-unavailable from mismatch"`

---

### Task 10: Final verification + release-gate accounting

- [ ] **Step 1: Full build + full suite:** `cmake --build build -j8 && ctest --test-dir build --output-on-failure`. Gate: no NEW failures vs the documented pre-existing set (see PR #269's attribution: shielded-fee cluster, P2P handshake, CSN soaks, ReleaseSuite env, IBDTorture path-grep — and note this branch's name contains no "fatal" so IBDTorture's grep false-positive does not apply; if the worktree path contains "fatal", expect and document it).
- [ ] **Step 2: Stale-object guard:** `touch src/daemon/services/assumeutxo_replay.cpp`, rebuild target, confirm `.o` mtime advanced, re-run `ctest -R AssumeUtxoReplay`.
- [ ] **Step 3: Spec Release Gate accounting** (docs/design/assumeutxo-fatal-state-machine.md, "Release Gate") — write the status into the PR description:
  1. Required tests pass in CI → now true at unit (16+ lifecycle, digest, replay) AND integration (e2e scenarios A/B/B2/C) level.
  1b. Spec Completion Criterion 4 (base hash + content commitment equal the compiled-in anchor) is satisfied at LOAD time on mainnet (registry check, `chainstate_service.cpp:8083-8106`); the replay binds to the load-time digest, which the load gate bound to the anchor. On regtest there is no anchor — that absence is precisely what lets the e2e simulate a compromised binary. State this chain of custody in the PR description.
  2. Fresh from-genesis background validation reaches base on a clean node under mixed fleet peers → STILL A FLEET TASK, not satisfied by regtest; name it as the remaining operational gate.
  3. RPC booleans → already shipped in PR #269.
  4. Release-notes documentation → drafting task for the release manager, not code.
  5. Signed binaries → release-process gate, unchanged.
  State plainly: after this plan, flipping the server default to fast bootstrap is blocked ONLY by gates 2/4/5.
- [ ] **Step 4: Hygiene:** `git diff --check` clean; `git log --oneline <base>..HEAD` (~10 commits); no TODO/FIXME in new files.
- [ ] **Step 5:** use superpowers:finishing-a-development-branch (PR against `feature/assumeutxo-fatal-lifecycle` if #269 is still open, else `dinero-main`). Do NOT merge or push without the human's go.

## Follow-ups that remain after this plan (explicitly NOT here)

- **Wallet-send safe-mode gating** (spec Fatal items 3/4 completion) — its own small plan; removes the `fatal_scope_note` caveat.
- **Mainnet fleet from-genesis validation run** (Release Gate item 2) — operational, not code.
- **Snapshot generation determinism doc** (from the spec discussion: whole-file SHA256 anchors require reproducible export — the sorted-record refactor in Task 2 helps; document the invariant where snapshot generation is specced).
- **Harness fixes** (separate tiny PRs): `test_ibd_torture.sh` log-level-prefixed fatal grep; `ColdStartConsensus`/`MiningPolicyE2E` WORKING_DIRECTORY/`./dinerod` mismatch.
