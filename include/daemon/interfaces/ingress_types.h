#pragma once

#include "primitives/uint256.h"
#include <string>
#include <cstdint>

namespace dinero {

// ============================================================================
// Transaction Ingress Types (Step 5)
// ============================================================================

/**
 * Machine-readable transaction rejection codes.
 * Maps to Bitcoin Core reject codes where applicable.
 */
enum class TxRejectCode {
    OK = 0,                      // Transaction accepted
    ALREADY_IN_MEMPOOL,          // Duplicate transaction
    ALREADY_IN_CHAIN,            // Transaction already confirmed
    INVALID_TX,                  // Failed consensus validation
    INSUFFICIENT_FEE,            // Fee rate below minimum
    DOUBLE_SPEND_NO_RBF,         // Conflicts without RBF signal
    RBF_REJECTED,                // RBF replacement failed rules
    TOO_MANY_ANCESTORS,          // Exceeds ancestor limit (25)
    ANCESTOR_SIZE_EXCEEDED,      // Exceeds ancestor size limit (101KB)
    TOO_MANY_DESCENDANTS,        // Would exceed descendant limit
    DESCENDANT_SIZE_EXCEEDED,    // Would exceed descendant size limit
    MEMPOOL_FULL,                // Mempool at capacity, tx not high enough priority
    MISSING_INPUTS,              // Referenced UTXOs not found
    SCRIPT_VERIFY_FAILED,        // Script/signature validation failed
    LOCKTIME_NOT_SATISFIED,      // Transaction not yet valid (timelock)
};

/**
 * Convert TxRejectCode to Bitcoin Core compatible string code.
 */
const char* TxRejectCodeToString(TxRejectCode code);

/**
 * Structured transaction acceptance result.
 * Provides machine-readable status and human explanation.
 */
struct TxAcceptResult {
    TxRejectCode code;           // Machine-readable result
    std::string message;         // Human-readable explanation
    uint256 txid;                // Transaction ID (for logging/tracking)

    bool accepted() const { return code == TxRejectCode::OK; }
    bool rejected() const { return code != TxRejectCode::OK; }

    static TxAcceptResult Accepted(const uint256& id) {
        return {TxRejectCode::OK, "Transaction accepted", id};
    }

    static TxAcceptResult Rejected(TxRejectCode code, const std::string& msg, const uint256& id = uint256()) {
        return {code, msg, id};
    }
};

// ============================================================================
// Block Ingress Types (Step 5)
// ============================================================================

/**
 * Machine-readable block rejection codes.
 * Maps to Bitcoin Core reject codes where applicable.
 */
enum class BlockRejectCode {
    OK = 0,                      // Block accepted
    INVALID_HEADER,              // Header validation failed (bad-header)
    INVALID_POW,                 // Proof of work check failed (high-hash)
    INVALID_MERKLE_ROOT,         // Merkle root mismatch (bad-txnmrklroot)
    INVALID_TIMESTAMP,           // Timestamp out of range (time-too-old, time-too-new)
    INVALID_COINBASE,            // Coinbase validation failed (bad-cb-amount, bad-cb-height)
    INVALID_TRANSACTION,         // Transaction validation failed (bad-txns-*)
    MISSING_PARENT,              // Parent block not found (bad-prevblk)
    INVALID_PARENT_LINK,         // Parent link validation failed (bad-chain)
    DUPLICATE,                   // Block already known (duplicate)

    // NON-TERMINAL. Another thread is mid-acceptance of this same hash; this
    // caller lost a benign race and the outcome is genuinely UNKNOWN.
    //
    // Deliberately NOT DUPLICATE, which asserts the body is already known --
    // here the winner may be performing the very first write, or may fail and
    // never write it at all.
    //
    // No terminal code can serve this. The two consumers wanted OPPOSITE wrong
    // things: the drain read DUPLICATE as success and advanced local_tip_height_
    // on a block that might never connect, while the validation queue read it
    // as failure and recorded RecordBlockFailure against an honest peer. That
    // they disagreed is the proof the outcome is neither.
    //
    // Contract for consumers: not success, not peer misconduct, no acceptance
    // notification. Leave the entry retryable and let the winner's resolution
    // supply the real terminal answer on a later pass.
    CONCURRENT_IN_FLIGHT,

    // NON-TERMINAL. The active tip moved between classifying this block and
    // acting on that classification, so the classification -- and anything
    // derived from it -- describes a chain state that has been left behind.
    //
    // Distinct from CONCURRENT_IN_FLIGHT: nothing else is processing this
    // hash, the world simply changed underneath. Same contract though: not
    // success, not the peer's fault, no acceptance notification. Retry and the
    // block is reclassified against the tip that is current then.
    //
    // Emitting a terminal rejection here would FALSELY reject an honest block:
    // the Utreexo root is computed against the live consensus_utxo_set_, so a
    // moved tip yields a mismatch that says nothing about the block.
    STALE_TIP_CLASSIFICATION,
    CHECKPOINT_VIOLATION,        // Violates checkpoint (checkpoint-mismatch)
    INVALID_UTREEXO_ROOT,        // Utreexo root verification failed (bad-utreexo-root)
    SIGOPS_LIMIT_EXCEEDED,       // Sigops limit exceeded (bad-blk-sigops)
    CONNECT_FAILED,              // Failed to connect block (db-error)
    PARSE_ERROR,                 // Failed to parse block data (bad-blk-length)

    // Phase B1: Template Staleness Rejection Codes (Mainnet Hardening)
    // These codes ensure miners cannot submit blocks built from obsolete state
    STALE_TIP_CHANGED,           // Tip changed since template creation (stale-tip)
    STALE_MEMPOOL_CHANGED,       // Mempool changed since template creation (stale-mempool)
    STALE_REORG,                 // Reorg invalidated the template (stale-reorg)
    STALE_TIMESTAMP,             // Template timestamp beyond acceptable drift (stale-time)
};

/**
 * Convert BlockRejectCode to Bitcoin Core compatible string code.
 */
const char* BlockRejectCodeToString(BlockRejectCode code);

/**
 * Structured block acceptance result.
 * Provides machine-readable status, human explanation, and chain state.
 */
struct BlockAcceptResult {
    BlockRejectCode code;        // Machine-readable result
    std::string reason;          // Human-readable explanation
    uint256 block_hash;          // Block hash
    uint64_t height;             // Block height (0 if rejected before connect)
    bool connected;              // True if block was connected to chain
    bool relayed;                // True if block was relayed to peers

    bool accepted() const { return code == BlockRejectCode::OK; }
    bool rejected() const { return code != BlockRejectCode::OK; }

    static BlockAcceptResult Accepted(const uint256& hash, uint64_t h, bool relay = true) {
        return {BlockRejectCode::OK, "Block accepted", hash, h, true, relay};
    }

    static BlockAcceptResult Rejected(BlockRejectCode c, const std::string& msg,
                                      const uint256& hash = uint256(), uint64_t h = 0) {
        return {c, msg, hash, h, false, false};
    }
};

} // namespace dinero
