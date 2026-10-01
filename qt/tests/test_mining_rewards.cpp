#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include "miningrewards.h"
#include "chaintiming.h"

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
    void panelTextForAnActiveMiner() {
        MiningRewardsSummary s;
        s.any = true; s.blocks24h = 312; s.una24h = qint64(31'200) * 100'000'000;
        s.maturingUna = qint64(9'900) * 100'000'000; s.nextUnlockBlocks = 2; s.lastFoundSecsAgo = 180;
        const MiningRewardsText t = miningRewardsText(s, ChainTiming{120, true});
        QCOMPARE(t.headline, QString("312 blocks · 31,200 DIN"));
        QCOMPARE(t.period, QString("found in the last 24 hours"));
        QCOMPARE(t.maturing, QString("Maturing: 9,900 DIN · next unlock in 2 blocks (~4 min)"));
        QCOMPARE(t.lastFound, QString("Last block found ~3 min ago"));
    }
    void panelTextWhenThePageWasFull() {
        MiningRewardsSummary s;
        s.any = true; s.blocks24h = 1600; s.una24h = qint64(160'000) * 100'000'000; s.truncated = true;
        s.lastFoundSecsAgo = 30;
        const MiningRewardsText t = miningRewardsText(s, ChainTiming{60, true});
        QCOMPARE(t.headline, QString("1600+ blocks · 160,000+ DIN"));
        QCOMPARE(t.maturing, QString("Nothing maturing"));
        QCOMPARE(t.lastFound, QString("Last block found ~30 sec ago"));
    }
    void panelTextWithoutRewards() {
        const MiningRewardsText t = miningRewardsText(MiningRewardsSummary{}, ChainTiming{});
        QCOMPARE(t.headline, QString("No mining rewards yet"));
        QCOMPARE(t.period, QString("in this wallet"));
        QVERIFY(t.maturing.isEmpty());
        QVERIFY(t.lastFound.isEmpty());
    }
    void formatsDinWithGrouping() {
        QCOMPARE(formatDinAmount(qint64(31'200) * 100'000'000), QString("31,200 DIN"));
        QCOMPARE(formatDinAmount(50'000'000), QString("0.5 DIN"));
        QCOMPARE(formatDinAmount(0), QString("0 DIN"));
    }
};
QTEST_GUILESS_MAIN(MiningRewardsTest)
#include "test_mining_rewards.moc"
