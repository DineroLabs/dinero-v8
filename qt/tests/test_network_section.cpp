// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"
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
        QCOMPARE(NetworkSection::torStatusText(NetworkSection::TorState::Off),
                 QString("Off. Tor will not be installed or started automatically."));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Detected)
                    .contains(QStringLiteral("restart")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Active,
                                                QStringLiteral("abc.onion"))
                    .contains(QStringLiteral("abc.onion")));
        QVERIFY(!NetworkSection::torStatusText(NetworkSection::TorState::Active,
                                                 QStringLiteral("user:secret@proxy"))
                     .contains(QStringLiteral("secret")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Error)
                    .contains(QStringLiteral("credentials are hidden")));
        QVERIFY(NetworkSection::torStatusText(NetworkSection::TorState::Unsupported)
                    .contains(QStringLiteral("Not available as a live switch")));
    }
};

QTEST_MAIN(TestNetworkSection)
#include "test_network_section.moc"
