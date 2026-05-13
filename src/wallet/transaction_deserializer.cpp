#include "wallet/transaction.h"

namespace dinero {

bool TransactionSerializer::Deserialize(Transaction& tx, const std::vector<uint8_t>& data) {
    size_t consumed = 0;
    if (!Deserialize(tx, data, consumed)) {
        return false;
    }
    // This overload is used for parsing a single transaction buffer (RPC hex, storage, mempool).
    // Require full consumption to avoid silently accepting malformed/extended payloads.
    return consumed == data.size();
}

bool TransactionSerializer::Deserialize(Transaction& tx, const std::string& hex) {
    auto data = FromHex(hex);
    if (data.empty()) return false;
    return Deserialize(tx, data);
}

} // namespace dinero

