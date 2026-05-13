#include "consensus_determinism_oracle_dd4.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<DeterminismViolation> DD4Oracle::observeTraces(
    const std::vector<ConsensusTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    if (traces.empty() || traces.size() < 2) {
        return violations;
    }

    // Get reorg events from all traces
    auto reference_reorgs = getReorgEvents(traces[0]);

    for (size_t i = 1; i < traces.size(); ++i) {
        auto current_reorgs = getReorgEvents(traces[i]);

        if (!reorgSequencesEqual(reference_reorgs, current_reorgs)) {
            // Violation: Reorg sequences differ
            std::ostringstream desc;
            desc << "Reorg non-determinism: run 0 had "
                 << reference_reorgs.size() << " reorgs, run " << i
                 << " had " << current_reorgs.size() << " reorgs.";

            DeterminismViolation v(
                getName(),
                desc.str(),
                traces[0].rng_seed
            );

            std::ostringstream details;
            details << "Reorg sequences differ between runs "
                   << "(ref_count=" << reference_reorgs.size()
                   << ", current_count=" << current_reorgs.size() << ")";
            v.details = details.str();

            violations.push_back(v);
            break;  // Only report first mismatch
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::vector<ConsensusEvent> DD4Oracle::getReorgEvents(
    const ConsensusTrace& trace
) const {
    std::vector<ConsensusEvent> reorg_events;

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::CHAIN_TIP_CHANGED) {
            reorg_events.push_back(event);
        }
    }

    return reorg_events;
}

bool DD4Oracle::reorgSequencesEqual(
    const std::vector<ConsensusEvent>& seq1,
    const std::vector<ConsensusEvent>& seq2
) const {
    if (seq1.size() != seq2.size()) {
        return false;
    }

    for (size_t i = 0; i < seq1.size(); ++i) {
        if (!eventsEqual(seq1[i], seq2[i])) {
            return false;
        }
    }

    return true;
}

} // namespace test
} // namespace consensus
} // namespace dinero
