#pragma once
// A wallet that was closed for a while has to read the blocks it missed
// before the node will report its balance. The node says how far it is in
// wallet.getsyncstate; the Wallet page says it instead of only "Balance
// unavailable". Pure, so it is tested without a window
// (tests/test_wallet_layout.cpp).
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QString>
#include <cmath>
#include <optional>

namespace WalletCatchUp {

struct Progress {
    quint64 scanned = 0;
    quint64 target = 0;
};

// Progress is shown only when the node says, in so many words, that the
// wallet is catching up and gives both heights as whole numbers that make
// sense. Anything else is "not said": nothing is invented.
inline std::optional<Progress> parse(const QJsonValue& reply) {
    if (!reply.isObject()) return std::nullopt;
    const QJsonObject o = reply.toObject();
    if (!o.value("is_syncing").isBool() || !o.value("is_syncing").toBool()) return std::nullopt;
    if (o.contains("wallet_loaded") && !o.value("wallet_loaded").toBool()) return std::nullopt;
    const auto height = [&](const char* key) -> std::optional<quint64> {
        const QJsonValue v = o.value(key);
        if (!v.isDouble()) return std::nullopt;
        const double d = v.toDouble();
        if (!(d >= 0.0) || d > 4294967295.0 || d != std::floor(d)) return std::nullopt;
        return static_cast<quint64>(d);
    };
    const auto scanned = height("last_scanned_height");
    const auto target = height("target_height");
    if (!scanned || !target || *target == 0 || *scanned > *target) return std::nullopt;
    return Progress{*scanned, *target};
}

inline QString banner(const Progress& p) {
    const QLocale en(QLocale::English);
    const quint64 left = p.target - p.scanned;
    return QString("This wallet is catching up with the chain: block %1 of %2 (%3 to go). "
                   "Its balance appears when it has finished; sending is paused until then.")
        .arg(en.toString(p.scanned), en.toString(p.target), en.toString(left));
}

}  // namespace WalletCatchUp
