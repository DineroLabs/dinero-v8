#pragma once

#include "types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace dinero {
namespace solo {

/**
 * Parsed block template from getblocktemplate RPC
 */
struct WorkTemplate {
    // Header fields
    uint32_t version = 1;
    std::string prev_hash;          // Hex string (64 chars)
    std::string merkle_root;        // Hex string (64 chars)
    std::string utreexo_root;       // Hex string (64 chars)
    uint64_t timestamp = 0;
    uint32_t difficulty_bits = 0;   // Compact target (nBits)
    uint32_t height = 0;

    // Target for mining comparison
    std::string target_hex;         // 256-bit target as hex
    Hash256 target;                 // Parsed target bytes

    // Coinbase info
    uint64_t coinbase_value = 0;

    // Serialized coinbase transaction (hex)
    std::string coinbase_txn_hex;

    // Non-coinbase transactions (pre-serialized)
    std::vector<std::string> transactions;

    // BIP22/BIP23 tip token. When echoed back in a subsequent
    // getblocktemplate call, the server holds the response open until
    // the tip advances. Empty string if the server response didn't
    // include it (older daemons pre-8bad44f15).
    std::string longpollid;

    /**
     * Parse work template from getblocktemplate JSON response
     */
    static std::optional<WorkTemplate> fromJson(const nlohmann::json& json);

    /**
     * Build 128-byte block header for mining
     * @param nonce Nonce value to use
     * @return Serialized header bytes
     */
    std::array<uint8_t, HEADER_SIZE> buildHeader(uint32_t nonce) const;

    /**
     * Build full block for submission
     * @param nonce Winning nonce
     * @return Serialized block as hex string
     */
    std::string buildBlock(uint32_t nonce) const;

    /**
     * Check if template is valid/usable
     */
    bool isValid() const;
};

} // namespace solo
} // namespace dinero
