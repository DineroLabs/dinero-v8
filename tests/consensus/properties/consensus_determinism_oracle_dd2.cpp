#include "consensus_determinism_oracle_dd2.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<DeterminismViolation> DD2Oracle::observeTraces(
    const std::vector<ConsensusTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    if (traces.empty() || traces.size() < 2) {
        return violations;
    }

    // Get message delivery events from all traces
    auto reference_messages = getMessageDeliveryEvents(traces[0]);

    for (size_t i = 1; i < traces.size(); ++i) {
        auto current_messages = getMessageDeliveryEvents(traces[i]);

        if (!messageSequencesEqual(reference_messages, current_messages)) {
            // Violation: Message delivery sequences differ
            std::ostringstream desc;
            desc << "Message delivery non-determinism: run 0 had "
                 << reference_messages.size() << " messages, run " << i
                 << " had " << current_messages.size() << " messages.";

            DeterminismViolation v(
                getName(),
                desc.str(),
                traces[0].rng_seed
            );

            std::ostringstream details;
            details << "Message delivery order differs between runs "
                   << "(ref_count=" << reference_messages.size()
                   << ", current_count=" << current_messages.size() << ")";
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

std::vector<ConsensusEvent> DD2Oracle::getMessageDeliveryEvents(
    const ConsensusTrace& trace
) const {
    std::vector<ConsensusEvent> message_events;

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::MESSAGE_DELIVERED ||
            event.type == ConsensusEventType::MESSAGE_SENT) {
            message_events.push_back(event);
        }
    }

    return message_events;
}

bool DD2Oracle::messageSequencesEqual(
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
