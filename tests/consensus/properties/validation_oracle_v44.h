#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>

// Ring 2 V4.4: Value Is Conserved (Inputs ≥ Outputs + Fee)
// Property: For each transaction, sum(inputs) ≥ sum(outputs) + fee

namespace dinero::consensus::test {

class ValidationOracleV44 : public ValidationPropertyOracle {
public:
    ValidationOracleV44() = default;

    std::string name() const override {
        return "V4.4: Value is conserved (inputs >= outputs + fee)";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        utxo_values_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        switch (event.type) {
            case ValidationEventType::BLOCK_CONNECTED:
                if (event.block.has_value()) {
                    checkBlockValueConservation(event.block.value(), event_index);
                }
                break;

            case ValidationEventType::UTXO_ADDED:
                if (event.outpoint.has_value() && event.coin.has_value()) {
                    utxo_values_[event.outpoint.value()] = event.coin.value().value.GetUna();
                }
                break;

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    utxo_values_.erase(event.outpoint.value());
                }
                break;

            default:
                break;
        }
    }

private:
    void checkBlockValueConservation(const Block& block, uint64_t event_index) {
        // Check each transaction in the block
        for (const auto& tx : block.vtx) {
            // Calculate input value
            uint64_t input_value = 0;
            if (!tx.IsCoinbase()) {
                for (const auto& input : tx.vin) {
                    OutPoint outpoint(input.prevout.txid, input.prevout.vout);

                    // In a real system, we'd look up the UTXO value
                    // For this oracle, we track them via UTXO_ADDED events
                    auto it = utxo_values_.find(outpoint);
                    if (it != utxo_values_.end()) {
                        input_value += it->second;
                    }
                }
            }

            // Calculate output value
            uint64_t output_value = 0;
            for (const auto& output : tx.vout) {
                output_value += output.value.GetUna();
            }

            // V4.4: For non-coinbase, inputs must be >= outputs
            if (!tx.IsCoinbase()) {
                if (input_value < output_value) {
                    reportViolation(
                        "V4.4",
                        "Value not conserved: inputs=" + std::to_string(input_value) +
                        " < outputs=" + std::to_string(output_value) +
                        " (txid=" + tx.GetTxid().AsUint256().GetHex() + ")",
                        event_index
                    );
                }
            }
            // For coinbase, output value should match subsidy + fees
            // (fee validation is done at block level, not per-tx)
        }
    }

    // Track UTXO values for input validation
    std::unordered_map<OutPoint, uint64_t> utxo_values_;
};

} // namespace dinero::consensus::test
