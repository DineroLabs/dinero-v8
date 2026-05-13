#pragma once

#include <string>
#include <cstdint>

namespace dinero {

enum class Chain { 
    MAINNET, 
    TESTNET, 
    REGTEST 
};

struct ChainParams {
    std::string name;          // "mainnet" | "testnet" | "regtest"
    std::string hrp;           // "din" | "tdin" | "rdin"
    uint32_t magic;            // P2P magic bytes
    
    // Default port configuration
    uint16_t rpc_port;         // JSON-RPC port
    uint16_t http_port;        // HTTP API port
    uint16_t ws_port;          // WebSocket port
    uint16_t p2p_port;         // P2P network port
    
    // Network identification
    std::string genesis_hash;  // Genesis block hash
    std::string network_id;    // Network identifier for logging
    
    // Mining and consensus parameters
    uint32_t pow_limit_bits;   // Proof of work difficulty limit
    uint32_t target_spacing;   // Block time target in seconds
    uint32_t retarget_interval; // Difficulty adjustment interval
    
    // Policy parameters
    uint64_t dust_threshold;   // Minimum output value (una)
    uint64_t min_relay_fee;    // Minimum relay fee rate (sat/kB)
    uint32_t max_block_size;   // Maximum block size in bytes
    
    // Address prefixes
    uint8_t pubkey_address_prefix;    // P2PKH address version
    uint8_t script_address_prefix;    // P2SH address version
    
    // Development and testing flags
    bool allow_min_difficulty;       // Allow minimum difficulty blocks
    bool require_standard_txs;       // Require standard transactions only
    bool mine_blocks_on_demand;      // Mine blocks on demand (regtest)
    
    // Genesis block parameters (for backward compatibility)
    struct {
        uint32_t nVersion;
        uint32_t nTime;
        uint32_t nNonce;
        uint32_t nBits;
        std::string coinbaseText;
        std::string genesisHashHex;
        std::string merkleRootHex;
    } genesis;
};

/**
 * Select the active chain parameters
 * Must be called before any network operations
 */
void SelectParams(Chain chain);

/**
 * Get the currently active chain parameters
 * Throws if SelectParams() has not been called
 */
const ChainParams& Params();

/**
 * Get the currently active chain
 * Throws if SelectParams() has not been called
 */
Chain GetActiveChain();

/**
 * Convert chain enum to string
 */
std::string ChainToString(Chain chain);

/**
 * Convert string to chain enum
 * Throws std::invalid_argument for unknown chains
 */
Chain StringToChain(const std::string& chain_str);

/**
 * Check if parameters have been initialized
 */
bool IsChainSelected();

} // namespace dinero
