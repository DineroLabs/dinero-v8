#pragma once

// SPDX-License-Identifier: MIT
// Phase D.1.b: Explicit Consensus Limits
// 🔒 CONSENSUS FROZEN: See include/consensus/freeze.h for version control

#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * @brief Consensus Size and Weight Limits (IMMUTABLE)
 *
 * These limits define the maximum sizes for blocks and transactions.
 * Exceeding these limits = invalid block/transaction = consensus rejection.
 *
 * Phase D Ground Rules:
 * - These constants are FROZEN
 * - Any change requires explicit "CONSENSUS CHANGE" approval
 * - Changing these values = network fork
 *
 * ⚠️ CRITICAL: These limits were MISSING from the codebase (Phase D.1.a finding)
 * They are now being made EXPLICIT to prevent consensus drift.
 */

// ============================================================================
// Block Size Limits
// ============================================================================

/**
 * @brief Maximum block size in bytes (base size, pre-SegWit style)
 *
 * This is the maximum serialized size of a block INCLUDING all transactions.
 * Blocks larger than this are INVALID.
 *
 * Value: 1,000,000 bytes (1 MB) - Bitcoin legacy limit
 * Rationale: Conservative limit for initial deployment
 *
 * 🔒 CONSENSUS LOCK: Increasing this = hard fork
 */
constexpr uint32_t MAX_BLOCK_SIZE = 1000000;  // 1 MB

/**
 * @brief Maximum block weight (SegWit-style accounting)
 *
 * Block weight = (base size * 3) + total size
 * This allows witness data to be "discounted" in weight calculation.
 *
 * Value: 4,000,000 weight units (4 MW) - Bitcoin SegWit limit
 * Effective max block size: ~4 MB with witness data
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_BLOCK_WEIGHT = 4000000;  // 4 million weight units

/**
 * @brief Maximum block sigops cost (signature operations)
 *
 * Prevents DoS attacks via excessive signature validation.
 * Defined in consensus/sigops.h as MAX_BLOCK_SIGOPS_COST.
 *
 * Value: 80,000 sigops (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_BLOCK_SIGOPS_COST = 80000;

// ============================================================================
// Transaction Size Limits
// ============================================================================

/**
 * @brief Maximum transaction size in bytes
 *
 * Individual transactions cannot exceed this size.
 * Transactions larger than this are INVALID.
 *
 * Value: 100,000 bytes (100 KB) - Bitcoin standard
 * Rationale: Prevents single transaction from dominating block
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_TX_SIZE = 100000;  // 100 KB

/**
 * @brief Maximum transaction weight (SegWit-style)
 *
 * Value: 400,000 weight units (Bitcoin standard)
 * Effective max tx size: ~400 KB with witness data
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_TX_WEIGHT = 400000;  // 400,000 weight units

/**
 * @brief Maximum transaction sigops cost
 *
 * Limits signature operations per transaction.
 *
 * Value: 16,000 sigops (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_TX_SIGOPS_COST = 16000;

// ============================================================================
// Script Limits
// ============================================================================

/**
 * @brief Maximum script size in bytes
 *
 * Individual scripts (scriptPubKey or scriptSig) cannot exceed this.
 *
 * Value: 10,000 bytes (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_SCRIPT_SIZE = 10000;  // 10 KB

/**
 * @brief Maximum number of opcodes per script
 *
 * Prevents infinite loops and excessive computation.
 *
 * Value: 201 opcodes (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_SCRIPT_OPCODES = 201;

/**
 * @brief Maximum script element size (pushed to stack)
 *
 * Individual elements pushed to the script stack cannot exceed this.
 *
 * Value: 520 bytes (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_SCRIPT_ELEMENT_SIZE = 520;

/**
 * @brief Maximum stack size during script execution
 *
 * The script stack cannot exceed this many elements.
 *
 * Value: 1,000 elements (Bitcoin limit)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_STACK_SIZE = 1000;

// ============================================================================
// Transaction Input/Output Limits
// ============================================================================

/**
 * @brief Maximum number of inputs in a transaction
 *
 * Soft limit to prevent pathological transactions.
 *
 * Value: 10,000 inputs (implied by MAX_TX_SIZE)
 * Note: This is not enforced separately, but limited by MAX_TX_SIZE
 *
 * 🔒 CONSENSUS LOCK: Separate enforcement would be hard fork
 */
