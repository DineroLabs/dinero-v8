#pragma once

#include "wallet/transaction.h"
#include "primitives/block.h"
#include "mempool/mempool.h"
#include "consensus/coins_db.h"
#include "mining/payout_spec.h"
#include <vector>
#include <memory>
#include <string>

// Phase 10d: Forward declaration for optional Utreexo integration
namespace dinero {
namespace consensus {
    class UtreexoForest;
}
}

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
 * Block template builder configuration
 */
struct BlockTemplateConfig {
    // Block limits (Bitcoin consensus)
    size_t max_block_weight;    // Default: 4,000,000 WU (1 MB base + 3 MB witness)
    size_t max_block_size;      // Default: 1,000,000 bytes (legacy limit)
    size_t max_sigops;          // Default: 80,000 signature operations

    // Mining address
    std::string mining_address; // Address to receive block reward

    // Template options
    bool include_witness;       // Include witness data (SegWit)
    bool sort_by_ancestor_fee;  // Sort by ancestor fee rate (CPFP)

    BlockTemplateConfig()
        : max_block_weight(4000000)
        , max_block_size(1000000)
        , max_sigops(80000)
        , include_witness(true)
        , sort_by_ancestor_fee(true)
    {}
};

/**
 * Block Template Builder
 *
 * Constructs block templates for mining by:
 * 1. Selecting transactions from mempool by fee rate
 * 2. Packaging ancestors before descendants (CPFP)
 * 3. Enforcing block weight and size limits
 * 4. Creating coinbase transaction with subsidy + fees
 * 5. Computing merkle root
 * 6. Assembling final block header
 *
 * This is the core of the mining engine - it converts the mempool
 * into a valid block ready for PoW mining.
 */
class BlockTemplateBuilder {
public:
    BlockTemplateBuilder(
        mempool::Mempool& mempool,
        consensus::CoinsDB& coins_db,
        const BlockTemplateConfig& config = BlockTemplateConfig(),
        consensus::UtreexoForest* utreexo_forest = nullptr  // Phase 10d: Optional Utreexo accumulator
    );

    ~BlockTemplateBuilder();

    // ========================================================================
    // Block Template Construction
    // ========================================================================

    /**
     * Create a new block template
     *
     * @param previous_block_hash  Hash of the previous block
     * @param height               Block height
     * @param timestamp            Block timestamp (current time)
     * @param bits                 Difficulty target (compact form)
     * @param mining_address       Address to receive block reward
     * @return                     Block template ready for mining
     */
    std::unique_ptr<BlockTemplate> createBlockTemplate(
        const std::string& previous_block_hash,
        uint32_t height,
        uint64_t timestamp,
        uint32_t bits,
        const std::string& mining_address
    );

    /**
     * Create a new block template with pool payouts (Phase C3)
     *
     * Supports daemon-defined pool payouts with weighted distribution.
     * The daemon constructs all coinbase outputs - miners never build them.
     *
     * @param previous_block_hash  Hash of the previous block
     * @param height               Block height
     * @param timestamp            Block timestamp (current time)
     * @param bits                 Difficulty target (compact form)
     * @param payout_spec          Payout specification (single or weighted)
     * @return                     Block template ready for mining
     */
    std::unique_ptr<BlockTemplate> createBlockTemplate(
        const std::string& previous_block_hash,
        uint32_t height,
        uint64_t timestamp,
        uint32_t bits,
        const dinero::PayoutSpec& payout_spec
    );

    /**
     * Update block timestamp and rebuild merkle root
     *
     * Allows miner to update timestamp without rebuilding entire template.
     */
    void updateTimestamp(BlockTemplate& template_block, uint64_t new_timestamp);

    // ========================================================================
    // Transaction Selection
    // ========================================================================

    /**
     * Select transactions from mempool for block
     *
     * Selection algorithm:
     * 1. Get all mempool transactions sorted by ancestor fee rate
     * 2. Iterate in descending fee rate order
     * 3. Add transaction if:
     *    - All ancestors are already in block
     *    - Adding it doesn't exceed block limits
     *    - Signature operations don't exceed limit
     * 4. Continue until block is full or no more transactions
     *
     * @param height              Block height (for coinbase maturity checks)
     * @param max_weight          Maximum block weight
     * @param max_sigops          Maximum signature operations
     * @param selected_txs        [out] Selected transactions
     * @param total_fees          [out] Total fees collected
     * @return                    True if selection succeeded
     */
    bool selectTransactions(
        uint32_t height,
        size_t max_weight,
        size_t max_sigops,
        std::vector<Transaction>& selected_txs,
        uint64_t& total_fees
    );

