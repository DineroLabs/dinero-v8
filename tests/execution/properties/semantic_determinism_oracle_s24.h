#pragma once

#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S24Oracle - Canonical Equivalence
 *
 * Property: Syntactically different but semantically equivalent scripts
 *           produce identical results
 *
 * Transformations tested:
 * 1. Arithmetic equivalence: OP_1 OP_1 OP_ADD ≡ OP_2
 * 2. Boolean equivalence: OP_1 OP_IF X OP_ENDIF ≡ X
 * 3. Stack manipulation: OP_DUP OP_DROP ≡ NOP (identity)
 * 4. Dead code elimination: OP_1 <unreachable> ≡ OP_1
 *
 * Violation conditions:
 * - Equivalent scripts produce different results
 * - Syntactic variation affects semantics
 * - Non-canonical forms not equivalent
 *
 * Observable facts:
 * - final_hash equality for equivalent scripts
 * - final_state equality for equivalent scripts
 * - Resource consumption may differ (operation_count)
 *
 * Pattern: Compare traces from syntactic variations
 */
class S24Oracle : public SemanticDeterminismOracle {
public:
    std::string getName() const override {
        return "S24: Canonical Equivalence";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) override;

private:
    /**
     * Verify canonical equivalence
     *
     * @param traces Traces from equivalent but syntactically different scripts
     * @return nullopt if equivalent, error description if violation
     */
    std::optional<std::string> verifyCanonicalEquivalence(
        const std::vector<ExecutionTrace>& traces
    ) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
