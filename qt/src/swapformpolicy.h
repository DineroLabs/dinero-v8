#pragma once
// Pure rules behind the Swap tab (DIN <-> BTC atomic swaps), kept out of the
// widget so they are unit-tested: exact amount parsing (no floating point),
// what blocks an offer, how swap states read, when cancel is allowed, and the
// review text a user sees before money is committed.
#include "shieldedtransferpolicy.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

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

// How this network's Bitcoin addresses start, for messages.
inline QString btcAddressStarts(const QString& btcHrp) {
    return btcHrp + (btcHrp == "bc" ? "1, 1 or 3" : "1, m, n or 2");
}

// A legacy Bitcoin address (P2PKH / P2SH) of this network with a good checksum.
// Exchanges and older wallets still hand these out.
inline bool isLegacyBtcAddress(const QString& address, const QString& btcHrp) {
    static const QByteArray alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    const QByteArray text = address.toLatin1();
    if (text.size() < 26 || text.size() > 35 || address != QString::fromLatin1(text)) return false;
    QByteArray bytes(25, '\0');  // version + 20-byte hash + 4-byte checksum, big-endian
    for (char c : text) {
        int carry = alphabet.indexOf(c);
        if (carry < 0) return false;
        for (int i = bytes.size() - 1; i >= 0; --i) {
            carry += 58 * static_cast<unsigned char>(bytes[i]);
            bytes[i] = static_cast<char>(carry & 0xff);
            carry >>= 8;
        }
        if (carry) return false;  // longer than an address
    }
    int zeros = 0, ones = 0;
    while (zeros < bytes.size() && bytes[zeros] == '\0') ++zeros;
    while (ones < text.size() && text[ones] == '1') ++ones;
    if (zeros != ones) return false;  // each leading 1 is exactly one leading zero byte
    const auto sha = [](const QByteArray& d) { return QCryptographicHash::hash(d, QCryptographicHash::Sha256); };
    if (sha(sha(bytes.left(21))).left(4) != bytes.right(4)) return false;
    const int version = static_cast<unsigned char>(bytes[0]);
    return btcHrp == "bc" ? (version == 0x00 || version == 0x05) : (version == 0x6f || version == 0xc4);
}

// Mirrors what the node accepts as a Bitcoin payout address: segwit (bc1…) by
// its prefix, legacy (1… / 3…) checked in full so a typo is caught here.
inline bool looksLikeBtcAddress(const QString& address, const QString& btcHrp) {
    const QString a = address.trimmed();
    return a.toLower().startsWith(btcHrp + "1") || isLegacyBtcAddress(a, btcHrp);
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
    if (btcAddress.trimmed().isEmpty()) { r.blocker = "Enter your Bitcoin address (where you receive the BTC)"; return r; }
    if (!looksLikeBtcAddress(btcAddress, btcHrp)) {
        r.blocker = "That is not a Bitcoin address of this network (it starts with " + btcAddressStarts(btcHrp) + ")";
        return r;
    }
    return r;
}

// Price in sat per DIN, rounded down; "" if undefined.
inline QString rate(qint64 dinUna, qint64 btcSat) {
    if (dinUna <= 0 || btcSat < 0) return {};
    const long double satPerDin = static_cast<long double>(btcSat) * 100000000.0L / dinUna;
    return QString::number(static_cast<qint64>(satPerDin)) + " sat per DIN";
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
    if (t.startsWith("dinswap1o")) return "offer";
    if (t.startsWith("dinswap1a")) return "accept";
    return {};
}

// Only before this side has locked anything (mirrors swap.cancel).
inline bool canCancel(const QString& state) { return state == "accepted" || state == "offer-sent"; }

inline bool finished(const QString& state) {
    return state == "done" || state == "refunded" || state == "aborted" || state == "lost";
}

inline QString stateLabel(const QString& state, const QString& role) {
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
