#include "mining_determinism_oracle_md2.h"
#include <sstream>

// Ring 4 Phase 4f: MD2 - Restart Replay Determinism Implementation

namespace mining_test {

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MD2Oracle::name() const {
    return "MD2: Restart Replay Determinism";
}

std::vector<DeterminismViolation> MD2Oracle::check(
    const MiningTrace& reference,
    const MiningTrace& candidate
) const {
    std::vector<DeterminismViolation> violations;

    // Must have same overall length for deterministic replay
    if (!sameLengths(reference, candidate)) {
        violations.emplace_back(
            "MD2",
            "Trace length mismatch in restart replay",
            0
        );
        return violations;
    }

    // Find restart boundary
    size_t restart_index = findRestartBoundary(reference);

    // No restart found → MD2 not applicable (no violations)
    if (restart_index == reference.events.size()) {
        return violations;
    }

    // Compare suffix after restart boundary
    if (!compareSuffix(reference, candidate, restart_index, violations)) {
        return violations;  // Divergence detected
    }

    // Suffix is identical
    return violations;
}

// ============================================================================
// Private Methods
// ============================================================================

size_t MD2Oracle::findRestartBoundary(const MiningTrace& trace) const {
    // Find first ERROR_OCCURRED event that indicates crash or restart
    // In Phase 4b, crashes/restarts are recorded as ERROR_OCCURRED events
    // with specific descriptions ("System crashed", "System restarted")

    for (size_t i = 0; i < trace.events.size(); ++i) {
        const auto& event = trace.events[i];

        // Check for ERROR_OCCURRED event type
        if (event.type == MiningEventType::ERROR_OCCURRED) {
            // Check if it's a crash or restart event
            if (event.error_message.has_value()) {
                const std::string& msg = *event.error_message;
                if (msg.find("crashed") != std::string::npos ||
                    msg.find("restarted") != std::string::npos) {
                    return i;
                }
            }
        }
    }

    // No restart boundary found
    return trace.events.size();
}

bool MD2Oracle::compareSuffix(
    const MiningTrace& reference,
    const MiningTrace& candidate,
    size_t restart_index,
    std::vector<DeterminismViolation>& violations
) const {
    const size_t count = reference.events.size();

    // Compare events after restart boundary
    for (size_t i = restart_index; i < count; ++i) {
        const auto& ev_ref = reference.events[i];
        const auto& ev_cand = candidate.events[i];

        if (!(ev_ref == ev_cand)) {
            std::ostringstream msg;
            msg << "Event divergence after restart at index " << i
                << " (restart boundary at " << restart_index << ")";

            violations.emplace_back("MD2", msg.str(), i);
            return false;
        }
    }

    // Compare states after restart boundary
    for (size_t i = restart_index; i < count && i < reference.snapshots.size(); ++i) {
        const auto& st_ref = reference.snapshots[i];
        const auto& st_cand = candidate.snapshots[i];

        if (!(st_ref == st_cand)) {
            std::ostringstream msg;
            msg << "State divergence after restart at index " << i
                << " (restart boundary at " << restart_index << ")";

            violations.emplace_back("MD2", msg.str(), i);
            return false;
        }
    }

    return true;
}

}  // namespace mining_test
