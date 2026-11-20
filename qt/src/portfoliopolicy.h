#pragma once
// Portfolio tab: what the node's answers mean, in words. Pure, so it is
// tested without a window (tests/test_portfolio_tab.cpp).
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace PortfolioPolicy {

inline QString formatBtc(qint64 sat) {
    return QString::number(sat / 100000000) + "." + QString::number(sat % 100000000).rightJustified(8, '0') + " BTC";
}

// "0.0002", "0,0002", ".5", "1" -> satoshi. nullopt for anything else: more
// than 8 decimals, letters, a sign, nothing, zero, or beyond 21 million.
inline std::optional<qint64> parseBtc(QString text) {
    text = text.trimmed().replace(',', '.');
    if (text.isEmpty() || text.count('.') > 1) return std::nullopt;
    const QString whole = text.section('.', 0, 0), part = text.contains('.') ? text.section('.', 1, 1) : QString();
    if (whole.isEmpty() && part.isEmpty()) return std::nullopt;
    if (whole.size() > 8 || part.size() > 8) return std::nullopt;
    for (const QChar c : whole + part) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    const qint64 sat = whole.toLongLong() * 100000000 + part.leftJustified(8, '0').toLongLong();
    if (sat <= 0 || sat > 2100000000000000LL) return std::nullopt;
    return sat;
}

struct CoinRow {
    QString coin;
    QString amount;    // empty: nothing to show yet
    QString note;      // how the amount is known, or why there is none
    bool available{true};
};

// One row per coin the app knows of. A coin it cannot serve is listed with
// the reason, not hidden. `dinTotal` < 0: not known yet.
// `withDinero` false: the other coins only, for the Wallet page, which shows
// the DIN balance itself.
inline QList<CoinRow> coinRows(double dinTotal, const QJsonObject& btcStatus, const QJsonObject& btcBalance,
                               const QString& btcProblem, bool withDinero = true) {
    QList<CoinRow> rows;
    if (withDinero) {
        rows.append({"Dinero (DIN)", dinTotal < 0 ? QString() : QString::number(dinTotal, 'f', 8) + " DIN",
                     dinTotal < 0 ? "Waiting for your node" : "Counted by your own node", true});
    }
    CoinRow btc{"Bitcoin (BTC)", {}, {}, true};
    if (btcStatus.isEmpty()) {
        btc.note = "Waiting for your node";
    } else if (btcStatus.value("enabled").toBool()) {
        if (btcProblem.contains("catching up")) {
            btc.note = "Getting ready: collecting Bitcoin's block headers (about a minute the first time)";
        } else if (!btcProblem.isEmpty()) {
            btc.note = "Not read: " + btcProblem;
        } else if (btcBalance.isEmpty()) {
            btc.note = "Reading";
        } else {
            btc.amount = formatBtc(btcBalance.value("sat").toVariant().toLongLong());
            btc.note = "Proven against Bitcoin block headers";
        }
    } else if (btcStatus.value("offered").toBool()) {
        btc.note = "Not turned on";
    } else {
        const QString why = btcStatus.value("why_not").toString("Not available on this node");
        const bool notRunning = why.startsWith("the Bitcoin wallet is not running on this node") || why.startsWith("swaps are disabled");
        btc.available = notRunning;   // it can be switched on below
        btc.note = notRunning ? "Not switched on" : why;
    }
    rows.append(btc);
    rows.append({"Zcash (ZEC)", {}, "Not available yet", false});
    rows.append({"Bitcoin Cash (BCH)", {}, "Not available yet", false});
    return rows;
}

// The node does not run the Bitcoin wallet at all (it is started with the
// node, like swaps): what the Wallet page says and offers. `wanted`: the
// user's own switch (SwapLaunchPolicy::bitcoinWalletKey). `nodeMessage`: what
// the node answered to btc.status.
struct BitcoinStartView {
    bool notRunning{false};  // this view applies
    QString text;
    bool canSwitchOn{false};
};

inline BitcoinStartView bitcoinStartView(bool wanted, const QString& nodeMessage) {
    BitcoinStartView v;
    const QString prefix = "the Bitcoin wallet is not running on this node";
    // An older node answers "swaps are disabled" for every request of this kind.
    if (!nodeMessage.startsWith(prefix) && !nodeMessage.startsWith("swaps are disabled")) return v;
    v.notRunning = true;
    QString reason = nodeMessage.startsWith(prefix) ? nodeMessage.mid(prefix.size()) : QString();
    if (reason.startsWith(": ")) reason = reason.mid(2);
    else reason.clear();
    if (reason.startsWith("swap.btc_wallet=1")) reason.clear();
    if (!wanted) {
        v.canSwitchOn = true;
        v.text = "Hold, receive and send Bitcoin in this wallet, with the recovery words you already have, and without "
                 "any other Bitcoin software. Switching it on lets Dinero follow the Bitcoin network.";
    } else if (reason.contains("built with DINERO_SWAP_LIGHT_BTC")) {
        v.text = "The Bitcoin wallet is switched on, but this copy of Dinero was built without it.";
    } else if (!reason.isEmpty()) {
        v.text = "The Bitcoin wallet is switched on, but cannot start here: " + reason;
    } else {
        v.text = "The Bitcoin wallet is switched on and starts the next time Dinero starts: quit Dinero (File → Quit) "
                 "and open it again.";
    }
    return v;
}

// A payment before it is sent (btc.preview / btc.bumpfee), line by line.
inline QStringList paymentReview(const QJsonObject& p) {
    const qint64 amount = p.value("amount_sat").toVariant().toLongLong(), fee = p.value("fee_sat").toVariant().toLongLong();
    const qint64 change = p.value("change_sat").toVariant().toLongLong(), dust = p.value("dust_to_fee_sat").toVariant().toLongLong();
    QStringList lines;
    lines << "To: " + p.value("to").toString();
    lines << "They receive: " + formatBtc(amount);
    lines << QString("Network fee: %1 (%2 sat per byte)").arg(formatBtc(fee)).arg(p.value("feerate_sat_per_vb").toDouble(), 0, 'f', 1);
    if (dust > 0) lines << QString("The fee includes %1 sat of change too small to send back.").arg(dust);
    lines << "Leaves your wallet: " + formatBtc(amount + fee);
    lines << (change > 0 ? "Comes back to you as change: " + formatBtc(change) : QString("No change: nothing comes back."));
    if (p.contains("replaces")) lines << "Replaces the payment sent before; only one of the two can be confirmed.";
    return lines;
}

struct TxRow {
    QString what, amount, state, txid;
    bool canRaiseFee{false};
};

inline QList<TxRow> historyRows(const QJsonArray& txs) {
    QList<TxRow> rows;
    for (const auto& entry : txs) {
        const QJsonObject t = entry.toObject();
        TxRow r;
        const bool received = t.value("received").toBool();
        const int confirmations = t.value("confirmations").toInt();
        r.what = received ? "Received" : "Sent";
        r.amount = formatBtc(t.value("amount_sat").toVariant().toLongLong());
        if (!received) r.amount += QString(" (fee %1 sat)").arg(t.value("fee_sat").toVariant().toLongLong());
        r.state = confirmations > 0 ? QString("Confirmed (%1)").arg(confirmations) : received ? "Waiting" : "Sent, not confirmed yet";
        r.txid = t.value("txid").toString();
        r.canRaiseFee = !received && confirmations == 0;
        rows.append(r);
    }
    return rows;
}

}  // namespace PortfolioPolicy
