#pragma once

#include "primitives/block.h"
#include "primitives/uint256.h"  // For uint256
#include "storage/block_storage.h"
#include "storage/tip_info.h"  // For arith_uint256
#include "consensus/chainwork.h"
#include <cstdint>
#include <memory>
#include <atomic>

namespace dinero {

// ============================================================================
// Pruning Invariants (F.7.2)
// ============================================================================
//
// These constants define the safety boundaries for pruning operations.
// Pruning itself is NOT implemented yet - these invariants make unsafe
// pruning impossible when it is eventually added.
//
// Bitcoin Core equivalents:
//   MIN_BLOCKS_TO_KEEP = 288 (Bitcoin: ~2 days at 10 min blocks)
//   MIN_UNDO_DEPTH = 288 (Dinero: ~2 days at 10 min target)
//
// Invariants enforced:
//   1. Cannot prune within MIN_UNDO_DEPTH of active tip (reorg safety)
//   2. Cannot prune block with descendants on active chain (ancestry safety)
//   3. Cannot prune without undo data present (disconnect safety)
//   4. Cannot prune without BLOCK_HAVE_UNDO flag set (consistency)
//
static constexpr int MIN_BLOCKS_TO_KEEP = 288;  // Minimum blocks to retain (~2 days)
static constexpr int MIN_UNDO_DEPTH = 288;      // Minimum undo depth for reorg safety

// Bitcoin-style block index
//
// This is the in-memory representation of a block in the chain.
// Stores both:
//   - Persistent metadata (serialized to ChainDB)
//   - Runtime chain pointers (pprev/pnext for fast traversal)
//
// Analogous to Bitcoin Core's CBlockIndex
//
// Architecture:
//   Disk:    ChainDB stores serialized BlockIndex (hash → BlockIndex)
//   Disk:    BlockStorage stores actual block bodies (blk*.dat)
//   Memory:  BlockIndex DAG with pprev/pnext linkage
//
class BlockIndex {
public:
    // ========================================================================
    // PERSISTENT FIELDS (serialized to ChainDB)
    // ========================================================================

    // Block identification
    uint256 hash;                    // Block hash
    uint256 hash_prev;               // Hash of previous block

    // Chain position
    int32_t height = -1;             // Height in the blockchain (-1 = not in chain)

    // File location (Bitcoin-style flat file storage)
    FilePosition file_pos;           // Location in blk*.dat file
    FilePosition undo_pos;           // Location in rev*.dat file (undo data)

    // Block header fields (for header validation without reading full block)
    // Dinero uses 112-byte headers: 80 bytes Bitcoin-style + 32 bytes Utreexo
    uint32_t version = 0;            // Block version
    uint256 merkle_root;             // Merkle root of transactions
    uint32_t time = 0;               // Block timestamp
    uint32_t bits = 0;               // Compact difficulty target
    uint32_t nonce = 0;              // Proof-of-work nonce
    uint256 utreexo_root;            // Utreexo accumulator root (32 bytes) - Dinero native

    // Chain work and difficulty
    arith_uint256 chain_work;        // Total work from genesis to this block
    uint32_t tx_count = 0;           // Number of transactions in this block

    // Validation status
    // NOTE: Data/failure flags MUST match consensus/block_lifecycle.h values
    // to prevent misinterpretation if status values cross layer boundaries.
    enum ValidationStatus : uint32_t {
        VALID_UNKNOWN      = 0,      // Not yet validated
        VALID_HEADER       = 1,      // Header validated
        VALID_TREE         = 2,      // Descendant of valid block
        VALID_TRANSACTIONS = 3,      // Transactions validated
        VALID_CHAIN        = 4,      // Part of best chain
        VALID_SCRIPTS      = 5,      // All scripts validated
        VALID_MASK         = 7,      // Mask for all validation bits (bits 0-2)

        // Failure flags (bits 5-6) — match consensus/block_index.h
        BLOCK_FAILED_VALID = 32,     // Block failed validation
        BLOCK_FAILED_CHILD = 64,     // Descendant of failed block

        // Data availability flags (bits 7-8) — match consensus/block_lifecycle.h
        BLOCK_HAVE_DATA    = 128,    // Block data is available
        BLOCK_HAVE_UNDO    = 256,    // Undo data is available
    };
    uint32_t status = VALID_UNKNOWN;

    // ========================================================================
    // RUNTIME FIELDS (memory-only, not serialized)
    // ========================================================================

    // Chain linkage pointers (for fast chain traversal)
    BlockIndex* pprev = nullptr;     // Pointer to previous block
    BlockIndex* pnext = nullptr;     // Pointer to next block in active chain
    BlockIndex* pskip = nullptr;     // Pointer to ancestor for efficient lookups

    // Sequence ID (for block database rebuilds)
    int32_t sequence_id = 0;         // Used during reindex operations

    // ========================================================================
    // METHODS
    // ========================================================================

    BlockIndex() = default;

    // Constructor from block header (Dinero 128-byte headers)
    explicit BlockIndex(const BlockHeader& header) {
        version = header.version;
        merkle_root = header.merkle_root;  // Phase M.1: Direct uint256 assignment
        time = header.timestamp;
        bits = header.difficulty;  // Phase 3: BlockHeader uses 'difficulty' not 'bits'
        nonce = header.nonce;
        hash_prev = !header.prev_block_hash.IsNull() ? header.prev_block_hash : header.prev_block_hash;  // Phase M.1: Use uint256
        utreexo_root = header.utreexo_root;  // Phase M.1: Direct uint256 assignment
    }

