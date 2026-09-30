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
}
