#pragma once

#include "validation_property_oracle.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <unordered_set>
#include <unordered_map>

// Ring 2 V4.1: Applying Valid Block Creates Correct UTXO Set
// Property: After BLOCK_CONNECTED, all inputs removed and all outputs added

namespace dinero::consensus::test {

class ValidationOracleV41 : public ValidationPropertyOracle {
public:
    ValidationOracleV41() = default;

    std::string name() const override {
        return "V4.1: Applying valid block creates correct UTXO set";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        expected_removals_.clear();
        expected_additions_.clear();
        actual_removals_.clear();
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

            case ValidationEventType::UTXO_SPENT:
                if (event.outpoint.has_value()) {
                    actual_removals_.insert(event.outpoint.value());
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
        // V4.1: Check that expected removals match actual removals
        for (const auto& expected : expected_removals_) {
            if (actual_removals_.find(expected) == actual_removals_.end()) {
                reportViolation(
                    "V4.1",
                    "Expected UTXO removal not observed: " + expected.ToString(),
                    0
                );
            }
        }

        // V4.1: Check that expected additions match actual additions
        for (const auto& expected : expected_additions_) {
            if (actual_additions_.find(expected) == actual_additions_.end()) {
                reportViolation(
                    "V4.1",
                    "Expected UTXO addition not observed: " + expected.ToString(),
                    0
                );
            }
        }
    }

private:
    void onBlockConnected(const Block& block, uint64_t event_index) {
        // Record expected removals (all inputs in block)
        for (const auto& tx : block.vtx) {
            if (!tx.IsCoinbase()) {
                for (const auto& input : tx.vin) {
                    OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                    expected_removals_.insert(outpoint);
                }
            }

            // Record expected additions (all outputs in block)
            TxId txid = tx.GetTxid();
            for (size_t vout = 0; vout < tx.vout.size(); vout++) {
                OutPoint outpoint(txid, static_cast<uint32_t>(vout));
                expected_additions_.insert(outpoint);
            }
        }
    }

    std::unordered_set<OutPoint> expected_removals_;
    std::unordered_set<OutPoint> expected_additions_;
    std::unordered_set<OutPoint> actual_removals_;
    std::unordered_set<OutPoint> actual_additions_;
};

} // namespace dinero::consensus::test
