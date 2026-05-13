#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>

// Ring 2 V4.2: Inputs Are Removed From UTXO Set
// Property: After BLOCK_CONNECTED, all non-coinbase inputs are marked UTXO_SPENT

namespace dinero::consensus::test {

class ValidationOracleV42 : public ValidationPropertyOracle {
public:
    ValidationOracleV42() = default;

    std::string name() const override {
        return "V4.2: Inputs are removed from UTXO set";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        expected_removals_.clear();
        actual_removals_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        switch (event.type) {
            case ValidationEventType::BLOCK_CONNECTED:
                if (event.block.has_value()) {
                    onBlockConnected(event.block.value(), event_index);
                }
                break;

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    actual_removals_.insert(event.outpoint.value());
                }
                break;

            default:
                break;
        }
    }

    void finalize() override {
        // V4.2: Check that all expected inputs were actually removed
        for (const auto& expected : expected_removals_) {
            if (actual_removals_.find(expected) == actual_removals_.end()) {
                reportViolation(
                    "V4.2",
                    "Expected input removal not observed: " + expected.ToString(),
                    0
                );
            }
        }

        // V4.2: Check for unexpected removals (should not happen in valid execution)
        for (const auto& actual : actual_removals_) {
            if (expected_removals_.find(actual) == expected_removals_.end()) {
                reportViolation(
                    "V4.2",
                    "Unexpected UTXO removal observed: " + actual.ToString(),
                    0
                );
            }
        }
    }

private:
    void onBlockConnected(const Block& block, uint64_t event_index) {
        // Record expected removals (all inputs in non-coinbase transactions)
        for (const auto& tx : block.vtx) {
            if (!tx.IsCoinbase()) {
                for (const auto& input : tx.vin) {
                    OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                    expected_removals_.insert(outpoint);
                }
            }
        }
    }

    std::unordered_set<OutPoint> expected_removals_;
    std::unordered_set<OutPoint> actual_removals_;
};

} // namespace dinero::consensus::test
