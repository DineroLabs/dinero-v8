#pragma once

#include "composition_state_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S20Oracle - Composition Determinism
 *
 * Property: Composed execution is deterministic
 *
 * Violation conditions:
 * - Non-deterministic composition
 * - Order-dependent outcomes
 * - Random composition behavior
 *
 * Observable facts:
 * - Trace hash computed
 * - Same inputs → same output
 * - Composition is repeatable
 *
 * Pattern: Check that composition is deterministic
 */
class S20Oracle : public CompositionStateOracle {
public:
    std::string getName() const override {
        return "S20: Composition Determinism";
    }

protected:
    std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify composition determinism
     *
     * @return nullopt if deterministic, error description if violation
     */
    std::optional<std::string> verifyCompositionDeterminism(const ExecutionTrace& trace) const;

    /**
     * Verify trace hash computed
     *
     * @return nullopt if computed, error description if violation
     */
    std::optional<std::string> verifyTraceHash(const ExecutionTrace& trace) const;

    /**
     * Verify no randomness
     *
     * @return nullopt if no randomness, error description if violation
     */
    std::optional<std::string> verifyNoRandomness(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
