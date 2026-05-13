#pragma once

#include "consensus_determinism_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DD5Oracle - Byzantine Determinism
 *
 * Property: Same seed produces identical Byzantine behavior
 *
 * Observable Byzantine Determinism Definition:
 * - Byzantine nodes perform malicious actions
 * - Actions are seeded and deterministic
 * - Same seed → same Byzantine action sequence
 * - No assumptions about attack strategy
 *
 * Violation Detection:
 * - Run Byzantine scenario N times with same seed
 * - Compare Byzantine action sequences (WITHHOLD_BLOCK, DOUBLE_SPEND_ATTEMPT, etc.)
 * - If any sequence differs → DD5 violation
 *
 * Why DD5 Matters:
 * - Byzantine attack reproducibility guarantee
 * - Enables testing attack scenarios reliably
 * - Critical for Byzantine tolerance validation
 * - Observable attack sequence equality
 *
 * Example Scenario (No Violation):
 * - Run 1: eve withholds block_1 at T=100, broadcasts block_2 at T=200
 * - Run 2: eve withholds block_1 at T=100, broadcasts block_2 at T=200 ✓
 * - Byzantine behavior identical
 *
 * Example Scenario (Violation):
 * - Run 1: eve withholds block_1 at T=100
 * - Run 2: eve broadcasts block_1 at T=100 ✗
 * - Non-deterministic Byzantine behavior
 *
 * Phase 5f Scope:
 * - Observable only: Do Byzantine action sequences match?
 * - Check Byzantine actions and events
 * - No inference about attack strategy
 */
class DD5Oracle : public ConsensusDeterminismOracle {
public:
    /**
     * Create DD5 oracle
     */
    DD5Oracle() = default;

    std::string getName() const override {
        return "DD5: Byzantine Determinism";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) override;

private:
    /**
     * Get all Byzantine actions from a trace
     */
    std::vector<ConsensusAction> getByzantineActions(
        const ConsensusTrace& trace
    ) const;

    /**
     * Check if two Byzantine action sequences are identical
     */
    bool byzantineSequencesEqual(
        const std::vector<ConsensusAction>& seq1,
        const std::vector<ConsensusAction>& seq2
    ) const;

    /**
     * Compare two actions for equality
     */
    bool actionsEqual(
        const ConsensusAction& a1,
        const ConsensusAction& a2
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
