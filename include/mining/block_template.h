#pragma once

#include "wallet/transaction.h"
#include "primitives/block.h"
#include <vector>
#include <string>

namespace dinero {
namespace mining {

// ============================================================================
// Phase 26: Block Template Builder
// ============================================================================

/**
 * Block template - A candidate block ready for mining
 *
 * Contains all information needed by a miner:
 * - Block header (version, previous block hash, merkle root, timestamp, bits, nonce)
 * - Transaction set (coinbase + selected mempool transactions)
 * - Fee information (total fees collected)
 * - Weight/size metrics (for SegWit)
 */
struct BlockTemplate {
    // Block data
    Block block;                           // The actual block to mine

    // Transaction selection results
    std::vector<Transaction> transactions; // All transactions (including coinbase)
    std::vector<uint64_t> fees;            // Fee for each tx (0 for coinbase)

    // Metrics
    uint64_t total_fees;                   // Total fees in block
    uint64_t block_subsidy;                // Block reward (100 DIN at height 2+, halving every 1,314,000 blocks)
    uint64_t coinbase_value;               // Subsidy + fees
    size_t block_size;                     // Block size in bytes
    size_t block_weight;                   // Block weight (SegWit metric)
    size_t num_transactions;               // Number of transactions
    size_t num_sigops;                     // Total signature operations

    // Mining metadata
    uint32_t height;                       // Block height
    uint64_t timestamp;                    // Block timestamp
    std::string previous_block_hash;       // Previous block hash
    uint32_t bits;                         // Difficulty target (compact form)

    BlockTemplate()
        : block{}
        , total_fees(0), block_subsidy(0), coinbase_value(0)
        , block_size(0), block_weight(0), num_transactions(0), num_sigops(0)
        , height(0), timestamp(0), bits(0)
    {
        // BlockHeader v1 commits to reserved[12]; keep it deterministically
        // zeroed even when BlockTemplate is default-constructed.
        block.header.ZeroReserved();
    }
};

/**
 * Block Template Builder
 *
 * Historical block-template construction moved to MiningService. This class now
 * only owns shared static helpers still used by mining and tests.
 */
class BlockTemplateBuilder {
public:
    /**
     * Calculate block subsidy (block reward)
     *
     * NOTE: Use ConsensusSubsidy::GetBlockSubsidy(height) instead - this is the authoritative source.
     *
     * Dinero's halving schedule (FROZEN MONETARY POLICY):
     * - Height 0: 0 (genesis unspendable, OP_RETURN)
     * - Height 1+: 100 DIN initial (100 * 100,000,000 una)
     * - Halves every 1,314,000 blocks (~5 years at 2 min blocks)
     * - Formula: 100 >> (pow_blocks / 1314000) where pow_blocks = height - 2
     *
     * @param height  Block height
     * @return        Block subsidy in una (una)
     */
    static uint64_t getBlockSubsidy(uint32_t height);

    // ========================================================================
    // Merkle Root Computation
    // ========================================================================

    /**
     * Calculate merkle root from transaction IDs
     *
     * Bitcoin's merkle tree construction:
     * 1. Hash all transaction IDs (double SHA256)
     * 2. If odd number of hashes, duplicate last hash
     * 3. Pair up hashes and hash each pair
     * 4. Repeat until single hash (merkle root)
     *
     * Example with 5 transactions:
     *          merkle_root
     *         /            \
     *      hash_AB        hash_CDEE
     *      /    \         /       \
     *   hash_A hash_B  hash_CD  hash_EE
     *                  /    \    /    \
     *               txid_C txid_D txid_E txid_E (duplicated)
     *
     * @param transactions  List of transactions
     * @return              Merkle root hash (hex string)
     */
    static std::string calculateMerkleRoot(const std::vector<Transaction>& transactions);

    /**
     * Build Merkle Tree with branches (for Stratum mining)
     *
     * Returns merkle root AND merkle branches needed for share validation.
     * Merkle branches allow miners to recompute the root with modified coinbase.
     *
     * Phase 28: Includes Bitcoin-style protections:
     * - Duplicate txid detection (CVE-2012-2459 protection)
     * - Proper odd-node duplication
     * - Endianness normalization
     *
     * @param transactions    List of transactions (coinbase first)
     * @param merkle_root     Output: Merkle root hash
     * @param merkle_branches Output: Merkle branch hashes (for Stratum)
     * @return                True if successful, false if duplicate txids found
     */
    static bool buildMerkleTree(
        const std::vector<Transaction>& transactions,
        std::string& merkle_root,
        std::vector<std::string>& merkle_branches
    );
};

} // namespace mining
} // namespace dinero
