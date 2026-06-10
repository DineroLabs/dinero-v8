# AssumeUTXO Fatal-State-Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the lifecycle state machine from `docs/design/assumeutxo-fatal-state-machine.md` — fatal-on-mismatch, loud stall, machine-readable retirement — replacing today's silent auto-rollback.

**Architecture:** A new self-contained `AssumeUtxoLifecycle` class (one header + one cpp) owns the six-state machine, transition guards, stall clock, and persistence via existing `UTXOIndex` metadata. `ChainstateService` owns one instance and drives it from three existing sites (startup restore, background-validation worker, completion handler); the auto-rollback failure branch is replaced with fatal + `EnterSafeMode`. The RPC layer surfaces the spec's JSON contract by reading the lifecycle status struct. All five spec-required tests run as gtest units against the real lifecycle + real temp sqlite `UTXOIndex` (matching `tests/daemon/test_assumeutxo_metadata_lifecycle.cpp` style); time is injected as `steady_clock::time_point` values, no mocks.

**Tech Stack:** C++17, CMake, GoogleTest (vendored), sqlite-backed `UTXOIndex` metadata store.

**Branch:** create `feature/assumeutxo-fatal-lifecycle` off `dinero-main` (use superpowers:using-git-worktrees at execution time). The spec lives on `codex/assumeutxo-fatal-state-spec`; merge or cherry-pick commit `45f5bf443` into this branch first so the spec file is present.

---

## Scope decision (read before executing)

The spec spans two subsystems. **This plan implements only subsystem 1.**

1. **Lifecycle state machine + fatal/stall semantics + persistence + RPC contract** (this plan).
2. **A real historical-replay engine** — connecting blocks genesis→base through the normal block-connection path and recomputing a state commitment. Today's `BackgroundValidationWorker` only scans block *availability* and `VerifyUTXOSetMatch()` only compares UTXO *count* + spot-checks. Building true replay is consensus-engine work → **separate follow-up plan**.

**Honesty consequence (deliberate):** `AssumeUtxoLifecycle::OnReplayComplete` takes a `replay_performed` flag. The production worker passes `false` until the replay engine exists, so a production node can reach `validating_history` / `validation_stalled` / `fatal_mismatch` but **never `fully_validated`**. This is spec-compliant: the spec's Release Gate forbids describing nodes as fully validated until real validation exists. Today's code *lies* (reports `Completed` after skipping every block); after this plan it tells the truth. Unit tests pass `replay_performed=true` to exercise the full machine.

**Spec deviations to flag in review:** none intended. Spec sections covered: States, Allowed/Forbidden Transitions, Completion Criteria (gated by `replay_performed`), Fatal Mismatch Semantics (items 1–7; item 4 "mark prior results untrusted" is implemented as EnterSafeMode + `fatal` RPC boolean — wallet-level recompute is the replay-engine plan's job), Stall Semantics, RPC Contract, UI Contract (RPC strings only; GUI out of scope), Persistence, Operator Reset, Implementation Notes, Required Tests 1–5 (at lifecycle level on synthetic events — the spec's regtest-fixture variants move to the replay-engine plan), Release Gate (documented, not all gates met by design).

## File map

| File | Action | Responsibility |
|---|---|---|
| `include/daemon/services/assumeutxo_lifecycle.h` | Create | State machine class, status struct, persistence keys |
| `src/daemon/services/assumeutxo_lifecycle.cpp` | Create | Transitions, guards, stall clock, persistence |
| `tests/daemon/test_assumeutxo_lifecycle.cpp` | Create | The 5 spec tests + happy path |
| `tests/CMakeLists.txt` | Modify (append after line ~3047) | Register test binary |
| `include/daemon/services/chainstate_service.h` | Modify (~line 296, ~line 994) | Own lifecycle, expose accessor |
| `src/daemon/services/chainstate_service.cpp` | Modify (lines 2817–2834, 11485–11639, 11735–11824) | Drive lifecycle, replace auto-rollback |
| `src/rpc/methods_blockchain_context.cpp` | Modify (`buildSnapshotBootstrapDiagnostics` line 350, registration ~line 3688) | Spec RPC contract + reset RPC |

Existing ground truth used throughout (verified 2026-06-09):
- `BackgroundValidationStatus` enum: `include/daemon/services/chainstate_service.h:301-306`
- Auto-rollback to replace: `src/daemon/services/chainstate_service.cpp:11780-11823`
- Worker availability scan (skippable bodies): `src/daemon/services/chainstate_service.cpp:11547-11639`
- Startup restore: `src/daemon/services/chainstate_service.cpp:2810-2860`
- Persistence helpers: `include/daemon/services/assumeutxo_state.h`
- Safe mode: `EnterSafeMode(reason)` declared `chainstate_service.h:272`, defined `chainstate_service.cpp:7394`
- Test style + CMake pattern: `tests/daemon/test_assumeutxo_metadata_lifecycle.cpp`, `tests/CMakeLists.txt:3023-3047`

Build/test commands used in every task:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release          # configure once
cmake --build build -j8 --target test_assumeutxo_lifecycle
ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure
```

---

### Task 1: Lifecycle skeleton + happy path

**Files:**
- Create: `include/daemon/services/assumeutxo_lifecycle.h`
- Create: `src/daemon/services/assumeutxo_lifecycle.cpp`
- Create: `tests/daemon/test_assumeutxo_lifecycle.cpp`
- Modify: `tests/CMakeLists.txt` (append a new block after the `AssumeUTXOMetadataLifecycle` block ending near line 3047)

- [ ] **Step 1: Register the test binary in CMake**

Append to `tests/CMakeLists.txt` directly after the `endif()` of the `test_assumeutxo_metadata_lifecycle` block (after the `message(STATUS "✅ AssumeUTXO metadata lifecycle test enabled")` / `endif()` pair, ~line 3048):

```cmake
# AssumeUTXO fatal-state-machine lifecycle tests (docs/design/assumeutxo-fatal-state-machine.md)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_assumeutxo_lifecycle.cpp)
  add_executable(test_assumeutxo_lifecycle
    tests/daemon/test_assumeutxo_lifecycle.cpp
    src/daemon/services/assumeutxo_lifecycle.cpp
  )
  add_dependencies(test_assumeutxo_lifecycle gtest)
  target_link_libraries(test_assumeutxo_lifecycle PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_assumeutxo_lifecycle)
  target_include_directories(test_assumeutxo_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME AssumeUtxoLifecycle COMMAND test_assumeutxo_lifecycle)
  set_tests_properties(AssumeUtxoLifecycle PROPERTIES
    LABELS "daemon;assumeutxo;state-machine"
    TIMEOUT 60
  )
  message(STATUS "✅ AssumeUTXO lifecycle state-machine test enabled")
endif()
```

- [ ] **Step 2: Write the failing happy-path test**

Create `tests/daemon/test_assumeutxo_lifecycle.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "daemon/services/assumeutxo_lifecycle.h"
#include "primitives/uint256.h"
#include "wallet/utxo_index.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace dinero {

class AssumeUtxoLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto unique_id = std::to_string(
            static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        temp_dir_ = fs::temp_directory_path() / ("dinero_assumeutxo_lc_" + unique_id);
        fs::create_directories(temp_dir_);
        utxo_index_ = std::make_unique<UTXOIndex>((temp_dir_ / "wallet.db").string());
        ASSERT_TRUE(utxo_index_->Initialize());
        t0_ = std::chrono::steady_clock::time_point{};  // deterministic epoch
        base_block_ = uint256::FromHexUnsafe(
            "00000015f97a45f358fee1562317c05590b042b190e288a60ad7218b7e4efffa");
    }

    void TearDown() override {
        utxo_index_.reset();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    // Fresh lifecycle over the SAME persistence — simulates a daemon restart.
    std::unique_ptr<assumeutxo::AssumeUtxoLifecycle> MakeLifecycle(
            std::chrono::seconds stall_timeout = 1800s) {
        return std::make_unique<assumeutxo::AssumeUtxoLifecycle>(
            utxo_index_.get(), /*logger=*/nullptr, stall_timeout);
    }

    std::unique_ptr<UTXOIndex> utxo_index_;
    fs::path temp_dir_;
    std::chrono::steady_clock::time_point t0_;
    uint256 base_block_;
    static constexpr uint32_t kBaseHeight = 48;  // small synthetic-fixture height
};

using State = assumeutxo::AssumeUtxoLifecycle::State;

TEST_F(AssumeUtxoLifecycleTest, HappyPathRetiresTrustMarker) {
    auto lc = MakeLifecycle();
    EXPECT_EQ(lc->GetState(), State::Disabled);

    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    EXPECT_EQ(lc->GetState(), State::SnapshotLoaded);

    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    for (uint32_t h = 0; h <= kBaseHeight; ++h) {
        lc->OnBlockValidated(h, t0_ + std::chrono::seconds(h));
    }

    ASSERT_TRUE(lc->OnReplayComplete(/*replay_performed=*/true,
                                     /*commitment_match=*/true,
                                     "aa", "aa",
                                     /*missing_body_count=*/0,
                                     t0_ + 100s));
    EXPECT_EQ(lc->GetState(), State::FullyValidated);

    const auto st = lc->GetStatus(t0_ + 101s);
    EXPECT_TRUE(st.history_fully_validated);
    EXPECT_FALSE(st.assumeutxo_active);
    EXPECT_FALSE(st.fatal);
    EXPECT_EQ(st.snapshot_base_height, kBaseHeight);
}

// Forbidden: snapshot_loaded -> fully_validated without replay+comparison.
TEST_F(AssumeUtxoLifecycleTest, CannotCompleteFromSnapshotLoaded) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    EXPECT_FALSE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_));
    EXPECT_EQ(lc->GetState(), State::SnapshotLoaded);
}

}  // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

(If `test_assumeutxo_metadata_lifecycle.cpp` has no `main` — gtest_main linked instead — match whichever pattern that file uses; check its bottom before keeping the explicit `main`.)

- [ ] **Step 3: Run test to verify it fails to compile (header missing)**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8 --target test_assumeutxo_lifecycle`
Expected: FAIL — `assumeutxo_lifecycle.h: No such file or directory`

- [ ] **Step 4: Write the minimal implementation**

Create `include/daemon/services/assumeutxo_lifecycle.h`:

```cpp
#pragma once

#include "primitives/uint256.h"
#include "wallet/utxo_index.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace dinero { class Logger; }

namespace dinero::assumeutxo {

// Persistence keys (extend the kActiveKey/kBaseBlockKey/kBaseHeightKey family
// in assumeutxo_state.h; stored in the same UTXOIndex metadata table).
inline constexpr const char* kLifecycleStateKey  = "assumeutxo_lifecycle_state";
inline constexpr const char* kFatalReasonKey     = "assumeutxo_fatal_reason";
inline constexpr const char* kFullyValidatedKey  = "assumeutxo_fully_validated";
inline constexpr const char* kLcBaseBlockKey     = "assumeutxo_lc_base_block";
inline constexpr const char* kLcBaseHeightKey    = "assumeutxo_lc_base_height";

// Required confirmation token for OperatorReset (spec: Operator Reset).
inline constexpr const char* kResetToken = "RESET-ASSUMEUTXO-FATAL";

// docs/design/assumeutxo-fatal-state-machine.md — six-state lifecycle.
class AssumeUtxoLifecycle {
public:
    enum class State {
        Disabled,
        SnapshotLoaded,
        ValidatingHistory,
        ValidationStalled,
        FatalMismatch,
        FullyValidated,
    };

    using TimePoint = std::chrono::steady_clock::time_point;

    struct Status {
        State state = State::Disabled;
        bool assumeutxo_active = false;       // true while node depends on assumed state
        bool history_fully_validated = false; // true only in FullyValidated
        bool fatal = false;                   // true only in FatalMismatch
        std::string fatal_reason;
        uint32_t snapshot_base_height = 0;
        uint256 snapshot_base_block;
        uint32_t current_validation_height = 0;
        uint32_t target_validation_height = 0;
        uint32_t missing_body_count = 0;
        int64_t stall_seconds = 0;
        std::string next_action;
    };

    // logger may be nullptr (tests); stall_timeout default per spec: 30 min.
    AssumeUtxoLifecycle(UTXOIndex* utxo_index, Logger* logger,
                        std::chrono::seconds stall_timeout = std::chrono::seconds(1800));

    // Disabled -> SnapshotLoaded. Refused (returns false) in FatalMismatch.
    bool OnSnapshotLoaded(const uint256& base_block, uint32_t base_height);
    // SnapshotLoaded -> ValidatingHistory (also re-entry from ValidationStalled).
    bool OnValidationStarted(TimePoint now);
    // Progress. In ValidationStalled, real progress recovers to ValidatingHistory.
    void OnBlockValidated(uint32_t height, TimePoint now);
    // Record bodies the scan could not retrieve (spec: never skippable success).
    void OnMissingBodies(uint32_t count);
    // Terminal evaluation. FullyValidated only if replay_performed &&
    // commitment_match && missing_body_count == 0. Mismatch -> FatalMismatch.
    bool OnReplayComplete(bool replay_performed, bool commitment_match,
                          const std::string& expected_commitment,
                          const std::string& recomputed_commitment,
                          uint32_t missing_body_count, TimePoint now);
    // Stall detection: ValidatingHistory -> ValidationStalled after timeout.
    void Tick(TimePoint now);
    // FatalMismatch -> Disabled, only with the exact confirmation token.
    bool OperatorReset(const std::string& confirm_token);
    // Startup rehydration. chainstate_matches_marker: caller verified the
    // persisted base block/height against the live chainstate. A FullyValidated
    // marker with a non-matching chainstate goes FATAL (spec: Persistence).
    void RestoreFromPersistence(bool chainstate_matches_marker);
    // Normal (non-fatal) clear, e.g. operator wipes datadir state.
    void Disable();

