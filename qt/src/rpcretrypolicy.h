#pragma once
#include <QJsonObject>
#include <QString>

namespace RpcRetryPolicy {
// Classify the original wire request. __replyAs only routes replies inside
// the app; it must never change whether replay is permitted.
inline bool RequiresExplicitRetry(const QJsonObject& request) {
    const auto value=request.value("method");
    if (!value.isString() || value.toString().isEmpty()) return true;
    const auto method=value.toString();
    return method == "wallet.shield" || method == "wallet.unshield" ||
        method == "wallet.transfer" || method == "wallet.sendtoaddress" ||
        method == "sendtoaddress" || method == "sendrawtransaction" ||
        method == "wallet.sendrawtransaction" || method.startsWith("wallet.covenant.") ||
        // Account creation, issuance and payments are effects. Reads also carry
        // the selected wallet binding and must not silently move to a peer.
        method.startsWith("wallet.orchard.");
}
}  // namespace RpcRetryPolicy
