// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"
#include <QComboBox>
#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::NetworkSection;

class TestNetworkSection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void tip_delta_in_sync() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27402, 27402),
                 QString("● In sync with the peer estimate"));
    }
    void tip_delta_behind() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27400, 27402),
                 QString("● 2 block(s) behind the peer estimate"));
    }
    void tip_delta_ahead() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27403, 27402),
                 QString("● 1 block(s) ahead of the peer estimate; peers may still be catching up"));
    }
    void tip_delta_zero_when_both_zero() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(0, 0), QString("—"));
    }
    void tor_state_mapping_is_honest_and_hides_untrusted_details() {
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Off)
                    .contains(QStringLiteral("Ordinary direct")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Active,
                                                QStringLiteral("abc.onion"))
                    .contains(QStringLiteral("abc.onion")));
        QVERIFY(!NetworkSection::torStatusText(NetworkSection::TorState::Active,
                                                 QStringLiteral("user:secret@proxy"))
                     .contains(QStringLiteral("secret")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Error)
                    .contains(QStringLiteral("credentials are hidden")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Unsupported)
                    .contains(QStringLiteral("older")));
    }
    void maps_real_onion_service_state_defensively() {
        NetworkSection s;
        auto* status = s.findChild<QLabel*>(QStringLiteral("torStatus"));
        auto* control = s.findChild<QComboBox*>(QStringLiteral("torReachabilityControl"));
        QVERIFY(status != nullptr);
        QVERIFY(control != nullptr);
        QVERIFY(!control->isEnabled());

        dinero::qt::dashboard::OnionServiceStatus onion;
        onion.available = true;
        s.setOnionServiceStatus(onion);
        QVERIFY(status->text().startsWith(QStringLiteral("Off.")));
        QCOMPARE(control->currentData().toString(), QStringLiteral("off"));
        QVERIFY(control->isEnabled());

        onion.requested = true;
        onion.active = true;
        onion.address = QStringLiteral("examplehiddenservice.onion");
        s.setOnionServiceStatus(onion);
        QVERIFY(status->text().contains(onion.address));
        QCOMPARE(control->currentData().toString(), QStringLiteral("automatic"));

        onion.active = false;
        onion.message = QStringLiteral("cookie=/secret/path credential=hunter2");
        s.setOnionServiceStatus(onion);
        QVERIFY(status->text().contains(QStringLiteral("not active")));
        QVERIFY(!status->text().contains(QStringLiteral("hunter2")));
        QVERIFY(!status->toolTip().contains(QStringLiteral("/secret/path")));
    }

    void old_daemon_keeps_tor_control_noninteractive() {
        NetworkSection s;
        auto* control = s.findChild<QComboBox*>(QStringLiteral("torReachabilityControl"));
        QVERIFY(control->toolTip().contains(QStringLiteral("local node cookie")));
        QVERIFY(control->toolTip().contains(QStringLiteral("no additional credentials")));
        dinero::qt::dashboard::OnionServiceStatus unavailable;
        s.setOnionServiceStatus(unavailable);
        QVERIFY(!control->isEnabled());
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Unsupported)
                    .contains(QStringLiteral("older")));
    }

    void relay_service_controls_are_dinero_only_and_bounded() {
        NetworkSection s;
        auto* control = s.findChild<QComboBox*>(QStringLiteral("relayServiceControl"));
        QVERIFY(control != nullptr);
        QCOMPARE(control->count(), 3);
        QVERIFY(control->toolTip().contains(QStringLiteral("Dinero P2P")));
        QVERIFY(control->toolTip().contains(QStringLiteral("not a web proxy")));
    }
};

QTEST_MAIN(TestNetworkSection)
#include "test_network_section.moc"
