// Copyright (c) 2026 Dinero Labs.

#include "segwitaddress.h"

#include <QByteArray>
#include <QVector>

namespace segwitaddress {

namespace {

constexpr quint32 kBech32mConst = 0x2bc830a3;
/// Payout types the SV2 coinbase accepts: Taproot (v1) and P2MR (v3,
/// include/wallet/p2mr_address.h). Both carry a 32-byte program.
constexpr int kTaprootVersion = 1;
constexpr int kP2mrVersion = 3;

quint32 polymod(const QVector<int>& values) {
    static const quint32 gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    quint32 chk = 1;
    for (int v : values) {
        const quint32 top = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ static_cast<quint32>(v);
        for (int i = 0; i < 5; ++i) {
            if ((top >> i) & 1) chk ^= gen[i];
        }
    }
    return chk;
}

}  // namespace

QString addressToScriptHex(const QString& address) {
    static const QString charset = QStringLiteral("qpzry9x8gf2tvdw0s3jn54khce6mua7l");
    const QString addr = address.trimmed().toLower();
    const int sep = addr.lastIndexOf('1');
    if (sep < 1 || sep + 7 > addr.length()) return {};

    const QString hrp = addr.left(sep);
    if (hrp != "din" && hrp != "tdin" && hrp != "rdin") return {};

    QVector<int> data5;
    data5.reserve(addr.length() - sep - 1);
    for (int i = sep + 1; i < addr.length(); ++i) {
        const int v = charset.indexOf(addr.at(i));
        if (v < 0) return {};
        data5.append(v);
    }
    if (data5.size() < 7) return {};  // version + program + checksum

    // BIP-350: v1+ addresses carry a bech32m checksum. Without this check a
    // single mistyped character still decodes to a well-formed script that
    // nobody can spend, and the coinbase would pay it.
    QVector<int> checked;
    for (QChar c : hrp) checked.append(c.toLatin1() >> 5);
    checked.append(0);
    for (QChar c : hrp) checked.append(c.toLatin1() & 31);
    checked += data5;
    if (polymod(checked) != kBech32mConst) return {};

    const int version = data5.first();
    if (version != kTaprootVersion && version != kP2mrVersion) return {};

    // Drop the version and checksum, then regroup 5-bit values into bytes.
    QByteArray program;
    int acc = 0;
    int bits = 0;
    for (int i = 1; i < data5.size() - 6; ++i) {
        acc = ((acc << 5) | data5.at(i)) & 0xfff;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            program.append(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    // Leftover padding must be shorter than a group and all zero (BIP-173).
    if (bits >= 5 || ((acc << (8 - bits)) & 0xff) != 0) return {};
    if (program.size() != 32) return {};

    QByteArray script;
    script.append(static_cast<char>(0x50 + version));  // OP_1 / OP_3
    script.append(static_cast<char>(0x20));
    script.append(program);
    return QString::fromLatin1(script.toHex());
}

}  // namespace segwitaddress
