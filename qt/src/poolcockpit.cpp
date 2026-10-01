// Copyright (c) 2026 Dinero Labs.

#include "poolcockpit.h"

#include <QByteArray>
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>

#include <vector>

namespace poolcockpit {

namespace {

constexpr qint64 kMinute = 60;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kDay = 24 * kHour;
/// One status refresh is 15 s; a first sample this close to the window's
/// start still counts as covering it.
constexpr qint64 kCoverageSlackSecs = 30;

const char* const kCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr quint32 kBech32Const = 1;
constexpr quint32 kBech32mConst = 0x2bc830a3;

quint32 polymod(const std::vector<quint8>& values) {
    static const quint32 gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    quint32 chk = 1;
    for (quint8 v : values) {
        const quint32 top = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (int i = 0; i < 5; ++i) {
            if ((top >> i) & 1) chk ^= gen[i];
        }
    }
    return chk;
}

QString plural(qint64 n, const char* unit) {
    return QString("%1 %2%3").arg(n).arg(QLatin1String(unit)).arg(n == 1 ? "" : "s");
}

}  // namespace

QString formatDuration(qint64 seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < kMinute) return QString("%1 s").arg(seconds);
    if (seconds < kHour) return QString("%1 min").arg(seconds / kMinute);
    if (seconds < kDay) {
        const qint64 minutes = (seconds % kHour) / kMinute;
        return minutes == 0 ? QString("%1 h").arg(seconds / kHour)
                            : QString("%1 h %2 min").arg(seconds / kHour).arg(minutes);
    }
    const qint64 hours = (seconds % kDay) / kHour;
    return hours == 0 ? plural(seconds / kDay, "day")
                      : plural(seconds / kDay, "day") + QString(" %1 h").arg(hours);
}

QString groupDigits(qint64 value) {
    return QLocale(QLocale::English).toString(value);
}

QString groupDigitsText(const QString& text) {
    static const QRegularExpression digits(QStringLiteral("^-?[0-9]{1,18}$"));
    if (!digits.match(text).hasMatch()) return text;
    return groupDigits(text.toLongLong());
}

QString scriptToAddress(const QString& scriptHex, const QString& hrp) {
    const QByteArray script = QByteArray::fromHex(scriptHex.toLatin1());
    if (script.size() < 4 || script.toHex() != scriptHex.toLower().toLatin1() || hrp.isEmpty()) return {};
    const quint8 op = static_cast<quint8>(script[0]);
    int version = -1;
    if (op == 0x00) version = 0;
    else if (op >= 0x51 && op <= 0x60) version = op - 0x50;
    const int length = static_cast<quint8>(script[1]);
    if (version < 0 || length < 2 || length > 40 || script.size() != length + 2) return {};
    if (version == 0 && length != 20 && length != 32) return {};

    // Witness program as 5-bit groups, prefixed by the version.
    std::vector<quint8> data{static_cast<quint8>(version)};
    quint32 acc = 0;
    int bits = 0;
    for (int i = 2; i < script.size(); ++i) {
        acc = (acc << 8) | static_cast<quint8>(script[i]);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            data.push_back((acc >> bits) & 31);
        }
    }
    if (bits > 0) data.push_back((acc << (5 - bits)) & 31);

    const QByteArray h = hrp.toLower().toLatin1();
    std::vector<quint8> values;
    for (char c : h) values.push_back(static_cast<quint8>(c) >> 5);
    values.push_back(0);
    for (char c : h) values.push_back(static_cast<quint8>(c) & 31);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0);
    // BIP-350: witness v0 keeps bech32, v1+ uses bech32m.
    const quint32 mod = polymod(values) ^ (version == 0 ? kBech32Const : kBech32mConst);

    QString out = QString::fromLatin1(h) + QLatin1Char('1');
    for (quint8 d : data) out += QLatin1Char(kCharset[d]);
    for (int i = 0; i < 6; ++i) out += QLatin1Char(kCharset[(mod >> (5 * (5 - i))) & 31]);
    return out;
}

QString hrpOf(const QString& address) {
    const int separator = address.lastIndexOf(QLatin1Char('1'));
    const QString hrp = separator > 0 ? address.left(separator).toLower() : QString();
    if (hrp == "din" || hrp == "tdin" || hrp == "rdin") return hrp;
    return QStringLiteral("din");
}

QString historyCoverage(qint64 first_sample_at, qint64 now, qint64 window_secs) {
    if (first_sample_at >= 0 && first_sample_at <= now - window_secs + kCoverageSlackSecs) return {};
    const qint64 since = first_sample_at >= 0 ? first_sample_at : now;
    return "collecting since " + QDateTime::fromSecsSinceEpoch(since).toString("HH:mm");
}

}  // namespace poolcockpit
