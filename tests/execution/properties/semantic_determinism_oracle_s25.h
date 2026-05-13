#pragma once

#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S25Oracle - Full Semantic Determinism (Closure)
 *
 * Property: Script execution is a pure deterministic function
 *
 * Meta-property that verifies:
 * - S21: Evaluation order independence ✓
 * - S22: Input permutation invariance ✓
 * - S23: Strategy independence ✓
 * - S24: Canonical equivalence ✓
 *
 * This is the closure property that proves Ring 7 semantics are frozen.
 * If S25 holds across 10,000+ iterations, execution semantics are
 * implementation-independent and mathematically well-defined.
 *
 * Violation conditions:
 * - Any S21-S24 violation
 * - Trace not finalized (missing hash)
 * - Non-deterministic RNG (missing seed)
 *
 * Observable facts:
 * - All traces have valid final_hash
 * - All traces have RNG seed
 * - All traces are reproducible
 *
 * Pattern: Meta-verification of determinism infrastructure
 */
class S25Oracle : public SemanticDeterminismOracle {
public:
    std::string getName() const override {
        return "S25: Full Semantic Determinism (Closure)";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) override;

private:
    /**
     * Verify full determinism (meta-check)
     *
     * @param traces Traces to verify have determinism properties
     * @return nullopt if fully deterministic, error description if violation
     */
    std::optional<std::string> verifyFullDeterminism(
        const std::vector<ExecutionTrace>& traces
    ) const;

    /**
     * Verify trace has determinism markers
     *
     * @param trace Trace to verify
     * @return nullopt if has markers, error description if missing
     */
    std::optional<std::string> verifyDeterminismMarkers(
        const ExecutionTrace& trace
    ) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
