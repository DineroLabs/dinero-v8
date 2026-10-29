#pragma once
// Pure rules behind the Swap tab (DIN <-> BTC / ZEC atomic swaps), kept out of the
// widget so they are unit-tested: exact amount parsing (no floating point),
// what blocks an offer, how swap states read, when cancel is allowed, and the
// review text a user sees before money is committed.
#include "shieldedtransferpolicy.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace SwapFormPolicy {

// Both DIN (una) and BTC (sat) have 8 decimals.
inline QString formatUnits(qint64 v) {
    return QString::number(v / 100000000) + "." + QString::number(v % 100000000).rightJustified(8, '0');
}

inline QString btcHrpForChain(const QString& chain) {
    const QString c = chain.toLower();
    if (c == "main" || c == "mainnet") return "bc";
    if (c == "test" || c == "testnet") return "tb";
    return "bcrt";
}

struct OfferReview {
    QString blocker;  // first thing to fix; empty when the offer can be created
    qint64 dinUna = 0;
    qint64 btcSat = 0;
};

// Mirrors what swap.offer refuses, so the button never promises more.
inline OfferReview reviewOffer(const QString& dinText, const QString& btcText, const QString& btcAddress,
                               const QString& btcHrp) {
    OfferReview r;
    if (dinText.trimmed().isEmpty()) { r.blocker = "Enter the DIN amount you sell"; return r; }
    if (!ShieldedTransferPolicy::parseDinToUna(dinText, &r.dinUna) || r.dinUna <= 0) {
        r.blocker = "Enter a valid DIN amount (up to 8 decimals)";
        return r;
    }
    if (btcText.trimmed().isEmpty()) { r.blocker = "Enter the BTC amount you want"; return r; }
    if (!ShieldedTransferPolicy::parseDinToUna(btcText, &r.btcSat) || r.btcSat <= 0) {
        r.blocker = "Enter a valid BTC amount (up to 8 decimals)";
        return r;
    }
    if (r.btcSat < 10000) { r.blocker = "BTC amount is too small (at least 0.00010000 BTC)"; return r; }
    const QString addr = btcAddress.trimmed().toLower();
    if (addr.isEmpty()) { r.blocker = "Enter your Bitcoin address (where you receive the BTC)"; return r; }
    if (!addr.startsWith(btcHrp + "1")) {
        r.blocker = QString("Enter a Bitcoin %1… address (P2WPKH, P2WSH or Taproot)").arg(btcHrp + "1");
        return r;
    }
    return r;
}

// Price in sat per DIN (zat per DIN for Zcash), rounded down; "" if undefined.
inline QString rate(qint64 dinUna, qint64 btcSat, const QString& unit = "sat") {
    if (dinUna <= 0 || btcSat < 0) return {};
    const long double satPerDin = static_cast<long double>(btcSat) * 100000000.0L / dinUna;
    return QString::number(static_cast<qint64>(satPerDin)) + " " + unit + " per DIN";
}

// --- Zcash (docs/design/din-zec-atomic-swaps.md) ---------------------------------
// Mirrors what dinerod refuses: an amount covering 20 claim fees, a
// transparent address of the node's network.
inline constexpr qint64 kZecSpendFeeZat = 20000;
inline constexpr qint64 kZecMinZat = 20 * kZecSpendFeeZat;  // 0.004 ZEC
inline constexpr int kZecDepositConfirmations = 10;

inline QStringList zecAddressPrefixes(const QString& chain) {
    const QString c = chain.toLower();
    if (c == "main" || c == "mainnet") return {"t1", "t3"};
    return {"tm", "t2"};  // testnet and regtest
}

inline bool looksLikeZecAddress(const QString& address, const QString& chain) {
    const QString a = address.trimmed();
    static const QString base58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (a.size() != 35) return false;
    for (const QChar ch : a) {
        if (!base58.contains(ch)) return false;
    }
    for (const auto& p : zecAddressPrefixes(chain)) {
        if (a.startsWith(p)) return true;
    }
    return false;
}

