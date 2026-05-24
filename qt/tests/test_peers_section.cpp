// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "peerssection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest/QtTest>

using dinero::qt::dashboard::PeersSection;
using dinero::qt::dashboard::PeerRow;

class TestPeersSection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void stoplight_glyph_per_range() {
        QCOMPARE(PeersSection::stoplightGlyph(92, true), QString("●"));
        QCOMPARE(PeersSection::stoplightGlyph(70, true), QString("●"));
        QCOMPARE(PeersSection::stoplightGlyph(69, true), QString("◐"));
        QCOMPARE(PeersSection::stoplightGlyph(40, true), QString("◐"));
        QCOMPARE(PeersSection::stoplightGlyph(39, true), QString("⚠"));
        QCOMPARE(PeersSection::stoplightGlyph(0,  true), QString("⚠"));
        QCOMPARE(PeersSection::stoplightGlyph(-1, true), QString("○"));
        QCOMPARE(PeersSection::stoplightGlyph(80, false), QString("○"));
    }

    void header_count_reflects_peer_list_size() {
        PeersSection s;
        QVector<PeerRow> peers;
        peers.append(PeerRow{});
        peers.append(PeerRow{});
        peers.append(PeerRow{});
        s.onPeersUpdated(peers);
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == "🛰 PEERS (3 connected)") found = true;
        }
        QVERIFY(found);
    }

    void rows_populate_with_addr_height_ping() {
        PeersSection s;
        PeerRow r;
        r.addr = "172.93.160.131:20999";
        r.fleet_name = "LA";
        r.height = 27402;
        r.ping_ms = 12;
        r.quality_score = 92;
        s.onPeersUpdated({r});

        auto* tree = s.findChild<QTreeWidget*>();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto* item = tree->topLevelItem(0);
        QVERIFY(item->text(0).contains("92"));
        QVERIFY(item->text(0).contains("●"));
        QVERIFY(item->text(2).contains("172.93.160.131:20999"));
        QVERIFY(item->text(2).contains("LA"));
        QCOMPARE(item->text(3), QString("27402"));
        QCOMPARE(item->text(4), QString("12 ms"));
    }

    void relay_virtual_peer_shows_via_annotation() {
        PeersSection s;
        PeerRow r;
        r.addr = "relay:abc:def";
        r.via_relay = true;
        r.relay_via_addr = "172.93.160.131:20999";
        r.is_inbound = true;
        s.onPeersUpdated({r});
        auto* tree = s.findChild<QTreeWidget*>();
        auto* item = tree->topLevelItem(0);
        QCOMPARE(item->text(1), QString("⇄"));
        QVERIFY(item->text(2).contains("(via 172.93.160.131:20999)"));
    }
};

QTEST_MAIN(TestPeersSection)
#include "test_peers_section.moc"
