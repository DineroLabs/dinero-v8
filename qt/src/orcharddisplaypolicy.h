#pragma once
// How Orchard money and Orchard transactions are worded across the app: the
// Orchard page, the Wallet page and the History page. Pure, so it is tested
// without a window (tests/test_orchard_tab.cpp).
#include "orchardcontract.h"
#include "orchardflow.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <optional>

namespace OrchardDisplay {

// The last Orchard balance the node reported for the selected account.
// `current` is false once something newer is known to exist that this figure
// does not cover yet (a new block, a payment of our own, a refused read).
struct KnownBalance {
    quint64 confirmedUna = 0;
    quint64 height = 0;      // the wallet checkpoint the figure was read at
    bool caughtUp = false;   // the node's own "account is at the tip" flag
    bool current = false;
};

// The Orchard page. A balance that was read once is never replaced by
// "Unknown" just because the node is busy: it is shown with the block it
// belongs to. "Unknown" is for a balance that was never read.
inline QString balanceText(const std::optional<KnownBalance>& b) {
    if (!b) return QStringLiteral("Unknown");
    const QString amount = OrchardFlow::FormatDin(b->confirmedUna) + " DIN";
    if (!b->current) return amount + QString(" at block %1 — updating").arg(b->height);
    return amount + " confirmed" + (b->caughtUp ? QString() : QString(" — synchronizing"));
}
inline QString balanceTip(const std::optional<KnownBalance>& b) {
    if (!b) return QStringLiteral("The node has not reported this account's balance yet.");
    if (!b->current)
        return QString("The last balance the node reported, at block %1. The wallet is catching up with newer blocks; "
                       "the figure refreshes by itself.").arg(b->height);
    return {};
}

// The Wallet page's "Shielded / private" column.
struct WalletPrivate {
    QString orchardText;     // the "Orchard:" value
    QString orchardTip;
    bool showRetired = false;
    QString retiredText;     // the old shielded pool, only when it holds something
    double countedDin = 0.0; // what the Orchard side adds to the wallet total
    bool orchardKnown = false;
};
inline QString groupedDin(double value) {
    return QLocale(QLocale::English).toString(value, 'f', 8) + " DIN";
}
// `orchardOffered`: this build has the Orchard page at all.
inline WalletPrivate walletPrivate(bool orchardOffered, const std::optional<KnownBalance>& orchard, double retiredDin,
                                   bool retiredLocked) {
    WalletPrivate v;
    if (!orchardOffered) {
        v.orchardText = QStringLiteral("—");
        v.orchardTip = QStringLiteral("This copy of Dinero was built without the Orchard page.");
    } else if (!orchard) {
        v.orchardText = QStringLiteral("Reading…");
        v.orchardTip = QStringLiteral("The Orchard balance has not been reported yet. It is not counted in the total until it is.");
    } else {
        v.orchardKnown = true;
        v.countedDin = static_cast<double>(orchard->confirmedUna) / 100000000.0;
        v.orchardText = groupedDin(v.countedDin) + (orchard->current ? QString() : QString(" (block %1)").arg(orchard->height));
        v.orchardTip = orchard->current
            ? QStringLiteral("Confirmed Orchard balance. Counted in the total above.")
            : QString("The last Orchard balance the node reported, at block %1. Counted in the total above.").arg(orchard->height);
    }
    v.showRetired = retiredDin > 0.0;
    if (v.showRetired) v.retiredText = groupedDin(retiredDin) + (retiredLocked ? QString(" (retired, locked)") : QString());
    return v;
}

// The History page: how one row's type is shown.
struct HistoryType {
    QString label;      // with its icon
    QString key;        // what the View filter compares: sent, received, mined, shield, unshield, or the raw type
    QString color;
    bool known = true;
};
inline HistoryType historyType(QString type) {
    type = type.toLower();
    if (type == "sent" || type == "send") return {QString::fromUtf8("📤 Send"), "sent", "#ff6b6b"};
    if (type == "received" || type == "receive") return {QString::fromUtf8("📥 Receive"), "received", "#51cf66"};
    if (type == "mined" || type == "generate" || type == "immature" || type == "mining" || type == "coinbase")
        return {QString::fromUtf8("⛏️ Mined"), "mined", "#339af0"};
    if (type == "shield") return {QString::fromUtf8("🛡️ Shield (into Orchard)"), "shield", "#b197fc"};
    if (type == "unshield") return {QString::fromUtf8("🔓 Unshield (out of Orchard)"), "unshield", "#b197fc"};
    return {QString::fromUtf8("❓ ") + type, type, "#868e96", false};
}
// The page asks for the newest `count` rows. What it says under the table.
inline QString historyFooter(int shown, int asked, const QString& filterLabel) {
    const QString what = filterLabel.compare("All", Qt::CaseInsensitive) == 0 ? QString("transactions")
                                                                             : filterLabel.toLower() + " transactions";
    if (shown >= asked) return QString("Showing the newest %1 %2. Older ones exist: use Show older.").arg(shown).arg(what);
    return QString("Showing all %1 %2.").arg(shown).arg(what);
}

// The Orchard page: everything known about one saved operation. `tx` is the
// node's blockchain.gettransaction answer for its transaction, when there is
// one; `txProblem` says why there is none.
inline QStringList operationDetail(const OrchardContract::Operation& op, const std::optional<QJsonObject>& tx,
                                   const QString& txProblem) {
    QStringList lines;
    lines << "Kind: " + (op.completionMethod == OrchardContract::kFinishShield ? QString("Shield: public DIN moved into Orchard")
                         : op.completionMethod == OrchardContract::kFinishSpend ? QString("Payment from Orchard: private send or unshield")
                                                                                 : QString("not recorded for this request"));
    lines << "State: " + (op.outcome == "confirmed" ? QString("Confirmed in block %1").arg(op.height)
                          : op.outcome == "conflicted" ? QString("Conflicted: another transaction used the same funds (seen at block %1)").arg(op.height)
                          : op.durableState == "signed" ? QString("Signed and sent, not confirmed yet")
                                                        : QString("Queued: its proof is not finished"));
    lines << "Operation: " + op.operationId;
    lines << (op.txid.isEmpty() ? QString("Transaction: none yet") : "Transaction: " + op.txid);
    if (tx) {
        const auto& t = *tx;
        const QString hash = t.value("blockhash").toString();
        if (t.contains("blockheight"))
            lines << QString("Block: %1").arg(t.value("blockheight").toVariant().toLongLong()) + (hash.isEmpty() ? QString() : " (" + hash + ")");
        if (t.contains("confirmations")) lines << QString("Confirmations: %1").arg(t.value("confirmations").toInt());
        const QString way = t.value("orchard_value_direction").toString();
        const QString moved = t.value("orchard_value_din").toString();
        if (way == "into_orchard") lines << "Moved into Orchard: " + moved + " DIN";
        else if (way == "out_of_orchard") lines << "Moved out of Orchard: " + moved + " DIN";
        else if (way == "within_orchard") lines << "Stayed inside Orchard: the amount is private";
        if (t.contains("fee_din")) lines << "Network fee: " + t.value("fee_din").toString() + " DIN";
        if (t.contains("total_input_value_din"))
            lines << QString("Public coins spent: %1 DIN (%2)").arg(t.value("total_input_value_din").toString())
                         .arg(t.value("input_count").toInt() == 1 ? QString("1 coin") : QString("%1 coins").arg(t.value("input_count").toInt()));
        if (t.contains("total_output_value_din") && t.value("output_count").toInt() > 0)
            lines << QString("Public coins created: %1 DIN (%2)").arg(t.value("total_output_value_din").toString())
                         .arg(t.value("output_count").toInt() == 1 ? QString("1 coin, your change") : QString("%1 coins").arg(t.value("output_count").toInt()));
        if (t.contains("size")) lines << QString("Size: %1 bytes").arg(t.value("size").toVariant().toLongLong());
    } else if (!op.txid.isEmpty()) {
        lines << "Details from the chain: " + (txProblem.isEmpty() ? QString("reading…") : txProblem);
    }
    return lines;
}

}  // namespace OrchardDisplay
