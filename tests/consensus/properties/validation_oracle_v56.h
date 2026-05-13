#pragma once

#include "validation_property_oracle.h"
#include <unordered_map>
#include <string>

// Ring 2 V5.6: Mempool Never Feeds Invalid Transactions to Blocks
// Property: Invalid txs never appear in block assembly trace

namespace dinero::consensus::test {

class ValidationOracleV56 : public ValidationPropertyOracle {
public:
    ValidationOracleV56() = default;

    std::string name() const override {
        return "V5.6: Mempool never feeds invalid transactions to blocks";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        invalid_txs_.clear();
        block_txs_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track invalid transactions
        if (event.type == ValidationEventType::TX_VALIDATED && !event.success) {
            if (event.transaction.has_value()) {
                std::string txid = event.transaction.value().GetTxid().AsUint256().GetHex();
                invalid_txs_[txid] = event.error_message;
            }
        }

        // Track transactions included in blocks
        if (event.type == ValidationEventType::BLOCK_CONNECTED && event.block.has_value()) {
            const Block& block = event.block.value();

            for (const auto& tx : block.vtx) {
                std::string txid = tx.GetTxid().AsUint256().GetHex();

                // V5.6: Invalid tx should never appear in a block
                auto it = invalid_txs_.find(txid);
                if (it != invalid_txs_.end()) {
                    reportViolation(
                        "V5.6",
                        "Invalid transaction included in block: txid=" +
                        txid + " (failure reason: " + it->second + ")",
                        event_index
                    );
                }

                block_txs_.insert(txid);
            }
        }
    }

private:
    std::unordered_map<std::string, std::string> invalid_txs_;
    std::unordered_set<std::string> block_txs_;
};

} // namespace dinero::consensus::test
