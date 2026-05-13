#pragma once

#include "covenant_semantic_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S12Oracle - Covenant Introspection Correctness
 *
 * Property: Introspection opcodes return correct values
 *
 * Violation conditions:
 * - Introspection returns incorrect transaction data
 * - Introspection reads wrong inputs/outputs
 * - Introspection ignores actual transaction structure
 *
 * Observable facts:
 * - Introspection operations complete successfully
 * - Stack contains expected values after introspection
 * - Execution deterministic (same TX → same introspection)
 *
 * Pattern: Check that introspection opcodes are accurate
 */
class S12Oracle : public CovenantSemanticOracle {
public:
    std::string getName() const override {
        return "S12: Covenant Introspection Correctness";
    }

protected:
    std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify introspection operations succeed
     *
     * @return nullopt if correct, error description if violation
     */
    std::optional<std::string> verifyIntrospectionSuccess(const ExecutionTrace& trace) const;

    /**
     * Verify stack state after introspection
     *
     * @return nullopt if correct, error description if violation
     */
    std::optional<std::string> verifyStackState(const ExecutionTrace& trace) const;

    /**
     * Verify introspection determinism
     *
     * @return nullopt if deterministic, error description if violation
     */
    std::optional<std::string> verifyDeterminism(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
