#pragma once

#include "semantic_safety_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S3Oracle - Taproot Leaf Isolation
 *
 * Property: Revealing one leaf doesn't enable others
 *
 * Violation conditions:
 * - Unrevealed leaf is activated
 * - Multiple leaves activated from single reveal
 * - Leaf activation without proper reveal
 *
 * Observable facts:
 * - Trace contains taproot_path (if Taproot used)
 * - Path reveals are recorded
 * - Only revealed leaves can execute
 * - Hidden paths remain inactive
 *
 * Pattern: Check single trace for Taproot isolation
 *
 * Phase 7b: Basic invariants (full implementation in Phase 7c)
 */
class S3Oracle : public SemanticSafetyOracle {
public:
    std::string getName() const override {
        return "S3: Taproot Leaf Isolation";
    }

protected:
    std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify Taproot path structure is valid
     *
     * @return nullopt if valid, error description if invalid
     */
    std::optional<std::string> verifyTaprootStructure(const ExecutionTrace& trace) const;

    /**
     * Check that only revealed paths are activated
     *
     * @return nullopt if isolated, error description if violation
     */
    std::optional<std::string> verifyPathIsolation(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
