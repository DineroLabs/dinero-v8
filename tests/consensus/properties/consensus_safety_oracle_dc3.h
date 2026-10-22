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
 * Violation Detection:
 * - Compare the outpoints spent by distinct transactions in honest nodes'
 *   explicitly supplied finalized selected histories.
 * - Report insufficient evidence if the trace lacks those observations.
 *
 * Example Violation:
 * - Alice accepts tx_1 spending utxo_x in block_a
 * - Bob accepts tx_2 (different!) also spending utxo_x in block_b
 * - Both at same height or in finalized chain
 * - Result: DC3 violation (double-spend)
 *
 * The Phase-5b simulator does not supply finalized spend observations, so a
 * simulator-only trace cannot establish DC3; its result is an evidence gap.
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
