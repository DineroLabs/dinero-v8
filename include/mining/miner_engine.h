#pragma once

#include "mining/block_template.h"
#include "primitives/block.h"
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

namespace dinero {

// Forward declaration for BIP68 MTP lookup
class ChainDB;

namespace mining {

// ============================================================================
// Phase 26.2: Mining Engine (PoW Loop)
// ============================================================================

/**
 * Mining statistics
 */
struct MiningStats {
    uint64_t hashes_computed;      // Total hashes computed
    uint64_t blocks_found;         // Total blocks mined
    uint64_t last_block_height;    // Height of last mined block
    std::string last_block_hash;   // Hash of last mined block
    uint64_t hash_rate;            // Current hash rate (hashes/sec)
    uint64_t mining_time_ms;       // Total mining time in milliseconds
    uint32_t current_nonce;        // Current nonce being tested

    MiningStats()
        : hashes_computed(0), blocks_found(0), last_block_height(0)
        , hash_rate(0), mining_time_ms(0), current_nonce(0)
    {}
};

/**
 * Mining callback - called when a block is found
 *
 * @param block  The mined block
 * @return       True if block was accepted, false otherwise
 */
using MiningCallback = std::function<bool(const Block& block)>;

/**
 * Mining Engine
 *
 * Implements Bitcoin's Proof-of-Work mining:
 * 1. Takes a block template
 * 2. Iterates through nonce values
 * 3. Computes block hash for each nonce
 * 4. Checks if hash meets difficulty target
 * 5. Calls callback when block is found
 *
 * Mining algorithm:
 * - Start with nonce = 0
 * - Compute block hash = SHA256(SHA256(block_header))
 * - If hash < target: block found!
 * - Otherwise: increment nonce and try again
 * - Update timestamp every 2^32 nonces (nonce overflow)
 *
 * The mining engine runs in a background thread and can be
 * started/stopped on demand.
 */
class MiningEngine {
public:
    MiningEngine();
    ~MiningEngine();

    // ========================================================================
    // Mining Control
    // ========================================================================

    /**
     * Start mining a block template
     *
     * Spawns a background thread that mines the template.
     * If already mining, stops current mining and starts new template.
     *
     * @param template_block  Block template to mine
     * @param callback        Callback when block is found
     * @return                True if mining started successfully
     */
    bool startMining(
        std::unique_ptr<BlockTemplate> template_block,
        MiningCallback callback
    );

    /**
     * Stop mining
     *
     * Gracefully stops the mining thread.
     * Waits for thread to finish before returning.
     */
    void stopMining();

    /**
     * Check if currently mining
     */
    bool isMining() const { return is_mining_.load(); }

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get mining statistics
     */
    MiningStats getStats() const;

    /**
     * Reset statistics
     */
    void resetStats();

    // ========================================================================
    // Difficulty Validation
    // ========================================================================

    /**
     * Check if block hash meets difficulty target
     *
     * Bitcoin difficulty check:
     * - Block hash must be less than target
     * - Target is derived from compact 'bits' field
     * - Lower hash = more difficult
     *
     * Example:
     * - bits = 0x1d00ffff (difficulty 1)
     * - target = 0x00000000ffff0000000000000000000000000000000000000000000000000000
     * - hash must be < target
     *
     * @param block_hash  Block hash (hex string)
     * @param bits        Difficulty target (compact form)
     * @return            True if hash meets target
     */
    static bool checkProofOfWork(const std::string& block_hash, uint32_t bits);

    /**
     * Convert compact difficulty bits to target (256-bit value)
     *
     * Compact format (4 bytes):
     * - Byte 0: exponent (how many bytes in target)
     * - Bytes 1-3: mantissa (first 3 bytes of target)
     *
     * Example: 0x1d00ffff
     * - Exponent: 0x1d (29 bytes)
     * - Mantissa: 0x00ffff
     * - Target: 0x00ffff * 2^(8*(29-3)) = 0x00000000ffff00...00
     *
     * @param bits  Compact difficulty bits
     * @return      Target as hex string (64 characters)
     */
    static std::string bitsToTarget(uint32_t bits);

    /**
     * Calculate difficulty from bits
     *
     * Difficulty = max_target / current_target
     * Where max_target is difficulty 1 (0x1d00ffff)
     *
     * @param bits  Compact difficulty bits
     * @return      Difficulty (1.0 = minimum difficulty)
     */
    static double calculateDifficulty(uint32_t bits);

    /**
     * Hash block header
     *
     * Computes double SHA256 of serialized block header.
     *
     * @param header  Block header to hash
     * @return        Block hash (hex string)
     */
    std::string hashBlockHeader(const BlockHeader& header) const;

private:
    // Mining state
    std::atomic<bool> is_mining_;
    std::unique_ptr<std::thread> mining_thread_;
    std::unique_ptr<BlockTemplate> current_template_;
    MiningCallback current_callback_;

    // Statistics
    mutable std::mutex stats_mutex_;
    MiningStats stats_;

    // Mining loop (runs in background thread)
    void miningLoop();

    // Mine a single block (synchronous, for testing)
    bool mineBlock(Block& block, uint32_t bits, uint32_t max_iterations);

    // Update timestamp in block template (when nonce overflows)
    void updateBlockTimestamp(BlockTemplate& template_block);
};

/**
 * Block Submission Validator
 *
 * Validates submitted blocks before accepting them to the chain.
 * This ensures blocks meet all consensus rules:
 * - Valid PoW (hash meets difficulty)
 * - Valid merkle root
 * - Valid coinbase
 * - Valid transactions
 * - Correct block height
 */
class BlockSubmissionValidator {
public:
    /**
     * Construct block submission validator
     *
     * @param coins_db   UTXO database for input validation
     * @param chain_db   Chain database for BIP68 MTP lookup (optional, nullptr = fail-closed)
     */
    BlockSubmissionValidator(
        consensus::CoinsDB& coins_db,
        ::dinero::ChainDB* chain_db = nullptr
    );

    /**
     * Validate submitted block
     *
     * Full validation includes:
     * 1. PoW validation (hash meets target)
     * 2. Block header validation (version, timestamp, etc.)
     * 3. Merkle root validation (recompute and compare)
     * 4. Coinbase validation (BIP 34 height, value <= subsidy + fees)
     * 5. Transaction validation (all txs valid, no double spends)
     * 6. BIP68 sequence lock validation (if chain_db provided)
     *
     * @param block             Block to validate
     * @param expected_height   Expected block height
     * @param expected_bits     Expected difficulty target
     * @param error             [out] Error message if validation fails
     * @return                  True if block is valid
     */
    bool validateBlock(
        const Block& block,
        uint32_t expected_height,
        uint32_t expected_bits,
        std::string& error
    );

private:
    consensus::CoinsDB& coins_db_;
    ::dinero::ChainDB* chain_db_;  // Optional: for BIP68 time-based MTP lookup

    bool validateProofOfWork(const Block& block, uint32_t expected_bits, std::string& error);
    bool validateMerkleRoot(const Block& block, std::string& error);
    bool validateCoinbase(const Block& block, uint32_t expected_height, std::string& error);
    bool validateTransactions(const Block& block, uint32_t expected_height, std::string& error);
};

} // namespace mining
} // namespace dinero
