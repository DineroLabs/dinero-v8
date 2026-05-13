#pragma once

#include "consensus_determinism_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DD2Oracle - Message Delivery Determinism
 *
 * Property: Same seed produces identical message delivery order
 *
 * Observable Message Delivery Definition:
 * - Network simulator delivers messages between nodes
 * - Message delivery order recorded in events (MESSAGE_DELIVERED)
 * - Same seed → same delivery sequence
 * - No assumptions about routing strategy
 *
 * Violation Detection:
 * - Run scenario N times with same seed
 * - Compare MESSAGE_DELIVERED event sequences
 * - If any sequence differs → DD2 violation
 *
 * Why DD2 Matters:
 * - Network simulation determinism guarantee
 * - Ensures message ordering is reproducible
 * - Critical for distributed consensus testing
 * - Observable delivery order equality
 *
 * Example Scenario (No Violation):
 * - Run 1: msg1→alice, msg2→bob, msg3→carol
 * - Run 2: msg1→alice, msg2→bob, msg3→carol ✓
 * - Delivery order identical
 *
 * Example Scenario (Violation):
 * - Run 1: msg1→alice, msg2→bob, msg3→carol
 * - Run 2: msg1→bob, msg2→alice, msg3→carol ✗
 * - Non-deterministic delivery order
 *
 * Phase 5f Scope:
 * - Observable only: Do message delivery sequences match?
 * - Check event sequences, not routing implementation
 * - No inference about why order changed
 */
class DD2Oracle : public ConsensusDeterminismOracle {
public:
    /**
     * Create DD2 oracle
     */
    DD2Oracle() = default;

    std::string getName() const override {
        return "DD2: Message Delivery Determinism";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) override;

private:
    /**
     * Get all MESSAGE_DELIVERED events from a trace
     */
    std::vector<ConsensusEvent> getMessageDeliveryEvents(
        const ConsensusTrace& trace
    ) const;

    /**
     * Check if two message delivery event sequences are identical
     */
    bool messageSequencesEqual(
        const std::vector<ConsensusEvent>& seq1,
        const std::vector<ConsensusEvent>& seq2
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
