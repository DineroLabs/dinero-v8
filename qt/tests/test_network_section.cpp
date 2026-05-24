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
                 QString("● in sync"));
    }
    void tip_delta_behind() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27400, 27402),
                 QString("● +2 behind net"));
    }
    void tip_delta_ahead() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(27403, 27402),
                 QString("● 1 ahead of net"));
    }
    void tip_delta_zero_when_both_zero() {
        QCOMPARE(NetworkSection::tipDeltaAnnotation(0, 0), QString("—"));
    }
};

QTEST_MAIN(TestNetworkSection)
#include "test_network_section.moc"
