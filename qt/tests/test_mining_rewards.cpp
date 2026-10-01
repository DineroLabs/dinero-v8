#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include "miningrewards.h"

namespace {
QJsonObject reward(double amount, int confirmations, qint64 time) {
    return QJsonObject{{"type", "mined"}, {"category", confirmations < 100 ? "immature" : "generate"},
                       {"is_coinbase", true}, {"amount", amount}, {"confirmations", confirmations},
                       {"time", double(time)}};
}
}

class MiningRewardsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void summarisesLastDayMaturingAndNextUnlock() {
        const qint64 now = 1'790'820'000;
        const QJsonArray txs{
            reward(100.0, 3, now - 360),           // maturing, newest
            reward(100.0, 98, now - 98 * 120),     // maturing, unlocks in 2 blocks
            reward(100.0, 150, now - 150 * 120),   // mature, inside 24 h
            reward(100.0, 900, now - 2 * 86400),   // mature, older than 24 h
            QJsonObject{{"type", "received"}, {"category", "receive"}, {"amount", 5.0},
                        {"confirmations", 4}, {"time", double(now - 100)}},  // not a mining reward
        };
        const MiningRewardsSummary s = summarizeMiningRewards(txs, now, /*requested=*/200);
        QVERIFY(s.any);
        QCOMPARE(s.blocks24h, 3);
        QCOMPARE(s.una24h, qint64(300) * 100'000'000);
        QCOMPARE(s.maturingUna, qint64(200) * 100'000'000);
        QCOMPARE(s.nextUnlockBlocks, std::optional<int>(2));
        QCOMPARE(s.lastFoundSecsAgo, std::optional<qint64>(360));
        QVERIFY(!s.truncated);
    }
    void tailRewardsAreCountedExactly() {
        const qint64 now = 2'000'000'000;
        const MiningRewardsSummary s = summarizeMiningRewards(QJsonArray{reward(0.5, 10, now - 60)}, now, 200);
        QCOMPARE(s.una24h, qint64(50'000'000));
        QCOMPARE(s.maturingUna, qint64(50'000'000));
    }
    void fullPageMeansTheWindowMayBeIncomplete() {
        const qint64 now = 1'790'820'000;
        QJsonArray txs;
        for (int i = 0; i < 5; ++i) txs.append(reward(100.0, 200 + i, now - 60 * i));
        QVERIFY(summarizeMiningRewards(txs, now, /*requested=*/5).truncated);
        QVERIFY(!summarizeMiningRewards(txs, now, /*requested=*/6).truncated);
    }
    void noRewardsAtAll() {
        const MiningRewardsSummary s = summarizeMiningRewards(QJsonArray{}, 1'790'820'000, 200);
        QVERIFY(!s.any);
        QCOMPARE(s.blocks24h, 0);
        QVERIFY(!s.nextUnlockBlocks.has_value());
        QVERIFY(!s.lastFoundSecsAgo.has_value());
    }
    void formatsDinWithGrouping() {
        QCOMPARE(formatDinAmount(qint64(31'200) * 100'000'000), QString("31,200 DIN"));
        QCOMPARE(formatDinAmount(50'000'000), QString("0.5 DIN"));
        QCOMPARE(formatDinAmount(0), QString("0 DIN"));
    }
};
QTEST_GUILESS_MAIN(MiningRewardsTest)
#include "test_mining_rewards.moc"
