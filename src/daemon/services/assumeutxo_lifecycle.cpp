#include "daemon/services/assumeutxo_lifecycle.h"

#include "common/logger.h"

namespace dinero::assumeutxo {

namespace {
constexpr const char* kStateNames[] = {
    "disabled", "snapshot_loaded", "validating_history",
    "validation_stalled", "fatal_mismatch", "fully_validated",
};
// ParseU32: safe alternative to std::stoul for persisted height values.
// Returns false (without throwing) if the string is non-numeric or out of range.
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

const char* AssumeUtxoLifecycle::StateName(State s) {
    return kStateNames[static_cast<int>(s)];
}

void AssumeUtxoLifecycle::SetState(State next, const std::string& reason) {
    if (next == state_) return;  // no-op: byte-identical to a same-value assign
    if (logger_) logger_->info(
        std::string("[AssumeUtxoLifecycle] #298 state ") + StateName(state_) +
        " -> " + StateName(next) + " (" + reason + ")");
    state_ = next;
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
    SetState(State::SnapshotLoaded, "snapshot loaded, fast bootstrap active");
    base_block_ = base_block;
    base_height_ = base_height;
    current_height_ = 0;
    missing_bodies_ = 0;
    Persist();
    // Fresh load: any stale progress marker from a prior run is invalid.
    if (utxo_index_) utxo_index_->DeleteMetadata(kLcProgressHeightKey);
    return true;
}

bool AssumeUtxoLifecycle::OnValidationStarted(TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::SnapshotLoaded && state_ != State::ValidationStalled &&
        state_ != State::ValidatingHistory) return false;
    if (state_ == State::ValidationStalled) {
        // Spec (Stall Semantics): leaving validation_stalled requires actual
        // progress. A worker restart only re-arms the stall clock; the stall
        // stays visible until OnBlockValidated delivers a real block.
        last_progress_time_ = now;
        has_progress_time_ = true;
        return true;
    }
    SetState(State::ValidatingHistory, "validation worker started");
    last_progress_time_ = now;
    has_progress_time_ = true;
    Persist();
    return true;
}

void AssumeUtxoLifecycle::OnBlockValidated(uint32_t height, TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::ValidatingHistory && state_ != State::ValidationStalled) return;
    if (state_ == State::ValidationStalled) {
        SetState(State::ValidatingHistory,
                 "real progress recovered stall");  // real progress recovers a stall
        Persist();
    }
    current_height_ = height;
    last_progress_time_ = now;
    has_progress_time_ = true;
    // Durable progress marker, throttled (see kLcProgressHeightKey): every
    // 100th block plus the base block itself, so restart resumes near here.
    if (utxo_index_ && (height % 100 == 0 || height == base_height_)) {
        utxo_index_->SetMetadata(kLcProgressHeightKey, std::to_string(height));
    }
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
    if (state_ != State::ValidatingHistory) {
        // Spec: leaving validation_stalled requires actual progress
        // (OnBlockValidated) before completion can be claimed.
        return false;
    }
    SetState(State::FullyValidated, "replay performed + commitment match + no missing bodies");
    Persist();
    if (logger_) {
        logger_->info(
            "[AssumeUtxoLifecycle] fully_validated: snapshot trust assumption retired at height " +
            std::to_string(base_height_));
        // #298 Greppable proof of a clean retirement (replay_performed,
        // commitment_match, and missing_bodies==0 are guaranteed here by the
        // guards above that already returned on any failing condition).
        logger_->info(
            "[AssumeUtxoLifecycle] #298 fully_validated PROOF: replay_performed=1 "
            "commitment_match=1 missing_bodies=0 height=" + std::to_string(base_height_));
    }
    return true;
}

