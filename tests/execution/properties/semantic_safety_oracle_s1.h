#pragma once

#include "semantic_safety_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S1Oracle - Script Determinism
 *
 * Property: Same inputs → same result
 *
 * Violation conditions:
 * - Trace is not well-formed
 * - Final hash mismatch (hash doesn't match recomputed hash)
 * - Non-deterministic behavior detected
 *
 * Observable facts:
 * - Trace contains rng_seed, script, witness
 * - Operations are recorded in order
 * - Final hash is computed from all execution steps
 * - Same seed + script + witness → same trace
 *
 * Pattern: Check single trace for deterministic properties
 *
 * Note: Full determinism testing (comparing two traces with same inputs)
 * is done in the property test, not in the oracle.
 */
class S1Oracle : public SemanticSafetyOracle {
public:
    std::string getName() const override {
        return "S1: Script Determinism";
    }

protected:
    std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify trace is well-formed
     *
     * @return nullopt if valid, error description if invalid
     */
    std::optional<std::string> verifyWellFormed(const ExecutionTrace& trace) const;

    /**
     * Verify final hash is correct
     *
     * @return nullopt if valid, error description if invalid
     */
    std::optional<std::string> verifyHash(const ExecutionTrace& trace) const;

    /**
     * Check for non-deterministic behavior markers
     *
     * @return nullopt if deterministic, error description if non-deterministic
     */
    std::optional<std::string> checkDeterminism(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
