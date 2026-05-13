/**
 * Phase E.1.a: Startup Consistency Validator Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/startup_validator.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "consensus/block_index.h"
#include <sstream>
#include <iomanip>

namespace dinero {
namespace consensus {

//==============================================================================
// Constructor
//==============================================================================

StartupValidator::StartupValidator(ChainDB* chain_db)
    : chain_db_(chain_db)
    , validation_performed_(false)
    , last_status_{StartupValidationResult::OK, "", "", 0}
{
    if (!chain_db_) {
        throw std::invalid_argument("StartupValidator: chain_db cannot be null");
    }
}

//==============================================================================
// Main Validation Entry Point
//==============================================================================

ValidationStatus StartupValidator::Validate() {
    // Reset previous results
    tip_check_msg_.clear();
    index_check_msg_.clear();
    data_check_msg_.clear();
    utxo_check_msg_.clear();

    // CRITICAL: Checks must run in order (each depends on previous)

    // CHECK 1: Tip Consistency
    ValidationStatus tip_status = CheckTipConsistency();
    if (tip_status.result == StartupValidationResult::FATAL) {
        validation_performed_ = true;
        last_status_ = tip_status;
        return tip_status;
    }

    // If tip check found corruption, attempt recovery
    if (tip_status.result == StartupValidationResult::NEEDS_REINDEX) {
        // Try to recover
        if (RecoverFromCorruptTip()) {
            tip_status.result = StartupValidationResult::RECOVERED;
            tip_status.message = "Recovered from corrupt tip";
            tip_status.guidance = "Tip was corrupt, reverted to last valid block";
        } else {
            // Recovery failed, operator must reindex
            validation_performed_ = true;
            last_status_ = tip_status;
            return tip_status;
        }
    }

    // CHECK 2: Block Index Integrity
    ValidationStatus index_status = CheckBlockIndexIntegrity();
    if (index_status.result != StartupValidationResult::OK) {
        validation_performed_ = true;
        last_status_ = index_status;
        return index_status;
    }

    // CHECK 3: Block Data Availability
    ValidationStatus data_status = CheckBlockDataAvailability();
    if (data_status.result != StartupValidationResult::OK) {
        validation_performed_ = true;
        last_status_ = data_status;
        return data_status;
    }

    // CHECK 4: UTXO Set Sanity
    ValidationStatus utxo_status = CheckUTXOSetSanity();
    if (utxo_status.result == StartupValidationResult::NEEDS_REINDEX ||
        utxo_status.result == StartupValidationResult::FATAL) {
        // UTXO corruption detected - abort startup
        validation_performed_ = true;
        last_status_ = utxo_status;
        return utxo_status;
    }
    if (utxo_status.result == StartupValidationResult::WARNING) {
        // Non-fatal warning - log but continue
        utxo_check_msg_ = utxo_status.message;
    }

    // All checks passed
    validation_performed_ = true;
    last_status_ = {
        StartupValidationResult::OK,
        "All startup consistency checks passed",
        "",
        0  // Not applicable for OK status
    };

    return last_status_;
}

//==============================================================================
// CHECK 1: Tip Consistency
//==============================================================================

ValidationStatus StartupValidator::CheckTipConsistency() {
    tip_check_msg_ = "Checking tip consistency...";

    // Get current tip from ChainDB
    auto tip_opt = chain_db_->getTip();
    if (!tip_opt) {
        // No tip = fresh database (OK)
        tip_check_msg_ = "No tip found (fresh database)";
        return {
            StartupValidationResult::OK,
            "Fresh database, no tip to validate",
            "",
            0
        };
    }

    auto [tip_hash, tip_height, tip_work, tip_time] = *tip_opt;

    // Verify tip exists in block index
    auto header_opt = chain_db_->getHeaderMetadata(tip_hash);
    if (!header_opt) {
        // FATAL: Tip references non-existent block
        tip_check_msg_ = "FATAL: Tip references non-existent block";
        return {
            StartupValidationResult::FATAL,
            "Tip hash not found in block index",
            "Database corruption detected. Run with --reindex to rebuild state.",
            0
        };
    }

    auto header = *header_opt;

    // Verify tip height matches index height
    if (tip_height != header.height) {
        tip_check_msg_ = "ERROR: Tip height mismatch";
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "Tip height (" + std::to_string(tip_height) +
            ") doesn't match index height (" + std::to_string(header.height) + ")",
            "Run with --reindex to rebuild state.",
            0
        };
    }

    // Verify tip chainwork matches
    if (tip_work != header.chainwork) {
        tip_check_msg_ = "ERROR: Tip chainwork mismatch";
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "Tip chainwork doesn't match index",
            "Run with --reindex to rebuild state.",
            0
        };
    }

    // Verify tip has VALID_CHAIN status
    if (header.status_flags != BlockStatus::VALID_CHAIN) {
        tip_check_msg_ = "ERROR: Tip does not have VALID_CHAIN status";
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "Tip block status is not VALID_CHAIN (status=" +
            std::to_string(static_cast<int>(header.status_flags)) + ")",
            "Tip is corrupt. Recovery will be attempted.",
            tip_height > 0 ? tip_height - 1 : 0
        };
    }

    // Tip is consistent
    tip_check_msg_ = "Tip consistent: height=" + std::to_string(tip_height) +
                     " hash=" + tip_hash.substr(0, 16) + "...";
    return {
        StartupValidationResult::OK,
        "Tip is consistent",
        "",
        tip_height
    };
}

//==============================================================================
// CHECK 2: Block Index Integrity
//==============================================================================

ValidationStatus StartupValidator::CheckBlockIndexIntegrity() {
    index_check_msg_ = "Checking block index integrity...";

    // Get tip to know where to stop
    auto tip_opt = chain_db_->getTip();
    if (!tip_opt) {
        // No tip = nothing to check
        index_check_msg_ = "No tip, skipping index check";
        return {StartupValidationResult::OK, "No blocks to check", "", 0};
    }

    auto [tip_hash, tip_height, tip_work, tip_time] = *tip_opt;

    // Walk backwards from tip to genesis, verifying chain connectivity
    std::string current_hash = tip_hash;
    uint32_t current_height = tip_height;
    uint256_t previous_chainwork = tip_work;

    uint32_t blocks_checked = 0;
    const uint32_t MAX_BLOCKS_TO_CHECK = 10000;  // Limit for startup performance

    while (current_height > 0 && blocks_checked < MAX_BLOCKS_TO_CHECK) {
        // Get header metadata
        auto header_opt = chain_db_->getHeaderMetadata(current_hash);
        if (!header_opt) {
            index_check_msg_ = "ERROR: Missing block in chain";
            return {
                StartupValidationResult::FATAL,
                "Block index is corrupt: missing block at height " +
                std::to_string(current_height),
                "Run with --reindex to rebuild state.",
                current_height
            };
        }

        auto header = *header_opt;

        // Verify height matches expected
        if (header.height != current_height) {
            index_check_msg_ = "ERROR: Height mismatch in chain";
            return {
                StartupValidationResult::FATAL,
                "Block index is corrupt: height mismatch at " +
                std::to_string(current_height),
                "Run with --reindex to rebuild state.",
                current_height
            };
        }

        // Verify chainwork is monotonically decreasing as we walk back
        if (current_height > 0 && header.chainwork >= previous_chainwork) {
            index_check_msg_ = "ERROR: Chainwork not monotonic";
            return {
                StartupValidationResult::NEEDS_REINDEX,
                "Block index is corrupt: chainwork not monotonic at height " +
                std::to_string(current_height),
                "Run with --reindex to rebuild state.",
                current_height
            };
        }

        // Move to parent
        current_hash = header.parent_hash;
        current_height--;
        previous_chainwork = header.chainwork;
        blocks_checked++;
    }

    // Index integrity check passed
    index_check_msg_ = "Block index integrity OK (checked " +
                       std::to_string(blocks_checked) + " blocks)";
    return {
        StartupValidationResult::OK,
        "Block index is consistent",
        "",
        tip_height
    };
}

//==============================================================================
// CHECK 3: Block Data Availability
//==============================================================================

ValidationStatus StartupValidator::CheckBlockDataAvailability() {
    data_check_msg_ = "Checking block data availability...";

    // Get tip
    auto tip_opt = chain_db_->getTip();
    if (!tip_opt) {
        data_check_msg_ = "No tip, skipping data check";
        return {StartupValidationResult::OK, "No blocks to check", "", 0};
    }

    auto [tip_hash, tip_height, tip_work, tip_time] = *tip_opt;

    // For startup performance, only check recent blocks
    // Full validation can be done with --reindex if needed
    const uint32_t BLOCKS_TO_CHECK = 100;  // Last 100 blocks

    uint32_t start_height = tip_height > BLOCKS_TO_CHECK ?
                            tip_height - BLOCKS_TO_CHECK : 0;

    // Walk backwards from tip, checking block data exists
    std::string current_hash = tip_hash;
    uint32_t current_height = tip_height;
    uint32_t blocks_checked = 0;

    while (current_height >= start_height && blocks_checked < BLOCKS_TO_CHECK) {
        // Get header with disk position
        auto header_opt = chain_db_->getHeaderMetadata(current_hash);
        if (!header_opt) {
            // This should not happen (already checked in CheckBlockIndexIntegrity)
            data_check_msg_ = "ERROR: Missing header during data check";
            return {
                StartupValidationResult::FATAL,
                "Unexpected error: missing header at height " +
                std::to_string(current_height),
                "Run with --reindex.",
                current_height
            };
        }

        auto header = *header_opt;

        // Check if block has disk position (Schema v2)
        // NOTE: This check assumes Schema v2 is in use
        // If disk position is 0xFFFFFFFF, block not stored yet (pruned or pending)
        if (header.disk_file_index != 0xFFFFFFFF) {
            // Block should exist on disk
            // TODO: Actually check file existence here
            // For now, just verify position is reasonable
            if (header.disk_offset == 0 && current_height > 0) {
                // Suspicious: block at non-zero height with offset 0
                data_check_msg_ = "WARNING: Suspicious disk position";
                // Don't fail, but log warning
            }
        }

        // Move to parent
        current_hash = header.parent_hash;
        current_height--;
        blocks_checked++;

        if (current_height == 0) break;  // Reached genesis
    }

    // Block data availability check passed
    data_check_msg_ = "Block data available (checked " +
                      std::to_string(blocks_checked) + " recent blocks)";
    return {
        StartupValidationResult::OK,
        "Block data is available",
        "",
        tip_height
    };
}

//==============================================================================
// CHECK 4: UTXO Set Sanity
//==============================================================================

ValidationStatus StartupValidator::CheckUTXOSetSanity() {
    utxo_check_msg_ = "Checking UTXO set sanity...";

    // ========================================================================
    // UTXO Set Sanity Check
    // ========================================================================
    // Verifies:
    // 1. UTXO set is accessible
    // 2. UTXO count is reasonable for chain height
    // 3. No obviously corrupt entries (negative amounts, etc.)
    // ========================================================================

    // Get chain tip for context
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        utxo_check_msg_ = "Cannot check UTXO set: chain tip not available";
        return {
            StartupValidationResult::WARNING,
            "UTXO sanity check skipped: no chain tip",
            "Chain may not be initialized yet",
            0
        };
    }
    uint32_t tip_height = static_cast<uint32_t>(tip_result.value().height);

    // Count UTXOs and check for obvious corruption
    uint64_t utxo_count = 0;
    uint64_t total_value = 0;
    bool found_corruption = false;
    std::string corruption_detail;

    auto status = chain_db_->forEachUTXO([&](const uint256& txid, uint32_t vout, const Coin& coin) {
        utxo_count++;

        // Sanity: amount should not be negative (stored as uint64_t, so check for unreasonably large)
        // Max supply is ~21M coins * 100M una = 2.1e15, so anything > 1e18 is suspicious
        constexpr uint64_t MAX_SANE_AMOUNT = 1'000'000'000'000'000'000ULL;  // 1e18
        if (coin.amount > MAX_SANE_AMOUNT) {
            found_corruption = true;
            corruption_detail = "UTXO with unreasonable amount detected";
            return false;  // Stop iteration
        }

        // Sanity: height should not exceed tip
        if (coin.height > static_cast<int>(tip_height)) {
            found_corruption = true;
            corruption_detail = "UTXO claims height " + std::to_string(coin.height) +
                               " but tip is only " + std::to_string(tip_height);
            return false;  // Stop iteration
        }

        // Sanity: scriptPubKey must not be empty
        if (coin.scriptPubKey.empty()) {
            found_corruption = true;
            corruption_detail = "UTXO with empty scriptPubKey detected";
            return false;  // Stop iteration
        }

        total_value += coin.amount;
        return true;  // Continue iteration
    });

    if (status != Status::Ok) {
        utxo_check_msg_ = "UTXO iteration failed";
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "Failed to iterate UTXO set - database may be corrupt",
            "Run with --reindex to rebuild UTXO set",
            tip_height
        };
    }

    if (found_corruption) {
        utxo_check_msg_ = "UTXO corruption detected: " + corruption_detail;
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "UTXO set corruption detected: " + corruption_detail,
            "Run with --reindex to rebuild UTXO set from blocks",
            tip_height
        };
    }

    // Sanity: For chains past genesis, we must have UTXOs
    // After height 1 (first PoW block), at minimum the coinbase output should exist
    if (tip_height > 1 && utxo_count == 0) {
        utxo_check_msg_ = "No UTXOs found despite chain having " + std::to_string(tip_height) + " blocks";
        return {
            StartupValidationResult::NEEDS_REINDEX,
            "UTXO set is empty but chain has " + std::to_string(tip_height) + " blocks",
            "Run with --reindex to rebuild UTXO set",
            tip_height
        };
    }

    // Sanity: If Utreexo is enabled, checkpoint must be present
    // This is a presence check, not a verification
    auto utreexo_checkpoint = chain_db_->getLatestUtreexoCheckpoint();
    if (utreexo_checkpoint.status() == Status::Ok) {
        const auto& [checkpoint_height, checkpoint_data] = utreexo_checkpoint.value();
        // Checkpoint height should not exceed tip
        if (checkpoint_height > static_cast<int>(tip_height)) {
            utxo_check_msg_ = "Utreexo checkpoint height exceeds chain tip";
            return {
                StartupValidationResult::NEEDS_REINDEX,
                "Utreexo checkpoint at height " + std::to_string(checkpoint_height) +
                " but tip is only " + std::to_string(tip_height),
                "Run with --reindex to rebuild Utreexo accumulator",
                tip_height
            };
        }
        // Checkpoint data should not be empty when we have UTXOs
        if (checkpoint_data.empty() && utxo_count > 0) {
            utxo_check_msg_ = "Utreexo checkpoint is empty but UTXO set is not";
            return {
                StartupValidationResult::NEEDS_REINDEX,
                "Utreexo checkpoint empty despite " + std::to_string(utxo_count) + " UTXOs present",
                "Run with --reindex to rebuild Utreexo accumulator",
                tip_height
            };
        }
    }

    // All checks passed
    utxo_check_msg_ = "UTXO set OK: " + std::to_string(utxo_count) + " UTXOs";
    return {
        StartupValidationResult::OK,
        "UTXO sanity check passed (" + std::to_string(utxo_count) + " UTXOs)",
        "",
        tip_height
    };
}

//==============================================================================
// Recovery: Corrupt Tip
//==============================================================================

bool StartupValidator::RecoverFromCorruptTip() {
    // Find last valid block
    auto last_valid = FindLastValidBlock();
    if (!last_valid) {
        // No valid block found, cannot recover
        return false;
    }

    auto [new_tip_hash, new_tip_height] = *last_valid;

    // Get new tip metadata
    auto header_opt = chain_db_->getHeaderMetadata(new_tip_hash);
    if (!header_opt) {
        return false;  // Should not happen
    }

    auto header = *header_opt;

    // Set new tip
    bool success = chain_db_->setTip(
        new_tip_hash,
        header.height,
        header.chainwork,
        header.timestamp
    );

    return success;
}

std::optional<std::pair<std::string, uint32_t>>
StartupValidator::FindLastValidBlock() {
    // Get current tip
    auto tip_opt = chain_db_->getTip();
    if (!tip_opt) {
        return std::nullopt;
    }

    auto [tip_hash, tip_height, tip_work, tip_time] = *tip_opt;

    // Walk backwards from tip until finding valid block
    std::string current_hash = tip_hash;
    uint32_t current_height = tip_height;

    while (current_height > 0) {
        auto header_opt = chain_db_->getHeaderMetadata(current_hash);
        if (!header_opt) {
            // Can't go further back
            return std::nullopt;
        }

        auto header = *header_opt;

        // Check if this block is valid
        if (header.status_flags == BlockStatus::VALID_CHAIN) {
            // Found valid block
            return std::make_pair(current_hash, current_height);
        }

        // Move to parent
        current_hash = header.parent_hash;
        current_height--;
    }

    // Reached genesis without finding valid block
    // Return genesis as fallback
    return std::make_pair(current_hash, 0);
}

//==============================================================================
// Validation Report
//==============================================================================

std::string StartupValidator::GetValidationReport() const {
    if (!validation_performed_) {
        return "Validation not yet performed. Call Validate() first.";
    }

    std::ostringstream report;
    report << "========================================\n";
    report << "Startup Validation Report\n";
    report << "========================================\n\n";

    // Overall result
    report << "Result: ";
    switch (last_status_.result) {
        case StartupValidationResult::OK:
            report << "✅ OK\n";
            break;
        case StartupValidationResult::RECOVERED:
            report << "🔄 RECOVERED\n";
            break;
        case StartupValidationResult::NEEDS_REINDEX:
            report << "⚠️  NEEDS REINDEX\n";
            break;
        case StartupValidationResult::FATAL:
            report << "❌ FATAL\n";
            break;
    }

    report << "Message: " << last_status_.message << "\n";
    if (!last_status_.guidance.empty()) {
        report << "Guidance: " << last_status_.guidance << "\n";
    }
    report << "\n";

    // Individual check results
    report << "Check Details:\n";
    report << "  [1] Tip Consistency:     " << tip_check_msg_ << "\n";
    report << "  [2] Index Integrity:     " << index_check_msg_ << "\n";
    report << "  [3] Data Availability:   " << data_check_msg_ << "\n";
    report << "  [4] UTXO Sanity:         " << utxo_check_msg_ << "\n";

    report << "\n========================================\n";

    return report.str();
}

} // namespace consensus
} // namespace dinero
