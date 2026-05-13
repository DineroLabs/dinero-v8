#pragma once

#include "consensus_determinism_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DD3Oracle - State Convergence Determinism
 *
 * Property: Same seed produces identical final state for all nodes
 *
 * Observable State Convergence Definition:
 * - Each node has final state (chain_tip, height, chainwork)
 * - Same seed → same final state for each node
 * - All nodes converge to same state deterministically
 * - No assumptions about convergence mechanism
 *
 * Violation Detection:
 * - Run scenario N times with same seed
 * - Compare final state for each node across runs
 * - If any node's final state differs → DD3 violation
 *
 * Why DD3 Matters:
 * - State machine determinism guarantee
 * - Ensures blockchain state is reproducible
 * - Critical for consensus correctness
 * - Observable state equality
 *
 * Example Scenario (No Violation):
 * - Run 1: alice (tip=ABC, h=10), bob (tip=ABC, h=10)
 * - Run 2: alice (tip=ABC, h=10), bob (tip=ABC, h=10) ✓
 * - Final states identical
 *
 * Example Scenario (Violation):
 * - Run 1: alice (tip=ABC, h=10)
 * - Run 2: alice (tip=DEF, h=11) ✗
 * - Non-deterministic state convergence
 *
 * Phase 5f Scope:
 * - Observable only: Do final states match?
 * - Check state fields, not convergence path
 * - No inference about why states differ
 */
class DD3Oracle : public ConsensusDeterminismOracle {
public:
    /**
     * Create DD3 oracle
     */
    DD3Oracle() = default;

    std::string getName() const override {
        return "DD3: State Convergence Determinism";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
