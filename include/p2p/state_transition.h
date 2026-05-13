/**
 * Phase G.3.4: State Transition Layer
 *
 * Applies validated blocks to chainstate (UTXO set + block index).
 * This is the ONLY module that mutates consensus-critical state.
 *
 * Design Principles:
 * - Consumes G.3.3 validation results (no re-validation)
 * - Two-phase durability: undo to filesystem, state to RocksDB
 * - All-or-nothing observable state (crash-safe)
 * - Reorg-safe (undo data reusable, non-consuming)
 *
 * What This IS:
 * - UTXO set updates (add/remove)
 * - Undo data persistence (rev*.dat)
 * - Block index updates (status flags, chainwork)
 * - Chainstate metadata (best block, height)
 *
 * What This Is NOT:
 * - Fork choice / ActivateBestChain (not yet implemented)
 * - Mempool interaction (not yet implemented)
 * - Wallet updates (not yet implemented)
 * - Networking / announcements (not yet implemented)
 * - Policy decisions (belongs in mempool layer)
 */

//=============================================================================
// ⚠️ ARCHITECTURAL GUARDRAIL — DO NOT VIOLATE ⚠️
//=============================================================================
//
// THIS MODULE IS THE **ONLY** MODULE THAT MAY MUTATE CONSENSUS STATE.
//
// This module answers ONLY ONE QUESTION:
//   "How do I apply a validated block to the UTXO set and block index?"
//
// This module MUST NEVER answer:
//   ❌ "Is this block valid?" (that's G.3.3)
//   ❌ "Should we switch to this fork?" (that's ActivateBestChain)
//   ❌ "Does this conflict with mempool?" (that's mempool layer)
//   ❌ "Should we relay this?" (that's P2P layer)
//
// ANY VALIDATION LOGIC BELONGS TO PHASE G.3.3 (Consensus Validation).
// ANY FORK-CHOICE LOGIC BELONGS TO A FUTURE PHASE (ActivateBestChain).
//
// If you are tempted to add ANY of the following to this module, STOP:
//   ❌ Block validation (already done in G.3.3)
//   ❌ Transaction validation (already done in G.3.3)
//   ❌ Signature verification (already done in G.3.3)
//   ❌ Script execution (already done in G.3.3)
//   ❌ Subsidy checks (already done in G.3.3)
//   ❌ Chainwork comparison (belongs in ActivateBestChain)
//   ❌ Fork selection (belongs in ActivateBestChain)
//   ❌ Mempool conflict resolution (belongs in mempool layer)
//   ❌ Wallet balance updates (belongs in wallet layer)
//   ❌ Network announcements (belongs in P2P layer)
//
// The separation between G.3.3 (pure evaluation) and G.3.4 (state mutation)
// is a CRITICAL architectural boundary. Bitcoin Core maintains this separation
// in CheckInputs() vs ConnectBlock() / DisconnectBlock().
//
// Violating this boundary creates:
//   - Non-deterministic state transitions
//   - Reorg bugs (state becomes inconsistent)
//   - Impossible-to-test code (side effects everywhere)
//   - Consensus failures (nodes diverge)
//
// This module is FROZEN as of Phase G.3.4 completion.
// Extensions require explicit architectural review.
//
//=============================================================================

#pragma once