    // Get block header without reading full block from disk (Dinero 128-byte)
    BlockHeader getBlockHeader() const {
        BlockHeader header;
        header.version = version;
        header.prev_block_hash = hash_prev;  // Phase M.1: Direct uint256 assignment
        header.prev_block_hash = hash_prev; // Phase M.1: Populate both fields
        header.merkle_root = merkle_root;  // Phase M.1: Direct uint256 assignment
        header.timestamp = time;
        header.difficulty = bits;  // Phase 3: BlockHeader uses 'difficulty' not 'bits'
        header.nonce = nonce;
        header.utreexo_root = utreexo_root;  // Phase M.1: Direct uint256 assignment
        return header;
    }

    // Chain walking helpers
    BlockIndex* getAncestor(int height) const {
        if (height > this->height || height < 0) {
            return nullptr;
        }

        BlockIndex* pindex_walk = const_cast<BlockIndex*>(this);
        int height_walk = this->height;

        while (height_walk > height) {
            // Use skip pointer if available and helpful
            int height_skip = get_skip_height(height_walk);
            int height_skip_prev = get_skip_height(height_walk - 1);

            if (pindex_walk->pskip != nullptr &&
                (height_skip == height ||
                 (height_skip > height && !(height_skip_prev < height_skip - 2 &&
                                            height_skip_prev >= height)))) {
                pindex_walk = pindex_walk->pskip;
                height_walk = height_skip;
            } else {
                pindex_walk = pindex_walk->pprev;
                height_walk--;
            }
        }

        return pindex_walk;
    }

    // Phase 20.1: Block Locator for headers-first sync
    // Generate a block locator vector for efficient fork detection
    // Uses exponential spacing: [tip, tip-1, tip-2, tip-4, tip-8, ..., genesis]
    // This allows O(log N) fork point detection during headers sync
    std::vector<uint256> getLocator() const {
        std::vector<uint256> locator;
        const BlockIndex* pindex = this;
        int step = 1;

        while (pindex) {
            locator.push_back(pindex->hash);

            // Stop at genesis
            if (pindex->height == 0) {
                break;
            }

            // Exponential spacing: go back 1, 2, 4, 8, 16, 32, 64, 128...
            int height = pindex->height - step;
            if (height < 0) {
                height = 0;
            }

            pindex = pindex->getAncestor(height);

            // After the first 10 blocks, increase step exponentially
            if (locator.size() > 10) {
                step *= 2;
            }
        }

        return locator;
    }

    // Validation status helpers
    bool isValid(ValidationStatus up_to = VALID_TRANSACTIONS) const {
        return (status & VALID_MASK) >= up_to;
    }

    bool haveData() const {
        return status & BLOCK_HAVE_DATA;
    }

    bool haveUndo() const {
        return status & BLOCK_HAVE_UNDO;
    }

    bool failedValid() const {
        return status & BLOCK_FAILED_VALID;
    }

    // Raise the validity level of this block index entry
    bool raiseValidity(ValidationStatus up_to) {
        if (status & BLOCK_FAILED_MASK) {
            return false;
        }
        if ((status & VALID_MASK) < up_to) {
            status = (status & ~VALID_MASK) | up_to;
            return true;
        }
        return false;
    }

    // Build the skip list (for efficient ancestor lookup)
    void buildSkip() {
        if (pprev) {
            pskip = pprev->getAncestor(get_skip_height(height));
        }
    }

    // ========================================================================
    // F.7.2: Pruning Safety Checks
    // ========================================================================
    //
    // Check if this block is safe to prune (logical check only, no deletion).
    // Returns true if ALL of the following conditions are met:
    //
    //   1. Block is older than MIN_UNDO_DEPTH from active tip
    //   2. Block has no descendants on the active chain
    //   3. Block has BLOCK_HAVE_UNDO flag set
    //   4. Undo data is available on disk (checked via block_storage)
    //
    // This is a pure logical check - it performs NO disk operations or deletions.
    // Future pruning code MUST call this before attempting to prune any block.
    //
    // Parameters:
    //   active_tip: Pointer to the current active chain tip
    //   block_storage: Pointer to BlockStorage for undo availability check
    //
    // Returns:
    //   true if block is safe to prune, false otherwise
    //
    bool isPrunable(const BlockIndex* active_tip, const class BlockStorage* block_storage) const;

    // Serialize to bytes for ChainDB storage
    std::string serialize() const;

    // Deserialize from bytes
    static BlockIndex deserialize(const std::string& data);

private:
    static constexpr uint32_t BLOCK_FAILED_MASK = BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD;

    // Calculate skip height for building skip list
    // Bitcoin's skip list algorithm for O(log n) ancestor lookups
    static int get_skip_height(int height) {
        if (height < 2) return 0;

        // Determine which height to jump back to.
        // Skip exponentially back in larger steps.
        return (height & 1) ? invert_lowest_one(invert_lowest_one(height - 1)) + 1 : invert_lowest_one(height);
    }

    static inline int invert_lowest_one(int n) {
        return n & (n - 1);
    }
};

} // namespace dinero
