#include <QtTest>
#include "upgradebanner.h"

class TestUpgradeBanner : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void hiddenWhenNothingToSay() {
        UpgradeBanner b;
        b.present({UpgradePolicy::State::None, -1}, QString(), QString());
        QVERIFY(b.isHidden());
    }
    void requiredShowsHeightAndCountdown() {
        UpgradeBanner b;
        b.setActivationHeight(125000);
        b.present({UpgradePolicy::State::UpdateRequired, 4100}, "8.1.13", "~2 days 20 h");
        QVERIFY(!b.isHidden());
        QVERIFY(b.text().contains("activates at block 125000"));
        QVERIFY(b.text().contains("4100 blocks"));
        QVERIFY(b.text().contains("~2 days 20 h"));
        QVERIFY(b.text().contains("disconnected"));
    }
    void scheduledReadyReassures() {
        UpgradeBanner b;
        b.setActivationHeight(125000);
        b.present({UpgradePolicy::State::ScheduledReady, 10}, "8.1.13", "~10 min");
        QVERIFY(b.text().contains("compatible"));
    }
    void overdueSaysOutOfDate() {
        UpgradeBanner b;
        b.setActivationHeight(125000);
        b.present({UpgradePolicy::State::RequiredOverdue, 0}, "8.1.13", QString());
        QVERIFY(b.text().contains("out of date"));
        QVERIFY(b.text().contains("125000"));
    }
    void backToHiddenAfterUpgrade() {
        UpgradeBanner b;
        b.present({UpgradePolicy::State::UpdateAvailable, -1}, "8.1.13", QString());
        QVERIFY(!b.isHidden());
        QVERIFY(b.text().contains("8.1.13 is available"));
        b.present({UpgradePolicy::State::None, -1}, QString(), QString());
        QVERIFY(b.isHidden());
    }
};
QTEST_MAIN(TestUpgradeBanner)
#include "test_upgrade_banner.moc"
