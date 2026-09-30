#pragma once
#include <QJsonObject>
#include <QString>

// Block interval as reported by the node. Display-only: consensus enforces
// block counts, so every duration produced here is an estimate.
QString formatApproxSeconds(qint64 seconds);

struct ChainTiming {
    int blockSeconds = 120;   // legacy mainnet target; used until the node reports otherwise
    bool fromNode = false;
    static ChainTiming fromEconomics(const QJsonObject& economicsGetinfo);
    QString approxDuration(qint64 blocks) const { return formatApproxSeconds(blocks * blockSeconds); }
};