void AssumeUtxoLifecycle::Tick(TimePoint now) {
    std::lock_guard<std::mutex> lock(mu_);

    // #298 Stall watchdog: while a stall persists (validation_stalled, or
    // validating_history still blocked on missing pre-base bodies), emit a
    // LOUD diagnostic throttled to once per kStallDiagInterval so a silent
    // indefinite stall becomes operator-visible. Purely diagnostic — it does
    // NOT change state. Runs before the early-return below precisely because
    // that guard skips validation_stalled. has_progress_time_ guards against a
    // bogus epoch-to-now duration on a stall restored from persistence.
    const bool stall_diag_eligible =
        (state_ == State::ValidationStalled) ||
        (state_ == State::ValidatingHistory && missing_bodies_ > 0);
    if (stall_diag_eligible && has_progress_time_ &&
        now - last_progress_time_ > kStallDiagInterval &&
        (!has_diag_emit_time_ || now - last_diag_emit_time_ >= kStallDiagInterval)) {
        last_diag_emit_time_ = now;
        has_diag_emit_time_ = true;
        if (logger_) logger_->error(
            "[AssumeUtxoLifecycle] #298 STALL DIAGNOSTIC: validation_stalled for " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                now - last_progress_time_).count()) +
            "s missing_bodies=" + std::to_string(missing_bodies_) +
            " current_height=" + std::to_string(current_height_) + "/" +
            std::to_string(base_height_) + " reason=missing-pre-base-bodies");
    }

    if (state_ != State::ValidatingHistory || !has_progress_time_) return;
    if (current_height_ >= base_height_ && missing_bodies_ == 0) return;
    if (now - last_progress_time_ >= stall_timeout_) {
        SetState(State::ValidationStalled, "stall timeout exceeded with no progress");
        Persist();
        if (logger_) logger_->error(
            "[AssumeUtxoLifecycle] validation_stalled: no historical block validated for " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                now - last_progress_time_).count()) +
            "s at height " + std::to_string(current_height_) + "/" + std::to_string(base_height_) +
            " (" + std::to_string(missing_bodies_) + " bodies missing)");
    }
}

void AssumeUtxoLifecycle::ForceFatal(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::FatalMismatch) return;  // keep the FIRST (root-cause) reason
    // EnterFatal's Persist() writes kLifecycleStateKey="fatal_mismatch" +
    // kFatalReasonKey and DELETES kFullyValidatedKey (state != FullyValidated
    // else-arm), and RestoreFromPersistence dispatches the fatal_mismatch
    // branch FIRST — so a prior fully_validated record cannot shadow this
    // across a restart.
    EnterFatal(reason, TimePoint{});
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
        utxo_index_->DeleteMetadata(kLcProgressHeightKey);
        utxo_index_->DeleteMetadata(kExpectedCommitmentKey);
        utxo_index_->DeleteMetadata(kExpectedUtreexoRootKey);
        // Must be cleared with its siblings. Left behind, snapshot A's root
        // outlives the reset and snapshot B's replay is compared against it.
        utxo_index_->DeleteMetadata(kExpectedShieldedRootKey);
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
        uint32_t h = 0;
        if (!ParseU32(bh.value(), h)) {
            EnterFatal("corrupt lifecycle metadata: non-numeric height value", TimePoint{});
            return;
        }
        base_height_ = h;
    }
    if (auto ph = utxo_index_->GetMetadata(kLcProgressHeightKey)) {
        // Resume from the last durable progress marker (throttled writes mean
        // this may trail actual progress by up to ~100 blocks).
        uint32_t progress_h = 0;
        if (!ParseU32(ph.value(), progress_h)) {
            EnterFatal("corrupt lifecycle metadata: non-numeric height value", TimePoint{});
            return;
        }
        current_height_ = progress_h;
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
    if (name == "validating_history") {
        state_ = State::ValidatingHistory;
        return;
    }
    if (name == "validation_stalled") {
        // Spec (Persistence): stalled remains stalled until progress resumes.
        state_ = State::ValidationStalled;
        return;
    }
    // Unknown value: leave Disabled.
}

void AssumeUtxoLifecycle::Disable() {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::FatalMismatch) return;  // only OperatorReset leaves fatal
    state_ = State::Disabled;
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
        utxo_index_->DeleteMetadata(kLcProgressHeightKey);
        utxo_index_->DeleteMetadata(kExpectedCommitmentKey);
        utxo_index_->DeleteMetadata(kExpectedUtreexoRootKey);
        // Must be cleared with its siblings. Left behind, snapshot A's root
        // outlives the reset and snapshot B's replay is compared against it.
        utxo_index_->DeleteMetadata(kExpectedShieldedRootKey);
    }
}

AssumeUtxoLifecycle::State AssumeUtxoLifecycle::GetState() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string AssumeUtxoLifecycle::StallReason() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::ValidationStalled && missing_bodies_ > 0) {
        return "missing-pre-base-bodies:" + std::to_string(missing_bodies_);
    }
    return "";
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
    } else {
        utxo_index_->DeleteMetadata(kFullyValidatedKey);
    }
    if (state_ == State::FatalMismatch) {
        utxo_index_->SetMetadata(kFatalReasonKey, fatal_reason_);
    } else {
        utxo_index_->DeleteMetadata(kFatalReasonKey);
    }
}

void AssumeUtxoLifecycle::EnterFatal(const std::string& reason, TimePoint /*now*/) {
    SetState(State::FatalMismatch, reason);
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
