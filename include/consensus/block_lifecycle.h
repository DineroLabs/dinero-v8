#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include "consensus/block_index.h"

namespace dinero {

/**
 * Extended block status flags (complements BlockStatus from block_index.h)
 *
 * These flags track block data availability and in-flight states,
 * completing the block lifecycle model.
 */
enum BlockDataStatus {
    // Data availability flags (bits 7-10)
    BLOCK_HAVE_DATA         = 128,  // Full block data is stored locally
    BLOCK_HAVE_UNDO         = 256,  // Undo data is stored (for reorg)

    // In-flight tracking flags (not persisted, runtime only)
    BLOCK_IN_FLIGHT         = 512,  // Block requested from peer, awaiting receipt

    // Combined masks
    BLOCK_DATA_MASK         = (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO),
};

// ─────────────────────────────────────────────────────────────────────────────
// Issue #456 — BlockStatus (block_index.h) and BlockDataStatus (above) are two
// separate enums whose values are OR-ed into the SAME uint32_t
// CBlockIndex::status field. Nothing in the language stops them being given
// overlapping bits, and that is exactly what happened: BLOCK_PRUNE_ELIGIBLE and
// BLOCK_IN_FLIGHT were both 512.
//
// It stayed latent only because BLOCK_PRUNE_ELIGIBLE was never written —
// pruning is documented in prune_service.h but not implemented — while
// BLOCK_IN_FLIGHT is actively set (block_lifecycle.cpp:137,157). Implementing
// pruning as documented would have made a prune-eligible block read back as
// "requested from peer, awaiting receipt".
//
// These assertions fail the build if the two enums are ever given overlapping
// bits again. Add a line here whenever either enum gains a value.
// ─────────────────────────────────────────────────────────────────────────────
static_assert((BLOCK_HAVE_DATA & BLOCK_VALID_MASK) == 0,
              "BLOCK_HAVE_DATA overlaps a BlockStatus validation bit");
static_assert((BLOCK_HAVE_UNDO & BLOCK_VALID_MASK) == 0,
              "BLOCK_HAVE_UNDO overlaps a BlockStatus validation bit");
static_assert((BLOCK_IN_FLIGHT & BLOCK_VALID_MASK) == 0,
              "BLOCK_IN_FLIGHT overlaps a BlockStatus validation bit");

// Compared as uint32_t: these are two distinct enum types sharing one status
// field, and comparing the enumerators directly is deprecated in C++20.
static_assert(static_cast<uint32_t>(BLOCK_HAVE_DATA) !=
                  static_cast<uint32_t>(BLOCK_PRUNE_ELIGIBLE),
              "BLOCK_HAVE_DATA collides with BLOCK_PRUNE_ELIGIBLE");
static_assert(static_cast<uint32_t>(BLOCK_HAVE_UNDO) !=
                  static_cast<uint32_t>(BLOCK_PRUNE_ELIGIBLE),
              "BLOCK_HAVE_UNDO collides with BLOCK_PRUNE_ELIGIBLE");
static_assert(static_cast<uint32_t>(BLOCK_IN_FLIGHT) !=
                  static_cast<uint32_t>(BLOCK_PRUNE_ELIGIBLE),
              "BLOCK_IN_FLIGHT collides with BLOCK_PRUNE_ELIGIBLE (#456)");

static_assert((BLOCK_HAVE_DATA & BLOCK_FAILED_VALID) == 0 &&
              (BLOCK_HAVE_UNDO & BLOCK_FAILED_VALID) == 0 &&
              (BLOCK_IN_FLIGHT & BLOCK_FAILED_VALID) == 0,
              "a BlockDataStatus bit collides with BLOCK_FAILED_VALID");
static_assert((BLOCK_HAVE_DATA & BLOCK_FAILED_CHILD) == 0 &&
              (BLOCK_HAVE_UNDO & BLOCK_FAILED_CHILD) == 0 &&
              (BLOCK_IN_FLIGHT & BLOCK_FAILED_CHILD) == 0,
              "a BlockDataStatus bit collides with BLOCK_FAILED_CHILD");

/**
 * Block Lifecycle State Machine
 *
 * This defines the valid state transitions for a block through its lifecycle.
 *
 * State Progression:
 * ┌──────────────────────────────────────────────────────────────────┐
 * │                        BLOCK LIFECYCLE                            │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * 1. HEADER_ONLY (initial state)
 *    ├─> Receive header from peer
 *    ├─> Validate: PoW, timestamp, version
 *    └─> Transition: BLOCK_VALID_HEADER
 *
 * 2. HEADER_VALIDATED (BLOCK_VALID_HEADER set)
 *    ├─> If parent missing: → ORPHANED
 *    ├─> If parent known: Request full block → IN_FLIGHT
 *    └─> Transition: BLOCK_IN_FLIGHT
 *
 * 3. IN_FLIGHT (BLOCK_IN_FLIGHT set)
 *    ├─> Receive full block data
 *    ├─> Store to disk
 *    └─> Transition: BLOCK_STORED
 *
 * 4. BLOCK_STORED (BLOCK_HAVE_DATA set)
 *    ├─> Validate: Merkle tree
 *    ├─> If valid: → BLOCK_VALID_TREE
 *    └─> If invalid: → INVALID (BLOCK_FAILED_VALID)
 *
 * 5. TREE_VALIDATED (BLOCK_VALID_TREE set)
 *    ├─> Validate all transactions (contextual checks)
 *    ├─> If valid: → BLOCK_VALID_TRANSACTIONS
 *    └─> If invalid: → INVALID (BLOCK_FAILED_VALID)
 *
 * 6. TRANSACTIONS_VALIDATED (BLOCK_VALID_TRANSACTIONS set)
 *    ├─> Check if can connect to active chain
 *    ├─> If yes: → CONNECTABLE
 *    └─> If no: → ORPHANED (wait for parent)
 *
 * 7. CONNECTABLE (BLOCK_VALID_CHAIN set)
 *    ├─> Validate all scripts
 *    ├─> If valid: → FULLY_VALIDATED
 *    └─> If invalid: → INVALID (BLOCK_FAILED_VALID)
 *
 * 8. FULLY_VALIDATED (BLOCK_VALID_SCRIPTS set)
 *    ├─> Compare chainwork with active tip
 *    ├─> If more work: → Trigger reorg → CONNECTED
 *    └─> If less work: Stay as candidate tip
 *
 * 9. CONNECTED (active chain)
 *    ├─> Block is part of active chain
 *    ├─> UTXO set updated
 *    └─> Can be disconnected during reorg
 *
 * 10. ORPHANED (parent missing)
 *     ├─> Stored in g_orphan_pool[parent_hash]
 *     ├─> Wait for parent to arrive
 *     ├─> On parent arrival: Replay validation from step 4
 *     └─> Subject to eviction if orphan pool full
 *
 * 11. INVALID (BLOCK_FAILED_VALID set)
 *     ├─> Block failed validation (cached in g_invalid_blocks)
 *     ├─> Reason stored for debugging
 *     ├─> All descendants marked BLOCK_FAILED_CHILD
 *     └─> Never reprocessed (unless invalidated manually)
 */

/**
 * Simplified state queries for block lifecycle
 */
inline bool IsHeaderOnly(uint32_t status) {
    return (status & BLOCK_VALID_MASK) == BLOCK_VALID_HEADER &&
           !(status & BLOCK_HAVE_DATA);
}

inline bool IsStored(uint32_t status) {
    return (status & BLOCK_HAVE_DATA) != 0;
}

inline bool IsOrphaned(const CBlockIndex* pindex) {
    // Block is orphaned if parent is missing or invalid
    return pindex && pindex->pprev == nullptr && pindex->height > 0;
}

inline bool IsConnectable(uint32_t status) {
    return (status & BLOCK_VALID_CHAIN) != 0;
}

inline bool IsFullyValidated(uint32_t status) {
    return (status & BLOCK_VALID_SCRIPTS) != 0;
}

inline bool IsFailed(uint32_t status) {
    return (status & BLOCK_FAILED_VALID) != 0 ||
           (status & BLOCK_FAILED_CHILD) != 0;
}

inline bool IsInFlight(uint32_t status) {
    return (status & BLOCK_IN_FLIGHT) != 0;
}

/**
 * P0 invariant: candidate-set eligibility requires block body + chain validation.
 * This is the SINGLE gate for all AddCandidate paths.
 *
 * A CBlockIndex may enter the candidate set ONLY if:
 *  1. Full block data is stored locally (BLOCK_HAVE_DATA)
 *  2. Block is connected to a valid chain  (BLOCK_VALID_CHAIN)
 *  3. Block has not failed validation
 *
 * Headers-only tips can exist but are NOT eligible for best-chain selection.
 */
inline bool IsEligibleForCandidacy(uint32_t status) {
    if (!(status & BLOCK_HAVE_DATA)) return false;
    if (!(status & BLOCK_VALID_CHAIN)) return false;
    if (status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) return false;
    return true;
}

/**
 * Reason codes for block validation failure
 */
enum class BlockRejectReason {
    UNKNOWN = 0,

