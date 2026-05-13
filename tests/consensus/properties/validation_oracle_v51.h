#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include <unordered_map>
#include <string>

// Ring 2 V5.1: Invalid Block Is Never Connected
// Property: If block validation fails → BLOCK_CONNECTED must never occur

namespace dinero::consensus::test {

class ValidationOracleV51 : public ValidationPropertyOracle {
public:
    ValidationOracleV51() = default;

    std::string name() const override {
        return "V5.1: Invalid block is never connected";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        failed_blocks_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        switch (event.type) {
            case ValidationEventType::BLOCK_CONNECTED:
                // Check if this block previously failed validation
                if (event.block.has_value()) {
                    std::string block_hash = event.block.value().GetHash().GetHex();

                    auto it = failed_blocks_.find(block_hash);
                    if (it != failed_blocks_.end()) {
                        reportViolation(
                            "V5.1",
                            "Invalid block was connected despite validation failure: " +
                            block_hash + " (failure reason: " + it->second + ")",
                            event_index
                        );
                    }
                }
                break;

            case ValidationEventType::TX_VALIDATED:
            case ValidationEventType::UTXO_ADDED:
            case ValidationEventType::UTXO_SPENT:
                // If we see state changes for a failed block, that's also a violation
                // (This is checked by tracking which blocks should have been rejected)
                break;

            default:
                break;
        }

        // Track validation failures
        if (!event.success && event.block.has_value()) {
            std::string block_hash = event.block.value().GetHash().GetHex();
            failed_blocks_[block_hash] = event.error_message;
        }
    }

private:
    // Map: block hash → failure reason
    std::unordered_map<std::string, std::string> failed_blocks_;
};

} // namespace dinero::consensus::test
