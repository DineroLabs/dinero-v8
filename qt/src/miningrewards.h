#pragma once
#include <QJsonArray>
#include <QString>
#include <optional>

// Summary of this wallet's mining rewards for the Overview panel, built from
// wallet.listtransactions {type:"mined"} (newest first).
struct MiningRewardsSummary {
    bool any = false;                        // at least one reward in the list
    int blocks24h = 0;                       // rewards whose block time is within 24 h
    qint64 una24h = 0;
    qint64 maturingUna = 0;                  // rewards not yet spendable (< maturity confirmations)
    std::optional<int> nextUnlockBlocks;     // blocks until the oldest maturing reward unlocks
    std::optional<qint64> lastFoundSecsAgo;  // age of the newest reward
    bool truncated = false;                  // a full page came back: older rewards may be missing
};

MiningRewardsSummary summarizeMiningRewards(const QJsonArray& transactions, qint64 nowSecs,
                                            int requestedCount, int maturity = 100);

// "31,200 DIN", "0.5 DIN": grouped whole part, trailing zeros trimmed, dot decimal.
QString formatDinAmount(qint64 una);
