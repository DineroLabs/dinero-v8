#include <QtTest>
#include <QJsonObject>
#include "chaintiming.h"

class TestChainTiming : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void defaultsTo120() { QCOMPARE(ChainTiming{}.blockSeconds, 120); QVERIFY(!ChainTiming{}.fromNode); }
    void readsNodeValue() {
        auto t = ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", 60}});
        QCOMPARE(t.blockSeconds, 60); QVERIFY(t.fromNode);
    }
    void rejectsMissingZeroNegativeString() {
        QCOMPARE(ChainTiming::fromEconomics(QJsonObject{}).blockSeconds, 120);
        QCOMPARE(ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", 0}}).blockSeconds, 120);
        QCOMPARE(ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", -60}}).blockSeconds, 120);
        QCOMPARE(ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", "60"}}).blockSeconds, 120);
        QCOMPARE(ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", 100000}}).blockSeconds, 120);
        QVERIFY(!ChainTiming::fromEconomics(QJsonObject{{"block_time_seconds", 0}}).fromNode);
    }
    void maturityAt120And60() {
        QCOMPARE((ChainTiming{120, true}).approxDuration(100), QString("~3 h 20 min"));
        QCOMPARE((ChainTiming{60, true}).approxDuration(100), QString("~1 h 40 min"));
    }
    void formatting() {
        QCOMPARE(formatApproxSeconds(30), QString("~30 sec"));
        QCOMPARE(formatApproxSeconds(120), QString("~2 min"));
        QCOMPARE(formatApproxSeconds(3600), QString("~1 h"));
        QCOMPARE(formatApproxSeconds(86400 + 3 * 3600), QString("~1 day 3 h"));
        QCOMPARE(formatApproxSeconds(2 * 86400), QString("~2 days"));
        QCOMPARE(formatApproxSeconds(0), QString("~0 min"));
        QCOMPARE((ChainTiming{60, true}).approxDuration(-5), QString("~0 min"));
    }
};
QTEST_GUILESS_MAIN(TestChainTiming)
#include "test_chain_timing.moc"
