#include <QtTest/QtTest>
#include <limits>
#include "../src/shieldedtransferpolicy.h"

class ShieldedTransferPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void onlyFreshOrRejectedIntentMaySubmit();
    void uncertainRestartNeverRetries();
    void parsesDinExactly();
    void rejectsInvalidDin();
    void productionLockoutIsReadOnly();
};

void ShieldedTransferPolicyTest::onlyFreshOrRejectedIntentMaySubmit() {
    QVERIFY(ShieldedTransferPolicy::maySubmit({}, false));
    QVERIFY(ShieldedTransferPolicy::maySubmit("rejected", false));
    QVERIFY(!ShieldedTransferPolicy::maySubmit("authorized", false));
    QVERIFY(!ShieldedTransferPolicy::maySubmit({}, true));
}

void ShieldedTransferPolicyTest::uncertainRestartNeverRetries() {
    QVERIFY(ShieldedTransferPolicy::uncertainAfterRestart("submitting"));
    QVERIFY(!ShieldedTransferPolicy::maySubmit("submitting", false));
}

void ShieldedTransferPolicyTest::parsesDinExactly() {
    qint64 una = 0;
    QVERIFY(ShieldedTransferPolicy::parseDinToUna("1", &una));
    QCOMPARE(una, 100000000LL);
    QVERIFY(ShieldedTransferPolicy::parseDinToUna("0.00000001", &una));
    QCOMPARE(una, 1LL);
    QVERIFY(ShieldedTransferPolicy::parseDinToUna("92233720368.54775807", &una));
    QCOMPARE(una, std::numeric_limits<qint64>::max());
}

void ShieldedTransferPolicyTest::rejectsInvalidDin() {
    qint64 una = 0;
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("", &una));
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("0", &una));
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("1.000000001", &una));
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("1.", &una));
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("-1", &una));
    QVERIFY(!ShieldedTransferPolicy::parseDinToUna("92233720368.54775808", &una));
}

void ShieldedTransferPolicyTest::productionLockoutIsReadOnly() {
    QVERIFY(!ShieldedTransferPolicy::showFundMovingControls(true, "dins"));
    QVERIFY(!ShieldedTransferPolicy::showFundMovingControls(true, "tdins"));
    QVERIFY(ShieldedTransferPolicy::showFundMovingControls(true, "rdins"));
    QVERIFY(ShieldedTransferPolicy::showFundMovingControls(false, "dins"));
}

QTEST_MAIN(ShieldedTransferPolicyTest)
#include "test_shielded_transfer_policy.moc"
