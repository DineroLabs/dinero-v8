#pragma once

#include <cstdint>

// Ring 4 Phase 4c: Consensus Parameters
// Purpose: Define consensus constants for property validation
// Source: Ring 1 frozen consensus specification
// Rule: Read-only, never modify consensus logic

namespace mining_test {

// ============================================================================
// ConsensusParams - Consensus constants for property checking
// ============================================================================

struct ConsensusParams {
    // Genesis subsidy (special case for block 0)
    uint64_t genesis_subsidy{0};  // No premine in Dinero

    // Initial block subsidy (Ring 1: 100 DIN)
    // Ring 4 Phase 4c: Matches current Dinero PoW subsidy numerically
    uint64_t initial_subsidy{100ULL * 100000000ULL};  // 100 DIN in una

    // Halving interval (blocks between subsidy halvings)
    uint32_t halving_interval{210000};  // Same as Bitcoin

    // Coinbase maturity (blocks until coinbase spendable)
    uint32_t coinbase_maturity{100};

    // Maximum block weight (for size validation)
    uint32_t max_block_weight{4000000};  // 4M weight units

    // Maximum transaction count per block
    uint32_t max_block_txs{10000};

    // Default constructor (uses mainnet params)
    ConsensusParams() = default;

    // Named constructors for different networks
    static ConsensusParams mainnet();
    static ConsensusParams testnet();
    static ConsensusParams regtest();

    // Equality for testing
    bool operator==(const ConsensusParams& other) const {
        return genesis_subsidy == other.genesis_subsidy &&
               initial_subsidy == other.initial_subsidy &&
               halving_interval == other.halving_interval &&
               coinbase_maturity == other.coinbase_maturity &&
               max_block_weight == other.max_block_weight &&
               max_block_txs == other.max_block_txs;
    }
};

// ============================================================================
// Network-specific parameters
// ============================================================================

inline ConsensusParams ConsensusParams::mainnet() {
    ConsensusParams params;
    params.genesis_subsidy = 0;  // No premine
    params.initial_subsidy = 100ULL * 100000000ULL;  // 100 DIN
    params.halving_interval = 210000;
    params.coinbase_maturity = 100;
    params.max_block_weight = 4000000;
    params.max_block_txs = 10000;
    return params;
}

inline ConsensusParams ConsensusParams::testnet() {
    ConsensusParams params;
    params.genesis_subsidy = 0;
    params.initial_subsidy = 100ULL * 100000000ULL;  // Same subsidy as mainnet
    params.halving_interval = 210000;
    params.coinbase_maturity = 100;
    params.max_block_weight = 4000000;
    params.max_block_txs = 10000;
    return params;
}

inline ConsensusParams ConsensusParams::regtest() {
    ConsensusParams params;
    params.genesis_subsidy = 0;
    params.initial_subsidy = 100ULL * 100000000ULL;
    params.halving_interval = 150;  // Faster halving for testing
    params.coinbase_maturity = 10;  // Faster maturity for testing
    params.max_block_weight = 4000000;
    params.max_block_txs = 10000;
    return params;
}

}  // namespace mining_test
