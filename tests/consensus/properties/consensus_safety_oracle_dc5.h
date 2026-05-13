#pragma once

#include "consensus_safety_oracle.h"
#include <map>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DC5Oracle - Finality Property
 *
 * Property: Blocks beyond a depth threshold never revert (reorganize)
 *
 * Finality: Once a block is buried under K confirmations, it should
 * never be removed from the chain (no deep reorgs)
 *
 * Violation Detection:
 * - Track CHAIN_TIP_CHANGED events (reorgs)
 * - Check if any reorg affects blocks beyond finality depth
 * - Look for blocks that were accepted then later disappeared
 *
 * Example Non-Violation:
 * - Block at height 10 confirmed
 * - Block at height 11 orphaned (shallow reorg)
 * - Block at height 10 still in chain
 * - Result: No finality violation (reorg was shallow)
 *
 * Example Violation:
 * - Block_A at height 10, buried under 20 blocks
 * - Massive reorg: block_A replaced with block_B
 * - Result: DC5 violation (finalized block reverted)
 *
 * Finality Depth:
 * - Bitcoin: ~6 blocks (common practice)
 * - Ethereum: ~32 blocks (before PoS finality)
 * - Configurable per network
 */
class DC5Oracle : public ConsensusSafetyOracle {
public:
    /**
     * Create DC5 oracle
     *
     * @param finality_depth Blocks beyond this depth should never reorg
     */
    explicit DC5Oracle(uint32_t finality_depth = 6)
        : finality_depth_(finality_depth)
    {}

    std::string getName() const override {
        return "DC5: Finality";
    }

protected:
    std::vector<Violation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint32_t finality_depth_;

    /**
     * Detect deep reorgs from CHAIN_TIP_CHANGED events
     */
    bool detectDeepReorg(
        const std::vector<ConsensusEvent>& tip_changed_events,
        uint32_t finality_depth
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
