#include <QtTest/QtTest>
#include "poolcockpit.h"

using namespace poolcockpit;

class PoolCockpitFormatTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void durationsReadLikeWords() {
        QCOMPARE(formatDuration(0), QString("0 s"));
        QCOMPARE(formatDuration(45), QString("45 s"));
        QCOMPARE(formatDuration(60), QString("1 min"));
        QCOMPARE(formatDuration(12 * 60 + 5), QString("12 min"));
        QCOMPARE(formatDuration(14400), QString("4 h"));
        QCOMPARE(formatDuration(4 * 3600 + 20 * 60), QString("4 h 20 min"));
        QCOMPARE(formatDuration(86400), QString("1 day"));
        QCOMPARE(formatDuration(796576), QString("9 days 5 h"));
        QCOMPARE(formatDuration(2 * 86400), QString("2 days"));
        QCOMPARE(formatDuration(-5), QString("0 s"));
    }
    void countsGetThousandsSeparators() {
        QCOMPARE(groupDigits(302343), QString("302,343"));
        QCOMPARE(groupDigits(7174), QString("7,174"));
        QCOMPARE(groupDigits(999), QString("999"));
        QCOMPARE(groupDigitsText("53183771145"), QString("53,183,771,145"));
        QCOMPARE(groupDigitsText("12.5"), QString("12.5"));
        QCOMPARE(groupDigitsText("Unavailable"), QString("Unavailable"));
    }
    void scriptsBecomeAddresses() {
        // Published vectors: BIP-173 (witness v0, bech32) and BIP-350 (v1, bech32m).
        QCOMPARE(scriptToAddress("0014751e76e8199196d454941c45d1b3a323f1433bd6", "bc"),
                 QString("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"));
        QCOMPARE(scriptToAddress("512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", "bc"),
                 QString("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0"));
        // Dinero HRPs, cross-checked with an independent reference implementation.
        QCOMPARE(scriptToAddress("5120000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "din"),
                 QString("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0shg7l3c"));
        QCOMPARE(scriptToAddress("5120000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "tdin"),
                 QString("tdin1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0su86pwd"));
        // P2MR is witness version 3 (include/wallet/p2mr_address.h), so din1r.
        QCOMPARE(scriptToAddress("53201111111111111111111111111111111111111111111111111111111111111111", "din"),
                 QString("din1rzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsjls6g6"));
        // Not witness programs: shown as hex by the caller.
        QCOMPARE(scriptToAddress("76a914751e76e8199196d454941c45d1b3a323f1433bd688ac", "din"), QString());
        QCOMPARE(scriptToAddress("5120abcd", "din"), QString());          // length byte lies
        QCOMPARE(scriptToAddress("zz", "din"), QString());
        QCOMPARE(scriptToAddress("", "din"), QString());
    }
    void hrpComesFromTheAddress() {
        QCOMPARE(hrpOf("din1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0shg7l3c"), QString("din"));
        QCOMPARE(hrpOf("tdin1pqqqsyqcyq5rqwzqfpg9scrgwpugpzysnzs23v9ccrydpk8qarc0su86pwd"), QString("tdin"));
        QCOMPARE(hrpOf(""), QString("din"));
        QCOMPARE(hrpOf("garbage"), QString("din"));
    }
    void historySaysWhenItIsStillCollecting() {
        const qint64 now = QDateTime(QDate(2026, 10, 1), QTime(9, 0)).toSecsSinceEpoch();
        const qint64 justConnected = now - 30;
        const QString since = QDateTime::fromSecsSinceEpoch(justConnected).toString("HH:mm");
        QCOMPARE(historyCoverage(justConnected, now, 5 * 60), "collecting since " + since);
        QCOMPARE(historyCoverage(justConnected, now, 24 * 3600), "collecting since " + since);
        const qint64 twoHoursAgo = now - 2 * 3600;
        QCOMPARE(historyCoverage(twoHoursAgo, now, 5 * 60), QString());
        QCOMPARE(historyCoverage(twoHoursAgo, now, 3600), QString());
        QVERIFY(historyCoverage(twoHoursAgo, now, 24 * 3600).startsWith("collecting since"));
        QVERIFY(historyCoverage(-1, now, 3600).startsWith("collecting"));  // no samples yet
    }
};
QTEST_GUILESS_MAIN(PoolCockpitFormatTest)
#include "test_pool_cockpit_format.moc"