    State GetState() const;
    Status GetStatus(TimePoint now) const;

    static const char* StateName(State s);

private:
    void Persist() /* callers hold mu_ */;
    void EnterFatal(const std::string& reason, TimePoint now);

    UTXOIndex* utxo_index_;       // not owned
    Logger* logger_;              // not owned, may be null
    const std::chrono::seconds stall_timeout_;

    mutable std::mutex mu_;
    State state_ = State::Disabled;
    uint256 base_block_;
    uint32_t base_height_ = 0;
    uint32_t current_height_ = 0;
    uint32_t missing_bodies_ = 0;
    std::string fatal_reason_;
    TimePoint last_progress_time_{};
    bool has_progress_time_ = false;
};

}  // namespace dinero::assumeutxo
```

Create `src/daemon/services/assumeutxo_lifecycle.cpp`:

```cpp
#include "daemon/services/assumeutxo_lifecycle.h"

#include "common/logger.h"

namespace dinero::assumeutxo {

namespace {
constexpr const char* kStateNames[] = {
    "disabled", "snapshot_loaded", "validating_history",
    "validation_stalled", "fatal_mismatch", "fully_validated",
};
}  // namespace

const char* AssumeUtxoLifecycle::StateName(State s) {
    return kStateNames[static_cast<int>(s)];
}

AssumeUtxoLifecycle::AssumeUtxoLifecycle(UTXOIndex* utxo_index, Logger* logger,
                                         std::chrono::seconds stall_timeout)
    : utxo_index_(utxo_index), logger_(logger), stall_timeout_(stall_timeout) {}

bool AssumeUtxoLifecycle::OnSnapshotLoaded(const uint256& base_block, uint32_t base_height) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::FatalMismatch) {
        if (logger_) logger_->error(
            "[AssumeUtxoLifecycle] REFUSED snapshot load: node is in fatal_mismatch; "
            "explicit operator reset required");
        return false;
    }
    if (state_ != State::Disabled) return false;
    state_ = State::SnapshotLoaded;
    base_block_ = base_block;
    base_height_ = base_height;
    current_height_ = 0;
    missing_bodies_ = 0;
    Persist();
    return true;
}

bool AssumeUtxoLifecycle::OnValidationStarted(TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::SnapshotLoaded && state_ != State::ValidationStalled) return false;
    state_ = State::ValidatingHistory;
    last_progress_time_ = now;
    has_progress_time_ = true;
    Persist();
    return true;
}

void AssumeUtxoLifecycle::OnBlockValidated(uint32_t height, TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::ValidatingHistory && state_ != State::ValidationStalled) return;
    if (state_ == State::ValidationStalled) {
        state_ = State::ValidatingHistory;  // real progress recovers a stall
        Persist();
    }
    current_height_ = height;
    last_progress_time_ = now;
    has_progress_time_ = true;
}

void AssumeUtxoLifecycle::OnMissingBodies(uint32_t count) {
    std::lock_guard<std::mutex> lock(mu_);
    missing_bodies_ = count;
}

bool AssumeUtxoLifecycle::OnReplayComplete(bool replay_performed, bool commitment_match,
                                           const std::string& expected_commitment,
                                           const std::string& recomputed_commitment,
                                           uint32_t missing_body_count, TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::ValidatingHistory && state_ != State::ValidationStalled) {
        return false;  // forbidden: e.g. snapshot_loaded -> fully_validated
    }
    missing_bodies_ = missing_body_count;
    if (!commitment_match) {
        EnterFatal(
            "snapshot commitment mismatch at base height " + std::to_string(base_height_) +
            " (base " + base_block_.GetHex() + "): expected commitment " + expected_commitment +
            ", recomputed " + recomputed_commitment, now);
        return false;
    }
    if (missing_body_count > 0) {
        // Spec: missing bodies are never success. Stay in progress; Tick() will stall.
        return false;
    }
    if (!replay_performed) {
        // Availability/count checks alone cannot retire the trust assumption.
        // Stay validating_history until the real replay engine reports.
        return false;
    }
    state_ = State::FullyValidated;
    Persist();
    if (logger_) logger_->info(
        "[AssumeUtxoLifecycle] fully_validated: snapshot trust assumption retired at height " +
        std::to_string(base_height_));
    return true;
}

void AssumeUtxoLifecycle::Tick(TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::ValidatingHistory || !has_progress_time_) return;
    if (current_height_ >= base_height_ && missing_bodies_ == 0) return;
    if (now - last_progress_time_ >= stall_timeout_) {
        state_ = State::ValidationStalled;
        Persist();
        if (logger_) logger_->error(
            "[AssumeUtxoLifecycle] validation_stalled: no historical block validated for " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                now - last_progress_time_).count()) +
            "s at height " + std::to_string(current_height_) + "/" + std::to_string(base_height_) +
            " (" + std::to_string(missing_bodies_) + " bodies missing)");
    }
}

bool AssumeUtxoLifecycle::OperatorReset(const std::string& confirm_token) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::FatalMismatch) return false;
    if (confirm_token != kResetToken) return false;
    if (logger_) logger_->error(
        "[AssumeUtxoLifecycle] AUDIT: operator reset of fatal_mismatch (reason was: " +
        fatal_reason_ + ")");
    state_ = State::Disabled;
    fatal_reason_.clear();
    base_block_.SetNull();
    base_height_ = 0;
    current_height_ = 0;
    missing_bodies_ = 0;
    has_progress_time_ = false;
    if (utxo_index_) {
        utxo_index_->DeleteMetadata(kLifecycleStateKey);
        utxo_index_->DeleteMetadata(kFatalReasonKey);
        utxo_index_->DeleteMetadata(kFullyValidatedKey);
        utxo_index_->DeleteMetadata(kLcBaseBlockKey);
        utxo_index_->DeleteMetadata(kLcBaseHeightKey);
    }
    return true;
}

void AssumeUtxoLifecycle::RestoreFromPersistence(bool chainstate_matches_marker) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!utxo_index_) return;
    auto state_meta = utxo_index_->GetMetadata(kLifecycleStateKey);
    if (!state_meta) return;  // nothing persisted: stay Disabled
    const std::string& name = state_meta.value();

    if (auto bb = utxo_index_->GetMetadata(kLcBaseBlockKey)) {
        base_block_ = uint256::FromHexUnsafe(bb.value());
    }
    if (auto bh = utxo_index_->GetMetadata(kLcBaseHeightKey)) {
        base_height_ = static_cast<uint32_t>(std::stoul(bh.value()));
    }

    if (name == "fatal_mismatch") {
        state_ = State::FatalMismatch;
        if (auto r = utxo_index_->GetMetadata(kFatalReasonKey)) fatal_reason_ = r.value();
        return;
    }
    if (name == "fully_validated") {
        if (chainstate_matches_marker) {
            state_ = State::FullyValidated;
        } else {
            // Spec (Persistence): marker present but chainstate mismatch -> FATAL.
            EnterFatal("fully_validated marker present but chainstate does not match "
                       "persisted base (corruption or tampering)",
                       TimePoint{});
        }
        return;
    }
    if (name == "snapshot_loaded") { state_ = State::SnapshotLoaded; return; }
    if (name == "validating_history" || name == "validation_stalled") {
        // Resume as validating; worker restart re-establishes progress + stall clock.
        state_ = State::ValidatingHistory;
        return;
    }
    // Unknown value: leave Disabled.
}

