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
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "hours"), 30);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "days"), 720);
        QCOMPARE(CovenantFormPolicy::delayBlocks(65535, "blocks"), 65535);
        QCOMPARE(CovenantFormPolicy::delayBlocks(92, "days"), 0);
        QCOMPARE(CovenantFormPolicy::delayBlocks(INT_MAX, "hours"), 0);
        QCOMPARE(CovenantFormPolicy::delayBlocks(1, "unknown"), 0);
    }
};
QTEST_GUILESS_MAIN(CovenantFormPolicyTest)
#include "test_covenant_form_policy.moc"
