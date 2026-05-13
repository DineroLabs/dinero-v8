#pragma once

// SPDX-License-Identifier: MIT
// Phase D.1.b: Explicit Transaction Validation Rules
// 🔒 CONSENSUS FROZEN: See include/consensus/freeze.h for version control
//
// ⚠️ CRITICAL FIX (Phase D.1.b): This file had MAX_MONEY = 21M (Bitcoin's supply)
//                                  Now corrected to 265.428M DIN (Dinero's actual supply)
//
// Phase D Ground Rules (In Effect):
// - ❌ NO new features
// - ❌ NO refactors for "cleanliness"
// - ❌ NO performance changes
// - ✅ ONLY: document, isolate, test consensus
//
// This file defines EXPLICIT transaction validation rules that were previously
// IMPLICIT in ConnectBlock(). Making these rules explicit prevents consensus drift.

#include "consensus/utxo_entry.h"
#include "primitives/transaction.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace dinero {
namespace consensus {

// Forward declaration - avoid pulling in RocksDB headers
class CoinsViewCache;

// ============================================================================
// Phase 23.1: Transaction Validation Results
// ============================================================================

/**
 * Transaction validation result codes
 *
 * These match Bitcoin Core's validation taxonomy and provide detailed
 * error reporting for mempool rejection, block validation, and reorg handling.
 */
enum class TxValidationResult {
    // Success
    OK = 0,

    // Input validation failures
    INPUT_NOT_FOUND,              // Input UTXO doesn't exist
    COINBASE_MATURITY_VIOLATION,  // Spending immature coinbase (< 100 confirmations)
    DUPLICATE_INPUTS,             // Same input spent twice in this transaction

    // Output validation failures
    INVALID_OUTPUT_VALUE,         // Negative or zero output, or exceeds MAX_MONEY
    OUTPUT_SUM_OVERFLOW,          // Sum of outputs overflows uint64_t

    // Value balance failures
    INSUFFICIENT_INPUT_VALUE,     // Total outputs > total inputs (no fee coverage)

    // Script validation (Phase 24 - stubbed for now)
    SCRIPT_VERIFY_FAILED,         // Script execution failed (P2PKH, P2SH, SegWit, Taproot)

    // Sequence lock failures (BIP 68)
    SEQUENCE_LOCK_FAIL,           // Relative locktime not satisfied

    // Transaction structure failures
    TX_EMPTY,                     // No inputs or no outputs (except coinbase)
    TX_OVERSIZED,                 // Transaction exceeds max size
    TX_MALFORMED,                 // Invalid encoding or structure

    // Coinbase-specific failures
    COINBASE_INVALID_SCRIPTSIG,   // Coinbase scriptSig < 2 or > 100 bytes
    COINBASE_INVALID_INPUT,       // Coinbase must have exactly 1 input with null outpoint
    COINBASE_INVALID_WITNESS,     // Coinbase must have exactly 1 witness item of 8 bytes (Utreexo)
    NON_COINBASE_HAS_NULL_INPUT,  // Non-coinbase tx has null input (0x00...00:0xFFFFFFFF)