void AssumeUtxoLifecycle::Disable() {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::FatalMismatch) return;  // only OperatorReset leaves fatal
    state_ = State::Disabled;
    Persist();
}

AssumeUtxoLifecycle::State AssumeUtxoLifecycle::GetState() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

AssumeUtxoLifecycle::Status AssumeUtxoLifecycle::GetStatus(TimePoint now) const {
    std::lock_guard<std::mutex> lock(mu_);
    Status st;
    st.state = state_;
    st.assumeutxo_active = (state_ == State::SnapshotLoaded ||
                            state_ == State::ValidatingHistory ||
                            state_ == State::ValidationStalled);
    st.history_fully_validated = (state_ == State::FullyValidated);
    st.fatal = (state_ == State::FatalMismatch);
    st.fatal_reason = fatal_reason_;
    st.snapshot_base_height = base_height_;
    st.snapshot_base_block = base_block_;
    st.current_validation_height = current_height_;
    st.target_validation_height = base_height_;
    st.missing_body_count = missing_bodies_;
    if (has_progress_time_ &&
        (state_ == State::ValidatingHistory || state_ == State::ValidationStalled)) {
        st.stall_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_progress_time_).count();
        if (st.stall_seconds < 0) st.stall_seconds = 0;
    }
    switch (state_) {
        case State::Disabled:
            st.next_action = "No snapshot loaded."; break;
        case State::SnapshotLoaded:
            st.next_action = "Fast bootstrap active; background validation pending."; break;
        case State::ValidatingHistory:
            st.next_action = "Background validation in progress."; break;
        case State::ValidationStalled:
            st.next_action = "Background validation STALLED; check peer/block availability."; break;
        case State::FatalMismatch:
            st.next_action = "FATAL: snapshot failed proof. Operator reset required "
                             "(blockchain.resetassumeutxofatal or wipe datadir)."; break;
        case State::FullyValidated:
            st.next_action = "Fully validated; snapshot trust assumption retired."; break;
    }
    return st;
}

void AssumeUtxoLifecycle::Persist() {
    if (!utxo_index_) return;
    utxo_index_->SetMetadata(kLifecycleStateKey, StateName(state_));
    utxo_index_->SetMetadata(kLcBaseBlockKey, base_block_.GetHex());
    utxo_index_->SetMetadata(kLcBaseHeightKey, std::to_string(base_height_));
    if (state_ == State::FullyValidated) {
        utxo_index_->SetMetadata(kFullyValidatedKey, "true");
    }
    if (state_ == State::FatalMismatch) {
        utxo_index_->SetMetadata(kFatalReasonKey, fatal_reason_);
    }
}

void AssumeUtxoLifecycle::EnterFatal(const std::string& reason, TimePoint /*now*/) {
    state_ = State::FatalMismatch;
    fatal_reason_ = reason;
    Persist();
    if (logger_) {
        logger_->error("[AssumeUtxoLifecycle] ═══════════════════════════════════════════");
        logger_->error("[AssumeUtxoLifecycle] FATAL MISMATCH — node was serving from state "
                       "that failed later proof");
        logger_->error("[AssumeUtxoLifecycle] " + reason);
        logger_->error("[AssumeUtxoLifecycle] Explicit operator reset required.");
        logger_->error("[AssumeUtxoLifecycle] ═══════════════════════════════════════════");
    }
}

}  // namespace dinero::assumeutxo
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j8 --target test_assumeutxo_lifecycle && ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure`
Expected: PASS (2 tests)

- [ ] **Step 6: Commit**

```bash
git add include/daemon/services/assumeutxo_lifecycle.h src/daemon/services/assumeutxo_lifecycle.cpp tests/daemon/test_assumeutxo_lifecycle.cpp tests/CMakeLists.txt
git commit -m "daemon: add AssumeUtxoLifecycle state machine skeleton (happy path)"
```

---

### Task 2: Spec Test 1 — Poisoned snapshot is fatal

**Files:**
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (append tests)
- Modify: `src/daemon/services/assumeutxo_lifecycle.cpp` (only if a test exposes a gap)

The fatal path was implemented in Task 1; this task proves it RED→GREEN by neutering: the assertions below MUST be run once against a deliberately broken guard to confirm they bite (see Step 2).

- [ ] **Step 1: Write the test** (append to `tests/daemon/test_assumeutxo_lifecycle.cpp`, inside `namespace dinero`):

```cpp
// Spec Required Test 1: load-time gates pass, genesis replay does not match.
TEST_F(AssumeUtxoLifecycleTest, PoisonedSnapshotIsFatal) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);

        // Replay recomputed a different commitment than the snapshot committed.
        EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/true,
                                          /*commitment_match=*/false,
                                          "deadbeef", "cafebabe", 0, t0_ + 20s));
        EXPECT_EQ(lc->GetState(), State::FatalMismatch);

        const auto st = lc->GetStatus(t0_ + 21s);
        EXPECT_TRUE(st.fatal);
        EXPECT_FALSE(st.history_fully_validated);
        // Log/RPC must carry both commitments (spec: Fatal Mismatch item 5).
        EXPECT_NE(st.fatal_reason.find("deadbeef"), std::string::npos);
        EXPECT_NE(st.fatal_reason.find("cafebabe"), std::string::npos);
        EXPECT_NE(st.fatal_reason.find(base_block_.GetHex()), std::string::npos);
    }
    // Restart preserves fatal_mismatch (spec: Persistence).
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
        EXPECT_TRUE(lc2->GetStatus(t0_).fatal);
        EXPECT_FALSE(lc2->GetStatus(t0_).fatal_reason.empty());
    }
}
```

- [ ] **Step 2: Prove the test bites (neuter check — mandatory)**

Temporarily change `if (!commitment_match) {` to `if (false && !commitment_match) {` in `OnReplayComplete`, rebuild, run:

Run: `cmake --build build -j8 --target test_assumeutxo_lifecycle && ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure`
Expected: `PoisonedSnapshotIsFatal` FAILS (state not FatalMismatch).

Revert the neuter exactly. Re-run.
Expected: PASS (3+ tests).

- [ ] **Step 3: Commit**

```bash
git add tests/daemon/test_assumeutxo_lifecycle.cpp
git commit -m "test(assumeutxo): poisoned snapshot drives fatal_mismatch and survives restart"
```

---

### Task 3: Spec Test 2 — Missing historical bodies cannot complete

**Files:**
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (append test)

- [ ] **Step 1: Write the test** (append inside `namespace dinero`):

```cpp
// Spec Required Test 2: missing bodies are never success.
TEST_F(AssumeUtxoLifecycleTest, MissingBodiesCannotComplete) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(10, t0_ + 10s);

    // Scan finished but 3 bodies were unavailable; commitment "matched" anyway.
    EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/true,
                                      /*commitment_match=*/true,
                                      "aa", "aa",
                                      /*missing_body_count=*/3, t0_ + 20s));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);
    EXPECT_FALSE(lc->GetStatus(t0_ + 21s).history_fully_validated);
    EXPECT_EQ(lc->GetStatus(t0_ + 21s).missing_body_count, 3u);

    // After the stall window with no progress -> validation_stalled.
    lc->Tick(t0_ + 20s + 1801s);
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);
    EXPECT_FALSE(lc->GetStatus(t0_ + 20s + 1802s).history_fully_validated);
}