constexpr uint32_t MAX_TX_INPUTS = 10000;

/**
 * @brief Maximum number of outputs in a transaction
 *
 * Soft limit to prevent pathological transactions.
 *
 * Value: 10,000 outputs (implied by MAX_TX_SIZE)
 * Note: This is not enforced separately, but limited by MAX_TX_SIZE
 *
 * 🔒 CONSENSUS LOCK: Separate enforcement would be hard fork
 */
constexpr uint32_t MAX_TX_OUTPUTS = 10000;

// ============================================================================
// Minimum Value Limits
// ============================================================================

/**
 * @brief Minimum transaction output value (dust limit)
 *
 * Outputs smaller than this are considered "dust" and may be rejected
 * by policy (not consensus).
 *
 * Value: 546 una (Bitcoin dust limit for P2WPKH)
 * Note: This is POLICY, not consensus (transactions with dust outputs
 *       are valid but may not be relayed)
 *
 * ⚠️ NOT A CONSENSUS RULE - This is relay policy
 */
constexpr uint64_t DUST_RELAY_LIMIT = 546;  // una

// ============================================================================
// Merkle Tree Limits
// ============================================================================

/**
 * @brief Maximum merkle tree depth
 *
 * Prevents merkle tree computation DoS.
 *
 * Value: 32 levels (allows up to 2^32 transactions)
 *
 * 🔒 CONSENSUS LOCK: Changing this = hard fork
 */
constexpr uint32_t MAX_MERKLE_DEPTH = 32;

// ============================================================================
// Validation Functions
// ============================================================================

/**
 * @brief Check if block size is within consensus limits
 *
 * @param block_size Block size in bytes
 * @return true if valid, false if exceeds limit
 */
constexpr bool IsValidBlockSize(uint32_t block_size) {
    return block_size > 0 && block_size <= MAX_BLOCK_SIZE;
}

/**
 * @brief Check if block weight is within consensus limits
 *
 * @param block_weight Block weight in weight units
 * @return true if valid, false if exceeds limit
 */
constexpr bool IsValidBlockWeight(uint32_t block_weight) {
    return block_weight > 0 && block_weight <= MAX_BLOCK_WEIGHT;
}

/**
 * @brief Check if transaction size is within consensus limits
 *
 * @param tx_size Transaction size in bytes
 * @return true if valid, false if exceeds limit
 */
constexpr bool IsValidTxSize(uint32_t tx_size) {
    return tx_size > 0 && tx_size <= MAX_TX_SIZE;
}

/**
 * @brief Check if transaction weight is within consensus limits
 *
 * @param tx_weight Transaction weight in weight units
 * @return true if valid, false if exceeds limit
 */
constexpr bool IsValidTxWeight(uint32_t tx_weight) {
    return tx_weight > 0 && tx_weight <= MAX_TX_WEIGHT;
}

/**
 * @brief Check if script size is within consensus limits
 *
 * @param script_size Script size in bytes
 * @return true if valid, false if exceeds limit
 */
constexpr bool IsValidScriptSize(uint32_t script_size) {
    return script_size <= MAX_SCRIPT_SIZE;
}

// ============================================================================
// Compile-Time Assertions (Sanity Checks)
// ============================================================================

// Verify MAX_TX_SIZE doesn't exceed MAX_BLOCK_SIZE
static_assert(MAX_TX_SIZE <= MAX_BLOCK_SIZE,
    "Maximum transaction size cannot exceed maximum block size");

// Verify MAX_TX_WEIGHT doesn't exceed MAX_BLOCK_WEIGHT
static_assert(MAX_TX_WEIGHT <= MAX_BLOCK_WEIGHT,
    "Maximum transaction weight cannot exceed maximum block weight");

// Verify MAX_TX_SIGOPS doesn't exceed MAX_BLOCK_SIGOPS
static_assert(MAX_TX_SIGOPS_COST <= MAX_BLOCK_SIGOPS_COST,
    "Maximum transaction sigops cannot exceed maximum block sigops");

// Verify script limits are sane
static_assert(MAX_SCRIPT_ELEMENT_SIZE < MAX_SCRIPT_SIZE,
    "Script element size must be less than max script size");

// Verify MAX_BLOCK_SIZE is at least 1 MB
static_assert(MAX_BLOCK_SIZE >= 1000000,
    "Maximum block size must be at least 1 MB");

} // namespace consensus
} // namespace dinero
