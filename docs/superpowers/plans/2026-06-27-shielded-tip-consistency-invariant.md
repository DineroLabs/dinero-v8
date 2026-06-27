# Shielded Tip-Consistency Invariant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ~6 scattered, inconsistent shielded-state-vs-tip-marker checks with one authoritative invariant, enforced at every shielded state-transition boundary, that fails into a scoped shielded-degraded mode (transparent keeps working) after a narrow/logged/bounded/re-checked auto-heal.

**Architecture:** A pure classifier (`ClassifyShieldedConsistency`) compares the in-memory shielded triple `{root, tree_size, nullifier_count}` against the persisted `ShieldedTipMarker` and returns a classified report. A thin `ChainstateService::CheckShieldedTipConsistency()` gathers inputs and calls it. Three boundaries (startup, AssumeUTXO restore, reorg/recovery) call the check and respond via auto-heal → re-check → degraded mode. Node-local only; not a consensus change.

**Tech Stack:** C++17, GoogleTest (`dinero_shielded` lib), RocksDB-backed `ChainDB`, existing `EnterSafeMode`/`CurrentShieldedStateSnapshot`/`getShieldedTipMarker` primitives.

## Global Constraints

- Base: `harden/shielded-tip-consistency` off `dinero-main` @ `e3feaca06` (= `v8.0.6`). Sequences **after** PR #330; rebase onto it before integrating the snapshot-restore boundary (Task 6).
- NOT a consensus change: no new block data, no validation rule for other nodes. A correctly-synced node MUST classify `Aligned` and behave identically to today.
- `MAX_SHIELDED_HEAL_BLOCKS = 2000` (config key `max_shielded_heal_blocks`, default 2000).
- Auto-heal MUST be: narrow (only enumerated classes), logged (trigger + exact height range + before/after triple), bounded (≤ cap AND all block bodies present, else abort — never partial), re-checked (re-run the invariant; only `Aligned` is success), atomic (tree+anchors+nullifiers+marker in one ChainDB batch).
- Shielded-degraded mode is DISTINCT from global safe mode: transparent sync/`scanutxos`/balance/spend stay functional; only blocks carrying shielded txs are refused; shielded RPC flagged `untrusted`.
- TDD: every behavioral change starts with a failing test that is verified to fail on the pre-change tree. SSH-signed commits as `Dinero Labs <team@dinerolabs.org>` (repo default). Commit message trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- In-memory triple source of truth: `ChainstateService::CurrentShieldedStateSnapshot()` (`src/daemon/services/chainstate_service.cpp:1102`) → `ShieldedStateSnapshot {uint256 root; uint64_t tree_size; uint64_t nullifier_count;}` (`include/daemon/services/chainstate_service.h:777`).
- Persisted truth: `ChainDB::getShieldedTipMarker()` → `ShieldedTipMarker {int32_t height; uint256 block_hash; uint256 shielded_root; uint64_t tree_size; uint64_t nullifier_count;}` (`include/storage/chain_db.h:339`).

---

### Task 1: Pure consistency classifier (header + types)

**Files:**
- Create: `include/consensus/shielded/shielded_consistency.h`

**Interfaces:**
- Produces:
  - `enum class ShieldedConsistency { Aligned, MarkerMissingNoActivity, MarkerMissingButActivityExists, TipHeightMismatch, RootMismatch, SizeMismatch, NullifierCountMismatch };`
  - `struct ShieldedTriple { dinero::uint256 root; uint64_t tree_size{0}; uint64_t nullifier_count{0}; };`
  - `struct ShieldedConsistencyInputs { ShieldedTriple observed; bool marker_present{false}; ShieldedTriple marker; int32_t marker_height{0}; dinero::uint256 marker_hash; uint32_t active_height{0}; dinero::uint256 active_hash; bool activity_below_tip{false}; };`
  - `struct ShieldedConsistencyReport { ShieldedConsistency status; std::string detail; ShieldedConsistencyInputs in; bool healable_class() const; };`
  - `ShieldedConsistencyReport ClassifyShieldedConsistency(const ShieldedConsistencyInputs&);`

- [ ] **Step 1: Create the header**