// replay_performed=false (today's availability+count scan) can never retire trust.
TEST_F(AssumeUtxoLifecycleTest, AvailabilityScanAloneCannotComplete) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
    EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/false,
                                      /*commitment_match=*/true, "aa", "aa", 0, t0_ + 20s));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);
}
```

- [ ] **Step 2: Verify RED-capability, then GREEN**

Neuter check: temporarily change `if (missing_body_count > 0)` to `if (false)` → `MissingBodiesCannotComplete` must FAIL. Revert.

Run: `cmake --build build -j8 --target test_assumeutxo_lifecycle && ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/daemon/test_assumeutxo_lifecycle.cpp
git commit -m "test(assumeutxo): missing bodies and availability-only scans cannot reach fully_validated"
```

---

### Task 4: Spec Test 3 — Stall is loud and recoverable

**Files:**
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (append test)

- [ ] **Step 1: Write the test:**

```cpp
// Spec Required Test 3: stall transition, metadata, recovery, completion.
TEST_F(AssumeUtxoLifecycleTest, StallIsLoudAndRecoverable) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(5, t0_ + 5s);

    // Just under the window: still validating.
    lc->Tick(t0_ + 5s + 1799s);
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    // Past the window: stalled, with machine-readable stall metadata.
    lc->Tick(t0_ + 5s + 1800s);
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);
    auto st = lc->GetStatus(t0_ + 5s + 1900s);
    EXPECT_GE(st.stall_seconds, 1800);
    EXPECT_FALSE(st.history_fully_validated);
    EXPECT_TRUE(st.assumeutxo_active);  // snapshot may stay foreground-usable

    // One real validated block recovers the stall.
    lc->OnBlockValidated(6, t0_ + 4000s);
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    // Reaching base with full replay + match completes.
    lc->OnBlockValidated(kBaseHeight, t0_ + 4100s);
    EXPECT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 4200s));
    EXPECT_EQ(lc->GetState(), State::FullyValidated);
}
```

- [ ] **Step 2: Verify**

Neuter check: change `now - last_progress_time_ >= stall_timeout_` to `false` → test must FAIL at the stall assertion. Revert.

Run: `ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure` (after rebuild)
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/daemon/test_assumeutxo_lifecycle.cpp
git commit -m "test(assumeutxo): stall is loud, machine-readable, and recoverable"
```

---

### Task 5: Spec Test 4 — Retirement persistence + restart marker gap

**Files:**
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (append tests)

- [ ] **Step 1: Write the tests:**

```cpp
// Spec Required Test 4 (persistence half): restart preserves fully_validated.
TEST_F(AssumeUtxoLifecycleTest, RetirementSurvivesRestart) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
        ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
        ASSERT_EQ(lc->GetState(), State::FullyValidated);
        ASSERT_EQ(utxo_index_->GetMetadata(
            assumeutxo::kFullyValidatedKey).value_or(""), "true");
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::FullyValidated);
        EXPECT_TRUE(lc2->GetStatus(t0_).history_fully_validated);
        EXPECT_FALSE(lc2->GetStatus(t0_).assumeutxo_active);
    }
}

// Spec Persistence rule: marker present + chainstate mismatch -> FATAL, not trust.
TEST_F(AssumeUtxoLifecycleTest, RetirementMarkerWithMismatchedChainstateIsFatal) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
        ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/false);
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
        EXPECT_TRUE(lc2->GetStatus(t0_).fatal);
        EXPECT_NE(lc2->GetStatus(t0_).fatal_reason.find("marker"), std::string::npos);
    }
}
```

- [ ] **Step 2: Verify**

Neuter check: in `RestoreFromPersistence`, swap the `chainstate_matches_marker` branch to always trust the marker → `RetirementMarkerWithMismatchedChainstateIsFatal` must FAIL. Revert.

Run: rebuild + `ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/daemon/test_assumeutxo_lifecycle.cpp
git commit -m "test(assumeutxo): retirement survives restart; tampered chainstate under marker goes fatal"
```

---

### Task 6: Spec Test 5 — Fatal state requires explicit reset

**Files:**
- Modify: `tests/daemon/test_assumeutxo_lifecycle.cpp` (append test)

- [ ] **Step 1: Write the test:**

```cpp
// Spec Required Test 5: fatal gates everything until explicit, token-confirmed reset.
TEST_F(AssumeUtxoLifecycleTest, FatalStateRequiresExplicitReset) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnReplayComplete(true, /*commitment_match=*/false, "aa", "bb", 0, t0_ + 10s);
        ASSERT_EQ(lc->GetState(), State::FatalMismatch);
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(true);
        ASSERT_EQ(lc2->GetState(), State::FatalMismatch);

        // New snapshot refused while fatal.
        EXPECT_FALSE(lc2->OnSnapshotLoaded(base_block_, kBaseHeight));
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);

        // Wrong/missing token refused.
        EXPECT_FALSE(lc2->OperatorReset(""));
        EXPECT_FALSE(lc2->OperatorReset("yes"));
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);

        // Correct token resets to Disabled and clears persisted fatal state.
        EXPECT_TRUE(lc2->OperatorReset(assumeutxo::kResetToken));
        EXPECT_EQ(lc2->GetState(), State::Disabled);
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFatalReasonKey).has_value());
        // Reset must NOT mark the prior snapshot valid (spec: Operator Reset).
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());

        // A fresh attempt is now permitted.
        EXPECT_TRUE(lc2->OnSnapshotLoaded(base_block_, kBaseHeight));
    }
}
```

- [ ] **Step 2: Verify**

Neuter check: make `OperatorReset` accept any token → the wrong-token assertions must FAIL. Revert.

Run: rebuild + `ctest --test-dir build -R AssumeUtxoLifecycle --output-on-failure`
Expected: PASS (all tests; suite now covers spec Required Tests 1–5)

- [ ] **Step 3: Commit**