    // ========================================================================
    // Coinbase Transaction
    // ========================================================================

    /**
     * Create coinbase transaction
     *
     * Coinbase structure:
     * - 1 input with null outpoint (0x00...00:0xFFFFFFFF)
     * - scriptSig contains block height (BIP 34) + extra nonce
     * - 1 or more outputs paying to mining address
     * - Total output value = block subsidy + fees
     *
     * @param height              Block height
     * @param block_subsidy       Block reward (100 DIN at height 2+, halves every 1,314,000 blocks)
     * @param total_fees          Total transaction fees
     * @param mining_address      Address to receive reward
     * @param extra_nonce         Extra nonce for mining (4 bytes)
     * @return                    Coinbase transaction
     */
    Transaction createCoinbase(
        uint32_t height,
        uint64_t block_subsidy,
        uint64_t total_fees,
        const std::string& mining_address,
        uint32_t extra_nonce = 0
    );

    /**
     * Create coinbase transaction with pool payouts (Phase C3)
     *
     * Constructs coinbase with multiple outputs based on PayoutSpec.
     * Daemon-defined: miners and stratum never build coinbase outputs.
     *
     * Coinbase structure:
     * - 1 input with null outpoint (0x00...00:0xFFFFFFFF)
     * - scriptSig contains block height (BIP 34) + witness nonce placeholder
     * - N outputs based on resolved payout amounts
     * - Total output value = block subsidy + fees (exact)
     *
     * Output ordering: deterministic (input order from PayoutSpec)
     *
     * @param height              Block height
     * @param block_subsidy       Block reward
     * @param total_fees          Total transaction fees
     * @param payout_spec         Payout specification
     * @param extra_nonce         Extra nonce for mining (4 bytes)
     * @return                    Coinbase transaction with multi-output
     */
    Transaction createCoinbase(
        uint32_t height,
        uint64_t block_subsidy,
        uint64_t total_fees,
        const dinero::PayoutSpec& payout_spec,
        uint32_t extra_nonce = 0
    );

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

    // ========================================================================
    // Weight and Size Calculation
    // ========================================================================

    /**
     * Calculate transaction weight (BIP 141 - SegWit)
     *
     * Weight formula:
     * weight = (base_size * 3) + total_size
     *
     * Where:
     * - base_size = size without witness data
     * - total_size = size with witness data
     *
     * This gives witness data a 75% discount.
     *
     * @param tx  Transaction
     * @return    Weight in weight units (WU)
     */
    static size_t calculateTransactionWeight(const Transaction& tx);

    /**
     * Count signature operations in transaction
     *
     * @param tx  Transaction
     * @return    Number of signature operations
     */
    static size_t countSigOps(const Transaction& tx);

    // Transaction selection state (public: needed by tests and addTransaction caller)
    struct SelectionState {
        std::unordered_set<std::string> selected_txids;
        size_t current_weight;
        size_t current_sigops;
        uint64_t current_fees;

        SelectionState()
            : current_weight(0), current_sigops(0), current_fees(0)
        {}
    };

private:
    // Dependencies
    mempool::Mempool& mempool_;
    consensus::CoinsDB& coins_db_;
    BlockTemplateConfig config_;
    consensus::UtreexoForest* utreexo_forest_;  // Phase 10d: Optional Utreexo accumulator (nullable)

    // Helper functions
    bool canAddTransaction(
        const mempool::MempoolEntry& entry,
        const SelectionState& state,
        size_t max_weight,
        size_t max_sigops
    ) const;

    bool hasAllAncestors(
        const mempool::MempoolEntry& entry,
        const SelectionState& state
    ) const;

    void addTransaction(
        const mempool::MempoolEntry& entry,
        SelectionState& state,
        std::vector<Transaction>& selected_txs
    );

    std::vector<uint8_t> buildCoinbaseScriptSig(uint32_t height, uint32_t extra_nonce) const;
    std::vector<uint8_t> buildMiningOutputScript(const std::string& address) const;
};

} // namespace mining
} // namespace dinero