```cpp
#pragma once
// Shielded tip-consistency invariant (node-local; NOT a consensus rule).
// Pure classifier comparing the in-memory shielded triple against the
// persisted ShieldedTipMarker. See docs/superpowers/specs/2026-06-27-
// shielded-tip-consistency-invariant-design.md.
#include <cstdint>
#include <string>
#include "primitives/uint256.h"   // dinero::uint256

namespace dinero::consensus::shielded {

enum class ShieldedConsistency {
    Aligned,
    MarkerMissingNoActivity,         // benign: no marker, no shielded activity ≤ tip
    MarkerMissingButActivityExists,  // desync: no marker but activity exists ≤ tip
    TipHeightMismatch,               // marker height/hash != active tip
    RootMismatch,
    SizeMismatch,
    NullifierCountMismatch,
};

struct ShieldedTriple {
    dinero::uint256 root;
    uint64_t tree_size{0};
    uint64_t nullifier_count{0};
};

struct ShieldedConsistencyInputs {
    ShieldedTriple observed;
    bool            marker_present{false};
    ShieldedTriple  marker;
    int32_t         marker_height{0};
    dinero::uint256 marker_hash;
    uint32_t        active_height{0};
    dinero::uint256 active_hash;
    bool            activity_below_tip{false};
};

struct ShieldedConsistencyReport {
    ShieldedConsistency        status{ShieldedConsistency::Aligned};
    std::string                detail;
    ShieldedConsistencyInputs  in;
    // Classes a bounded forward-replay can repair.
    bool healable_class() const {
        switch (status) {
            case ShieldedConsistency::MarkerMissingNoActivity:        // persist-only
            case ShieldedConsistency::MarkerMissingButActivityExists:
            case ShieldedConsistency::TipHeightMismatch:
            case ShieldedConsistency::RootMismatch:
            case ShieldedConsistency::SizeMismatch:
            case ShieldedConsistency::NullifierCountMismatch:
                return true;
            case ShieldedConsistency::Aligned:
                return false;
        }
        return false;
    }
};

// Pure: no I/O, no side effects. Order of checks: tip-height first (a lagging
// marker is classified as TipHeightMismatch even if triples would differ),
// then marker-presence, then root, size, nullifier_count.
ShieldedConsistencyReport ClassifyShieldedConsistency(const ShieldedConsistencyInputs& in);

}  // namespace dinero::consensus::shielded
```

- [ ] **Step 2: Commit**

```bash
git add include/consensus/shielded/shielded_consistency.h
git commit -m "feat(shielded): consistency classifier types (header)"
```

---

### Task 2: Classifier implementation + unit tests (TDD)

**Files:**
- Create: `src/consensus/shielded/shielded_consistency.cpp`
- Create: `src/test/shielded_consistency_tests.cpp`
- Modify: the `dinero_shielded` source list (find with `grep -rn "shielded_validation.cpp" --include=CMakeLists.txt .` — add `shielded_consistency.cpp` beside it)
- Modify: `tests/CMakeLists.txt` (register `test_shielded_consistency`)

**Interfaces:**
- Consumes: Task 1 header.
- Produces: `ClassifyShieldedConsistency` definition.

- [ ] **Step 1: Write the failing tests**

```cpp
// src/test/shielded_consistency_tests.cpp
#include <gtest/gtest.h>
#include "consensus/shielded/shielded_consistency.h"
using namespace dinero::consensus::shielded;

static dinero::uint256 R(uint8_t b){ dinero::uint256 r; std::fill(r.data, r.data+32, b); return r; }

static ShieldedConsistencyInputs base() {
    ShieldedConsistencyInputs in;
    in.observed = {R(1), 10, 4};
    in.marker_present = true;
    in.marker = {R(1), 10, 4};
    in.marker_height = 52066; in.marker_hash = R(9);
    in.active_height = 52066; in.active_hash = R(9);
    in.activity_below_tip = true;
    return in;
}

TEST(ShieldedConsistency, AlignedWhenAllMatch) {
    EXPECT_EQ(ClassifyShieldedConsistency(base()).status, ShieldedConsistency::Aligned);
}
TEST(ShieldedConsistency, EmptyTreeUnderPopulatedMarkerIsSizeMismatch) {
    auto in = base(); in.observed = {R(0), 0, 0};
    auto rep = ClassifyShieldedConsistency(in);
    EXPECT_EQ(rep.status, ShieldedConsistency::SizeMismatch);
    EXPECT_NE(rep.detail.find("tree_size"), std::string::npos);
    EXPECT_NE(rep.detail.find("52066"), std::string::npos);   // names height
}
TEST(ShieldedConsistency, RootMismatchDetected) {
    auto in = base(); in.observed.root = R(2);
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::RootMismatch);
}
TEST(ShieldedConsistency, NullifierCountMismatchDetected) {
    auto in = base(); in.observed.nullifier_count = 3;
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::NullifierCountMismatch);
}
TEST(ShieldedConsistency, MarkerMissingWithActivityIsDesync) {
    auto in = base(); in.marker_present = false;
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::MarkerMissingButActivityExists);
}
TEST(ShieldedConsistency, MarkerMissingNoActivityIsBenign) {
    auto in = base(); in.marker_present = false; in.activity_below_tip = false; in.observed = {R(0),0,0};
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::MarkerMissingNoActivity);
}
TEST(ShieldedConsistency, LaggingMarkerIsTipHeightMismatch) {
    auto in = base(); in.marker_height = 52000;   // marker behind active tip
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::TipHeightMismatch);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build <build> --target test_shielded_consistency` → Expected: FAIL (link error: `ClassifyShieldedConsistency` undefined).

