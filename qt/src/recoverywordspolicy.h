#pragma once
// The reminder that a wallet's recovery words were never confirmed as written
// down, and what is sent when the user confirms. Pure, so it is unit-tested
// (tests/test_recovery_words_policy.cpp).
//
// The words are shown once when a wallet is created. A user who closes that
// window early (wallet creation can take minutes on a first run) has a wallet
// and has never seen its only backup. The node can show the words again
// (wallet.exportseed) and records the confirmation
// (wallet.acknowledgeseedbackup); the reminder is what tells the user.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace RecoveryWordsPolicy {

// `walletInfo`: the node's wallet.getinfo answer. A refusal, no wallet, and a
// wallet that has no words to show (an old raw-seed wallet) get no reminder:
// it would offer something the node cannot do.
inline bool reminderNeeded(const QJsonObject& walletInfo) {
    if (walletInfo.contains("error")) return false;
    if (!walletInfo.value("hd_enabled").toBool(false)) return false;
    if (!walletInfo.value("mnemonic_backup_available").toBool(false)) return false;
    return walletInfo.value("backup_required").toBool(false);
}

inline QString reminderText() {
    return QStringLiteral("<b>Your recovery words are not confirmed.</b> They are the only backup of this wallet: "
                          "without them, a lost or broken computer means lost coins. Show them, write them down on "
                          "paper, and confirm.");
}
inline QString reminderButton() { return QStringLiteral("Show my recovery words"); }

// `shown`: the node's wallet.exportseed answer the words came from.
inline QString confirmButton(const QJsonObject& shown) {
    return shown.value("passphrase_required").toBool(false)
               ? QStringLiteral("I have written down these words and my passphrase")
               : QStringLiteral("I have written these words down");
}
// wallet.acknowledgeseedbackup [mnemonic, passphrase_backed_up]: the node
// checks the words against the wallet's own before it records anything.
inline QJsonArray confirmParams(const QString& words, const QJsonObject& shown) {
    return QJsonArray{words.simplified(), shown.value("passphrase_required").toBool(false)};
}

inline bool confirmationRecorded(const QJsonObject& response, const QString& walletName) {
    return !walletName.isEmpty() &&
           response.value("success").toBool(false) &&
           response.value("backup_acknowledged").toBool(false) &&
           response.value("wallet_name").toString() == walletName;
}

}  // namespace RecoveryWordsPolicy
