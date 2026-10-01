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
    void reviewSimpleLock() {
        const auto isPublic = [](const QString& a) { return a.startsWith("din1p") || a.startsWith("din1r"); };
        const auto ready = CovenantFormPolicy::review("vault", "din1pabc", "1.5", {}, isPublic);
        QVERIFY2(ready.blocker.isEmpty(), qPrintable(ready.blocker));
        QCOMPARE(ready.lockedUna, 150000000LL);
        QCOMPARE(ready.deliveredUna, 149999000LL);  // the reserved withdrawal fee comes out of it
        QCOMPARE(ready.recipients, 1);
        QCOMPARE(CovenantFormPolicy::review("timelock", "din1rabc", "2", {}, isPublic).deliveredUna, 199999000LL);

        QCOMPARE(CovenantFormPolicy::review("vault", " ", "1", {}, isPublic).blocker, QString("Enter a recipient address"));
        QCOMPARE(CovenantFormPolicy::review("vault", "dins1abc", "1", {}, isPublic).blocker,
                 QString("Enter a public din1p… or din1r… address"));
        QCOMPARE(CovenantFormPolicy::review("vault", "din1pabc", "", {}, isPublic).blocker, QString("Enter an amount"));
        QCOMPARE(CovenantFormPolicy::review("vault", "din1pabc", "1.000000001", {}, isPublic).blocker,
                 QString("Enter a valid amount (up to 8 decimals)"));
        QCOMPARE(CovenantFormPolicy::review("vault", "din1pabc", "0.00001", {}, isPublic).blocker,
                 QString("Amount must be more than 0.00001000 DIN (the reserved withdrawal fee)"));
    }
    void reviewBatchPayment() {
        const auto isPublic = [](const QString& a) { return a.startsWith("din1p"); };
        const auto ready = CovenantFormPolicy::review("payroll", "", "",
            {{"din1pa", "1"}, {"", ""}, {"din1pb", "0.5"}}, isPublic);
        QVERIFY2(ready.blocker.isEmpty(), qPrintable(ready.blocker));
        QCOMPARE(ready.recipients, 2);
        QCOMPARE(ready.deliveredUna, 150000000LL);
        QCOMPARE(ready.lockedUna, 150001000LL);  // outputs plus the reserved withdrawal fee
        QCOMPARE(CovenantFormPolicy::review("payroll", "", "", {{"", ""}}, isPublic).blocker,
                 QString("Add at least one recipient"));
        QCOMPARE(CovenantFormPolicy::review("payroll", "", "", {{"din1pa", "1"}, {"din1pb", ""}}, isPublic).blocker,
                 QString("Fix batch row 2: a public address and a positive amount"));
        QCOMPARE(CovenantFormPolicy::review("payroll", "", "", {{"dins1x", "1"}}, isPublic).blocker,
                 QString("Fix batch row 1: a public address and a positive amount"));
    }
    void reviewUnavailableTemplates() {
        const auto any = [](const QString&) { return true; };
        QCOMPARE(CovenantFormPolicy::review("conditional", "din1pa", "1", {}, any).blocker,
                 QString("This template is not available yet"));
        QCOMPARE(CovenantFormPolicy::review("custom", "din1pa", "1", {}, any).blocker,
                 QString("This template is not available yet"));
    }
};
QTEST_GUILESS_MAIN(CovenantFormPolicyTest)
#include "test_covenant_form_policy.moc"
