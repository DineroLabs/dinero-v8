#pragma once
#include "shieldedtransferpolicy.h"
#include <QList>
#include <functional>

namespace CovenantFormPolicy {
constexpr qint64 spendFeeUna = 1000;
inline QString formatUna(qint64 value) {
    return QString::number(value / 100000000) + "." +
           QString::number(value % 100000000).rightJustified(8, '0');
}
inline bool appendAmount(const QString& text, qint64& total, qint64& value) {
    if (!ShieldedTransferPolicy::parseDinToUna(text, &value) || total < 0 ||
        value > std::numeric_limits<qint64>::max() - spendFeeUna - total) return false;
    total += value;
    return true;
}
inline int delayBlocks(int duration, const QString& unit, int blockSeconds) {
    // Wall-clock durations are estimates converted with the node's current block
    // time; consensus enforces the resulting block count from funding confirmation.
    if (blockSeconds <= 0) blockSeconds = 120;
    int multiplier = 0;
    if (unit == "blocks") multiplier = 1;
    else if (unit == "hours") multiplier = qMax(1, 3600 / blockSeconds);
    else if (unit == "days") multiplier = qMax(1, 86400 / blockSeconds);
    if (multiplier == 0 || duration <= 0 || duration > 65535 / multiplier) return 0;
    return duration * multiplier;
}
struct BatchRow { QString address; QString amount; };
struct Review {
    QString blocker;          // first thing the user must fix; empty when ready
    qint64 lockedUna = 0;     // value the funding transaction locks
    qint64 deliveredUna = 0;  // what the recipients can withdraw in total
    int recipients = 0;
};
// Mirrors the checks MainWindow applies when Create Contract is pressed, so the
// live review never promises something the create path would refuse.
inline Review review(const QString& templateKey, const QString& recipient, const QString& amount,
                     const QList<BatchRow>& rows,
                     const std::function<bool(const QString&)>& isPublicAddress) {
    Review r;
    if (templateKey == "vault" || templateKey == "timelock") {
        const QString address = recipient.trimmed();
        if (address.isEmpty()) { r.blocker = "Enter a recipient address"; return r; }
        if (!isPublicAddress(address)) { r.blocker = "Enter a public din1p\u2026 or din1r\u2026 address"; return r; }
        if (amount.trimmed().isEmpty()) { r.blocker = "Enter an amount"; return r; }
        qint64 value = 0;
        if (!ShieldedTransferPolicy::parseDinToUna(amount.trimmed(), &value) || value < 0) {
            r.blocker = "Enter a valid amount (up to 8 decimals)"; return r;
        }
        if (value <= spendFeeUna) {
            r.blocker = "Amount must be more than " + formatUna(spendFeeUna) + " DIN (the reserved withdrawal fee)";
            return r;
        }
        r.lockedUna = value;
        r.deliveredUna = value - spendFeeUna;
        r.recipients = 1;
        return r;
    }
    if (templateKey == "payroll") {
        qint64 total = 0;
        for (int i = 0; i < rows.size(); ++i) {
            const QString address = rows[i].address.trimmed();
            const QString value = rows[i].amount.trimmed();
            if (address.isEmpty() && value.isEmpty()) continue;
            qint64 rowUna = 0;
            if (!isPublicAddress(address) || !appendAmount(value, total, rowUna)) {
                r = Review{};
                r.blocker = QString("Fix batch row %1: a public address and a positive amount").arg(i + 1);
                return r;
            }
            ++r.recipients;
        }
        if (r.recipients == 0) { r.blocker = "Add at least one recipient"; return r; }
        r.deliveredUna = total;
        r.lockedUna = total + spendFeeUna;
        return r;
    }
    r.blocker = "This template is not available yet";
    return r;
}
}