- [ ] **Step 3: Implement the classifier**

```cpp
// src/consensus/shielded/shielded_consistency.cpp
#include "consensus/shielded/shielded_consistency.h"
#include <sstream>

namespace dinero::consensus::shielded {

static std::string hex16(const dinero::uint256& h){ return h.GetHex().substr(0,16); }

ShieldedConsistencyReport ClassifyShieldedConsistency(const ShieldedConsistencyInputs& in) {
    ShieldedConsistencyReport rep; rep.in = in;
    auto set = [&](ShieldedConsistency s, const std::string& why){
        rep.status = s;
        std::ostringstream os;
        os << "[Shielded] " << why
           << " observed{size=" << in.observed.tree_size
           << ",nf=" << in.observed.nullifier_count
           << ",root=" << hex16(in.observed.root) << "}"
           << " marker{height=" << in.marker_height
           << ",size=" << in.marker.tree_size
           << ",nf=" << in.marker.nullifier_count
           << ",root=" << hex16(in.marker.root) << "}"
           << " active_height=" << in.active_height
           << ". Transparent funds unaffected and scannable (scanutxos);"
              " repair: 'reconcileshielded' or resync.";
        rep.detail = os.str();
    };

    if (!in.marker_present) {
        if (in.activity_below_tip) { set(ShieldedConsistency::MarkerMissingButActivityExists,
            "tip marker missing but shielded activity exists at/below tip."); return rep; }
        rep.status = ShieldedConsistency::MarkerMissingNoActivity;
        rep.detail = "[Shielded] no marker, no activity ≤ tip (benign; will persist).";
        return rep;
    }
    if (in.marker_height != static_cast<int32_t>(in.active_height) || in.marker_hash != in.active_hash) {
        set(ShieldedConsistency::TipHeightMismatch, "tip marker height/hash disagrees with active tip."); return rep;
    }
    if (in.observed.root != in.marker.root)          { set(ShieldedConsistency::RootMismatch, "commitment-tree root mismatch."); return rep; }
    if (in.observed.tree_size != in.marker.tree_size){ set(ShieldedConsistency::SizeMismatch, "commitment-tree size mismatch."); return rep; }
    if (in.observed.nullifier_count != in.marker.nullifier_count){ set(ShieldedConsistency::NullifierCountMismatch, "nullifier-count mismatch."); return rep; }
    rep.status = ShieldedConsistency::Aligned;
    rep.detail = "[Shielded] aligned.";
    return rep;
}

}  // namespace dinero::consensus::shielded
```

- [ ] **Step 4: Register sources** — add `shielded_consistency.cpp` to the `dinero_shielded` source list; append this block to `tests/CMakeLists.txt` (mirror the `test_shielded_validation` block at line 1345):

