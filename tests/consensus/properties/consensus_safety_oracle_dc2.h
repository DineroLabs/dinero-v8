#pragma once

#include "consensus_safety_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DC2Oracle - Validity Property
 *
 * Property: Only valid blocks are accepted by honest nodes
 *
 * Validation Rules (simplified for Phase 5b):
 * - No BLOCK_REJECTED events for honest nodes
 * - All BLOCK_ACCEPTED events have success=true
 * - Blocks build on valid parent (chain continuity)
 *
 * Full validation (Phase 5c+):
 * - Valid transactions only
 * - Correct subsidy
 * - Valid proof-of-work
 * - Correct merkle root
 *
 * Violation Detection:
 * - Check for BLOCK_REJECTED events on honest nodes (shouldn't happen if all valid)
 * - Check for success=false on BLOCK_ACCEPTED events
 *
 * Example Violation:
 * - Alice (honest) accepts a block with an invalid transaction
 * - Result: DC2 violation (invalid block accepted)
 */
class DC2Oracle : public ConsensusSafetyOracle {
public:
    std::string getName() const override {
        return "DC2: Validity";
    }

protected:
    std::vector<Violation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
