#pragma once
// What the wallet wizard says when the node refuses to create a wallet.
// Pure, so it is tested without a window (tests/test_wallet_layout.cpp).
#include <QString>

namespace WalletCreateRefusal {

struct Shown {
    QString text;
    bool retry = false;   // offer "Retry": waiting will make it work
    bool calm = false;    // nothing broke: amber, not the red error box
};

// `code` and `error` are the node's error_code and error for wallet.createhd.
inline Shown shown(const QString& code, const QString& error, const QString& rollbackError) {
    if (code == "snapshot_bootstrap_pending")
        return {"Your node is still loading its starting snapshot. No wallet was created. Retry in a few minutes.", true, true};
    if (code == "orchard_history_pending")
        return {"Orchard history is still downloading and verifying. No wallet was created. Retry after sync progresses.", true, true};
    // This node can never credit a new wallet. The node's own sentence says
    // why and where to create it instead; waiting does not help, so no Retry.
    if (code == "native_wallet_unservable" && !error.trimmed().isEmpty()) return {error, false, true};
    return {"❌ Error generating seed: " + error + (rollbackError.isEmpty() ? QString() : "\n\nRollback warning:\n" + rollbackError),
            false, false};
}

}  // namespace WalletCreateRefusal
