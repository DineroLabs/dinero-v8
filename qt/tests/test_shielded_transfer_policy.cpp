#include <QtTest/QtTest>
#include "../src/shieldedtransferpolicy.h"

class ShieldedTransferPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void onlyFreshOrRejectedIntentMaySubmit();
    void uncertainRestartNeverRetries();
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

QTEST_MAIN(ShieldedTransferPolicyTest)
#include "test_shielded_transfer_policy.moc"
