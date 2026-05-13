#pragma once

#include "consensus_byzantine_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DB4Oracle - Block Withholding Tolerance
 *
 * Property: Network makes progress despite Byzantine nodes withholding blocks
 *
 * Observable Block Withholding Definition:
 * - Byzantine nodes mine blocks but don't broadcast them
 * - Honest nodes continue mining and producing blocks
 * - Network chain grows despite withheld blocks
 * - No assumptions about Byzantine withholding strategy
 *
 * Violation Detection:
 * - Detect block withholding activity (BLOCK_WITHHELD events)
 * - Check if network produced blocks after withholding began
 * - If no progress → DB4 violation (withholding attack succeeded)
 *
 * Why DB4 Matters:
 * - Selfish mining and withholding attacks
 * - Ensures network liveness despite strategic block release
 * - Observable progress check: Did chain grow despite withholding?
 * - No inference about Byzantine timing strategy
 *
 * Example Scenario (No Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve mines block at T=100 but withholds it
 * - Alice mines block at T=150 and broadcasts ✓
 * - Bob accepts alice's block ✓
 * - Network continued despite eve's withholding
 *
 * Example Scenario (Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve withholds blocks starting at T=100
 * - Alice and bob stop making progress (no blocks after T=100) ✗
 * - Withholding attack caused network stall
 *
 * Phase 5e Scope:
 * - Observable only: Did network progress despite withholding?
 * - Check for block withholding events
 * - Check for network block production after withholding
 * - No inference about why withholding succeeded
 */
class DB4Oracle : public ByzantineToleranceOracle {
public:
    /**
     * Create DB4 oracle
     */
    DB4Oracle() = default;

    std::string getName() const override {
        return "DB4: Block Withholding Tolerance";
    }

protected:
    std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Get timestamp when first block withholding occurred
     */
    std::optional<uint64_t> getBlockWithholdingStartTime(
        const ConsensusTrace& trace
    ) const;

    /**
     * Check if network produced blocks after withholding began
     */
    bool didNetworkProduceBlocksAfterWithholding(
        const ConsensusTrace& trace,
        uint64_t withholding_start_time
    ) const;

    /**
     * Count number of withheld blocks
     */
    size_t countWithheldBlocks(const ConsensusTrace& trace) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
