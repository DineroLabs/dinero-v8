#include "consensus_determinism_oracle_dd3.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<DeterminismViolation> DD3Oracle::observeTraces(
    const std::vector<ConsensusTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    if (traces.empty() || traces.size() < 2) {
        return violations;
    }

    // Check if all traces have same final state
    bool same_state = tracesHaveSameFinalState(traces);

    if (!same_state) {
        // Violation: Final states differ across runs
        std::ostringstream desc;
        desc << "State convergence non-determinism: " << traces.size()
             << " runs with seed " << traces[0].rng_seed
             << " produced different final states.";

        DeterminismViolation v(
            getName(),
            desc.str(),
            traces[0].rng_seed
        );

        // Build detailed report of state differences
        std::ostringstream details;
        auto nodes = traces[0].getAllNodes();
        details << "State differences detected for nodes: ";

        bool first_diff = true;
        for (const auto& node_id : nodes) {
            auto state0 = getFinalStateForNode(traces[0], node_id);
            auto state1 = getFinalStateForNode(traces[1], node_id);

            if (state0 && state1 && !statesEqual(*state0, *state1)) {
                if (!first_diff) details << ", ";
                details << node_id << " (h0=" << state0->chain_height
                       << " vs h1=" << state1->chain_height << ")";
                first_diff = false;
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