#include "consensus_validator.h"
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace p2p {

//=============================================================================
// Block Index - Block Metadata
//=============================================================================

/**
 * BlockIndex - Metadata for a single block in the block tree
 *
 * This structure tracks block position in the tree and chainwork.
 * It does NOT contain the block data itself (that's in block storage).
 */
struct BlockIndex {
    Hash256 hash;
    Hash256 prev_hash;
    uint32_t height;
    uint64_t chainwork;  // Cumulative work from genesis to this block
    bool connected;      // BLOCK_CONNECTED flag

    // Block data location (for loading real blocks - L2.3)
    uint32_t block_file_id;      // blk*.dat file number (0 = not stored)
    uint64_t block_file_offset;  // Byte offset in blk*.dat
    uint32_t block_size;         // Size of block data

    // Undo data location (for disconnection)
    uint32_t undo_file_id;
    uint64_t undo_file_offset;
    uint64_t undo_length;
    uint32_t undo_checksum;

    BlockIndex()
        : height(0), chainwork(0), connected(false),
          block_file_id(0), block_file_offset(0), block_size(0),
          undo_file_id(0), undo_file_offset(0), undo_length(0), undo_checksum(0) {}
};

//=============================================================================
// Forward Declarations
//=============================================================================

class IBlockIndexDB;       // Block index persistence interface

//=============================================================================
// UTXO View Interface (Mutable)
//=============================================================================

/**
 * IUTXOView - Mutable UTXO set interface
 *
 * This interface abstracts UTXO set operations for state transitions.
 * It enables:
 * - Adding UTXOs (new outputs from connected blocks)
 * - Removing UTXOs (spent inputs from connected blocks)
 * - Querying UTXOs (for validation and disconnect)
 */
class IUTXOView {
public:
    virtual ~IUTXOView() = default;

    // Add a UTXO (called during ConnectBlock)
    virtual void addUTXO(const OutPoint& outpoint, const TxOut& txout) = 0;

    // Remove a UTXO (called during ConnectBlock for spent inputs)
    virtual void removeUTXO(const OutPoint& outpoint) = 0;

    // Query UTXO (read-only)
    virtual std::optional<TxOut> getUTXO(const OutPoint& outpoint) const = 0;
    virtual bool hasUTXO(const OutPoint& outpoint) const = 0;
};

//=============================================================================
// Connection Failure Classification
//=============================================================================

enum class ConnectFailReason {
    NONE,           // No failure (success)
    PRECONDITION,   // Precondition violation (double-connect, parent not connected, etc.)
    UNDO_IO,        // Undo data write failure (filesystem I/O error)
    DB_COMMIT,      // Database commit failure (RocksDB write error)
    INVARIANT,      // Internal invariant violation (should never happen)
    CORRUPTION      // Data corruption detected (checksum mismatch, invalid data)
};

//=============================================================================
// Block Connection Result
//=============================================================================

struct BlockConnectionResult {
    bool ok;
    std::string error;
    ConnectFailReason fail_reason;

    // Undo data location (for DisconnectBlock)
    uint32_t undo_file_id;      // Which revNNNNN.dat file
    uint64_t undo_file_offset;  // Byte offset in file
    uint64_t undo_length;       // Length in bytes
    uint32_t undo_checksum;     // CRC32 checksum

    BlockConnectionResult() : ok(true), error(""), fail_reason(ConnectFailReason::NONE),
                              undo_file_id(0), undo_file_offset(0),
                              undo_length(0), undo_checksum(0) {}

    static BlockConnectionResult Ok(uint32_t file_id, uint64_t offset,
                                     uint64_t length, uint32_t checksum) {
        BlockConnectionResult result;
        result.ok = true;
        result.fail_reason = ConnectFailReason::NONE;
        result.undo_file_id = file_id;
        result.undo_file_offset = offset;
        result.undo_length = length;
        result.undo_checksum = checksum;
        return result;
    }

    static BlockConnectionResult Fail(const std::string& err, ConnectFailReason reason) {
        BlockConnectionResult result;
        result.ok = false;
        result.error = err;
        result.fail_reason = reason;
        return result;
    }
};

//=============================================================================
// Block Disconnection Result
//=============================================================================

struct BlockDisconnectionResult {
    bool ok;
    std::string error;

    BlockDisconnectionResult() : ok(true), error("") {}

    static BlockDisconnectionResult Ok() {
        return BlockDisconnectionResult();
    }

    static BlockDisconnectionResult Fail(const std::string& err) {
        BlockDisconnectionResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

//=============================================================================
// Block Index DB Interface
//=============================================================================

/**
 * IBlockIndexDB - Interface for block index persistence
 *
 * This interface abstracts block metadata storage operations.
 */
class IBlockIndexDB {
public:
    virtual ~IBlockIndexDB() = default;

    // Check if block is connected (BLOCK_CONNECTED flag set)
    virtual bool isBlockConnected(const Hash256& block_hash) const = 0;

    // Mark block as connected
    virtual void markBlockConnected(const Hash256& block_hash, bool connected) = 0;

    // Get block index by hash (for ActivateBestChain fork-point finding)
    virtual struct BlockIndex* getBlockIndex(const Hash256& block_hash) = 0;

    // Commit pending changes to database
    virtual bool commitBatch() = 0;

    /**
     * Update block undo position (L2.4: Persist undo positions)
     *
     * Called after ConnectBlock to persist undo file position in block index.
     * This allows reorg to find undo data on restart.
     *
     * @param block_hash    Block hash
     * @param file_id       Undo file number (revNNNNN.dat)
     * @param offset        Byte offset in undo file
     * @param length        Undo data length
     * @param checksum      Undo data checksum
     *
     * USAGE:
     * - Called from ActivateBestChain after ConnectBlock succeeds
     * - Updates in-memory BlockIndex (caller must persist via commitBatch)
     */
    virtual void setBlockUndoPosition(const Hash256& block_hash,
                                      uint32_t file_id,
                                      uint64_t offset,
                                      uint64_t length,
                                      uint32_t checksum) = 0;

    // Additional methods for block metadata will be added as needed
};

//=============================================================================
// Undo Storage Interface
//=============================================================================

/**
 * IUndoStorage - Interface for block and undo data persistence
 *
 * This interface abstracts filesystem operations for block and undo data.
 * It enables:
 * - Loading blocks from blk*.dat files (L2.3)
 * - Writing undo data to rev*.dat files (append-only)
 * - Reading undo data for disconnection (reusable, checksummed)
 * - Checking undo existence (for startup validation)
 * - Pruning old undo data (once outside MIN_BLOCKS_TO_KEEP window)
 */
class IUndoStorage {
public:
    virtual ~IUndoStorage() = default;

    /**
     * Load block from disk storage (L2.3)
     *
     * @param file_id     blk*.dat file number
     * @param offset      Byte offset in file
     * @param size        Size of block data
     * @param out_block   Output: loaded block
     * @return true if load succeeded, false otherwise
     *
     * USAGE:
     * - ActivateBestChain needs to load blocks for connect/disconnect
     * - Fail hard if block missing (no fallback, no retry)
     * - Read from disk only (no caching, no network)
     */
    virtual bool loadBlock(uint32_t file_id,
                          uint64_t offset,
                          uint32_t size,
                          Block& out_block) const = 0;

    /**
     * Check if undo data exists for a block
     *
     * @param block_hash  Block hash
     * @return true if undo data exists, false otherwise
     *
     * USAGE:
     * - Startup invariant checks (BLOCK_CONNECTED requires undo)
     * - Reorg safety checks (disconnect requires undo)
     * - Avoids leaking storage implementation details
     */
    virtual bool hasUndo(const Hash256& block_hash) const = 0;

    /**
     * Write undo data to storage
     *
     * @param block_hash     Block hash
     * @param data           Serialized undo data
     * @param out_file_id    Output: which revNNNNN.dat file
     * @param out_offset     Output: byte offset in file
     * @param out_length     Output: length in bytes
     * @param out_checksum   Output: CRC32 checksum
     * @return true if write succeeded, false otherwise
     */
    virtual bool writeUndo(const Hash256& block_hash,
                          const std::vector<uint8_t>& data,
                          uint32_t& out_file_id,
                          uint64_t& out_offset,
                          uint64_t& out_length,
                          uint32_t& out_checksum) = 0;

    /**
     * Read undo data from storage
     *
     * @param block_hash         Block hash
     * @param file_id            Which revNNNNN.dat file
     * @param offset             Byte offset in file
     * @param length             Length in bytes
     * @param expected_checksum  Expected CRC32 checksum
     * @param out_data           Output: deserialized undo data
     * @return true if read succeeded and checksum matched, false otherwise
     */
    virtual bool readUndo(const Hash256& block_hash,
                         uint32_t file_id,
                         uint64_t offset,
                         uint64_t length,
                         uint32_t expected_checksum,
                         std::vector<uint8_t>& out_data) = 0;
};

//=============================================================================
// ConnectBlock: Apply Validated Block to Chainstate
//=============================================================================

/**
 * ConnectBlock - Apply a validated block to the UTXO set and block index
 *
 * This is the AUTHORITATIVE state transition function.
 * It consumes validation results from G.3.3 and applies mutations.
 *
 * PRECONDITIONS (Caller MUST ensure):
 * ✅ Block has passed G.3.2 (Structural Validation)
 * ✅ Block has passed G.3.3 (Consensus Validation)
 * ✅ Parent block is fully connected (BLOCK_CONNECTED flag set)
 * ✅ No descendant blocks are connected (would violate tree invariant)
 * ✅ UTXO view contains parent's state (all inputs exist)
 * ✅ No concurrent ConnectBlock/DisconnectBlock operations (single-threaded)
 * ✅ Sufficient disk space for undo data (~1-10 MB per block)
 * ✅ Sufficient disk space for RocksDB batch (~1-10 MB per block)
 * ❌ MUST NOT be called if block is already BLOCK_CONNECTED (double-connect forbidden)
 * ❌ MUST NOT be called if parent has any other connected child (tree invariant)
 *
 * POSTCONDITIONS (If success):
 * ✅ All transaction outputs added to UTXO set (except OP_RETURN)
 * ✅ All transaction inputs removed from UTXO set (except coinbase)
 * ✅ Undo data written to rev*.dat (Phase A - filesystem durability)
 * ✅ UTXO + metadata committed to RocksDB (Phase B - database durability)
 * ✅ Block status updated to BLOCK_CONNECTED
 * ✅ Block index metadata updated (height, chainwork, forward links)
 * ✅ Chainstate metadata updated (best block, height)
 * ✅ Undo mapping stored (file_id, offset, length, checksum)
 * ✅ Observable state is all-or-nothing (crash-safe)
 *
 * POSTCONDITIONS (If failure):
 * ✅ NO UTXO mutations (rolled back)
 * ✅ NO block index updates (rolled back)
 * ✅ NO chainstate metadata updates (rolled back)
 * ✅ Undo data MAY exist on disk (but not referenced, safe to prune)
 * ✅ Observable state is unchanged (crash-safe)
 *
 * UNDO ASYMMETRY RULE:
 * - Undo data MAY exist without a connected block (orphaned undo is safe)
 * - A connected block MUST NEVER exist without undo data (invariant violation)
 *
 * DURABILITY MODEL (Two-Phase):
 *
 * Phase A: Undo Data Persistence (Filesystem)
 * - Serialize BlockUndo to memory buffer
 * - Write to revNNNNN.dat (append-only)
 * - fsync() to ensure durability
 * - Atomic rename (if creating new file)
 * - Calculate CRC32 checksum
 * - On failure: abort, no observable state change
 *
 * Phase B: State Commit (RocksDB)
 * - Build RocksDB WriteBatch:
 *   - Add UTXO entries (new outputs)
 *   - Delete UTXO entries (spent inputs)
 *   - Update block index metadata
 *   - Update chainstate metadata (best block, height)
 *   - Store undo mapping (file_id, offset, length, checksum)
 * - Commit batch atomically (single WriteOptions{sync: true})
 * - On failure: undo data exists but not referenced (safe to prune)
 *
 * CRASH SAFETY:
 *
 * Crash Point 1: Before undo write
 *   → Recovery: No observable change, restart from parent
 *
 * Crash Point 2: During undo write (partial file)
 *   → Recovery: No DB reference, safe to prune incomplete file
 *
 * Crash Point 3: After undo write, before DB commit
 *   → Recovery: Undo exists but not referenced, safe to prune
 *
 * Crash Point 4: During DB commit (RocksDB handles atomicity)
 *   → Recovery: Either fully committed or fully rolled back
 *
 * Crash Point 5: After DB commit
 *   → Recovery: Block fully connected, restart from this block
 *
 * OBSERVABLE STATE INVARIANT:
 *   Either: Block fully connected (BLOCK_CONNECTED + undo + UTXO)
 *   Or: Block never existed (no mutations)
 *   Never: Partial state (some mutations but not all)
 *
 * REORG SAFETY:
 * - Undo data is REUSABLE (not consumed by DisconnectBlock)
 * - Undo checksum prevents silent corruption
 * - Undo must exist for MIN_BLOCKS_TO_KEEP (288 blocks = 2 days)
 * - DisconnectBlock validates undo checksum before use
 *
 * PERFORMANCE CHARACTERISTICS:
 * - Average block: 1-10 ms (modern SSD, RocksDB WAL enabled)
 * - Large block (10k txs): 50-200 ms
 * - Undo write: ~1 MB/block (dominated by scriptPubKey)
 * - RocksDB batch: ~1 MB/block (dominated by UTXO entries)
 *
 * BITCOIN CORE EQUIVALENCE:
 * - This function is equivalent to Bitcoin Core's ConnectBlock()
 * - Matches v0.16+ semantics (two-phase durability model)
 * - Matches undo data format (reusable, checksummed)
 * - Matches crash recovery semantics (all-or-nothing)
 *
 * @param block              The validated block to connect
 * @param height             Block height (parent height + 1)
 * @param utxo_view          Mutable UTXO set (will be updated)
 * @param block_index_db     Block index persistence interface
 * @param undo_storage       Undo data persistence interface (rev*.dat)
 * @param params             Consensus parameters (unused in G.3.4, for future BIP activation)
 *
 * @return BlockConnectionResult with undo location (if success) or error (if failure)
 *
 * USAGE EXAMPLE:
 *
 *   // Step 1: Validate block (G.3.2 + G.3.3)
 *   auto validation_result = validator.validateBlock(block, height, utxo_view, params);
 *   if (!validation_result.ok) {
 *       return BlockConnectionResult::Fail(validation_result.error);
 *   }
 *
 *   // Step 2: Connect block (G.3.4)
 *   auto connection_result = ConnectBlock(block, height, utxo_view,
 *                                          block_index_db, undo_storage, params);
 *   if (!connection_result.ok) {
 *       return connection_result;  // State unchanged
 *   }
 *
 *   // Step 3: Block is now fully connected
 *   // UTXO set updated, undo data persisted, block index updated
 *
 * ERROR HANDLING:
 * - Validation failures: Return error, no state mutation
 * - Undo write failures: Return error, no state mutation
 * - DB commit failures: Return error, undo exists but not referenced
 * - Corruption detected: Return error, no state mutation
 *
 * THREAD SAFETY:
 * - NOT THREAD SAFE
 * - Caller must ensure single-threaded access
 * - No concurrent ConnectBlock/DisconnectBlock operations
 * - Lock order (if multi-threaded in future):
 *   1. cs_main (chainstate lock)
 *   2. utxo_view lock (internal)
 *   3. block_index_db lock (internal)
 */
BlockConnectionResult ConnectBlock(
    const Block& block,
    uint32_t height,
    IUTXOView& utxo_view,
    IBlockIndexDB& block_index_db,
    IUndoStorage& undo_storage,
    const ConsensusParams& params
);

//=============================================================================
// DisconnectBlock: Revert Block Using Undo Data
//=============================================================================

/**
 * DisconnectBlock - Revert a connected block using undo data
 *
 * This is the AUTHORITATIVE reorg function.
 * It restores UTXO state using undo data (does NOT re-execute block).
 *
 * PRECONDITIONS (Caller MUST ensure):
 * ✅ Block is fully connected (BLOCK_CONNECTED flag set)
 * ✅ No descendant blocks are connected (would violate tree invariant)
 * ✅ Undo data exists (BLOCK_HAVE_UNDO flag set)
 * ✅ Undo mapping is valid (file_id, offset, length, checksum)
 * ✅ UTXO view contains block's state (ready to revert)
 * ✅ No concurrent ConnectBlock/DisconnectBlock operations (single-threaded)
 * ❌ MUST NOT be called if block is already disconnected (not idempotent)
 *
 * POSTCONDITIONS (If success):
 * ✅ All transaction outputs removed from UTXO set (except coinbase if spent)
 * ✅ All transaction inputs restored to UTXO set (from undo data)
 * ✅ UTXO + metadata committed to RocksDB (atomic)
 * ✅ Block status updated to BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO (not CONNECTED)
 * ✅ Block index metadata updated (chainwork unchanged, forward links cleared)
 * ✅ Chainstate metadata updated (best block = parent)
 * ✅ Undo data preserved (reusable for future disconnects)
 * ✅ Observable state is all-or-nothing (crash-safe)
 *
 * POSTCONDITIONS (If failure):
 * ✅ NO UTXO mutations (rolled back)
 * ✅ NO block index updates (rolled back)
 * ✅ NO chainstate metadata updates (rolled back)
 * ✅ Observable state is unchanged (crash-safe)
 * ✅ Undo data preserved (not consumed)
 *
 * UNDO DATA PROPERTIES:
 * - REUSABLE: DisconnectBlock does NOT consume undo data
 * - CHECKSUMMED: Undo checksum validated before use (detects corruption)
 * - RETAINED: Undo must exist for MIN_BLOCKS_TO_KEEP (288 blocks)
 * - FORMAT: Matches Bitcoin Core undo format (scriptPubKey + value + height)
 * - NON-IDEMPOTENT: DisconnectBlock is NOT idempotent (calling twice is precondition violation)
 *
 * CRASH SAFETY:
 *
 * Crash Point 1: Before undo load
 *   → Recovery: Block still connected, restart from current state
 *
 * Crash Point 2: During undo load (I/O error)
 *   → Recovery: Block still connected, undo intact
 *
 * Crash Point 3: After undo load, before DB commit
 *   → Recovery: Block still connected, undo intact
 *
 * Crash Point 4: During DB commit (RocksDB handles atomicity)
 *   → Recovery: Either fully disconnected or fully connected
 *
 * Crash Point 5: After DB commit
 *   → Recovery: Block disconnected, restart from parent
 *
 * OBSERVABLE STATE INVARIANT:
 *   Either: Block fully disconnected (parent is best, UTXO restored)
 *   Or: Block still connected (current state unchanged)
 *   Never: Partial state (some UTXOs restored but not all)
 *
 * REORG SCENARIO (100-block reorg):
 * 1. DisconnectBlock(block_100) → UTXO at height 99
 * 2. DisconnectBlock(block_99)  → UTXO at height 98
 * ... (undo data reused 100 times)
 * 100. DisconnectBlock(block_1) → UTXO at genesis
 *
 * PERFORMANCE CHARACTERISTICS:
 * - Average block: 1-5 ms (modern SSD, RocksDB WAL enabled)
 * - Large block (10k txs): 20-100 ms
 * - Undo read: ~1 MB/block (dominated by scriptPubKey)
 * - RocksDB batch: ~1 MB/block (dominated by UTXO entries)
 *
 * BITCOIN CORE EQUIVALENCE:
 * - This function is equivalent to Bitcoin Core's DisconnectBlock()
 * - Matches v0.16+ semantics (non-consuming undo)
 * - Matches undo data format (reusable, checksummed)
 * - Matches crash recovery semantics (all-or-nothing)
 *
 * @param block              The connected block to disconnect
 * @param height             Block height (for validation)
 * @param utxo_view          Mutable UTXO set (will be updated)
 * @param block_index_db     Block index persistence interface
 * @param undo_storage       Undo data persistence interface (rev*.dat)
 * @param undo_file_id       Undo file ID (from BlockConnectionResult)
 * @param undo_file_offset   Undo file offset (from BlockConnectionResult)
 * @param undo_length        Undo data length (from BlockConnectionResult)
 * @param undo_checksum      Undo checksum (from BlockConnectionResult)
 *
 * @return BlockDisconnectionResult (ok or error)
 *
 * USAGE EXAMPLE (Reorg):
 *
 *   // Step 1: Disconnect current tip
 *   auto disconnect_result = DisconnectBlock(current_tip, height, utxo_view,
 *                                             block_index_db, undo_storage,
 *                                             undo_file_id, undo_offset,
 *                                             undo_length, undo_checksum);
 *   if (!disconnect_result.ok) {
 *       return disconnect_result;  // State unchanged
 *   }
 *
 *   // Step 2: Connect new tip
 *   auto connect_result = ConnectBlock(new_tip, height, utxo_view,
 *                                       block_index_db, undo_storage, params);
 *   if (!connect_result.ok) {
 *       // CRITICAL: Must re-connect old tip or enter safe mode
 *       return connect_result;
 *   }
 *
 * ERROR HANDLING:
 * - Undo data missing: Return error, no state mutation
 * - Undo checksum mismatch: Return error, no state mutation (corruption detected)
 * - DB commit failures: Return error, no state mutation
 * - Corruption detected: Return error, no state mutation
 *
 * THREAD SAFETY:
 * - NOT THREAD SAFE
 * - Caller must ensure single-threaded access
 * - No concurrent ConnectBlock/DisconnectBlock operations
 */
BlockDisconnectionResult DisconnectBlock(
    const Block& block,
    uint32_t height,
    IUTXOView& utxo_view,
    IBlockIndexDB& block_index_db,
    IUndoStorage& undo_storage,
    uint32_t undo_file_id,
    uint64_t undo_file_offset,
    uint64_t undo_length,
    uint32_t undo_checksum
);

} // namespace p2p
} // namespace dinero
