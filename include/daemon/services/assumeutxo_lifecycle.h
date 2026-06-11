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
// Last durable validation-progress height. Written by OnBlockValidated on a
// throttle (height % 100 == 0, or height == base height) so a restart resumes
// from at most ~100 blocks back instead of zero.
inline constexpr const char* kLcProgressHeightKey = "assumeutxo_lc_progress_height";
// Expected content commitment (SHA256 over sorted UTXO records) persisted by
// LoadSnapshot so the replay engine can verify its recomputed digest.
inline constexpr const char* kExpectedCommitmentKey  = "assumeutxo_expected_commitment";
// v3 utreexo root (hex) persisted by LoadSnapshot. Only present for v3 snapshots.
inline constexpr const char* kExpectedUtreexoRootKey = "assumeutxo_expected_utreexo_root";

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
    // SnapshotLoaded -> ValidatingHistory. Also re-accepted in ValidatingHistory
    // (worker restart re-arms the stall clock) and in ValidationStalled, where it
    // re-arms the clock but the state REMAINS ValidationStalled — leaving a stall
    // requires actual progress (OnBlockValidated), not just a worker restart.
    // Call once per background-validation worker start.
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
    // Direct fatal entry, usable from ANY non-fatal state (including
    // FullyValidated, where OnReplayComplete's mismatch path is refused by
    // the state guard). Needed for post-promotion proof failures, e.g. a
    // higher-work fork below the snapshot base (spec: fatal, not reorg).
    // No-op if already FatalMismatch (the first reason is the root cause).
    void ForceFatal(const std::string& reason);
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
