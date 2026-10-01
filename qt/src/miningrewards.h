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

struct ChainTiming;

// Panel wording; durations use the node's block time.
struct MiningRewardsText {
    QString headline;   // "312 blocks · 31,200 DIN"
    QString period;     // "found in the last 24 hours"
    QString maturing;   // "Maturing: 9,900 DIN · next unlock in 2 blocks (~4 min)"
    QString lastFound;  // "Last block found ~3 min ago"
};
MiningRewardsText miningRewardsText(const MiningRewardsSummary& s, const ChainTiming& timing);
