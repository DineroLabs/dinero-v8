#pragma once

#include "consensus/outpoint.h"
#include "primitives/uint256.h"
#include "primitives/transaction.h"
#include <unordered_set>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dinero {
namespace mempool {

// ============================================================================
// Phase 25: Mempool (Transaction Pool)
// ============================================================================

/**
 * Mempool entry - A transaction in the mempool with metadata
 *
 * Phase M.0: Migrated to uint256 for transaction identity
 *
 * Tracks all information needed for fee estimation, eviction, and mining:
 * - Transaction data
 * - Fee information (absolute fee, fee rate, ancestor fees)
 * - Dependency tracking (parents, children)
 * - Timing information (when added)
 * - Size metrics (virtual size for SegWit)
 */
struct MempoolEntry {
    // Transaction data
    Transaction tx;
    TxId txid;  // Phase M.4.3-C: Semantic type safety (malleability-proof)

    // Helper: Get txid as hex (RPC boundary only)
    std::string GetTxIdHex() const { return txid.AsUint256().GetHex(); }

    // Fee information
    uint64_t fee;              // Absolute fee in una
    uint64_t base_fee;         // Fee without descendants
    size_t size;               // Transaction size in bytes
    size_t vsize;              // Virtual size (for SegWit weight calculation)
    double fee_rate;           // Fee per vbyte (sat/vB)

    // Ancestor tracking (for CPFP - Child Pays For Parent)
    uint64_t ancestor_fee;     // Total fees of all ancestors + this tx
    size_t ancestor_size;      // Total size of all ancestors + this tx
    size_t ancestor_count;     // Number of ancestor transactions

    // Descendant tracking
    uint64_t descendant_fee;   // Total fees of all descendants + this tx
    size_t descendant_size;    // Total size of all descendants + this tx
    size_t descendant_count;   // Number of descendant transactions

    // Dependency graph (Phase M.4.3-C: Type-safe TxId)
    std::unordered_set<TxId> parents;     // Transactions this depends on (direct parents only)
    std::unordered_set<TxId> children;    // Transactions depending on this (direct children only)

    // F.9.6: Cached ancestor set (computed ONCE at admission)
    // This is the KEY to O(A) complexity - never recompute by traversal
    // Phase M.4.3-C: Type-safe TxId (cannot be WTxId)
    std::vector<TxId> ancestors;          // ALL ancestors (parents, grandparents, etc.) - sorted for determinism

    // Metadata
    uint64_t time_added;       // Timestamp when added to mempool
    uint32_t height;           // Block height when added

    // RBF (Replace-By-Fee)
    bool signals_rbf;          // BIP 125: Signals replacement

    // Phase C.2: Covenant tracking (in-memory only, not persisted)
    bool has_covenant_input;   // Does this tx spend any covenant-locked UTXO?
    uint32_t covenant_count;   // Number of covenant inputs (for policy limits)

    MempoolEntry()
        : fee(0), base_fee(0), size(0), vsize(0), fee_rate(0.0)
        , ancestor_fee(0), ancestor_size(0), ancestor_count(0)
        , descendant_fee(0), descendant_size(0), descendant_count(0)
        , time_added(0), height(0), signals_rbf(false)
        , has_covenant_input(false), covenant_count(0)
    {}
};

/**
 * Mempool acceptance result
 */
enum class MempoolAcceptResult {
    OK = 0,
    ALREADY_IN_MEMPOOL,
    ALREADY_IN_CHAIN,
    INVALID_TX,              // Failed consensus validation
    INSUFFICIENT_FEE,        // Fee too low
    MEMPOOL_FULL,            // Mempool at capacity
    CONFLICTS_WITH_MEMPOOL,  // Double spend without RBF
    RBF_REJECTED,            // RBF attempted but failed rules
    TOO_MANY_ANCESTORS,      // Exceeds ancestor limit
    TOO_MANY_DESCENDANTS,    // Exceeds descendant limit
    MISSING_INPUTS,          // Parent transactions not in mempool or UTXO
    SCRIPT_VERIFY_FAILED,    // Script validation failed
    LOCKTIME_NOT_SATISFIED,  // Transaction not yet valid

