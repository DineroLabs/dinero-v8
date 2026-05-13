#pragma once

#include "consensus_safety_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DC4Oracle - Total Ordering Property
 *
 * Property: All honest nodes have a consistent block sequence at each height
 *
 * Total Ordering: For any two heights H1 < H2, if nodes agree on blocks
 * at both heights, they must be on the same chain (no permutations)
 *
 * Violation Detection:
 * - If nodes agree at height H but have different parent hashes
 * - Indicates inconsistent chain histories
 *
 * Example Non-Violation:
 * - All nodes: genesis → block_1 → block_2 → block_3
 * - Result: Total ordering maintained
 *
 * Example Violation:
 * - Alice: genesis → block_a → block_c
 * - Bob:   genesis → block_b → block_c
 * - Both agree on block_c at height 2, but different at height 1
 * - Result: DC4 violation (ordering inconsistency)
 *
 * Note: This is closely related to DC1 (Agreement).
 * DC1 checks block agreement at each height.
 * DC4 checks that the entire chain history is consistent.
 */
class DC4Oracle : public ConsensusSafetyOracle {
public:
    std::string getName() const override {
        return "DC4: Total Ordering";
    }

protected:
    std::vector<Violation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
