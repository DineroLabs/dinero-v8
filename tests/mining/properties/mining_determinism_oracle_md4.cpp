#include "mining_determinism_oracle_md4.h"
#include <sstream>

// Ring 4 Phase 4f: MD4 - No Hidden Entropy Sources Implementation

namespace mining_test {

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MD4Oracle::name() const {
    return "MD4: No Hidden Entropy Sources";
}

std::vector<DeterminismViolation> MD4Oracle::check(
    const MiningTrace& reference,
    const MiningTrace& candidate
) const {
    std::vector<DeterminismViolation> violations;

    // Any divergence with same seed implies hidden entropy
    // This is stricter than MD1 - we're auditing entropy sources

    if (!sameLengths(reference, candidate)) {
        violations.emplace_back(
            "MD4",
            "Trace length mismatch under identical seed - hidden entropy suspected",
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
            msg << "Event divergence under identical seed at index " << i
                << " - hidden entropy detected";

            violations.emplace_back("MD4", msg.str(), i);
            return violations;
        }
    }

    // Compare states element-by-element
    for (size_t i = 0; i < count && i < reference.snapshots.size(); ++i) {
        const auto& st_ref = reference.snapshots[i];
        const auto& st_cand = candidate.snapshots[i];

        if (!(st_ref == st_cand)) {
            std::ostringstream msg;
            msg << "State divergence under identical seed at index " << i
                << " - hidden entropy detected";

            violations.emplace_back("MD4", msg.str(), i);
            return violations;
        }
    }

    // No divergence - all entropy comes from seed
    return violations;
}

}  // namespace mining_test
