// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "topologysection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QtTest/QtTest>

using dinero::qt::dashboard::TopologyEdge;
using dinero::qt::dashboard::TopologyNode;
using dinero::qt::dashboard::TopologySection;
using dinero::qt::dashboard::TopologySnapshot;

class TestTopologySection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void empty_snapshot_renders_header() {
        TopologySection s;
        s.setTopologySnapshot({});
        QCOMPARE(s.renderedNodeCountForTest(), 0);
        QCOMPARE(s.renderedEdgeCountForTest(), 0);
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text() == QStringLiteral("Topology — 0 nodes / 0 paths")) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }

    void snapshot_counts_are_rendered() {
        TopologySnapshot snap;
        TopologyNode self;
        self.id = "self";
        self.label = "This node";
        self.kind = "self";
        self.connected = true;
        TopologyNode peer;
        peer.id = "peer:1.2.3.4:20999";
        peer.label = "1.2.3.4:20999";
        peer.endpoint = "1.2.3.4:20999";
        peer.kind = "direct";
        peer.connected = true;
        snap.nodes = {self, peer};
        TopologyEdge edge;
        edge.from_id = "self";
        edge.to_id = peer.id;
        edge.kind = "direct";
        snap.edges = {edge};

        TopologySection s;
        s.setTopologySnapshot(snap);
        QCOMPARE(s.renderedNodeCountForTest(), 2);
        QCOMPARE(s.renderedEdgeCountForTest(), 1);
    }
};

QTEST_MAIN(TestTopologySection)
#include "test_topology_section.moc"
