#pragma once

#include "consensus_safety_oracle.h"
#include <map>
#include <set>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DC3Oracle - Integrity Property
 *
 * Property: No double-spend survives across the network
 *
 * Double-Spend: Same UTXO spent in two different transactions
 * appearing in finalized blocks on honest nodes
 *
 * Violation Detection (simplified for Phase 5b):
 * - Check for duplicate transaction IDs in finalized blocks
 * - Check for conflicting transactions accepted by different honest nodes
 *
 * Example Violation:
 * - Alice accepts tx_1 spending utxo_x in block_a
 * - Bob accepts tx_2 (different!) also spending utxo_x in block_b
 * - Both at same height or in finalized chain
 * - Result: DC3 violation (double-spend)
 *
 * Note: Full UTXO tracking will be added in later phases
 */
class DC3Oracle : public ConsensusSafetyOracle {
public:
    std::string getName() const override {
        return "DC3: Integrity";
    }

protected:
    std::vector<Violation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
