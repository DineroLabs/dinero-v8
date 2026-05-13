#pragma once

#include "consensus_byzantine_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DB2Oracle - Eclipse Resistance
 *
 * Property: Honest nodes converge to same chain despite Byzantine nodes
 *
 * Observable Eclipse Resistance Definition:
 * - Byzantine nodes are present (is_byzantine=true)
 * - Honest nodes exist (is_byzantine=false)
 * - All honest nodes converge to same chain tip
 * - No assumptions about Byzantine attack strategy
 *
 * Violation Detection:
 * - Byzantine nodes exist in trace
 * - Honest nodes exist in trace
 * - Check if all honest nodes have same chain tip at end
 * - If honest nodes disagree → DB2 violation (eclipse attack succeeded)
 *
 * Why DB2 Matters:
 * - Eclipse attacks isolate victims with false information
 * - Fundamental network topology resilience guarantee
 * - Observable convergence check: Did honest nodes agree?
 * - No inference about Byzantine intent or peer topology
 *
 * Example Scenario (No Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve attempts to eclipse alice with false chain
 * - Alice and bob both converge to same chain tip ✓
 * - Eclipse attack failed
 *
 * Example Scenario (Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve successfully feeds alice different chain
 * - Alice has chain tip X, bob has chain tip Y ✗
 * - Eclipse attack succeeded (honest nodes diverged)
 *
 * Phase 5e Scope:
 * - Observable only: Did honest nodes converge despite Byzantine presence?
 * - No peer topology required (check outcome, not mechanism)
 * - Check final state convergence
 */
class DB2Oracle : public ByzantineToleranceOracle {
public:
    /**
     * Create DB2 oracle
     */
    DB2Oracle() = default;

    std::string getName() const override {
        return "DB2: Eclipse Resistance";
    }

protected:
    std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
