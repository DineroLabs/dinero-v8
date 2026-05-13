#pragma once

#include "consensus_byzantine_tolerance_oracle.h"
#include <unordered_map>
#include <unordered_set>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DB3Oracle - Double-Spend Resistance
 *
 * Property: Conflicting transactions cannot both be confirmed
 *
 * Observable Double-Spend Resistance Definition:
 * - Two transactions conflict if they spend the same input
 * - A transaction is "confirmed" if included in an accepted block
 * - At most one transaction from a conflict set can be confirmed
 * - No assumptions about Byzantine double-spend strategy
 *
 * Violation Detection:
 * - Identify conflicting transaction pairs
 * - Check if both transactions were confirmed (in accepted blocks)
 * - If both confirmed → DB3 violation (double-spend succeeded)
 *
 * Why DB3 Matters:
 * - Fundamental blockchain safety property
 * - Prevents double-spending attacks
 * - Observable safety check: Were conflicting txs both confirmed?
 * - No inference about attacker intent
 *
 * Example Scenario (No Violation):
 * - Byzantine node creates tx1 and tx2 (both spend same input)
 * - tx1 confirmed in block at height 100
 * - tx2 rejected (conflicts with tx1) ✓
 * - Double-spend prevented
 *
 * Example Scenario (Violation):
 * - Byzantine node creates tx1 and tx2 (both spend same input)
 * - tx1 confirmed in block on one fork
 * - tx2 confirmed in block on another fork
 * - Network doesn't converge, both remain confirmed ✗
 * - Double-spend succeeded
 *
 * Phase 5e Scope:
 * - Observable only: Were conflicting txs both confirmed?
 * - Check transaction confirmation in accepted blocks
 * - No inference about why double-spend succeeded
 */
class DB3Oracle : public ByzantineToleranceOracle {
public:
    /**
     * Create DB3 oracle
     */
    DB3Oracle() = default;

    std::string getName() const override {
        return "DB3: Double-Spend Resistance";
    }

protected:
    std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Find all confirmed transactions (included in accepted blocks)
     */
    std::unordered_set<std::string> getConfirmedTransactions(
        const ConsensusTrace& trace
    ) const;

    /**
     * Find all conflicting transaction pairs
     * Returns map of tx_hash -> set of conflicting tx hashes
     */
    std::unordered_map<std::string, std::unordered_set<std::string>>
    findConflictingSets(const ConsensusTrace& trace) const;

    /**
     * Check if a transaction was confirmed on a specific node
     */
    bool wasTransactionConfirmed(
        const ConsensusTrace& trace,
        const std::string& tx_hash,
        const NodeID& node_id
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
