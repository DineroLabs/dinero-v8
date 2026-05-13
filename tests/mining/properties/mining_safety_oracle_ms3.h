#pragma once

#include "mining_safety_oracle.h"
#include <map>
#include <set>

// Ring 4 Phase 4d: MS3 - No Invalid Transaction Inclusion
// Property: Templates must not include invalid transactions

namespace mining_test {

/**
 * MS3Oracle - No Invalid Transaction Inclusion
 *
 * Safety property: ∀ templates T, all transactions in T are valid
 *
 * Rationale:
 * - Templates should only include valid transactions
 * - No double-spends allowed
 * - Transaction count should not exceed block limits
 * - Transactions should be well-formed
 *
 * Detection strategy (Phase 4d - PLACEHOLDER):
 * - Check transaction count doesn't exceed max_block_txs
 * - Verify mempool consistency (no duplicate tx inclusions)
 * - Check template has at least coinbase transaction
 * - Full UTXO validation deferred to Phase 4h
 *
 * Phase 4d scope:
 * - Basic consistency checks only
 * - Count validation and mempool tracking
 * - Conservative detection (placeholder validation)
 * - Phase 4h will add full UTXO validation
 */
class MS3Oracle : public MiningSafetyOracle {
public:
    explicit MS3Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track transactions in mempool
    std::set<uint64_t> mempool_txs_;

    // Track transactions included in templates
    // Map: template_id -> set of tx hashes
    std::map<uint64_t, std::set<uint64_t>> template_txs_;

    // Track transaction count by height
    std::map<uint32_t, uint32_t> tx_count_by_height_;

    // Template ID counter
    uint64_t next_template_id_{0};
};

}  // namespace mining_test
