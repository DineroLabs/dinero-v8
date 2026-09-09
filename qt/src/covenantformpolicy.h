#pragma once
#include "shieldedtransferpolicy.h"

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
inline int delayBlocks(int duration, const QString& unit) {
    // All current networks target 120 seconds. Wall-clock durations are estimates;
    // consensus enforces the resulting block count from funding confirmation.
    const int multiplier = unit == "blocks" ? 1 : unit == "hours" ? 30 : unit == "days" ? 720 : 0;
    if (multiplier == 0 || duration <= 0 || duration > 65535 / multiplier) return 0;
    return duration * multiplier;
}
}