    // Header validation failures
    INVALID_POW,              // Proof-of-work check failed
    INVALID_TIMESTAMP,        // Timestamp too far in future/past
    INVALID_VERSION,          // Unknown or unsupported version

    // Tree validation failures
    INVALID_MERKLE_ROOT,      // Merkle root mismatch
    DUPLICATE_TX,             // Duplicate transaction in block
    OVERSIZED_BLOCK,          // Block size exceeds limit

    // Transaction validation failures
    INVALID_TX,               // Transaction failed validation
    BAD_COINBASE,             // Coinbase transaction malformed
    BAD_BLOCK_REWARD,         // Block reward exceeds allowed amount

    // Chain validation failures
    PARENT_MISSING,           // Parent block not found
    PARENT_INVALID,           // Parent block is invalid
    CHECKPOINT_MISMATCH,      // Block conflicts with checkpoint

    // Script validation failures
    SCRIPT_VERIFY_FAILED,     // Script execution failed

    // Consensus failures
    BAD_UTXO_SET,            // UTXO set inconsistency
    BAD_DIFFICULTY,          // Difficulty retarget incorrect

    // Resource limits
    TOO_MANY_SIGOPS,         // Signature operations exceed limit

    // Manual operations
    MANUAL_INVALIDATION,     // Block manually invalidated via RPC

