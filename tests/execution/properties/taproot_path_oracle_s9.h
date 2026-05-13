#pragma once

#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S9Oracle - Path Commitment Completeness
 *
 * Property: All executable paths are committed
 *
 * Violation conditions:
 * - Path executed without commitment
 * - Commitment incomplete
 * - Missing Merkle proof for executed path
 *
 * Observable facts:
 * - Each executed path has Merkle proof
 * - All paths are properly committed
 * - No uncommitted paths can execute
 *
 * Pattern: Check that executed paths are properly committed
 */
class S9Oracle : public TaprootPathOracle {
public:
    std::string getName() const override {
        return "S9: Path Commitment Completeness";
    }

protected:
    std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify path commitments are complete
     *
     * @return nullopt if complete, error description if violation
     */
    std::optional<std::string> verifyCommitmentCompleteness(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
