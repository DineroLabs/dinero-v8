#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include "primitives/block.h"
#include "primitives/uint256.h"

namespace dinero {

// Block validation status flags (Bitcoin-compatible)
enum BlockStatus {
    BLOCK_VALID_UNKNOWN      = 0,
    BLOCK_VALID_HEADER       = 1,    // Header validated
    BLOCK_VALID_TREE         = 2,    // Merkle tree validated
    BLOCK_VALID_TRANSACTIONS = 4,    // All transactions validated
    BLOCK_VALID_CHAIN        = 8,    // Block connects to valid chain
    BLOCK_VALID_SCRIPTS      = 16,   // All scripts validated

    BLOCK_VALID_MASK         = 31,   // All validation flags

    // Failure flags
    BLOCK_FAILED_VALID       = 32,   // Block failed validation
    BLOCK_FAILED_CHILD       = 64,   // Block has failed child

    // Phase P.1: Prune eligibility (derived, restart-safe)
    // Note: BLOCK_HAVE_DATA and BLOCK_HAVE_UNDO are defined in block_lifecycle.h
    BLOCK_PRUNE_ELIGIBLE     = 512,  // Block is safe to prune (all conditions met)
};

/**
 * Reorg statistics for logging and metrics
 */
struct ReorgStats {
    uint256 fork_hash;
    uint256 old_tip_hash;
    uint256 new_tip_hash;
    int disconnect_depth{0};
    int connect_depth{0};
    int64_t duration_ms{0};
    int resurrected_txs{0};
};

/**
 * CBlockIndex - Represents a block in the blockchain index
 *
 * This is the core structure for tracking blocks and their relationships.
 * It stores essential metadata for fork-choice and reorg operations.
 */
class CBlockIndex {
public:
    // Block identification (canonical uint256)
    uint256 hash;           // This block's hash
    uint256 prev_hash;      // Previous block hash
    uint32_t height{0};     // Height in the chain

    // Block header data
    uint32_t version{0};
    uint256 merkle_root;
    uint64_t timestamp{0};
    uint32_t bits{0};       // Compact difficulty target
    uint32_t nonce{0};

    // Chain state
    std::string chainwork;  // Total work from genesis to this block (hex string)
    uint32_t status{0};     // Block validation status flags

    // Phase P.2: Disk storage positions (Bitcoin Core CDiskBlockPos pattern)
    // Populated by BlockStorage when block data is written to disk
    // Required for pruning: must know WHERE to zero out data
    uint32_t file_number{0};  // blk00000.dat file number (0 = not stored)
    uint32_t data_pos{0};     // Offset of block data in file
    uint32_t data_size{0};    // Size of block data
    uint32_t undo_file{0};    // rev00000.dat file number (0 = no undo)
    uint32_t undo_pos{0};     // Offset of undo data in undo file
    uint32_t undo_size{0};    // Size of undo data

    // Pointers for chain navigation (NOT persisted - runtime only)
    CBlockIndex* pprev{nullptr};        // Previous block
    std::vector<CBlockIndex*> children; // Child blocks

    // Constructor
    CBlockIndex() = default;
    CBlockIndex(const BlockHeader& header, uint32_t height);

    // Utility methods
    uint256 GetBlockHash() const { return hash; }
    bool IsGenesis() const { return height == 0; }
    bool IsValid() const { return (status & BLOCK_VALID_MASK) != 0; }
    bool IsConnectable() const { return (status & BLOCK_VALID_CHAIN) != 0; }

    // Chainwork comparison
    bool HasMoreWork(const CBlockIndex* other) const;

    // BIP113: Median Time Past for locktime validation
    // Returns median of last 11 block timestamps (or all if < 11 ancestors)
    uint64_t GetMedianTimePast() const;

    // Create from block header
    static std::unique_ptr<CBlockIndex> FromHeader(const BlockHeader& header, uint32_t height);
};

/**
 * Chainwork calculation utilities
 */
namespace chainwork {
    
    /**
     * Calculate work for a single block from its difficulty bits
     * Work = 2^256 / (target + 1)
     */
    std::string WorkForBits(uint32_t bits);
    
    /**
     * Add two chainwork values (hex strings representing 256-bit numbers)
     */
    std::string AddWork(const std::string& work_a, const std::string& work_b);
    
    /**
     * Compare two chainwork values
     * Returns: -1 if a < b, 0 if a == b, 1 if a > b
     */
    int CompareWork(const std::string& work_a, const std::string& work_b);
    
    /**
     * Convert compact difficulty bits to 256-bit target (hex string)
     */
    std::string BitsToTarget(uint32_t bits);
    
    /**
     * Calculate work from target (hex string)
     */
    std::string TargetToWork(const std::string& target_hex);
}

/**
 * Candidate tips ordered by chainwork (most work first, then by hash for determinism)
 *
 * CONSENSUS RULE (v0.15.0.4):
 * When two competing chains have equal cumulative chainwork, the chain whose
 * tip block hash is lexicographically smallest is selected as the active chain.
 *
 * This guarantees deterministic fork resolution across all nodes without
 * timing dependencies, preventing network splits under equal-work scenarios.
 */
struct ByWorkThenHash {
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const {
        if (!a || !b) return false;

        // Primary ordering: Highest cumulative chainwork wins
        int work_cmp = chainwork::CompareWork(a->chainwork, b->chainwork);
        if (work_cmp != 0) {
            return work_cmp > 0; // Higher work first
        }

        // CONSENSUS TIE-BREAKING RULE: Lowest block hash wins
        // This is the deterministic rule that prevents network splits
        return a->GetBlockHash() < b->GetBlockHash();
    }
};

// Global candidate tips set
extern std::set<CBlockIndex*, ByWorkThenHash> g_candidates;

// Block index management
extern std::unordered_map<uint256, std::unique_ptr<CBlockIndex>> g_block_index;

// Orphan pool for headers/blocks with missing parents
extern std::unordered_map<uint256, std::vector<CBlockIndex*>> g_orphan_pool;

/**
 * Block index management functions
 */
CBlockIndex* FindBlockIndex(const uint256& hash);
CBlockIndex* AddBlockIndex(const BlockHeader& header, uint32_t height);
void UpdateChainwork(CBlockIndex* block_index);
void AddCandidate(CBlockIndex* block_index);
void RemoveCandidate(CBlockIndex* block_index);
CBlockIndex* GetBestCandidate();

/**
 * Header-first sync functions
 */
bool MaybeQueueOrphan(CBlockIndex* block_index);
void OnParentValidated(CBlockIndex* parent_index);
void ProcessOrphanQueue(const uint256& parent_hash);
bool CanConnect(CBlockIndex* block_index);
void MarkBlockValid(CBlockIndex* block_index, uint32_t validation_flags);

} // namespace dinero