```cmake
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_consistency_tests.cpp)
  add_executable(test_shielded_consistency src/test/shielded_consistency_tests.cpp)
  add_dependencies(test_shielded_consistency gtest dinero_shielded)
  target_link_libraries(test_shielded_consistency PRIVATE GTest::gtest GTest::gtest_main dinero_shielded)
  target_include_directories(test_shielded_consistency BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME ShieldedConsistency COMMAND test_shielded_consistency)
  set_tests_properties(ShieldedConsistency PROPERTIES LABELS "shielded;consensus;mandatory" TIMEOUT 300)
endif()
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build <build> --target test_shielded_consistency && ctest --test-dir <build> -R '^ShieldedConsistency$' --output-on-failure`
Expected: all 7 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/consensus/shielded/shielded_consistency.* src/consensus/shielded/shielded_consistency.cpp src/test/shielded_consistency_tests.cpp tests/CMakeLists.txt <dinero_shielded CMakeLists>
git commit -m "feat(shielded): consistency classifier + unit tests"
```

---

### Task 3: `CheckShieldedTipConsistency()` member — gather inputs, call classifier, replace scattered checks

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (declare method)
- Modify: `src/daemon/services/chainstate_service.cpp` (define; replace inline comparisons at `~1481`, `~2189`, `~8966`, `~9357`, `~9801`, `~10571` with calls)

**Interfaces:**
- Consumes: Task 1/2 classifier; existing `CurrentShieldedStateSnapshot()`, `getShieldedTipMarker()`, `RangeHasShieldedActivity()`.
- Produces: `consensus::shielded::ShieldedConsistencyReport CheckShieldedTipConsistency(uint32_t expected_height, const uint256& expected_hash) const;`

- [ ] **Step 1: Implement the gatherer** (maps existing members → `ShieldedConsistencyInputs`, calls `ClassifyShieldedConsistency`). Set `activity_below_tip = RangeHasShieldedActivity(1, expected_height)` only when the marker is absent (avoid the O(range) scan otherwise).

- [ ] **Step 2: Refactor the 6 sites** to call `CheckShieldedTipConsistency` and branch on `.status == Aligned`. Behavior for `Aligned` MUST be identical to today (no functional change yet — response policy lands in Task 5). Keep each site's current non-aligned action for now (so this task is a pure, reviewable refactor).

- [ ] **Step 3: Build + run full shielded ctest suite** — `ctest --test-dir <build> -L shielded --output-on-failure`. Expected: no regressions (esp. shielded reorg-invertibility). This proves the refactor is behavior-preserving.

- [ ] **Step 4: Commit** — `refactor(shielded): centralize tip-consistency check into one invariant`

---

### Task 4: Shielded-degraded mode state + status RPC field (TDD)

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (`bool shielded_degraded_`, `std::string shielded_degraded_reason_`, `void EnterShieldedDegraded(const ShieldedConsistencyReport&)`, `bool IsShieldedDegraded() const`, `void ClearShieldedDegraded()`)
- Modify: `src/daemon/services/chainstate_service.cpp` (definitions; `EnterShieldedDegraded` logs `report.detail` at error level + sets flags; does NOT touch `safe_mode_active_`)
- Modify: `src/rpc/methods_blockchain_context.cpp` — `rpc_context_getsynchealth` (~line 840): add `result["shielded_state"] = IsShieldedDegraded() ? "degraded" : "ok";` and `result["shielded_degraded_reason"]` when degraded.

**Interfaces:**
- Produces: `IsShieldedDegraded()`, `EnterShieldedDegraded()`, RPC field `shielded_state`.

- [ ] **Step 1: Failing test** — extend `test_shielded_consistency` or add a small chainstate-level test asserting: after `EnterShieldedDegraded(report)`, `IsShieldedDegraded()==true`, `IsInSafeMode()==false` (degraded ≠ global safe mode). Verify it fails (methods undefined).
- [ ] **Step 2: Implement** the flag + methods + RPC field.
- [ ] **Step 3: Gate connect** — in the block-connect path, refuse to connect a block whose `block.vtx` contains a shielded tx while `shielded_degraded_` (return a clear `shielded-degraded-refusing-shielded-block` error); transparent-only blocks connect normally. (Site: where `setShieldedState`/`ApplyBlockShielded` runs — `src/consensus/block_validation.cpp:2007`; pass a `shielded_degraded` predicate or check in `ConnectTip` before applying.)
- [ ] **Step 4: Run tests + manual** — unit test passes; build daemon.
- [ ] **Step 5: Commit** — `feat(shielded): scoped degraded mode + status RPC field`

---

### Task 5: Wire boundaries to respond (startup + reorg) via degraded mode

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` — startup (after `LoadShieldedState()` @ `1652`) and reorg/recovery (`~2189`): on non-`Aligned` & non-benign, attempt auto-heal (Task 6 hook; stub = no-op until Task 6), re-check, then `EnterShieldedDegraded(report)` instead of the old blunt `EnterSafeMode`/`fail_recovery`. `MarkerMissingNoActivity` → persist marker + continue.

**Interfaces:**
- Consumes: Task 3 check, Task 4 degraded mode.

