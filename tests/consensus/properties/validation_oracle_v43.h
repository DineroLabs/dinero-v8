#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>

// Ring 2 V4.3: Outputs Are Added To UTXO Set
// Property: After BLOCK_CONNECTED, all transaction outputs emit UTXO_ADDED events

namespace dinero::consensus::test {

class ValidationOracleV43 : public ValidationPropertyOracle {
public:
    ValidationOracleV43() = default;

    std::string name() const override {
        return "V4.3: Outputs are added to UTXO set";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        expected_additions_.clear();
        actual_additions_.clear();
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

            case ValidationEventType::UTXO_ADDED:
                if (event.outpoint.has_value()) {
                    actual_additions_.insert(event.outpoint.value());
                }
                break;

            default:
                break;
        }
    }

    void finalize() override {
        // V4.3: Check that all expected outputs were actually added
        for (const auto& expected : expected_additions_) {
            if (actual_additions_.find(expected) == actual_additions_.end()) {
                reportViolation(
                    "V4.3",
                    "Expected output addition not observed: " + expected.ToString(),
                    0
                );
            }
        }

        // V4.3: Check for unexpected additions (should not happen in valid execution)
        for (const auto& actual : actual_additions_) {
            if (expected_additions_.find(actual) == expected_additions_.end()) {
                reportViolation(
                    "V4.3",
                    "Unexpected UTXO addition observed: " + actual.ToString(),
                    0
                );
            }
        }
    }

private:
    void onBlockConnected(const Block& block, uint64_t event_index) {
        // Record expected additions (all outputs in all transactions)
        for (const auto& tx : block.vtx) {
            TxId txid = tx.GetTxid();
            for (size_t vout = 0; vout < tx.vout.size(); vout++) {
                OutPoint outpoint(txid, static_cast<uint32_t>(vout));
                expected_additions_.insert(outpoint);
            }
        }
    }

    std::unordered_set<OutPoint> expected_additions_;
    std::unordered_set<OutPoint> actual_additions_;
};

} // namespace dinero::consensus::test