inline OfferReview reviewZecOffer(const QString& dinText, const QString& zecText, const QString& zecAddress,
                                  const QString& chain) {
    OfferReview r;
    if (dinText.trimmed().isEmpty()) { r.blocker = "Enter the DIN amount you sell"; return r; }
    if (!ShieldedTransferPolicy::parseDinToUna(dinText, &r.dinUna) || r.dinUna <= 0) {
        r.blocker = "Enter a valid DIN amount (up to 8 decimals)";
        return r;
    }
    if (zecText.trimmed().isEmpty()) { r.blocker = "Enter the ZEC amount you want"; return r; }
    if (!ShieldedTransferPolicy::parseDinToUna(zecText, &r.btcSat) || r.btcSat <= 0) {
        r.blocker = "Enter a valid ZEC amount (up to 8 decimals)";
        return r;
    }
    if (r.btcSat < kZecMinZat) { r.blocker = "ZEC amount is too small (at least 0.004 ZEC)"; return r; }
    if (zecAddress.trimmed().isEmpty()) { r.blocker = "Enter your Zcash address (where you receive the ZEC)"; return r; }
    if (!looksLikeZecAddress(zecAddress, chain)) {
        r.blocker = "Enter a transparent Zcash address (" + zecAddressPrefixes(chain).join("… or ") + "…)";
        return r;
    }
    return r;
}

inline bool isZecOffer(const QString& text) { return text.trimmed().startsWith("dinswap2o"); }

// Bob's deposit as swap.status / swap.list report it.
struct DepositView {
    qint64 dueZat = 0;
    qint64 seenZat = 0;       // at least 1 confirmation
    qint64 confirmedZat = 0;  // at least kZecDepositConfirmations
    int confirmations = 0;    // of the newest payment
    bool reachable = true;
    bool ready = false;       // covered at kZecDepositConfirmations: the reply can be sent
    QString hint;
};

inline DepositView depositView(const QJsonObject& s) {
    DepositView v;
    v.dueZat = s.value("deposit_due_zat").toVariant().toLongLong();
    v.reachable = !s.contains("deposit_status");
    v.seenZat = s.value("deposit_seen_zat").toVariant().toLongLong();
    v.confirmedZat = s.value("deposit_confirmed_zat").toVariant().toLongLong();
    v.confirmations = s.value("deposit_confirmations").toInt();
    v.ready = v.reachable && v.dueZat > 0 && v.confirmedZat >= v.dueZat;
    if (!v.reachable) v.hint = "your node cannot reach its Zcash node; the deposit address still stands";
    else if (v.ready) v.hint = {};
    else if (v.seenZat == 0) v.hint = "pay " + formatUnits(v.dueZat) + " ZEC to the deposit address";
    else if (v.seenZat < v.dueZat) v.hint = formatUnits(v.dueZat - v.seenZat) + " ZEC still due";
    else v.hint = QString("wait for %1 more Zcash confirmations of the deposit")
                      .arg(qMax(1, kZecDepositConfirmations - v.confirmations));
    return v;
}

inline QString timeLeft(qint64 seconds) {
    if (seconds <= 0) return "passed";
    const qint64 d = seconds / 86400, h = (seconds % 86400) / 3600, m = (seconds % 3600) / 60;
    if (d > 0) return QString("%1 d %2 h").arg(d).arg(h);
    if (h > 0) return QString("%1 h %2 min").arg(h).arg(m);
    return QString("%1 min").arg(qMax<qint64>(m, 1));
}

inline QString kindOfText(const QString& text) {
    const QString t = text.trimmed();
    if (t.startsWith("dinswap1o") || t.startsWith("dinswap2o")) return "offer";
    if (t.startsWith("dinswap1a")) return "accept";
    return {};
}

// Only before this side has locked anything (mirrors swap.cancel).
inline bool canCancel(const QString& state) {
    return state == "accepted" || state == "offer-sent" || state == "awaiting-deposit" || state == "deposit-confirmed";
}

inline bool finished(const QString& state) {
    return state == "done" || state == "refunded" || state == "aborted" || state == "lost";
}

