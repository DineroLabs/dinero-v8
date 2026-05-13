#pragma once

#include "storage/chain_db.h"
#include "storage/tip_info.h"
#include "consensus/subsidy.h"  // Canonical monetary policy
#include "consensus/target_helpers.h"
#include "wallet/utxo_index.h"
#include <algorithm>
#include <vector>
#include <ctime>

/**
 * Direct ChainDB access helpers (SimpleBlockchain replacement)
 *
 * This header provides convenience functions that were previously provided
 * by SimpleBlockchain, but now operate directly on RocksDB ChainDB.
 *
 * Usage:
 *   uint32_t height = GetChainHeight(dinero::dinero::legacy::g_chain_db_direct());
 *   std::string hash = GetBestBlockHash(dinero::dinero::legacy::g_chain_db_direct());
 */

namespace dinero {

// Global extern declarations (defined in src/daemon/main.cpp)
extern ChainDB* g_chain_db_direct;
extern UTXOIndex* g_utxo_set_direct;

namespace storage {

/**
 * Get current chain height
 */
inline uint32_t GetChainHeight(ChainDB* db) {
    if (!db) return 0;
    auto tip_result = db->getTip();
    if (tip_result.status() != Status::Ok) return 0;
    return tip_result.value().height;
}

/**
 * Get best block hash
 */
inline std::string GetBestBlockHash(ChainDB* db) {
    if (!db) return "";
    auto tip_result = db->getTip();
    if (tip_result.status() != Status::Ok) return "";
    return tip_result.value().hash.GetHex();  // Phase M.0: Convert uint256 to hex
}

/**
 * Get total coins mined (total_issued)
 * Note: This needs to be tracked separately - ChainDB doesn't store total_issued
 * For now, calculate from block rewards up to current height
 */
inline uint64_t GetTotalCoinsMined(ChainDB* db) {
    if (!db) return 0;
    uint32_t height = GetChainHeight(db);
    // Use proper total issued calculation (all PoW rewards from genesis)
    return dinero::ConsensusSubsidy::GetTotalIssuedAtHeight(height);
}

/**
 * Get block hash by height
 */
inline std::string GetBlockHash(ChainDB* db, uint32_t height) {
    if (!db) return "";
    auto hash_result = db->getBlockHashByHeight(height);
    if (hash_result.status() != Status::Ok) return "";
    return hash_result.value().GetHex();  // Phase M.0: Convert uint256 to hex
}

/**
 * Check if block exists
 */
inline bool HaveBlock(ChainDB* db, const std::string& hash) {
    if (!db) return false;
    uint256 hash_uint256 = uint256::FromHexUnsafe(hash);  // Phase M.0: Convert hex to uint256
    auto status = db->hasBlock(hash_uint256);
    return status == Status::Ok;
}

/**
 * Get median time past (BIP113)
 * Calculates median of last 11 block timestamps
 */
inline uint32_t GetMedianTimePast(ChainDB* db) {
    if (!db) return static_cast<uint32_t>(std::time(nullptr));

    auto tip_result = db->getTip();
    if (tip_result.status() != Status::Ok) {
        return static_cast<uint32_t>(std::time(nullptr));
    }
    uint32_t current_height = tip_result.value().height;

    // Collect timestamps from last 11 blocks
    std::vector<uint32_t> timestamps;
    timestamps.reserve(11);

    for (int32_t h = current_height; h >= 0 && timestamps.size() < 11; --h) {
        auto hash_result = db->getBlockHashByHeight(h);
        if (hash_result.status() != Status::Ok) break;

        // WORKAROUND: For block 1, use getTip() if getHeader() fails
        auto header_result = db->getHeader(hash_result.value());
        if (header_result.status() != Status::Ok) {
            // If this is block 1 (height==1) and we have the tip, use tip timestamp
            if (h == 1 && current_height == 1) {
                timestamps.push_back(tip_result.value().timestamp);
                continue;
            }
            break;
        }

        // Phase 3: BlockHeader has timestamp (uint64_t)
        timestamps.push_back(static_cast<uint32_t>(header_result.value().timestamp));
    }

    if (timestamps.empty()) {
        return static_cast<uint32_t>(std::time(nullptr));
    }

    // Return median
    std::sort(timestamps.begin(), timestamps.end());
    return timestamps[timestamps.size() / 2];
}

/**
 * Get mining phase description
 */
inline std::string GetMiningPhase(ChainDB* db) {
    if (!db) return "Unknown";
    uint32_t height = GetChainHeight(db);

    // Phase 1: Heights 1-180,000 (CPU-friendly)
    if (height >= 1 && height <= 180'000) {
        return "Phase 1 (CPU-Friendly)";
    } else {
        return "Phase 2 (Post-Halving)";
    }
}

/**
 * Get block reward for given height
 */
inline uint64_t GetBlockReward(ChainDB* db, uint32_t height) {
    if (!db) return 0;
    // New simplified API: subsidy only depends on height (halving schedule)
    // Phase M.6.2: Extract raw value from AmountUna
    return dinero::ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
}

/**
 * Get difficulty bits for given height
 *
 * Unified difficulty from block 0: 0x1d31ffce (50× easier than Bitcoin genesis)
 * - Block 0: Genesis (unified 0x1d31ffce)
 * - Blocks 1-200,002: Bootstrap phase DAA (starts at 0x1d31ffce, adjusts)
 * - Blocks 200,003+: ASERT phase (anchored at 200,002)
 */
inline uint32_t GetDifficultyBits(ChainDB* db, uint32_t height) {
    // Unified difficulty from genesis - actual adjustment via DAA
    return 0x1d31ffce; // Dinero unified difficulty (50× easier than Bitcoin)
}

/**
 * Get calculated difficulty value for given height
 * Returns human-readable difficulty (not compact bits)
 */
inline double GetDifficulty(ChainDB* db, uint32_t height) {
    uint32_t current_bits = GetDifficultyBits(db, height);
    uint32_t pow_limit_bits = 0x1f00ffff; // Chain's easiest difficulty

    // Use DifficultyFromBits helper from target_helpers.h
    // Note: target_helpers.h must be included in files using this function
    return ::dinero::DifficultyFromBits(current_bits, pow_limit_bits);
}

} // namespace storage
} // namespace dinero
