#include "mining_determinism_oracle_md5.h"
#include <sstream>

// Ring 4 Phase 4f: MD5 - Deterministic Crash Recovery Implementation

namespace mining_test {

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MD5Oracle::name() const {
    return "MD5: Deterministic Crash Recovery";
}

size_t MD5Oracle::findCrashRecoveryBoundary(const MiningTrace& trace) const {
    // Find the first event after a crash+recovery sequence
    bool found_crash = false;

    for (size_t i = 0; i < trace.events.size(); ++i) {
        const auto& event = trace.events[i];

        if (event.type == MiningEventType::ERROR_OCCURRED) {
            if (event.error_message.has_value()) {
                const std::string& msg = *event.error_message;

                if (msg.find("crashed") != std::string::npos) {
                    found_crash = true;
                } else if (found_crash && msg.find("restarted") != std::string::npos) {
                    // Found crash followed by restart
                    // Return the index AFTER the restart event
                    return i + 1;
                }
            }
        }
    }

    // No crash+recovery boundary found
    return trace.events.size();
}

std::vector<DeterminismViolation> MD5Oracle::check(
    const MiningTrace& reference,
    const MiningTrace& candidate
) const {
    std::vector<DeterminismViolation> violations;

    // Find crash recovery boundaries in both traces
    const size_t ref_boundary = findCrashRecoveryBoundary(reference);
    const size_t cand_boundary = findCrashRecoveryBoundary(candidate);

    // If boundaries differ, recovery happened at different points
    if (ref_boundary != cand_boundary) {
        std::ostringstream msg;
        msg << "Crash recovery boundary mismatch: reference="
            << ref_boundary << ", candidate=" << cand_boundary;

        violations.emplace_back("MD5", msg.str(), 0);
        return violations;
    }

    // If no crash recovery found in either trace, compare entire traces
    if (ref_boundary >= reference.events.size() &&
        cand_boundary >= candidate.events.size()) {
        // No crash recovery - fall back to full trace comparison
        if (!sameLengths(reference, candidate)) {
            violations.emplace_back(
                "MD5",
                "Trace length mismatch (no crash recovery found)",
                0
            );
            return violations;
        }

        // Compare all events
        const size_t count = reference.events.size();
        for (size_t i = 0; i < count; ++i) {
            if (!(reference.events[i] == candidate.events[i])) {
                std::ostringstream msg;
                msg << "Event divergence at index " << i
                    << " (no crash recovery)";

                violations.emplace_back("MD5", msg.str(), i);
                return violations;
            }
        }

        return violations;
    }

    // Compare post-recovery suffixes
    const size_t ref_remaining = reference.events.size() - ref_boundary;
    const size_t cand_remaining = candidate.events.size() - cand_boundary;

    if (ref_remaining != cand_remaining) {
        std::ostringstream msg;
        msg << "Post-recovery trace length mismatch: reference="
            << ref_remaining << ", candidate=" << cand_remaining;

        violations.emplace_back("MD5", msg.str(), ref_boundary);
        return violations;
    }

    // Compare post-recovery events element-by-element
    for (size_t i = 0; i < ref_remaining; ++i) {
        const size_t ref_idx = ref_boundary + i;
        const size_t cand_idx = cand_boundary + i;

        const auto& ev_ref = reference.events[ref_idx];
        const auto& ev_cand = candidate.events[cand_idx];

        if (!(ev_ref == ev_cand)) {
            std::ostringstream msg;
            msg << "Post-recovery event divergence at recovery_offset="
                << i << " (absolute index=" << ref_idx << ")";

            violations.emplace_back("MD5", msg.str(), ref_idx);
            return violations;
        }
    }

    // Compare post-recovery states element-by-element
    for (size_t i = 0; i < ref_remaining &&
         (ref_boundary + i) < reference.snapshots.size(); ++i) {
        const size_t ref_idx = ref_boundary + i;
        const size_t cand_idx = cand_boundary + i;

        if (cand_idx >= candidate.snapshots.size()) {
            break;  // Candidate has fewer snapshots
        }

        const auto& st_ref = reference.snapshots[ref_idx];
        const auto& st_cand = candidate.snapshots[cand_idx];

        if (!(st_ref == st_cand)) {
            std::ostringstream msg;
            msg << "Post-recovery state divergence at recovery_offset="
                << i << " (absolute index=" << ref_idx << ")";

            violations.emplace_back("MD5", msg.str(), ref_idx);
            return violations;
        }
    }

    // Post-recovery paths are identical - crash recovery is deterministic
    return violations;
}

}  // namespace mining_test
