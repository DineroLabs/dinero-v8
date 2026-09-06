#pragma once

#include <QString>
#include <limits>

namespace ShieldedTransferPolicy {
inline bool maySubmit(const QString& stage, bool operationRunning) {
    return !operationRunning && (stage.isEmpty() || stage == "rejected");
}

inline bool uncertainAfterRestart(const QString& stage) {
    return stage == "submitting";
}

inline bool showFundMovingControls(bool productLockout, const QString& activeHrp) {
    return !productLockout || activeHrp == "rdins";
}

inline bool parseDinToUna(const QString& input, qint64* unaOut) {
    if (!unaOut) return false;
    const QString text = input.trimmed();
    if (text.isEmpty() || text.startsWith('-') || text.startsWith('+')) return false;

    const QStringList parts = text.split('.');
    if (parts.size() > 2 || parts[0].isEmpty()) return false;
    for (const QChar c : parts[0]) {
        if (!c.isDigit()) return false;
    }
    QString fraction = parts.size() == 2 ? parts[1] : QString();
    if (parts.size() == 2 && fraction.isEmpty()) return false;
    if (fraction.size() > 8) return false;
    for (const QChar c : fraction) {
        if (!c.isDigit()) return false;
    }
    while (fraction.size() < 8) fraction.append('0');

    bool wholeOk = false;
    bool fractionOk = false;
    const quint64 whole = parts[0].toULongLong(&wholeOk);
    const quint64 fractional = fraction.isEmpty() ? 0 : fraction.toULongLong(&fractionOk);
    if (!wholeOk || (!fraction.isEmpty() && !fractionOk)) return false;
    constexpr quint64 kScale = 100000000ULL;
    const quint64 max = static_cast<quint64>(std::numeric_limits<qint64>::max());
    if (whole > max / kScale) return false;
    const quint64 value = whole * kScale + fractional;
    if (value > max) return false;
    *unaOut = static_cast<qint64>(value);
    return value > 0;
}
}  // namespace ShieldedTransferPolicy
