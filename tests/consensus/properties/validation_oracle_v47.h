#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>

// Ring 2 V4.7: No UTXO Duplication After Apply/Revert Cycles
// Property: Apply → Disconnect → Apply cycles must not create duplicate UTXOs

namespace dinero::consensus::test {

class ValidationOracleV47 : public ValidationPropertyOracle {
public:
    ValidationOracleV47() = default;

    std::string name() const override {
        return "V4.7: No UTXO duplication after apply/revert cycles";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        addition_counts_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        switch (event.type) {
            case ValidationEventType::UTXO_ADDED:
                if (event.outpoint.has_value()) {
                    OutPoint outpoint = event.outpoint.value();

                    // Track how many times this UTXO has been added
                    addition_counts_[outpoint]++;

                    // V4.7: A UTXO should never be added more than once
                    if (addition_counts_[outpoint] > 1) {
                        reportViolation(
                            "V4.7",
                            "UTXO added multiple times (duplication detected): " +
                            outpoint.ToString() +
                            " (count: " + std::to_string(addition_counts_[outpoint]) + ")",
                            event_index
                        );
                    }
                }
                break;

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    // When a UTXO is spent, reset its addition count
                    // This allows it to be re-added if the block is disconnected and reconnected
                    // (but it should only be added once per active lifetime)
                    addition_counts_.erase(event.outpoint.value());
                }
                break;

            default:
                break;
        }
    }

private:
    // Map: OutPoint → number of times added
    std::unordered_map<OutPoint, size_t> addition_counts_;
};

} // namespace dinero::consensus::test