```bash
git add tests/daemon/test_assumeutxo_lifecycle.cpp
git commit -m "test(assumeutxo): fatal_mismatch gates snapshot loads until token-confirmed operator reset"
```

---

### Task 7: Wire lifecycle into ChainstateService (replace auto-rollback)

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (~line 296 public section, ~line 1000 private members)
- Modify: `src/daemon/services/chainstate_service.cpp` (startup restore ~2817–2834; worker 11547–11639; completion handler 11735–11824)

This task is mechanical delegation — the logic is already covered by Tasks 1–6's tests. Verification here = full build + entire existing ctest suite stays green + one neuter check on the fatal branch.

- [ ] **Step 1: Add member + accessors to the header**

In `include/daemon/services/chainstate_service.h`, add the include at the top with the other service includes:

```cpp
#include "daemon/services/assumeutxo_lifecycle.h"
```

In the public section right after `GetAssumeUTXOBaseHeight()` (line ~298):

```cpp
    // Fatal-state-machine lifecycle (docs/design/assumeutxo-fatal-state-machine.md).
    // Never nullptr after Initialize(); guarded internally.
    assumeutxo::AssumeUtxoLifecycle* GetAssumeUtxoLifecycle();
```

In the private section after `bg_validation_mutex_` (line ~1000):

```cpp
    // Lazily constructed once utxo_index_ exists (EnsureAssumeUtxoLifecycle()).
    std::unique_ptr<assumeutxo::AssumeUtxoLifecycle> assumeutxo_lifecycle_;
    std::mutex assumeutxo_lifecycle_init_mutex_;
    void EnsureAssumeUtxoLifecycle();
```

- [ ] **Step 2: Implement the lazy initializer in the cpp**

Add near the other AssumeUTXO state helpers (after `ClearAssumeUTXOState`, ~line 1610):

```cpp
void ChainstateService::EnsureAssumeUtxoLifecycle() {
    std::lock_guard<std::mutex> lock(assumeutxo_lifecycle_init_mutex_);
    if (!assumeutxo_lifecycle_) {
        assumeutxo_lifecycle_ = std::make_unique<assumeutxo::AssumeUtxoLifecycle>(
            utxo_index_.get(), logger_.get());
    }
}

assumeutxo::AssumeUtxoLifecycle* ChainstateService::GetAssumeUtxoLifecycle() {
    EnsureAssumeUtxoLifecycle();
    return assumeutxo_lifecycle_.get();
}
```

(If `logger_` is not a smart pointer in this class — check its declaration — pass it as the raw pointer it is; the lifecycle takes `Logger*`.)

- [ ] **Step 3: Drive lifecycle from the startup restore block (cpp 2817–2834)**

Immediately AFTER the existing `SetAssumeUTXOState(restored_base_block, restored_base_height, /*persist_metadata=*/false);` at line 2834, add:

```cpp
        // Fatal-state-machine restore (spec: Persistence). chainstate_matches:
        // the persisted base must still be what the consensus UTXO set says.
        EnsureAssumeUtxoLifecycle();
        {
            bool chainstate_matches = true;
            if (consensus_utxo_set_) {
                chainstate_matches =
                    (consensus_utxo_set_->GetBestBlock() == restored_base_block &&
                     consensus_utxo_set_->GetHeight() == restored_base_height);
            }
            assumeutxo_lifecycle_->RestoreFromPersistence(chainstate_matches);
        }
```

ALSO handle the not-active path (fully_validated/fatal survive even when `kActiveKey` is cleared): in the `else` of `if (active_meta && active_meta.value() == "true")` — if no `else` exists, add one:

```cpp
    } else {
        // Not in assumed mode, but a fully_validated or fatal_mismatch lifecycle
        // record may still be persisted; restore it (marker-vs-chainstate check
        // uses the persisted lc base, verified against chaindb when available).
        EnsureAssumeUtxoLifecycle();
        bool chainstate_matches = true;
        if (auto bh = utxo_index_->GetMetadata(assumeutxo::kLcBaseHeightKey)) {
            if (auto bb = utxo_index_->GetMetadata(assumeutxo::kLcBaseBlockKey)) {
                const uint32_t h = static_cast<uint32_t>(std::stoul(bh.value()));
                if (chain_db_ && h > 0) {
                    auto hash_result = chain_db_->getBlockHashByHeight(h);
                    chainstate_matches = hash_result.ok() &&
                        hash_result.value() == uint256::FromHexUnsafe(bb.value());
                }
            }
        }
        assumeutxo_lifecycle_->RestoreFromPersistence(chainstate_matches);
    }
```

- [ ] **Step 4: Drive lifecycle from LoadSnapshot success + worker**

(a) Find the success exit of `LoadSnapshot` (cpp 7754–7949, where `SnapshotImportResult` success is returned after `SetAssumeUTXOState(...)` — grep `SetAssumeUTXOState` at line 8204 region for the post-load call). Immediately after that `SetAssumeUTXOState(header.block_hash, header.block_height, /*persist_metadata=*/true);` add:

```cpp
        EnsureAssumeUtxoLifecycle();
        if (!assumeutxo_lifecycle_->OnSnapshotLoaded(header.block_hash, header.block_height)) {
            // Fatal gate: spec forbids loading a new snapshot in fatal_mismatch.
            return consensus::SnapshotImportResult::Error(
                "node is in assumeutxo fatal_mismatch state; operator reset required");
        }
```

