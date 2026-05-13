#pragma once

#include "semantic_safety_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S4Oracle - Key-Path ≠ Script-Path Semantics
 *
 * Property: Distinct execution paths have distinct meanings
 *
 * Violation conditions:
 * - Key path and script path produce same trace
 * - Execution path not clearly distinguished
 * - Path selection not recorded
 *
 * Observable facts:
 * - Trace indicates which path was taken (key vs script)
 * - Different paths have different operation sequences
 * - Path choice is deterministic given inputs
 *
 * Pattern: Check single trace for path distinction
 *
 * Phase 7b: Basic invariants (full implementation in Phase 7c)
 */
class S4Oracle : public SemanticSafetyOracle {
public:
    std::string getName() const override {
        return "S4: Key-Path ≠ Script-Path Semantics";
    }

protected:
    std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify path selection is clear
     *
     * @return nullopt if clear, error description if ambiguous
     */
    std::optional<std::string> verifyPathDistinction(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
