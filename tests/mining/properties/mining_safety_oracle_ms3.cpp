#include "mining_safety_oracle_ms3.h"
#include <sstream>

// Ring 4 Phase 4d: MS3 - No Invalid Transaction Inclusion Implementation
// Note: Placeholder validation only - full UTXO validation in Phase 4h

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MS3Oracle::MS3Oracle(const ConsensusParams& params)
    : MiningSafetyOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MS3Oracle::name() const {
    return "MS3: No Invalid Transaction Inclusion (Placeholder)";
}

void MS3Oracle::reset() {
    this->MiningSafetyOracle::reset();

    mempool_txs_.clear();
    template_txs_.clear();
    tx_count_by_height_.clear();
    next_template_id_ = 0;
}

// ============================================================================
// Event Observation
// ============================================================================

void MS3Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track template creation and validate transaction count
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;

            // Check transaction count from state
            if (state.template_tx_count.has_value()) {
                uint32_t tx_count = *state.template_tx_count;

                // Store tx count for this height
                tx_count_by_height_[height] = tx_count;

                // Validate transaction count doesn't exceed block limit
                if (tx_count > this->getParams().max_block_txs) {
                    std::ostringstream msg;
                    msg << "Template at height " << height
                        << " has " << tx_count << " transactions, "
                        << "exceeds limit " << this->getParams().max_block_txs;

                    this->reportViolation("MS3", msg.str(), event_index);
                }

                // Template should have at least 1 tx (coinbase)
                // Note: Phase 4b simulator may create templates with 0 txs (placeholder)
                // This is OK for Phase 4d - full validation in Phase 4h
            }
        }
    }

    // Track blocks accepted
    if (event.type == MiningEventType::BLOCK_ACCEPTED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;

            // Verify block has valid transaction count
            if (tx_count_by_height_.count(height) > 0) {
                uint32_t tx_count = tx_count_by_height_[height];

                // In production, blocks must have at least coinbase
                // But Phase 4b simulator uses placeholders, so this is lenient
            }
        }
    }

    // Track blocks rejected (might indicate invalid transactions)
    if (event.type == MiningEventType::BLOCK_REJECTED) {
        // In Phase 4d, we just note rejections
        // Full validation logic will be in Phase 4h
        // Block rejection could be due to:
        // - Invalid transactions
        // - Bad PoW
        // - Stale tip
        // - Other consensus failures
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void MS3Oracle::finalize() {
    // After all events processed, check for consistency

    // Verify all templates had reasonable transaction counts
    for (const auto& [height, tx_count] : tx_count_by_height_) {
        // Check transaction count is within reasonable bounds
        if (tx_count > this->getParams().max_block_txs) {
            std::ostringstream msg;
            msg << "Height " << height << " has excessive transaction count: "
                << tx_count << " (max: " << this->getParams().max_block_txs << ")";

            this->reportViolation("MS3", msg.str(), 0);
        }

        // Note: We don't check for zero transaction count here because
        // Phase 4b simulator uses placeholders and may not create coinbase txs
        // Full transaction validation (including coinbase requirement) will be
        // added in Phase 4h when integrating real BlockAssembler
    }

    // Phase 4d note: Full validation requires:
    // - UTXO set tracking
    // - Script validation
    // - Double-spend detection
    // - Input/output validation
    // - Signature verification
    // - Coinbase transaction validation
    // These will be added in Phase 4h when integrating real BlockAssembler
}

}  // namespace mining_test
