#pragma once

#include "validation_property_oracle.h"
#include <unordered_map>
#include <string>

// Ring 2 V5.5: Reorg Never Commits an Invalid Chain
// Property: Competing chain with invalid block is never selected or applied

namespace dinero::consensus::test {

class ValidationOracleV55 : public ValidationPropertyOracle {
public:
    ValidationOracleV55() = default;

    std::string name() const override {
        return "V5.5: Reorg never commits an invalid chain";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        invalid_blocks_.clear();
        current_tip_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track invalid blocks
        if (!event.success && event.block.has_value()) {
            std::string block_hash = event.block.value().GetHash().GetHex();
            invalid_blocks_.insert(block_hash);
        }

        // Track tip changes (reorgs)
        if (event.type == ValidationEventType::BLOCK_CONNECTED && event.success) {
            if (event.block.has_value()) {
                std::string block_hash = event.block.value().GetHash().GetHex();

                // V5.5: Tip should never move to an invalid block
                if (invalid_blocks_.count(block_hash) > 0) {
                    reportViolation(
                        "V5.5",
                        "Reorg committed an invalid block as new tip: " + block_hash,
                        event_index
                    );
                }

                current_tip_ = block_hash;
            }
        }

        // Track disconnections (reorg start)
        if (event.type == ValidationEventType::BLOCK_DISCONNECTED) {
            // Reorg in progress - verify the new chain doesn't contain invalid blocks
        }
    }

private:
    std::unordered_set<std::string> invalid_blocks_;
    std::string current_tip_;
};

} // namespace dinero::consensus::test
