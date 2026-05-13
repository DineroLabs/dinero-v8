#pragma once

#include <cstdint>
#include <vector>
#include <string>

// Forward declarations
namespace dinero {
    struct Transaction;  // From wallet/transaction.h
    struct TxInput;
    struct TxOutput;
    struct Block;        // From primitives/block.h
} // namespace dinero

namespace dinero {
namespace consensus {

/**
 * @file sigops.h
 * @brief Signature operation counting and validation
 *
 * This module implements Bitcoin-compatible signature operation (sigops) counting
 * to prevent DoS attacks via blocks with excessive signature validation operations.
 *
 * Consensus-critical constants:
 * - MAX_BLOCK_SIGOPS_COST: 80,000 (maximum sigops per block)
 * - WITNESS_SCALE_FACTOR: 4 (witness data weight scaling)
 *
 * Mainnet Blocker Fix: This enforces the missing MAX_BLOCK_SIGOPS limit
 * identified in MAINNET_BLOCKERS.md Section 1.1
 */

// ═══════════════════════════════════════════════════════════════════════════
// Consensus Constants
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Maximum signature operations cost per block
 * Bitcoin-compatible value: 80,000 sigops
 */
static constexpr unsigned int MAX_BLOCK_SIGOPS_COST = 80000;

/**
 * Witness scale factor for sigop cost calculation
 * Witness sigops are counted with 4x scaling factor
 */
static constexpr unsigned int WITNESS_SCALE_FACTOR = 4;

/**
 * Maximum sigops in a single transaction (mempool policy)
 * More restrictive than block limit for DoS protection
 */
static constexpr unsigned int MAX_TX_SIGOPS_COST = 16000;

/**
 * Maximum multisig public keys per script (consensus rule)
 */
static constexpr unsigned int MAX_PUBKEYS_PER_MULTISIG = 20;

// ═══════════════════════════════════════════════════════════════════════════
// Script Opcodes (for sigop counting)
// ═══════════════════════════════════════════════════════════════════════════

enum ScriptOpcode : uint8_t {
    OP_0 = 0x00,
    OP_1 = 0x51,
    OP_CHECKSIG = 0xac,
    OP_CHECKSIGVERIFY = 0xad,
    OP_CHECKMULTISIG = 0xae,
    OP_CHECKMULTISIGVERIFY = 0xaf,
};

// ═══════════════════════════════════════════════════════════════════════════
// Legacy Sigop Counting
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Count signature operations in a script (non-witness)
 *
 * Counts:
 * - OP_CHECKSIG / OP_CHECKSIGVERIFY: 1 sigop each
 * - OP_CHECKMULTISIG / OP_CHECKMULTISIGVERIFY: 20 sigops (worst case)
 *
 * @param script Script bytes to analyze
 * @param accurate If true, parse multisig n value; if false, use worst case (20)
 * @return Number of signature operations
 */
unsigned int GetLegacySigOpCount(const std::vector<uint8_t>& script, bool accurate = false);

/**
 * Overload for string scripts (hex-encoded)
 */
unsigned int GetLegacySigOpCount(const std::string& script_hex, bool accurate = false);

// ═══════════════════════════════════════════════════════════════════════════
// Witness Sigop Counting
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Count signature operations in witness data
 *
 * For witness transactions (SegWit):
 * - P2WPKH: 1 sigop
 * - P2WSH: Count sigops in witness script
 * - P2TR: 0 sigops (Taproot uses different validation)
 *
 * @param tx Transaction to analyze
 * @param utxo_scripts Previous output scripts (scriptPubKeys) for each input
 * @return Witness signature operation count (scaled)
 */
unsigned int GetWitnessSigOpCost(const dinero::Transaction& tx,
                                  const std::vector<std::vector<uint8_t>>& utxo_scripts);

/**
 * Check if script is P2WPKH (witness v0 pubkey hash)
 * Format: OP_0 <20 bytes>
 */
bool IsP2WPKH(const std::vector<uint8_t>& script);

/**
 * Check if script is P2WSH (witness v0 script hash)
 * Format: OP_0 <32 bytes>
 */
bool IsP2WSH(const std::vector<uint8_t>& script);

/**
 * Check if script is P2TR (witness v1 taproot)
 * Format: OP_1 <32 bytes>
 */
bool IsP2TR(const std::vector<uint8_t>& script);

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Sigop Cost
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Calculate total signature operation cost for a transaction
 *
 * Combines:
 * - Legacy sigops (from inputs and outputs)
 * - Witness sigops (scaled by WITNESS_SCALE_FACTOR)
 *
 * @param tx Transaction to analyze
 * @param utxo_scripts Previous output scripts for inputs (empty = count legacy only)
 * @return Total sigop cost
 */
unsigned int GetTransactionSigOpCost(const dinero::Transaction& tx,
                                      const std::vector<std::vector<uint8_t>>& utxo_scripts = {});

// ═══════════════════════════════════════════════════════════════════════════
// Block Sigop Validation
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Validate signature operation count for a block
 *
 * Sums sigop cost across all transactions and checks against MAX_BLOCK_SIGOPS_COST.
 * This is a consensus rule and MUST be enforced for mainnet.
 *
 * @param block Block to validate
 * @param[out] sigop_cost Total sigop cost calculated
 * @param[out] error Error message if validation fails
 * @return true if block sigops are within consensus limit, false otherwise
 */
bool CheckBlockSigops(const dinero::Block& block,
                       unsigned int& sigop_cost,
                       std::string& error);

/**
 * Validate signature operation count for a single transaction (mempool policy)
 *
 * @param tx Transaction to validate
 * @param[out] sigop_cost Total sigop cost calculated
 * @param[out] error Error message if validation fails
 * @return true if tx sigops are within policy limit, false otherwise
 */
bool CheckTransactionSigops(const dinero::Transaction& tx,
                             unsigned int& sigop_cost,
                             std::string& error);

} // namespace consensus
} // namespace dinero