    // Phase 8: Stateless validation failures
    PROOF_MISSING,           // BlockUtreexoData missing post-activation
    PROOF_INVALID,           // Proof failed cryptographic verification
    ROOT_MISMATCH,           // Proof root ≠ header.utreexo_root
    PROOF_OUTPOINT_MISMATCH, // Proof does not match spent outpoint
    ACCUMULATOR_STATE_ERROR, // Accumulator update inconsistent
    PROOF_TOO_LARGE,         // Proof size exceeds MAX_PROOF_SIZE
};

/**
 * @brief Convert BlockRejectReason to string for logging
 */
inline const char* BlockRejectReasonToString(BlockRejectReason reason) {
    switch (reason) {
        case BlockRejectReason::UNKNOWN: return "UNKNOWN";
        case BlockRejectReason::INVALID_POW: return "INVALID_POW";
        case BlockRejectReason::INVALID_TIMESTAMP: return "INVALID_TIMESTAMP";
        case BlockRejectReason::INVALID_VERSION: return "INVALID_VERSION";
        case BlockRejectReason::INVALID_MERKLE_ROOT: return "INVALID_MERKLE_ROOT";
        case BlockRejectReason::DUPLICATE_TX: return "DUPLICATE_TX";
        case BlockRejectReason::OVERSIZED_BLOCK: return "OVERSIZED_BLOCK";
        case BlockRejectReason::INVALID_TX: return "INVALID_TX";
        case BlockRejectReason::BAD_COINBASE: return "BAD_COINBASE";
        case BlockRejectReason::BAD_BLOCK_REWARD: return "BAD_BLOCK_REWARD";
        case BlockRejectReason::PARENT_MISSING: return "PARENT_MISSING";
        case BlockRejectReason::PARENT_INVALID: return "PARENT_INVALID";
        case BlockRejectReason::CHECKPOINT_MISMATCH: return "CHECKPOINT_MISMATCH";
        case BlockRejectReason::SCRIPT_VERIFY_FAILED: return "SCRIPT_VERIFY_FAILED";
        case BlockRejectReason::BAD_UTXO_SET: return "BAD_UTXO_SET";
        case BlockRejectReason::BAD_DIFFICULTY: return "BAD_DIFFICULTY";
        case BlockRejectReason::TOO_MANY_SIGOPS: return "TOO_MANY_SIGOPS";
        case BlockRejectReason::MANUAL_INVALIDATION: return "MANUAL_INVALIDATION";
        case BlockRejectReason::PROOF_MISSING: return "PROOF_MISSING";
        case BlockRejectReason::PROOF_INVALID: return "PROOF_INVALID";
        case BlockRejectReason::ROOT_MISMATCH: return "ROOT_MISMATCH";
        case BlockRejectReason::PROOF_OUTPOINT_MISMATCH: return "PROOF_OUTPOINT_MISMATCH";
        case BlockRejectReason::ACCUMULATOR_STATE_ERROR: return "ACCUMULATOR_STATE_ERROR";
        case BlockRejectReason::PROOF_TOO_LARGE: return "PROOF_TOO_LARGE";
        default: return "UNKNOWN";
    }
}

/**
 * Invalid block cache entry
 */
struct InvalidBlockEntry {
    uint256 block_hash;          // Phase M.0: uint256 for consensus identity
    BlockRejectReason reason;
    std::string reason_string;  // Human-readable explanation
    uint64_t timestamp;          // When block was marked invalid

