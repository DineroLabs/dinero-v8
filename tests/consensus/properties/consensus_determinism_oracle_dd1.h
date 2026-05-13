#pragma once

#include "consensus_determinism_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DD1Oracle - Trace Reproducibility
 *
 * Property: Same RNG seed produces identical trace hash
 *
 * Observable Trace Reproducibility Definition:
 * - Run same scenario multiple times with same seed
 * - Each run produces a trace with final_hash
 * - All final_hash values must be identical
 * - No assumptions about execution path
 *
 * Violation Detection:
 * - Run scenario N times (typically 10-1000)
 * - Compare all final_hash values
 * - If any hash differs → DD1 violation (non-determinism)
 *
 * Why DD1 Matters:
 * - Fundamental determinism guarantee
 * - Enables reproducible testing and debugging
 * - Required for all other determinism properties
 * - Observable guarantee: Same input → same output
 *
 * Example Scenario (No Violation):
 * - Scenario: 3 nodes, 10 blocks, seed=42
 * - Run 1: final_hash = 0xABCD1234
 * - Run 2: final_hash = 0xABCD1234 ✓
 * - Run 3: final_hash = 0xABCD1234 ✓
 * - Perfect reproducibility
 *
 * Example Scenario (Violation):
 * - Scenario: 3 nodes, 10 blocks, seed=42
 * - Run 1: final_hash = 0xABCD1234
 * - Run 2: final_hash = 0xDEADBEEF ✗
 * - Non-determinism detected
 *
 * Phase 5f Scope:
 * - Observable only: Do hashes match?
 * - No inference about why non-determinism occurred
 * - Check outcome, not mechanism
 */
class DD1Oracle : public ConsensusDeterminismOracle {
public:
    /**
     * Create DD1 oracle
     */
    DD1Oracle() = default;

    std::string getName() const override {
        return "DD1: Trace Reproducibility";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
