/**
 * Phase E.1.a: Startup Consistency Validator
 *
 * PRODUCTION HARDENING: This module ensures the node answers exactly ONE of:
 * - ✅ "Fully consistent — continuing"
 * - 🔄 "Recovering safely"
 * - ❌ "Cannot proceed — operator action required"
 *
 * Philosophy:
 * - Silent corruption is worse than loud failure
 * - Ambiguity causes chain splits
 * - Must check consistency BEFORE accepting blocks
 *
 * Checks performed on every daemon startup:
 * 1. Tip consistency (tip exists, valid chainwork, proper status)
 * 2. Block index integrity (connected chain, valid parent links)
 * 3. Block data availability (files exist, positions valid)
 * 4. UTXO set sanity (if checksum available)
 *
 * Recovery actions:
 * - Corrupt tip → revert to last known good
 * - Corrupt index → offer reindex
 * - Missing block files → fail loudly with guidance
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DINERO_CONSENSUS_STARTUP_VALIDATOR_H
#define DINERO_CONSENSUS_STARTUP_VALIDATOR_H

#include <cstdint>
#include <string>
#include <optional>

namespace dinero {

// Forward declaration
class ChainDB;

namespace consensus {

/**
 * Startup validation result codes
 *
 * These determine what the daemon does after validation:
 * - OK: Continue normal operation
 * - RECOVERED: Continue after safe recovery
 * - NEEDS_REINDEX: User must pass --reindex flag
 * - FATAL: Cannot proceed, operator action required
 */
enum class StartupValidationResult {
    OK = 0,              // ✅ Fully consistent
    RECOVERED,           // 🔄 Recovered safely
    NEEDS_REINDEX,       // ⚠️  Must reindex
    FATAL,               // ❌ Cannot proceed
};

/**
 * Detailed validation status
 *
 * Contains result code + human-readable explanation
 */
struct ValidationStatus {
    StartupValidationResult result;
    std::string message;          // Human-readable explanation
    std::string guidance;         // What operator should do
    uint32_t last_valid_height;   // Last known good block height (0 if unknown)

    /**
     * Check if validation passed (OK or RECOVERED)
     */
    bool isOK() const {
        return result == StartupValidationResult::OK ||
               result == StartupValidationResult::RECOVERED;
    }

    /**
     * Check if operator action required
     */
    bool needsOperatorAction() const {
        return result == StartupValidationResult::NEEDS_REINDEX ||
               result == StartupValidationResult::FATAL;
    }
};

/**
 * Startup Consistency Validator
 *
 * Performs all critical checks on daemon startup.
 * Must be called BEFORE accepting any blocks.
 *
 * Usage:
 *   StartupValidator validator(chain_db);
 *   ValidationStatus status = validator.Validate();
 *
 *   if (!status.isOK()) {
 *       if (status.result == StartupValidationResult::NEEDS_REINDEX) {
 *           std::cerr << status.message << "\n";
 *           std::cerr << "Run with --reindex to rebuild state\n";
 *           return EXIT_FAILURE;
 *       }
 *       else if (status.result == StartupValidationResult::FATAL) {
 *           std::cerr << "FATAL: " << status.message << "\n";
 *           std::cerr << status.guidance << "\n";
 *           return EXIT_FAILURE;
 *       }
 *   }
 *
 *   // Safe to proceed
 */
class StartupValidator {
public:
    /**
     * Constructor
     *
     * @param chain_db  ChainDB instance to validate
     */
    explicit StartupValidator(dinero::ChainDB* chain_db);

    /**
     * Perform all startup consistency checks
     *
     * Returns validation status with result + guidance.
     * This function is idempotent and safe to call multiple times.
     *
     * Checks performed:
     * 1. Tip consistency
     * 2. Block index integrity
     * 3. Block data availability
     * 4. UTXO set sanity (if available)
     *
     * @return  Validation status (OK, RECOVERED, NEEDS_REINDEX, or FATAL)
     */
    ValidationStatus Validate();

    /**
     * Get detailed validation report (for logging/debugging)
     *
     * Returns multi-line report with check-by-check status.
     *
     * @return  Human-readable validation report
     */
    std::string GetValidationReport() const;

private:
    // ========================================================================
    // Individual Validation Checks
    // ========================================================================

    /**
     * CHECK 1: Tip Consistency
     *
     * Verifies:
     * - Tip hash exists in block index
     * - Tip has valid chainwork
     * - Tip status is VALID_CHAIN
     * - Tip height matches expected
     *
     * @return  OK if tip is valid, error code + message otherwise
     */
    ValidationStatus CheckTipConsistency();

    /**
     * CHECK 2: Block Index Integrity
     *
     * Verifies:
     * - All blocks from genesis to tip form connected chain
     * - Parent hashes are valid
     * - Chainwork is monotonically increasing
     * - No missing blocks in main chain
     *
     * @return  OK if index is valid, error code + message otherwise
     */
    ValidationStatus CheckBlockIndexIntegrity();

    /**
     * CHECK 3: Block Data Availability
     *
     * Verifies:
     * - Block files exist for all indexed blocks
     * - Disk positions are valid (within file bounds)
     * - No torn writes (partial blocks)
     *
     * @return  OK if all block data present, error code + message otherwise
     */
    ValidationStatus CheckBlockDataAvailability();

    /**
     * CHECK 4: UTXO Set Sanity
     *
     * Verifies (if checksum available):
     * - UTXO count is reasonable
     * - UTXO checksum matches expected
     *
     * NOTE: This is a sanity check, not a full validation.
     *       Full UTXO validation requires --reindex.
     *
     * @return  OK if UTXO set looks sane, warning otherwise
     */
    ValidationStatus CheckUTXOSetSanity();

    // ========================================================================
    // Recovery Actions
    // ========================================================================

    /**
     * Attempt to recover from corrupt tip
     *
     * Strategy:
     * - Find last block with VALID_CHAIN status
     * - Revert tip to that block
     * - Mark blocks after as FAILED_CHILD
     *
     * @return  true if recovery succeeded, false otherwise
     */
    bool RecoverFromCorruptTip();

    /**
     * Find last valid block before corruption
     *
     * Walks backwards from current tip until finding block with:
     * - VALID_CHAIN status
     * - Valid block data on disk
     * - Valid parent link
     *
     * @return  Block hash + height, or nullopt if no valid block found
     */
    std::optional<std::pair<std::string, uint32_t>> FindLastValidBlock();

    // ========================================================================
    // Member Variables
    // ========================================================================

    dinero::ChainDB* chain_db_;          // ChainDB instance to validate

    // Validation report (populated by Validate())
    std::string tip_check_msg_;
    std::string index_check_msg_;
    std::string data_check_msg_;
    std::string utxo_check_msg_;

    bool validation_performed_;          // Has Validate() been called?
    ValidationStatus last_status_;       // Last validation result
};

} // namespace consensus
} // namespace dinero

#endif // DINERO_CONSENSUS_STARTUP_VALIDATOR_H