(Match the actual error-construction idiom used elsewhere in `LoadSnapshot` — read the surrounding returns and use the same factory/struct. If the gate should fire BEFORE any state mutation, hoist the `OnSnapshotLoaded` check to the top of `LoadSnapshot`, right after the preflight checks at ~7801, and keep only `OnSnapshotLoaded`'s success path here.)

(b) In `StartBackgroundValidation()` (11485), after `bg_validation_status_ = BackgroundValidationStatus::InProgress;` (line 11512) add:

```cpp
    EnsureAssumeUtxoLifecycle();
    assumeutxo_lifecycle_->OnValidationStarted(std::chrono::steady_clock::now());
```

(c) In `BackgroundValidationWorker()` (11547–11639), make missing bodies loop-and-retry instead of skip-and-succeed. Replace the single-pass scan body (lines 11556–11620, from `const uint32_t target_height = ...` through the `blocks_skipped > 0` warning block) with:

```cpp
        const uint32_t target_height = assumeutxo_base_height_;
        const uint32_t log_interval = std::max(1u, target_height / 100);

        // Spec: missing bodies are NEVER skippable success. Re-scan until all
        // bodies are present (bodies backfill as IBD proceeds) or we are told
        // to stop; Tick() drives the loud-stall transition while we wait.
        uint32_t blocks_skipped = 0;
        while (true) {
            blocks_skipped = 0;
            for (uint32_t height = 0; height <= target_height; ++height) {
                if (bg_validation_should_stop_) {
                    logger_->warning("[BackgroundValidation] Validation stopped by request");
                    OnBackgroundValidationComplete(false, "Validation stopped by user");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(bg_validation_mutex_);
                    bg_validation_current_height_ = height;
                }
                auto hash_result = chain_db_->getBlockHashByHeight(height);
                if (!hash_result.ok()) { blocks_skipped++; continue; }
                auto block_result = getBlockByHash(hash_result.value());
                if (!block_result.ok()) { blocks_skipped++; continue; }
                {
                    std::lock_guard<std::mutex> lock(bg_validation_mutex_);
                    bg_validation_current_height_ = height;
                    bg_validation_blocks_validated_++;
                }
                assumeutxo_lifecycle_->OnBlockValidated(height, std::chrono::steady_clock::now());
                if (height % log_interval == 0 || height == target_height) {
                    double percent = (static_cast<double>(height) /
                                      static_cast<double>(target_height)) * 100.0;
                    logger_->info("[BackgroundValidation] Progress: " + std::to_string(height) +
                                "/" + std::to_string(target_height) + " (" +
                                std::to_string(static_cast<int>(percent)) + "%)");
                }
            }
            if (blocks_skipped == 0) break;

            assumeutxo_lifecycle_->OnMissingBodies(blocks_skipped);
            logger_->warning("[BackgroundValidation] " + std::to_string(blocks_skipped) +
                             "/" + std::to_string(target_height + 1) +
                             " bodies unavailable — waiting for backfill (spec: missing"
                             " bodies are not success); re-scan in 30s");
            for (int i = 0; i < 30 && !bg_validation_should_stop_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            assumeutxo_lifecycle_->Tick(std::chrono::steady_clock::now());
            if (bg_validation_should_stop_) {
                OnBackgroundValidationComplete(false, "Validation stopped by user");
                return;
            }
        }
        assumeutxo_lifecycle_->OnMissingBodies(0);
        logger_->info("[BackgroundValidation] All bodies present; verifying UTXO set integrity...");
```

(`EnsureAssumeUtxoLifecycle()` must be called once at worker start, right after the `chain_db_` null-check. Add `#include <thread>` if not already present.)

(d) Still in the worker, after `VerifyUTXOSetMatch()` (the lines at 11622–11632), replace with:

```cpp
        const bool match = VerifyUTXOSetMatch();
        // replay_performed=false: this pass is availability+count verification
        // only. The lifecycle will NOT enter fully_validated from it (honest
        // mode until the real replay engine lands — see plan Scope decision).
        assumeutxo_lifecycle_->OnReplayComplete(
            /*replay_performed=*/false, match,
            /*expected_commitment=*/"(snapshot utxo count)",
            /*recomputed_commitment=*/match ? "(count match)" : "(count mismatch)",
            /*missing_body_count=*/0, std::chrono::steady_clock::now());
        if (!match) {
            std::string error = "UTXO set mismatch at snapshot height " + std::to_string(target_height);
            logger_->error("[BackgroundValidation] CRITICAL: " + error);
            OnBackgroundValidationComplete(false, error);
            return;
        }
        OnBackgroundValidationComplete(true, "");
```

- [ ] **Step 5: Replace the auto-rollback failure branch (cpp 11780–11823)**

Replace the ENTIRE `} else {` failure branch of `OnBackgroundValidationComplete` (everything from `bg_validation_status_ = BackgroundValidationStatus::Failed;` through the final `logger_->error("═══...");` before the closing brace) with:

```cpp
        bg_validation_status_ = BackgroundValidationStatus::Failed;
        bg_validation_error_ = error;

        // Spec (Fatal Mismatch Semantics): a mismatch is NOT an automatic
        // rollback-to-genesis. Persist fatal, halt assumed-state decisions via
        // safe mode, and require explicit operator reset. The previous
        // auto-rollback (ClearAll + ClearAssumeUTXOState) is intentionally GONE.
        logger_->error("═══════════════════════════════════════════════════════════════════════");
        logger_->error("❌ BACKGROUND VALIDATION FAILED — ENTERING FATAL STATE");
        logger_->error("Error: " + error);
        logger_->error("Snapshot base height: " + std::to_string(assumeutxo_base_height_));
        logger_->error("Snapshot base hash:   " + assumeutxo_base_block_.GetHex());
        logger_->error("Node was serving from state that failed later proof.");
        logger_->error("Balances/confirmations derived while assumed are PROVISIONAL.");
        logger_->error("Operator reset required: blockchain.resetassumeutxofatal");
        logger_->error("  (or wipe the datadir and resync from genesis).");
        logger_->error("═══════════════════════════════════════════════════════════════════════");

        EnsureAssumeUtxoLifecycle();
        if (assumeutxo_lifecycle_->GetState() !=
            assumeutxo::AssumeUtxoLifecycle::State::FatalMismatch) {
            // Worker exceptions / operational failures that bypassed
            // OnReplayComplete still converge to fatal here.
            assumeutxo_lifecycle_->OnReplayComplete(
                /*replay_performed=*/false, /*commitment_match=*/false,
                "(background validation)", error, 0,
                std::chrono::steady_clock::now());
        }
        EnterSafeMode("assumeutxo fatal: " + error);
```

**Note:** one behavior question the implementer must resolve by reading `EnterSafeMode` (cpp 7394): if it takes a lock also held here (`bg_validation_mutex_` is held in this function), call it AFTER releasing — restructure so the fatal block runs outside the `lock_guard` scope (collect strings under lock, act after). Deadlock check is part of this step.

- [ ] **Step 6: Gate the success-path trust clear (cpp 11763–11765)**

Replace:

```cpp
        if (chaindb_caught_up) {
            logger_->info("[BackgroundValidation] ChainDB at snapshot height — exiting AssumeUTXO mode");
            ClearAssumeUTXOState(/*clear_persisted_metadata=*/true);
        } else {
```

with:

```cpp
        const bool lifecycle_fully_validated =
            assumeutxo_lifecycle_ &&
            assumeutxo_lifecycle_->GetState() ==
                assumeutxo::AssumeUtxoLifecycle::State::FullyValidated;
        if (chaindb_caught_up && lifecycle_fully_validated) {
            logger_->info("[BackgroundValidation] ChainDB at snapshot height and history "
                          "fully validated — exiting AssumeUTXO mode");
            ClearAssumeUTXOState(/*clear_persisted_metadata=*/true);
        } else if (chaindb_caught_up) {
            logger_->info("[BackgroundValidation] ChainDB caught up but historical replay "
                          "is not complete — keeping AssumeUTXO trust marker (spec: only a "
                          "completed genesis-to-base comparison retires trust)");
        } else {
```

- [ ] **Step 7: Build everything + full test suite**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: clean build; ALL existing tests pass; `AssumeUtxoLifecycle` passes.
If any existing test asserted the old auto-rollback behavior, read it — the spec deliberately changed that behavior; update the test to the new contract and say so in the commit message.

- [ ] **Step 8: Commit**

```bash
git add include/daemon/services/chainstate_service.h src/daemon/services/chainstate_service.cpp
git commit -m "daemon: replace assumeutxo auto-rollback with fatal state machine

Background-validation failure now persists fatal_mismatch and enters safe
mode instead of silently wiping the UTXO set and resyncing (spec:
docs/design/assumeutxo-fatal-state-machine.md). Missing historical bodies
re-scan instead of counting as success; trust marker is only cleared when
the lifecycle reaches fully_validated."
```

---

### Task 8: RPC contract + operator reset RPC

**Files:**
- Modify: `src/rpc/methods_blockchain_context.cpp` (`buildSnapshotBootstrapDiagnostics` line 350–471; new handler + registration ~line 3688)

- [ ] **Step 1: Surface lifecycle fields in `buildSnapshotBootstrapDiagnostics`**

In `buildSnapshotBootstrapDiagnostics` (line 350), right after `snapshot["background_validation_complete"] = ...` (line 356), add:

```cpp
    // Fatal-state-machine contract (docs/design/assumeutxo-fatal-state-machine.md).
    if (auto* lc = chainstate->GetAssumeUtxoLifecycle()) {
        const auto st = lc->GetStatus(std::chrono::steady_clock::now());
        snapshot["history_validation_state"] =
            dinero::assumeutxo::AssumeUtxoLifecycle::StateName(st.state);
        snapshot["history_fully_validated"] = st.history_fully_validated;
        snapshot["fatal"] = st.fatal;
        snapshot["fatal_reason"] = st.fatal_reason;
        snapshot["assumeutxo_active"] = st.assumeutxo_active;  // override legacy bool
        snapshot["current_validation_height"] = st.current_validation_height;
        snapshot["target_validation_height"] = st.target_validation_height;
        snapshot["missing_body_count"] = st.missing_body_count;
        snapshot["stall_seconds"] = static_cast<Json::Int64>(st.stall_seconds);
        snapshot["next_action"] = st.next_action;  // lifecycle wins over legacy strings
        if (st.snapshot_base_height > 0) {
            snapshot["snapshot_base_height"] = st.snapshot_base_height;
            snapshot["snapshot_base_block"] = st.snapshot_base_block.GetHex();
        }
    }
```

Then DELETE the legacy `next_action` if/else chain at lines 454–468 ONLY IF the lifecycle block above always runs (it does — `GetAssumeUtxoLifecycle()` never returns nullptr); otherwise leave it as the fallback and ensure the lifecycle assignment comes after it. Keep field types consistent with the surrounding `din::Json` usage (`Json::UInt64`/`Json::Int64` casts as used at lines 387, 418).

- [ ] **Step 2: Add the reset RPC handler**

Add after `rpc_context_getbackgroundvalidationprogress` (ends ~line 1916), following the exact pattern of `rpc_context_getblockcount` (line 485):

```cpp
/**
 * blockchain.resetassumeutxofatal - Explicit operator reset of fatal_mismatch
 * (docs/design/assumeutxo-fatal-state-machine.md: Operator Reset). Requires
 * params {"confirm": "RESET-ASSUMEUTXO-FATAL"}; writes an audit log entry.
 */
static din::Json rpc_context_resetassumeutxofatal(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }
    std::string confirm;
    if (params.isObject() && params.isMember("confirm")) {
        confirm = params["confirm"].asString();
    } else if (params.isArray() && params.size() >= 1) {
        confirm = params[0].asString();
    }
    auto* lc = chainstate->GetAssumeUtxoLifecycle();
    if (!lc->GetStatus(std::chrono::steady_clock::now()).fatal) {
        result["error"] = "Node is not in assumeutxo fatal_mismatch state";
        return result;
    }
    if (!lc->OperatorReset(confirm)) {
        result["error"] = std::string("Reset refused: pass {\"confirm\": \"") +
                          dinero::assumeutxo::kResetToken + "\"}";
        return result;
    }
    result["reset"] = true;
    result["next_action"] = "Fatal state cleared. Load a snapshot or sync from genesis.";
    return result;
}
```

(Adjust the params-extraction idiom to match how nearby handlers in this file read named/positional params — copy from `rpc_context_loadtxoutset`.)

- [ ] **Step 3: Register it**

In `registerBlockchainMethodsContext()` after the `getsnapshotbootstrapstatus` registration (~line 3688):

```cpp
    g_rpcRegistry.registerHandler("blockchain.resetassumeutxofatal",
                                 rpc_context_resetassumeutxofatal,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("resetassumeutxofatal", "blockchain.resetassumeutxofatal");
```

- [ ] **Step 4: Build + test**

Run: `cmake --build build -j8 && ctest --test-dir build --output-on-failure`
Expected: clean build, all tests pass. (RPC handlers here are thin delegation over the tested lifecycle; the params idiom is verified by compilation + the existing RPC suite if present.)

- [ ] **Step 5: Commit**

```bash
git add src/rpc/methods_blockchain_context.cpp
git commit -m "rpc: expose assumeutxo lifecycle contract + blockchain.resetassumeutxofatal"
```

---

### Task 9: Final verification + spec cross-check

- [ ] **Step 1: Full clean build + full suite**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected: all green. Then `touch src/daemon/services/assumeutxo_lifecycle.cpp`, rebuild, confirm the `.o` rebuilt (stale-object guard), re-run `ctest -R AssumeUtxoLifecycle`.

- [ ] **Step 2: Spec compliance sweep**

For each spec section (States, Transitions, Completion Criteria, Fatal Semantics, Stall, RPC Contract, Persistence, Operator Reset, Required Tests), grep the implementation for its anchor and note file:line in the PR description. Verify the five forbidden transitions each have a refusing code path:
- `snapshot_loaded -> fully_validated`: `OnReplayComplete` state guard (Task 1 test `CannotCompleteFromSnapshotLoaded`)
- `validating_history -> fully_validated` with skipped bodies: `missing_body_count > 0` guard (Task 3)
- `fatal_mismatch -> validating_history` without reset: `OnSnapshotLoaded`/`OnValidationStarted` guards (Task 6)
- `validation_stalled -> fully_validated` without resumed progress: completion requires `OnReplayComplete` from a validating/stalled state with match+no-missing+replay — stall recovery path tested in Task 4
- fully_validated marker + chainstate mismatch -> fatal (Task 5)

- [ ] **Step 3: git hygiene + handoff**

```bash
git diff --check
git log --oneline dinero-main..HEAD
```

Expected: no whitespace errors; ~8 commits. Then use superpowers:finishing-a-development-branch (do NOT merge or push without the human's go).

## Follow-up plan (explicitly out of scope here)

A second plan (`assumeutxo-replay-engine`, to be written via superpowers:writing-plans when scheduled) — real genesis→base block connection + canonical state-commitment recompute, regtest fixture chain with snapshot at low height, flipping `replay_performed=true`, and the spec's Release Gate items 1–2. Until that lands, no production node reports `fully_validated` — by design.
