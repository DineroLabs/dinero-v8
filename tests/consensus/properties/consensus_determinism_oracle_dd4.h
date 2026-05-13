#pragma once

#include "consensus_determinism_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DD4Oracle - Reorg Determinism
 *
 * Property: Same fork scenario produces identical reorg resolution
 *
 * Observable Reorg Determinism Definition:
 * - Chain reorgs occur when nodes switch to longer chain
 * - Reorg events recorded (CHAIN_TIP_CHANGED)
 * - Same seed → same reorg sequence
 * - No assumptions about fork resolution strategy
 *
 * Violation Detection:
 * - Run fork scenario N times with same seed
 * - Compare CHAIN_TIP_CHANGED event sequences
 * - If any sequence differs → DD4 violation
 *
 * Why DD4 Matters:
 * - Fork resolution determinism guarantee
 * - Ensures consensus algorithm is reproducible
 * - Critical for testing reorg handling
 * - Observable reorg sequence equality
 *
 * Example Scenario (No Violation):
 * - Run 1: alice switches from block_a to block_b at T=100
 * - Run 2: alice switches from block_a to block_b at T=100 ✓
 * - Reorg resolution identical
 *
 * Example Scenario (Violation):
 * - Run 1: alice switches from block_a to block_b
 * - Run 2: alice switches from block_a to block_c ✗
 * - Non-deterministic reorg resolution
 *
 * Phase 5f Scope:
 * - Observable only: Do reorg sequences match?
 * - Check CHAIN_TIP_CHANGED events
 * - No inference about fork choice rule
 */
class DD4Oracle : public ConsensusDeterminismOracle {
public:
    /**
     * Create DD4 oracle
     */
    DD4Oracle() = default;

    std::string getName() const override {
        return "DD4: Reorg Determinism";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) override;

private:
    /**
     * Get all CHAIN_TIP_CHANGED events from a trace
     */
    std::vector<ConsensusEvent> getReorgEvents(
        const ConsensusTrace& trace
    ) const;

    /**
     * Check if two reorg event sequences are identical
     */
    bool reorgSequencesEqual(
        const std::vector<ConsensusEvent>& seq1,
        const std::vector<ConsensusEvent>& seq2
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