    // Internal errors
    INTERNAL_ERROR                // Database error or unexpected condition
};

/**
 * Convert validation result to human-readable string
 */
inline const char* TxValidationResultToString(TxValidationResult result) {
    switch (result) {
        case TxValidationResult::OK: return "OK";
        case TxValidationResult::INPUT_NOT_FOUND: return "INPUT_NOT_FOUND";
        case TxValidationResult::COINBASE_MATURITY_VIOLATION: return "COINBASE_MATURITY_VIOLATION";
        case TxValidationResult::DUPLICATE_INPUTS: return "DUPLICATE_INPUTS";
        case TxValidationResult::INVALID_OUTPUT_VALUE: return "INVALID_OUTPUT_VALUE";
        case TxValidationResult::OUTPUT_SUM_OVERFLOW: return "OUTPUT_SUM_OVERFLOW";
        case TxValidationResult::INSUFFICIENT_INPUT_VALUE: return "INSUFFICIENT_INPUT_VALUE";
        case TxValidationResult::SCRIPT_VERIFY_FAILED: return "SCRIPT_VERIFY_FAILED";
        case TxValidationResult::SEQUENCE_LOCK_FAIL: return "SEQUENCE_LOCK_FAIL";
        case TxValidationResult::TX_EMPTY: return "TX_EMPTY";
        case TxValidationResult::TX_OVERSIZED: return "TX_OVERSIZED";
        case TxValidationResult::TX_MALFORMED: return "TX_MALFORMED";
        case TxValidationResult::COINBASE_INVALID_SCRIPTSIG: return "COINBASE_INVALID_SCRIPTSIG";
        case TxValidationResult::COINBASE_INVALID_INPUT: return "COINBASE_INVALID_INPUT";
        case TxValidationResult::COINBASE_INVALID_WITNESS: return "COINBASE_INVALID_WITNESS";
        case TxValidationResult::NON_COINBASE_HAS_NULL_INPUT: return "NON_COINBASE_HAS_NULL_INPUT";
        case TxValidationResult::INTERNAL_ERROR: return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

// ============================================================================
// Consensus Constants (Phase D.1.b: EXPLICIT LOCK)
// ============================================================================

/**
 * @brief Maximum money supply (265,428,000 DIN = 26,542,800,000,000,000 una)
 *
 * This is the absolute maximum value that can exist in the Dinero system.
 * Any value exceeding this is INVALID (consensus violation).
 *
 * Value: 26,542,800,000,000,000 una (sanity bound)
 *
 * Dinero supply schedule (Fair Launch v3 — no premine):
 * - Genesis: 100 DIN burned via OP_RETURN (unspendable)
 * - PoW emission: 100 DIN initial, halving every 1,314,000 blocks
 * - Tail emission: 1 DIN/block floor forever (no hard cap)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint64_t MAX_MONEY = 26542800000000000ULL;  // 265,428,000 DIN

/**
 * Validate that an amount is in the valid range (0, MAX_MONEY]
 *
 * @param amount  Amount to validate (in una)
 * @return        true if amount is valid (> 0 and <= MAX_MONEY)
 */
constexpr bool IsValidAmount(uint64_t amount) {
    return amount > 0 && amount <= MAX_MONEY;
}

/**
 * Validate that an amount is in the valid range [0, MAX_MONEY]
 *
 * Same as IsValidAmount() but also accepts 0
 *
 * @param amount  Amount to validate (in una)
 * @return        true if amount is valid (>= 0 and <= MAX_MONEY)
 */
constexpr bool IsValidAmountOrZero(uint64_t amount) {
    return amount <= MAX_MONEY;
}

// Coinbase maturity (100 confirmations)
// Already implemented in UTXOEntry::isMature(), but defined here for clarity
constexpr uint32_t COINBASE_MATURITY = 100;

// Maximum transaction size in bytes (100KB, matches consensus/limits.h)
// Note: Bitcoin Core's MAX_STANDARD_TX_WEIGHT = 400000 is in weight units, not bytes.
constexpr size_t MAX_TX_SIZE = 100000;

// ============================================================================
// Validation Context
// ============================================================================

/**
 * MTP lookup function type for BIP68 time-based sequence locks
 *
 * Returns the Median Time Past for the block at the given height.
 * Returns std::nullopt if the height is invalid or lookup fails.
 *
 * BIP68 time-based locks need the MTP of the block that confirmed the UTXO
 * to enforce: current_mtp >= utxo_mtp + (relative_locktime * 512)
 */
using MtpLookupFn = std::function<std::optional<uint64_t>(uint32_t height)>;

/**
 * Context for transaction validation
 * Contains block height, median time past (MTP), and other consensus parameters
 */
struct TxValidationContext {
    uint32_t block_height;        // Current block height (for coinbase maturity)
    uint64_t median_time_past;    // Median time of last 11 blocks (for sequence locks)
    bool check_sequence_locks;    // Enable BIP 68 sequence lock validation

    // F.10.9: AssumeValid optimization (IBD performance)
    // When true, skip script/signature verification (PoW + structure still validated)
    // Safe because: (1) minimum chainwork ensures we're on real chain, (2) only during IBD
    bool skip_script_verification;

    // BIP68: MTP lookup for time-based relative locks
    // If provided, enables time-based sequence locks (bit 22 set)
    // If nullptr, time-based locks are rejected (fail-closed)
    MtpLookupFn mtp_at_height;

    TxValidationContext()
        : block_height(0)
        , median_time_past(0)
        , check_sequence_locks(true)
        , skip_script_verification(false)
        , mtp_at_height(nullptr)
    {}

    TxValidationContext(uint32_t height, uint64_t mtp)
        : block_height(height)
        , median_time_past(mtp)
        , check_sequence_locks(true)
        , skip_script_verification(false)
        , mtp_at_height(nullptr)
    {}

    // Constructor with MTP lookup for full BIP68 support
    TxValidationContext(uint32_t height, uint64_t mtp, MtpLookupFn mtp_lookup)
        : block_height(height)
        , median_time_past(mtp)
        , check_sequence_locks(true)
        , skip_script_verification(false)
        , mtp_at_height(std::move(mtp_lookup))
    {}
};

/**
 * Transaction validation output
 * Contains validation result, fee, and detailed error information
 */
struct TxValidationOutput {
    TxValidationResult result;
    uint64_t fee;                 // Transaction fee (value_in - value_out)
    std::string error_message;    // Human-readable error description

    TxValidationOutput()
        : result(TxValidationResult::OK)
        , fee(0)
    {}

    TxValidationOutput(TxValidationResult r, const std::string& msg = "")
        : result(r)
        , fee(0)
        , error_message(msg)
    {}

    bool ok() const { return result == TxValidationResult::OK; }
};

// ============================================================================
// Core Validation Functions
// ============================================================================

/**
 * Phase 23.1.A-F: Validate transaction against UTXO set
 *
 * This is the heart of consensus validation. Performs:
 * 1. Input validation (UTXO existence, coinbase maturity)
 * 2. Output validation (amount checks, script encoding)
 * 3. Value balance (inputs ≥ outputs)
 * 4. Duplicate input detection
 * 5. Script verification (Phase 24 - stubbed for now)
 * 6. Sequence lock validation (BIP 68 - stubbed for now)
 *
 * @param tx             Transaction to validate
 * @param view           UTXO view (CoinsViewCache for block validation)
 * @param ctx            Validation context (block height, MTP)
 * @param is_coinbase    True if this is a coinbase transaction
 * @return               Validation output (result, fee, error message)
 */
TxValidationOutput validateTransaction(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx,
    bool is_coinbase = false
);

/**
 * Validate coinbase transaction
 *
 * Coinbase validation rules:
 * 1. Exactly 1 input with null outpoint (0x00...00:0xFFFFFFFF)
 * 2. scriptSig size between 2 and 100 bytes
 * 3. All outputs valid (amount checks)
 *
 * @param tx             Coinbase transaction
 * @param ctx            Validation context
 * @return               Validation output
 */
TxValidationOutput validateCoinbase(
    const Transaction& tx,
    const TxValidationContext& ctx
);

/**
 * Check if transaction is a coinbase
 *
 * @param tx             Transaction to check
 * @return               True if coinbase (1 input with null outpoint)
 */
bool isCoinbase(const Transaction& tx);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Check for duplicate inputs within a transaction
 *
 * @param tx             Transaction to check
 * @return               True if duplicates found
 */
bool hasDuplicateInputs(const Transaction& tx);

/**
 * Validate transaction output amounts
 *
 * Checks:
 * 1. Each output amount > 0 and ≤ MAX_MONEY
 * 2. Sum of outputs ≤ MAX_MONEY
 * 3. No overflow in sum
 *
 * Phase M.6.3: Uses checked arithmetic with AmountUna
 *
 * @param tx             Transaction to validate
 * @param total_out      Output: Total output value (Phase M.6.3: AmountUna for type safety)
 * @return               OK or error code
 */
TxValidationResult validateOutputs(const Transaction& tx, AmountUna& total_out);

/**
 * Validate transaction inputs against UTXO set
 *
 * Checks:
 * 1. All inputs exist in UTXO set
 * 2. Coinbase maturity (100 blocks)
 * 3. Script verification (Phase 24 - stubbed)
 *
 * Phase M.6.3: Uses checked arithmetic with AmountUna
 *
 * @param tx             Transaction to validate
 * @param view           UTXO view
 * @param ctx            Validation context
 * @param total_in       Output: Total input value (Phase M.6.3: AmountUna for type safety)
 * @return               OK or error code
 */
TxValidationResult validateInputs(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx,
    AmountUna& total_in
);

/**
 * Check sequence locks (BIP 68 relative locktime)
 *
 * Phase 23.3: Full BIP68 implementation for height-based locks.
 *
 * Validates that all inputs satisfy their relative locktime constraints:
 * - If nSequence bit 31 is set, locktime is disabled for that input
 * - If bit 22 is clear: height-based lock (blocks since UTXO confirmed)
 * - If bit 22 is set: time-based lock (currently unsupported, returns false)
 *
 * @param tx             Transaction to check
 * @param view           UTXO view (to look up UTXO confirmation heights)
 * @param ctx            Validation context (current block height, MTP)
 * @return               True if all sequence locks satisfied, false otherwise
 */
bool checkSequenceLocks(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx
);

/**
 * Verify script execution (P2PKH, P2SH, SegWit, Taproot)
 *
 * Phase 24 - STUB for now
 *
 * @param scriptSig      Unlocking script
 * @param scriptPubKey   Locking script
 * @param witness        Witness data (SegWit/Taproot)
 * @param tx             Transaction being validated
 * @param input_index    Input index being validated
 * @param amount         Input amount (for SegWit signature hash)
 * @return               True if script verification succeeds
 */
bool verifyScript(
    const std::vector<uint8_t>& scriptSig,
    const std::vector<uint8_t>& scriptPubKey,
    const std::vector<std::vector<uint8_t>>& witness,
    const Transaction& tx,
    uint32_t input_index,
    uint64_t amount,
    const std::vector<uint64_t>& all_input_amounts,
    const std::vector<std::vector<uint8_t>>& all_input_scriptpubkeys,
    const std::vector<uint8_t>& all_input_confidential_flags,
    const std::vector<std::vector<uint8_t>>& all_input_commitments
);

// ============================================================================
// Compile-Time Assertions (Phase D.1.b: Consensus Invariants)
// ============================================================================

// 🔒 CRITICAL: Verify MAX_MONEY matches Dinero supply (NOT Bitcoin's!)
static_assert(MAX_MONEY == 26542800000000000ULL,
    "🔒 CONSENSUS VIOLATION: MAX_MONEY changed! Must be 265,428,000 DIN");

// Verify MAX_MONEY fits in uint64_t without overflow
static_assert(MAX_MONEY < UINT64_MAX,
    "🔒 CONSENSUS VIOLATION: MAX_MONEY exceeds uint64_t range");

// Verify COINBASE_MATURITY is locked at 100 blocks
static_assert(COINBASE_MATURITY == 100,
    "🔒 CONSENSUS VIOLATION: COINBASE_MATURITY changed! Must be 100 blocks");

// Cross-check: must match consensus/limits.h
static_assert(MAX_TX_SIZE == 100000,
    "🔒 CONSENSUS VIOLATION: MAX_TX_SIZE must be 100,000 bytes (consensus/limits.h)");

} // namespace consensus
} // namespace dinero