- [ ] **Step 1: Integration teeth-test (a)** — `src/test/` (or the repo's daemon integration harness; locate via `grep -rn "ChainstateService" src/test`): construct a chainstate whose shielded tree is empty under a populated marker with no healable blocks → assert `IsShieldedDegraded()`, the diagnostic names size+height, `IsInSafeMode()==false`, and a transparent path (e.g. a UTXO query) still works. Verify FAILS on current tree.
- [ ] **Step 2: Implement** the startup + reorg response.
- [ ] **Step 3: Run** — test (a) passes; full `-L shielded` suite green.
- [ ] **Step 4: Commit** — `feat(shielded): degrade (not wedge) on startup/reorg desync`

---

### Task 6: Bounded/logged/re-checked auto-heal + `reconcileshielded` RPC

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (`bool TryShieldedAutoHeal(const ShieldedConsistencyReport&, uint32_t max_blocks, bool operator_forced, std::string& log_out);`, config `uint32_t max_shielded_heal_blocks_{2000};`)
- Modify: `src/daemon/services/chainstate_service.cpp` (define heal; read config `max_shielded_heal_blocks`)
- Modify: an RPC methods file — register `reconcileshielded` (operator-confirmed, unbounded: calls `TryShieldedAutoHeal(report, /*max*/UINT32_MAX, /*operator_forced*/true, log)`; requires `{"confirm": true}` like `safemode.exit`).
- Modify: snapshot restore boundary (in `LoadSnapshot`, post-#330 restore) to run check → heal → re-check → degrade, AND persist restored shielded state to ChainDB (the durability gap).

**Interfaces:**
- Consumes: Task 3 check, Task 4 degraded mode, existing forward-replay primitives (the catch-up replay used near `~1962`).
- Produces: `TryShieldedAutoHeal(...)`, RPC `reconcileshielded`.

- [ ] **Step 1: Failing tests** — (c) marker-missing-but-blocks-available within bound → heal replays, re-check `Aligned`, not degraded; (d) gap > `MAX_SHIELDED_HEAL_BLOCKS` or a block body missing → heal returns false, node degraded, NO partial state persisted (assert triple unchanged from pre-heal). Verify both FAIL pre-implementation.
- [ ] **Step 2: Implement heal** — narrow (only `healable_class()`), bounded (≤ max AND all bodies present else abort), atomic (single ChainDB batch: frontier+anchors+nullifiers+marker), logged (trigger + `[start,end]` + before/after triple into `log_out`), re-checked (`CheckShieldedTipConsistency` again; only `Aligned` → success).
- [ ] **Step 3: Wire heal into the boundaries** (replace the Task-5 stub) + the snapshot boundary; add `reconcileshielded` RPC.
- [ ] **Step 4: Run** — tests (c)(d) pass; `-L shielded` green.
- [ ] **Step 5: Commit** — `feat(shielded): bounded auto-heal + reconcileshielded RPC`

---

### Task 7: Composition + false-positive integration tests

**Files:**
- Create/Modify: integration test(s) in the daemon test harness.

- [ ] **Step 1: Test (b) no false positive** — a correctly-synced node (populated tree matching marker) → `Aligned`, connects a shielded-bearing block, RPC `shielded_state=="ok"`. Verify it would FAIL if the classifier were wrong (mutate marker → expect degrade).
- [ ] **Step 2: Test (e) #330 composition** (only after rebase onto #330) — boot from a v4 snapshot that restores tree+anchors+nullifiers → snapshot boundary `Aligned`, a post-snapshot shielded spend connects (no `AnchorInvalid`).
- [ ] **Step 3: Run full suite** — `ctest --test-dir <build> -L "shielded;consensus" --output-on-failure`. Expected: all green.
- [ ] **Step 4: Commit** — `test(shielded): consistency composition + no-false-positive`

---

## Self-Review

**Spec coverage:** invariant (T1/2/3) ✓; three boundaries (T5 startup+reorg, T6 snapshot) ✓; degraded mode + diagnostic + transparent-survives (T4/5) ✓; auto-heal narrow/logged/bounded/re-checked/atomic (T6) ✓; `reconcileshielded` (T6) ✓; persistence-after-restore durability gap (T6) ✓; not-a-consensus-change / no-false-positive (T3 refactor green + T7b) ✓; testing matrix (a)-(e) ✓.

**Placeholder scan:** classifier code is complete; integration tasks (5–7) point at exact sites but require the implementer to locate the daemon integration harness (`grep -rn "ChainstateService" src/test`) — flagged, not hidden. Heal's forward-replay reuses the existing catch-up primitive near `~1962`; implementer reads it before Step 2 of Task 6.

**Type consistency:** `ShieldedTriple`/`ShieldedConsistencyInputs`/`ShieldedConsistencyReport`/`ClassifyShieldedConsistency` names consistent T1↔T2↔T3. `IsShieldedDegraded`/`EnterShieldedDegraded`/`TryShieldedAutoHeal` consistent T4↔T5↔T6.
