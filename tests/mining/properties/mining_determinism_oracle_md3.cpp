#include "mining_determinism_oracle_md3.h"
#include <sstream>

// Ring 4 Phase 4f: MD3 - Action Commutativity Implementation

namespace mining_test {

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MD3Oracle::name() const {
    return "MD3: Action Commutativity (Where Allowed)";
}

std::vector<DeterminismViolation> MD3Oracle::check(
    const MiningTrace& reference,
    const MiningTrace& candidate
) const {
    std::vector<DeterminismViolation> violations;

    // Structural mismatch is an immediate failure
    // If swapping independent actions changes trace length,
    // the actions were NOT actually independent
    if (!sameLengths(reference, candidate)) {
        violations.emplace_back(
            "MD3",
            "Trace length mismatch under action reordering",
            0
        );
        return violations;
    }

    const size_t count = reference.events.size();

    // Compare events element-by-element
    for (size_t i = 0; i < count; ++i) {
        const auto& ev_ref = reference.events[i];
        const auto& ev_cand = candidate.events[i];

        if (!(ev_ref == ev_cand)) {
            std::ostringstream msg;
            msg << "Event divergence after commuting independent actions at index " << i;

            violations.emplace_back("MD3", msg.str(), i);
            return violations;
        }
    }

    // Compare states element-by-element
    for (size_t i = 0; i < count && i < reference.snapshots.size(); ++i) {
        const auto& st_ref = reference.snapshots[i];
        const auto& st_cand = candidate.snapshots[i];

        if (!(st_ref == st_cand)) {
            std::ostringstream msg;
            msg << "State divergence after commuting independent actions at index " << i;

            violations.emplace_back("MD3", msg.str(), i);
            return violations;
        }
    }

    // Traces are identical - actions truly commute
    return violations;
}

}  // namespace mining_test
