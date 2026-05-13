#include "storage/chain_height_provider.h"
#include "storage/chain_db.h"  // RocksDB headers isolated to this .cpp file only
#include "din_json.h"
#include "consensus/target_helpers.h"
#include "consensus/asert_params.h"  // For DIFFICULTY_1_BITS constant
#include <iostream>
#include <iomanip>
#include <sstream>

namespace dinero {

/**
 * @brief ChainDB-backed implementation of ChainHeightProvider
 *
 * This class wraps ChainDB access and isolates RocksDB headers from the rest of the codebase.
 * Only this translation unit sees RocksDB types.
 *
 * Extended to support:
 * - Best block hash retrieval
 * - Network difficulty calculation
 * - Block header queries
 */
class ChainDBHeightProvider : public ChainHeightProvider {
public:
    explicit ChainDBHeightProvider(ChainDB* chain_db) : chain_db_(chain_db) {}

    uint32_t GetBestHeight() const override {
        if (!chain_db_) {
            return 0;
        }

        auto tip_result = chain_db_->getTip();
        if (!tip_result.ok()) {
            // Chain tip not available (likely empty chain or initialization issue)
            return 0;
        }

        return static_cast<uint32_t>(tip_result.value().height);
    }

    std::string GetBestHash() const override {
        if (!chain_db_) {
            return "0000000000000000000000000000000000000000000000000000000000000000";
        }

        auto tip_result = chain_db_->getTip();
        if (!tip_result.ok()) {
            return "0000000000000000000000000000000000000000000000000000000000000000";
        }

        return tip_result.value().hash;
    }

    double GetDifficulty() const override {
        if (!chain_db_) {
            return 1.0;
        }

        auto tip_result = chain_db_->getTip();
        if (!tip_result.ok()) {
            return 1.0;
        }

        // Get tip height and fetch block header to get bits
        int height = tip_result.value().height;
        if (height == 0) {
            return 1.0;  // Genesis block
        }

        // Get block hash at tip height
        auto hash_result = chain_db_->getBlockHashByHeight(height);
        if (!hash_result.ok()) {
            return 1.0;
        }

        // Get header to extract bits
        auto header_result = chain_db_->getHeader(hash_result.value());
        if (!header_result.ok()) {
            return 1.0;
        }

        uint32_t bits = header_result.value().bits;

        // Convert bits to difficulty (same formula as Bitcoin)
        // difficulty = max_target / current_target
        // where max_target corresponds to "difficulty 1.0" (Bitcoin genesis)

        arith_uint256 max_target;
        max_target.SetCompact(ASERTConsensus::DIFFICULTY_1_BITS);  // difficulty 1.0 reference

        arith_uint256 current_target;
        current_target.SetCompact(bits);

        if (current_target.IsZero()) {
            return 1.0;
        }

        // Calculate difficulty
        arith_uint256 difficulty_int = max_target / current_target;

        // Convert arith_uint256 to double
        // For small values, use the lowest word
        // For larger values, this is an approximation
        double difficulty = static_cast<double>(difficulty_int.GetWord(0));
        if (difficulty_int.GetWord(1) > 0 || difficulty_int.GetWord(2) > 0 || difficulty_int.GetWord(3) > 0) {
            // For large values, scale appropriately
            difficulty += static_cast<double>(difficulty_int.GetWord(1)) * 18446744073709551616.0; // 2^64
        }

        return difficulty > 0.0 ? difficulty : 1.0;
    }

    ::Json::Value GetBlockHeader(const std::string& hash) const override {
        ::Json::Value result;

        if (!chain_db_) {
            return result;  // Return empty/null object
        }

        // Get header from ChainDB
        auto header_result = chain_db_->getHeader(hash);
        if (!header_result.ok()) {
            return result;  // Block not found
        }

        auto header = header_result.value();

        // Get height
        auto height_result = chain_db_->getBlockHeight(hash);
        int height = height_result.ok() ? height_result.value() : -1;

        // Build header JSON using correct BlockHeader field names
        result["hash"] = hash;
        result["version"] = static_cast<int>(header.version);
        result["previousblockhash"] = header.prev_block_hash;  // Use prevBlockHash field
        result["merkleroot"] = header.merkle_root;            // Use merkleRoot field
        result["time"] = static_cast<::Json::UInt>(header.timestamp);  // Use global Json namespace
        result["bits"] = header.difficulty;
        result["nonce"] = static_cast<::Json::UInt>(header.nonce);     // Use global Json namespace
        result["height"] = height;
        result["difficulty"] = GetDifficulty();  // Current network difficulty

        return result;
    }

    bool IsAvailable() const override {
        return chain_db_ != nullptr;
    }

private:
    ChainDB* chain_db_;
};

// Global singleton (set once at daemon startup)
static ChainHeightProvider* g_chain_height_provider = nullptr;

ChainHeightProvider* GetGlobalChainHeightProvider() {
    return g_chain_height_provider;
}

void SetGlobalChainHeightProvider(ChainHeightProvider* provider) {
    if (g_chain_height_provider != nullptr) {
        std::cerr << "⚠️  WARNING: ChainHeightProvider already set - ignoring duplicate call" << std::endl;
        return;
    }
    g_chain_height_provider = provider;
    std::cout << "✅ Global ChainHeightProvider initialized" << std::endl;
}

} // namespace dinero

// Factory function for creating ChainDB-backed provider (global namespace for C linkage)
dinero::ChainHeightProvider* CreateChainDBHeightProvider(dinero::ChainDB* chain_db) {
    return new dinero::ChainDBHeightProvider(chain_db);
}