    InvalidBlockEntry(const uint256& hash,
                     BlockRejectReason r,
                     const std::string& msg)
        : block_hash(hash),
          reason(r),
          reason_string(msg),
          timestamp(std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch()).count()) {}
};

/**
 * In-flight block tracking entry
 */
struct InFlightBlock {
    uint256 block_hash;          // Phase M.0: uint256 for consensus identity
    uint64_t peer_id;            // Peer we requested from
    uint64_t request_time;       // When we requested (Unix timestamp)
    uint64_t timeout_time;       // When request times out
    bool duplicate_request;      // True if we already requested this

    static constexpr uint64_t DEFAULT_TIMEOUT_SECS = 120; // 2 minutes

    InFlightBlock() : peer_id(0), request_time(0), timeout_time(0), duplicate_request(false) {}  // Phase M.0: Default constructor

    InFlightBlock(const uint256& hash, uint64_t peer)
        : block_hash(hash),
          peer_id(peer),
          duplicate_request(false) {
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        request_time = now;
        timeout_time = now + DEFAULT_TIMEOUT_SECS;
        duplicate_request = false;
    }

    bool IsTimedOut() const {
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return now >= timeout_time;
    }
};

/**
 * Global state tracking (extends block_index.h globals)
 */

// Invalid block cache (hash → rejection reason)
extern std::unordered_map<uint256, InvalidBlockEntry> g_invalid_blocks;

// In-flight block tracking (hash → request info)
extern std::unordered_map<uint256, InFlightBlock> g_inflight_blocks;

// Blocks with invalid ancestors (hash → invalid ancestor hash)
extern std::unordered_map<uint256, uint256> g_invalid_descendants;

/**
 * Block lifecycle management functions
 */

// Mark block as invalid and propagate to descendants
void MarkBlockInvalid(CBlockIndex* pindex, BlockRejectReason reason, const std::string& message);
void PropagateInvalidToDescendants(CBlockIndex* pindex);

// Check if block is invalid or has invalid ancestor
bool IsBlockInvalid(const uint256& block_hash);
bool HasInvalidAncestor(const CBlockIndex* pindex);

// #309: true iff every block on the branch from pindex back to the first
// connected (BLOCK_VALID_CHAIN) ancestor has its body (BLOCK_HAVE_DATA). This is
// the whole-branch-data precondition for treating a not-yet-validated side branch
// as a reorg candidate: full per-block validation is deferred to the reorg
// ConnectTip walk, but the walk can only succeed if no body is missing. A partial
// branch (a body gap above the connected base) must NOT be a candidate, else the
// reorg connect-walks into the gap and aborts. Genesis-with-data counts as a base.
bool BranchHasDataToConnectedBase(const CBlockIndex* pindex);

// In-flight tracking
void MarkBlockInFlight(const uint256& block_hash, uint64_t peer_id);
void MarkBlockReceived(const uint256& block_hash);
bool IsBlockInFlightFrom(const uint256& block_hash, uint64_t& peer_id);
void CleanupTimedOutRequests();

// State transition helpers
bool CanProgressToNextState(const CBlockIndex* pindex);
void TransitionBlockState(CBlockIndex* pindex, uint32_t new_status_flags);

} // namespace dinero
