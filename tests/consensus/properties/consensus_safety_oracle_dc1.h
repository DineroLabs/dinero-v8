#pragma once

#include "consensus_safety_oracle.h"
#include <map>
#include <set>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DC1Oracle - Agreement Property
 *
 * Property: Honest nodes agree on the same block at each finalized height
 *
 * Finalization: A height is "finalized" when:
 * - All honest nodes have reached at least that height
 * - Sufficient confirmations (depth threshold) have passed
 *
 * Violation Detection:
 * - For each height H where all honest nodes have blocks
 * - Check if any two honest nodes have different block hashes at height H
 * - Ignore heights that are not yet finalized (near tip)
 *
 * Why DC1 Matters:
 * - Fundamental consensus property
 * - Ensures network convergence
 * - Prevents permanent chain splits
 * - Foundation for double-spend prevention
 *
 * Example Violation:
 * - Alice: height 10 → block_a
 * - Bob:   height 10 → block_b (different!)
 * - Carol: height 10 → block_a
 * - Result: Bob disagrees with Alice/Carol → DC1 violation
 *
 * Example Non-Violation:
 * - Alice: height 10 → block_a (tip at height 10)
 * - Bob:   height 9  → block_x (still catching up)
 * - Result: No violation (Bob hasn't reached height 10 yet)
 */
class DC1Oracle : public ConsensusSafetyOracle {
public:
    /**
     * Create DC1 oracle
     *
     * @param finalization_depth Number of confirmations required for finalization
     *                           (default: 6, similar to Bitcoin's common practice)
     */
    explicit DC1Oracle(uint32_t finalization_depth = 6)
        : finalization_depth_(finalization_depth)
    {}

    std::string getName() const override {
        return "DC1: Agreement";
    }

protected:
    std::vector<Violation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint32_t finalization_depth_;

    /**
     * Build map of node_id → (height → block_hash) from final states
     */
    std::map<NodeID, std::map<uint32_t, std::string>> buildChainMaps(
        const ConsensusTrace& trace,
        const std::vector<NodeID>& honest_nodes
    ) const;

    /**
     * Get minimum height across all honest nodes
     */
    uint32_t getMinHeight(const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps) const;

    /**
     * Get finalization cutoff (max_height - finalization_depth)
     */
    uint32_t getFinalizationCutoff(const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps) const;

    /**
     * Check if all honest nodes agree at a specific height
     */
    bool checkAgreementAtHeight(
        uint32_t height,
        const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps,
        std::vector<NodeID>& disagreeing_nodes
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
