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
        utxo_index_->DeleteMetadata(kLcProgressHeightKey);
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
    if (auto ph = utxo_index_->GetMetadata(kLcProgressHeightKey)) {
        // Resume from the last durable progress marker (throttled writes mean
        // this may trail actual progress by up to ~100 blocks).
        current_height_ = static_cast<uint32_t>(std::stoul(ph.value()));
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
    }
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
