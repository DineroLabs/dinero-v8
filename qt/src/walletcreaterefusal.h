#pragma once
// What the wallet wizard says when the node refuses to create a wallet.
// Pure, so it is tested without a window (tests/test_wallet_layout.cpp).
#include <QString>
#include <QCoreApplication>

namespace WalletCreateRefusal {

struct Shown {
    QString text;
    bool retry = false;   // offer "Retry": waiting will make it work
    bool calm = false;    // nothing broke: amber, not the red error box
};

// The node guarantees that these two typed refusals create no wallet. A
// transport failure, an English message, or an unknown code cannot authorize
// an automatic second creation request.
inline bool mayAutoRetry(const QString& code, bool retryable) {
    return retryable && (code == "snapshot_bootstrap_pending" || code == "orchard_history_pending");
}

// `code` and `error` are the node's error_code and error for wallet.createhd.
inline Shown shown(const QString& code, const QString& error, const QString& rollbackError) {
    if (code == "snapshot_bootstrap_pending")
        return {QCoreApplication::translate("CreateSeedPage", "Your node is still loading its starting snapshot. No wallet was created. Retry in a few minutes."), true, true};
    if (code == "orchard_history_pending")
        return {QCoreApplication::translate("CreateSeedPage", "Orchard history is still downloading and verifying. No wallet was created."), true, true};
    // This node can never credit a new wallet. The node's own sentence says
    // why and where to create it instead; waiting does not help, so no Retry.
    if (code == "native_wallet_unservable" && !error.trimmed().isEmpty()) return {error, false, true};
    return {QCoreApplication::translate("CreateSeedPage", "Error creating wallet: ") + error + (rollbackError.isEmpty() ? QString() : QStringLiteral("\n\n") + QCoreApplication::translate("CreateSeedPage", "Rollback warning:") + QStringLiteral("\n") + rollbackError),
            false, false};
}

// ConnectionManager delivers some node refusals through its string-only
// error callback. Preserve the node's specific explanation in that path too.
inline Shown shownFromTransportError(const QString& error, const QString& rollbackError) {
    if (error.contains("This node cannot show money in a new wallet", Qt::CaseInsensitive) &&
        error.contains("data was started from an older snapshot", Qt::CaseInsensitive) &&
        error.contains("No wallet was created", Qt::CaseInsensitive))
        return shown("native_wallet_unservable", error, rollbackError);
    if (error.contains("still loading its starting snapshot", Qt::CaseInsensitive))
        return shown("snapshot_bootstrap_pending", error, rollbackError);
    if (error.contains("Orchard history is not ready; wallet was not created", Qt::CaseInsensitive))
        return shown("orchard_history_pending", error, rollbackError);
    return shown({}, error, rollbackError);
}

}  // namespace WalletCreateRefusal
