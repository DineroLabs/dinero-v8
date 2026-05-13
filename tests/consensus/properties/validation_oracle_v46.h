#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_map>

// Ring 2 V4.6: Coinbase Maturity Enforced in UTXO Queries
// Property: Coinbase outputs must mature (100 blocks) before spending

namespace dinero::consensus::test {

class ValidationOracleV46 : public ValidationPropertyOracle {
public:
    ValidationOracleV46() = default;

    std::string name() const override {
        return "V4.6: Coinbase maturity enforced in UTXO queries";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        coinbase_heights_.clear();
        current_height_ = 0;
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track current height from state
        if (state.height > 0) {
            current_height_ = state.height;
        }

        switch (event.type) {
            case ValidationEventType::UTXO_ADDED:
                if (event.outpoint.has_value() && event.coin.has_value()) {
                    if (event.coin.value().isCoinbase) {
                        coinbase_heights_[event.outpoint.value()] = event.coin.value().height;
                    }
                }
                break;

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    auto it = coinbase_heights_.find(event.outpoint.value());
                    if (it != coinbase_heights_.end()) {
                        uint32_t coinbase_height = it->second;
                        uint32_t maturity = 100;

                        // V4.6: Check if coinbase is mature (100 blocks)
                        if (current_height_ < coinbase_height + maturity) {
                            reportViolation(
                                "V4.6",
                                "Immature coinbase spent: created at height " +
                                std::to_string(coinbase_height) +
                                ", spent at height " + std::to_string(current_height_) +
                                " (needs " + std::to_string(coinbase_height + maturity) + ")",
                                event_index
                            );
                        }

                        coinbase_heights_.erase(it);
                    }
                }
                break;

            default:
                break;
        }
    }

private:
    // Map: OutPoint → creation height (for coinbase outputs only)
    std::unordered_map<OutPoint, uint32_t> coinbase_heights_;
    uint32_t current_height_{0};
};

} // namespace dinero::consensus::test