inline QString stateLabelBtc(const QString& state, const QString& role) {
    const bool alice = role == "din-seller";
    if (state == "offer-sent") return "Offer sent — waiting for the buyer's accept";
    if (state == "accepted") return alice ? "Starting — locking your DIN" : "Waiting for the seller's DIN lock";
    if (state == "din-lock-broadcast") return "DIN lock sent — waiting for it to confirm";
    if (state == "din-locked") return alice ? "DIN locked — waiting for the buyer's BTC" : "DIN locked — confirming before you lock BTC";
    if (state == "btc-lock-broadcast") return "BTC lock sent — waiting for it to confirm";
    if (state == "btc-locked") return alice ? "BTC locked — claiming it" : "BTC locked — waiting for the seller to claim";
    if (state == "btc-claim-broadcast") return "BTC claimed — waiting for confirmations";
    if (state == "din-claim-broadcast") return "DIN claimed — waiting for confirmations";
    if (state == "din-refund-broadcast") return "Refunding your DIN";
    if (state == "btc-refund-broadcast") return "Refunding your BTC";
    if (state == "done") return "Done";
    if (state == "sweeping") return "Done — moving the DIN to your wallet";
    if (state == "refunded") return "Refunded — your coins are back";
    if (state == "aborted") return "Cancelled — nothing was locked";
    if (state == "lost") return "ATTENTION: the swap went wrong — see events";
    if (state.startsWith("paused")) return "Paused — unlock the wallet to continue";
    return state;
}

// The same states read for the swap's counter chain ("zec": ZEC in place of BTC).
inline QString stateLabel(const QString& state, const QString& role, const QString& chain = QString()) {
    if (state == "awaiting-deposit") return "Waiting for your ZEC deposit";
    if (state == "deposit-confirmed") return "Deposit confirmed — accept the offer to start";
    QString label = stateLabelBtc(state, role);
    if (chain == "zec") label.replace("BTC", "ZEC");
    return label;
}

inline QString localTime(qint64 unix) {
    return QDateTime::fromSecsSinceEpoch(unix).toString("yyyy-MM-dd HH:mm");
}

// The review shown before swap.accept, from swap.decode's answer.
// `blocker` is set when the offer must not be accepted.
struct AcceptReview {
    QString blocker;
    QString text;
};
inline AcceptReview reviewDecodedOffer(const QJsonObject& d, const QString& expectedNetwork, qint64 now) {
    AcceptReview r;
    if (d.value("kind").toString() != "offer") { r.blocker = "This is not a swap offer"; return r; }
    if (d.value("network").toString() != expectedNetwork) {
        r.blocker = "This offer is for " + d.value("network").toString() + ", your node is on " + expectedNetwork;
        return r;
    }
    if (!d.value("acceptable_now").toBool()) {
        r.blocker = "This offer cannot be accepted: " + d.value("reason").toString();
        return r;
    }
    const qint64 din = d.value("din_amount_una").toVariant().toLongLong();
    const qint64 btc = d.value("btc_amount_sat").toVariant().toLongLong();
    const qint64 tBtc = d.value("t_btc_unix").toVariant().toLongLong();
    const qint64 tDin = d.value("t_din_unix").toVariant().toLongLong();
    if (d.value("chain").toString() == "zec") {
        r.text = QString(
                     "You send %1 ZEC and receive %2 DIN (%3).\n\n"
                     "First you pay %4 ZEC (the amount plus a 0.0002 ZEC funding fee) into a deposit address of "
                     "your node, from any Zcash wallet. You can withdraw it until the seller locks DIN.\n"
                     "Your node locks the ZEC from the deposit after the DIN lock has %5 confirmations; it comes "
                     "back to you after %6 (in %7) if the seller never claims it.\n"
                     "The seller's DIN is locked until %8.\n\n"
                     "Your node must stay online and unlocked until the swap finishes.")
                     .arg(formatUnits(btc), formatUnits(din), rate(din, btc, "zat"), formatUnits(btc + 20000))
                     .arg(d.value("n_din").toInt())
                     .arg(localTime(tBtc), timeLeft(tBtc - now), localTime(tDin));
        return r;
    }
    r.text = QString(
                 "You send %1 BTC and receive %2 DIN (%3).\n\n"
                 "Your BTC is locked until the seller claims it, or until %4 (in %5) — then it comes back to you.\n"
                 "The seller's DIN is locked until %6.\n"
                 "You lock BTC only after the DIN lock has %7 confirmations.\n\n"
                 "Your node must stay online and unlocked until the swap finishes, "
                 "or run a watchtower (dinero-swap-tower).")
                 .arg(formatUnits(btc), formatUnits(din), rate(din, btc), localTime(tBtc), timeLeft(tBtc - now),
                      localTime(tDin))
                 .arg(d.value("n_din").toInt());
    return r;
}

}  // namespace SwapFormPolicy
