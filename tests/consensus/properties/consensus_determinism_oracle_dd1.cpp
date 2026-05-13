#include "consensus_determinism_oracle_dd1.h"
#include <sstream>
#include <iomanip>

namespace dinero {
namespace consensus {
namespace test {

std::vector<DeterminismViolation> DD1Oracle::observeTraces(
    const std::vector<ConsensusTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    if (traces.empty()) {
        // No traces to check - property trivially holds
        return violations;
    }

    if (traces.size() < 2) {
        // Need at least 2 traces to check reproducibility
        return violations;
    }

    // Check if all traces have the same hash
    bool same_hash = tracesHaveSameHash(traces);

    if (!same_hash) {
        // Violation: Traces have different hashes (non-deterministic execution)
        std::ostringstream desc;
        desc << "Trace reproducibility violated: " << traces.size() << " runs with seed "
             << traces[0].rng_seed << " produced different trace hashes.";

        DeterminismViolation v(
            getName(),
            desc.str(),
            traces[0].rng_seed
        );

        // Build detailed report of hash mismatches
        std::ostringstream details;
        details << "Hash values: ";
        for (size_t i = 0; i < traces.size(); ++i) {
            details << "run" << i << "=0x" << std::hex << std::setw(16) << std::setfill('0')
                   << traces[i].final_hash;
            if (i < traces.size() - 1) {
                details << ", ";
            }
        }

        v.details = details.str();
        violations.push_back(v);
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
