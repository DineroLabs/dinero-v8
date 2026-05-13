#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>

// Ring 2 V4.5: Reorg Correctly Reverts UTXO Set
// Property: After BLOCK_DISCONNECTED, UTXO set returns to pre-connection state

namespace dinero::consensus::test {

class ValidationOracleV45 : public ValidationPropertyOracle {
public:
    ValidationOracleV45() = default;

    std::string name() const override {
        return "V4.5: Reorg correctly reverts UTXO set";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        utxo_snapshots_.clear();
        current_utxos_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        switch (event.type) {
            case ValidationEventType::BLOCK_CONNECTED:
                // Save UTXO set snapshot before connection
                utxo_snapshots_.push_back(current_utxos_);
                break;

            case ValidationEventType::BLOCK_DISCONNECTED:
                // Verify UTXO set reverted to pre-connection state
                if (!utxo_snapshots_.empty()) {
                    auto expected_utxos = utxo_snapshots_.back();
                    utxo_snapshots_.pop_back();

                    // Check if current UTXO set matches the snapshot
                    if (current_utxos_ != expected_utxos) {
                        reportViolation(
                            "V4.5",
                            "UTXO set mismatch after block disconnect (reorg didn't restore state correctly)",
                            event_index
                        );
                    }
                }
                break;

            case ValidationEventType::UTXO_ADDED:
                if (event.outpoint.has_value()) {
                    current_utxos_.insert(event.outpoint.value());
                }
                break;

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    current_utxos_.erase(event.outpoint.value());
                }
                break;

            default:
                break;
        }
    }

private:
    // Stack of UTXO set snapshots (one per block connection)
    std::vector<std::unordered_set<OutPoint>> utxo_snapshots_;

    // Current UTXO set
    std::unordered_set<OutPoint> current_utxos_;
};

} // namespace dinero::consensus::test
