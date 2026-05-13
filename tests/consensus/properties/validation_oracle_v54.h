#pragma once

#include "validation_property_oracle.h"
#include <vector>
#include <string>

// Ring 2 V5.4: Consensus Rule Violations Abort the Block
// Property: Violations of any V1–V4 property abort block application

namespace dinero::consensus::test {

class ValidationOracleV54 : public ValidationPropertyOracle {
public:
    ValidationOracleV54() = default;

    std::string name() const override {
        return "V5.4: Consensus rule violations abort the block";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        consensus_violations_.clear();
        successfully_connected_blocks_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track consensus violations (simulated by error messages)
        if (!event.success && event.block.has_value()) {
            std::string block_hash = event.block.value().GetHash().GetHex();

            // Check if error message indicates a consensus rule violation
            bool is_consensus_violation =
                event.error_message.find("merkle") != std::string::npos ||
                event.error_message.find("coinbase") != std::string::npos ||
                event.error_message.find("value") != std::string::npos ||
                event.error_message.find("UTXO") != std::string::npos ||
                event.error_message.find("input") != std::string::npos ||
                event.error_message.find("output") != std::string::npos;

            if (is_consensus_violation) {
                consensus_violations_.insert(block_hash);
            }
        }

        // Track successfully connected blocks
        if (event.type == ValidationEventType::BLOCK_CONNECTED && event.success) {
            if (event.block.has_value()) {
                std::string block_hash = event.block.value().GetHash().GetHex();

                // V5.4: Block with consensus violations must NOT be connected
                if (consensus_violations_.count(block_hash) > 0) {
                    reportViolation(
                        "V5.4",
                        "Block with consensus rule violations was connected: " + block_hash,
                        event_index
                    );
                }

                successfully_connected_blocks_.insert(block_hash);
            }
        }
    }

private:
    std::unordered_set<std::string> consensus_violations_;
    std::unordered_set<std::string> successfully_connected_blocks_;
};

} // namespace dinero::consensus::test