    // Phase C.2: Covenant-specific rejections (policy-only, not consensus)
    COVENANT_ANCESTOR_MISSING,   // Covenant parent not confirmed or in mempool
    COVENANT_RBF_FORBIDDEN,      // Cannot RBF covenant transactions (policy)
    TOO_MANY_COVENANT_INPUTS,    // Exceeds covenant input limit (DoS protection)
};

inline const char* MempoolAcceptResultToString(MempoolAcceptResult result) {
    switch (result) {
        case MempoolAcceptResult::OK:
            return "OK";
        case MempoolAcceptResult::ALREADY_IN_MEMPOOL:
            return "Transaction already in mempool";
        case MempoolAcceptResult::ALREADY_IN_CHAIN:
            return "Transaction already in blockchain";
        case MempoolAcceptResult::INVALID_TX:
            return "Transaction failed consensus validation";
        case MempoolAcceptResult::INSUFFICIENT_FEE:
            return "Fee rate too low";
        case MempoolAcceptResult::MEMPOOL_FULL:
            return "Mempool at capacity";
        case MempoolAcceptResult::CONFLICTS_WITH_MEMPOOL:
            return "Double spend without RBF";
        case MempoolAcceptResult::RBF_REJECTED:
            return "RBF attempted but failed rules";
        case MempoolAcceptResult::TOO_MANY_ANCESTORS:
            return "Exceeds ancestor limit";
        case MempoolAcceptResult::TOO_MANY_DESCENDANTS:
            return "Exceeds descendant limit";
        case MempoolAcceptResult::MISSING_INPUTS:
            return "Parent transactions not found";
        case MempoolAcceptResult::SCRIPT_VERIFY_FAILED:
            return "Script validation failed";
        case MempoolAcceptResult::LOCKTIME_NOT_SATISFIED:
            return "Transaction not yet valid";
        case MempoolAcceptResult::COVENANT_ANCESTOR_MISSING:
            return "Covenant parent transaction not confirmed or in mempool";
        case MempoolAcceptResult::COVENANT_RBF_FORBIDDEN:
            return "Replace-by-fee forbidden for covenant transactions (policy)";
        case MempoolAcceptResult::TOO_MANY_COVENANT_INPUTS:
            return "Transaction exceeds maximum covenant input limit (DoS protection)";
        default:
            return "Unknown error";
    }
}

/**
 * Mempool submission mode
 *
 * TEST_ONLY: Skip script/signature validation for policy testing
 * - Used by wallet.sendtoaddress with test_mode=true
 * - Enables mempool policy testing without Phase 34 (signing)
 * - Transactions marked TEST_ONLY are never relayed
 * - Only available in regtest mode
 */
enum class MempoolSubmitMode {
    NORMAL,      // Full validation (requires valid signatures)
    TEST_ONLY    // Skip script validation (policy testing only)
};

/**
 * Mempool configuration
 */
struct MempoolConfig {
    size_t max_size_mb;           // Maximum mempool size (default: 300 MB)
    double min_fee_rate;          // Minimum fee rate (sat/vB) (default: 1.0)
    size_t max_ancestors;         // Max ancestor count (default: 25)
    size_t max_descendants;       // Max descendant count (default: 25)
    size_t max_ancestor_size_kb;  // Max ancestor size (default: 101 KB)
    bool enable_rbf;              // Enable Replace-By-Fee (default: false)
    uint64_t expiry_hours;        // Transaction expiry time (default: 336 = 2 weeks)

    // Phase C.2: Covenant policy limits (DoS protection)
    size_t max_covenant_inputs_per_tx;  // Max covenant inputs in one tx (default: 10)
    bool allow_covenant_rbf;            // Allow RBF for covenant txs (default: false - conservative)

    // Phase E.2.a: Validation scratch space limits (DoS protection)
    size_t max_validation_memory_mb;    // Max memory for single tx validation (default: 50 MB)
    size_t max_script_stack_bytes;      // Max script stack size (default: 10 MB)
    size_t max_signature_cache_mb;      // Max signature verification cache (default: 100 MB)

    MempoolConfig()
        : max_size_mb(300)
        , min_fee_rate(1.0)
        , max_ancestors(25)
        , max_descendants(25)
        , max_ancestor_size_kb(101)
        , enable_rbf(false)
        , expiry_hours(336)
        , max_covenant_inputs_per_tx(10)
        , allow_covenant_rbf(false)
        , max_validation_memory_mb(50)
        , max_script_stack_bytes(10 * 1024 * 1024)
        , max_signature_cache_mb(100)
    {}
};

} // namespace mempool
} // namespace dinero
