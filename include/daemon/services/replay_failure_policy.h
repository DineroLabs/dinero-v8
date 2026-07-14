#pragma once

#include <cstdint>
#include <map>

namespace dinero::assumeutxo {

// Decides whether a genesis->base replay validation failure is treated as a
// transient local fault (retry the pass with a fresh engine and fresh body
// reads) or a confirmed snapshot mismatch (fatal).
//
// Why this exists (2026-07-14 field incident): a background replay pass
// failed block 143 with bad-utreexo-root even though the stored body was
// byte-identical to the canonical block AND an earlier pass in the same run
// had already validated that height (lc_progress 1600). The failure was
// transient (torn read / heap corruption), but the worker escalated it
// straight to a persisted fatal_mismatch + safe mode, bricking the node
// until operator reset. A real snapshot mismatch is stable by nature — it
// reproduces on a fresh pass with freshly-read, merkle-consistent bodies —
// so requiring confirmation costs one extra pass in the fraud case and
// saves the node in the transient case.
//
// The #298 fatal semantics ("mismatch = fatal, no auto-rollback") are
// preserved for CONFIRMED mismatches; this only inserts the confirmation.
class ReplayFailurePolicy {
public:
    enum class Action {
        kRetryPass,     // discard the pass, re-read bodies, replay again
        kConfirmFatal,  // the failure is confirmed (or retries exhausted)
    };

    explicit ReplayFailurePolicy(uint32_t confirmations_required = 2,
                                 uint32_t max_total_retries = 8)
        : confirmations_required_(confirmations_required),
          max_total_retries_(max_total_retries) {}

    // A height failed real validation this pass.
    Action OnValidationFailure(uint32_t height) {
        const uint32_t count = ++fail_counts_[height];
        if (count >= confirmations_required_) {
            return Action::kConfirmFatal;
        }
        if (total_retries_ >= max_total_retries_) {
            // Runaway guard: failures hopping across heights forever still
            // converge to the spec fatal instead of retrying unbounded.
            return Action::kConfirmFatal;
        }
        ++total_retries_;
        return Action::kRetryPass;
    }

    // A height passed validation: any earlier failure there was transient.
    void OnValidationSuccess(uint32_t height) {
        if (!fail_counts_.empty()) {
            fail_counts_.erase(height);
        }
    }

    uint32_t TotalRetries() const { return total_retries_; }

private:
    uint32_t confirmations_required_;
    uint32_t max_total_retries_;
    uint32_t total_retries_ = 0;
    std::map<uint32_t, uint32_t> fail_counts_;
};

}  // namespace dinero::assumeutxo
