#pragma once

#include "validation_property_oracle.h"
#include "primitives/transaction.h"
#include <unordered_map>
#include <string>

// Ring 2 V5.2: Invalid Transaction Is Never Applied
// Property: A rejected tx must never emit UTXO_SPENT or UTXO_ADDED

namespace dinero::consensus::test {

class ValidationOracleV52 : public ValidationPropertyOracle {
public:
    ValidationOracleV52() = default;

    std::string name() const override {
        return "V5.2: Invalid transaction is never applied";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        failed_txs_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track failed transactions
        if (event.type == ValidationEventType::TX_VALIDATED && !event.success) {
            if (event.transaction.has_value()) {
                std::string txid = event.transaction.value().GetTxid().AsUint256().GetHex();
                failed_txs_[txid] = event.error_message;
            }
        }

        // Check if UTXO events correspond to failed transactions
        if (event.type == ValidationEventType::UTXO_ADDED ||
            event.type == ValidationEventType::UTXO_SPENT) {

            if (event.outpoint.has_value()) {
                std::string txid = event.outpoint.value().txid.AsUint256().GetHex();

                auto it = failed_txs_.find(txid);
                if (it != failed_txs_.end()) {
                    reportViolation(
                        "V5.2",
                        "Invalid transaction caused UTXO state change: txid=" +
                        txid + " (failure reason: " + it->second + ")",
                        event_index
                    );
                }
            }
        }
    }

private:
    // Map: txid → failure reason
    std::unordered_map<std::string, std::string> failed_txs_;
};

} // namespace dinero::consensus::test
