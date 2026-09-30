#include <QtTest/QtTest>
#include "covenantformpolicy.h"
class CovenantFormPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void exactBatchFunding() {
        qint64 total = 0, value = 0;
        QVERIFY(CovenantFormPolicy::appendAmount("0.00000001", total, value));
        QVERIFY(CovenantFormPolicy::appendAmount("1.12345678", total, value));
        QCOMPARE(CovenantFormPolicy::formatUna(total + CovenantFormPolicy::spendFeeUna), QString("1.12346679"));
    }
    void invalidRowsDoNotAlterTotal() {
        for (const QString& text : {QString(""), QString("nan"), QString("-1"), QString("1.000000001"), QString("92233720368.54775807")}) {
            qint64 total = 42, value = 0;
            QVERIFY(!CovenantFormPolicy::appendAmount(text, total, value));
            QCOMPARE(total, 42LL);
        }
    }
    void relativeBlockDelay() {
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "hours", 120), 30);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "days", 120), 720);
        QCOMPARE(CovenantFormPolicy::delayBlocks(65535, "blocks", 120), 65535);
        QCOMPARE(CovenantFormPolicy::delayBlocks(92, "days", 120), 0);
        QCOMPARE(CovenantFormPolicy::delayBlocks(INT_MAX, "hours", 120), 0);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "unknown", 120), 0);
    }
    void delayBlocksFollowsBlockTime() {
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "days", 120), 720);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "days", 60), 1440);
        QCOMPARE(CovenantFormPolicy::delayBlocks(2, "hours", 60), 120);
        QCOMPARE(CovenantFormPolicy::delayBlocks(5, "blocks", 60), 5);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "days", 0), 720);
        QCOMPARE(CovenantFormPolicy::delayBlocks(46, "days", 60), 0);
        QCOMPARE(CovenantFormPolicy::delayBlocks(45, "days", 60), 64800);
    }
};
QTEST_GUILESS_MAIN(CovenantFormPolicyTest)
#include "test_covenant_form_policy.moc"
