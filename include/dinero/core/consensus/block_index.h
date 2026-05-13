#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include "primitives/block.h"

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
};

/**
 * Reorg statistics for logging and metrics
 */
struct ReorgStats {
    std::string fork_hash;
    std::string old_tip_hash;
    std::string new_tip_hash;
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
    // Block identification
    std::string hash;           // This block's hash
    std::string prev_hash;      // Previous block hash
    uint32_t height{0};         // Height in the chain
    
    // Block header data
    uint32_t version{0};
    std::string merkle_root;
    uint64_t timestamp{0};
    uint32_t bits{0};           // Compact difficulty target
    uint32_t nonce{0};
    
    // Chain state
    std::string chainwork;      // Total work from genesis to this block (hex string)
    uint32_t status{0};         // Block validation status flags
    
    // Pointers for chain navigation
    CBlockIndex* pprev{nullptr};        // Previous block
    std::vector<CBlockIndex*> children; // Child blocks
    
    // Constructor
    CBlockIndex() = default;
    CBlockIndex(const BlockHeader& header, uint32_t height);
    
    // Utility methods
    std::string GetBlockHash() const { return hash; }
    bool IsGenesis() const { return height == 0; }
    bool IsValid() const { return (status & BLOCK_VALID_MASK) != 0; }
    bool IsConnectable() const { return (status & BLOCK_VALID_CHAIN) != 0; }
    
    // Chainwork comparison
    bool HasMoreWork(const CBlockIndex* other) const;
    
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
 */
struct ByWorkThenHash {
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const {
        if (!a || !b) return false;
        
        int work_cmp = chainwork::CompareWork(a->chainwork, b->chainwork);
        if (work_cmp != 0) {
            return work_cmp > 0; // Higher work first
        }
        
        // Tie-break by full 256-bit hash comparison (deterministic)
        return CompareBlockHashes(a->GetBlockHash(), b->GetBlockHash()) < 0;
    }
    
private:
    // Compare two hex hash strings as 256-bit big-endian integers
    static int CompareBlockHashes(const std::string& hash_a, const std::string& hash_b) {
        if (hash_a.length() != 64 || hash_b.length() != 64) {
            return hash_a.compare(hash_b); // Fallback to string compare
        }
        
        // Convert hex to bytes and compare big-endian
        for (size_t i = 0; i < 64; i += 2) {
            uint8_t byte_a = HexToByte(hash_a.substr(i, 2));
            uint8_t byte_b = HexToByte(hash_b.substr(i, 2));
            if (byte_a < byte_b) return -1;
            if (byte_a > byte_b) return 1;
        }
        return 0; // Equal
    }
    
    static uint8_t HexToByte(const std::string& hex) {
        return static_cast<uint8_t>(std::stoul(hex, nullptr, 16));
    }
};

// Global candidate tips set
extern std::set<CBlockIndex*, ByWorkThenHash> g_candidates;

// Block index management
extern std::unordered_map<std::string, std::unique_ptr<CBlockIndex>> g_block_index;

// Orphan pool for headers/blocks with missing parents
extern std::unordered_map<std::string, std::vector<CBlockIndex*>> g_orphan_pool;

/**
 * Block index management functions
 */
CBlockIndex* FindBlockIndex(const std::string& hash);
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
void ProcessOrphanQueue(const std::string& parent_hash);
bool CanConnect(CBlockIndex* block_index);
void MarkBlockValid(CBlockIndex* block_index, uint32_t validation_flags);

} // namespace dinero
