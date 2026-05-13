#include "consensus/tx_parser.h"

#include <exception>

namespace dinero {
namespace consensus {

bool TransactionParser::ParseTransaction(const std::string& hex_str, Transaction& tx, std::string& error) {
    try {
        const std::vector<uint8_t> raw = TransactionSerializer::FromHex(hex_str);
        if (raw.empty()) {
            error = "Invalid hex string";
            return false;
        }

        size_t consumed = 0;
        if (!TransactionSerializer::Deserialize(tx, raw, consumed)) {
            error = "Failed to deserialize transaction";
            return false;
        }

        if (consumed != raw.size()) {
            error = "Transaction has trailing bytes";
            return false;
        }

        error.clear();
        return true;
    } catch (const std::exception&) {
        error = "Invalid hex string";
        return false;
    }
}

bool TransactionParser::ParseCoinbaseTransaction(const std::string& hex_str, Transaction& tx, std::string& error) {
    if (!ParseTransaction(hex_str, tx, error)) {
        return false;
    }

    if (!tx.IsCoinbase()) {
        error = "Transaction is not a coinbase";
        return false;
    }

    error.clear();
    return true;
}

std::string TransactionParser::SerializeTransaction(const Transaction& tx) {
    return TransactionSerializer::ToHex(tx.Serialize(TxSerializationMode::WithWitness));
}

std::string TransactionParser::CalculateTxId(const Transaction& tx) {
    return tx.GetTxid().AsUint256().GetHex();
}

} // namespace consensus
} // namespace dinero
