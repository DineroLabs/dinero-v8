// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QtTest/QtTest>

using dinero::qt::dashboard::DynamicP2POverview;
using dinero::qt::dashboard::HintRow;
using dinero::qt::dashboard::NodeIdentity;
using dinero::qt::dashboard::NodePoller;
using dinero::qt::dashboard::PeerRow;
using dinero::qt::dashboard::TopologySnapshot;

namespace {
PeerRow peer(const QString& addr, int quality, bool relay = false) {
    PeerRow r;
    r.addr = addr;
    r.quality_score = quality;
    r.via_relay = relay;
    r.handshake_complete = true;
    r.fleet_name = addr.startsWith("172.93.160.131") ? "LA" : QString{};
    return r;
}

HintRow hint(const QString& target, const QString& endpoint) {
    HintRow h;
    h.target_node_id_hex = target;
    h.endpoint = endpoint;
    h.net = "ipv4";
    return h;
}

const auto findNode = [](const TopologySnapshot& s, const QString& id) {
    for (const auto& n : s.nodes) {
        if (n.id == id) return n;
    }
    return dinero::qt::dashboard::TopologyNode{};
};
}

class TestTopologySnapshot : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void includes_self_and_connected_peer() {
        NodeIdentity id;
        id.node_id_hex = "0102030405060708090a0102030405060708090a";
        id.local_port = 20999;

        const auto snap = NodePoller::BuildTopologySnapshot(
            id, {peer("1.2.3.4:20999", 92)}, {}, {});

        QCOMPARE(snap.nodes.size(), 2);
        QCOMPARE(snap.edges.size(), 1);
        QCOMPARE(findNode(snap, "self").kind, QString("self"));
        const auto p = findNode(snap, "peer:1.2.3.4:20999");
        QCOMPARE(p.kind, QString("direct"));
        QCOMPARE(p.bucket, QString("hot"));
        QVERIFY(p.connected);
        QCOMPARE(snap.edges[0].kind, QString("direct"));
    }

    void fleet_peer_keeps_fleet_kind_and_label() {
        const auto snap = NodePoller::BuildTopologySnapshot(
            {}, {peer("172.93.160.131:20999", 88)}, {}, {});
        const auto p = findNode(snap, "peer:172.93.160.131:20999");
        QCOMPARE(p.kind, QString("fleet"));
        QCOMPARE(p.label, QString("LA"));
        QCOMPARE(p.bucket, QString("hot"));
    }

    void relay_virtual_peer_creates_relay_edge() {
        PeerRow r = peer("relay:in:96.9.226.98:20999:3d44555041b9657c:0",
                         60, true);

        const auto snap = NodePoller::BuildTopologySnapshot({}, {r}, {}, {});

        QCOMPARE(snap.nodes.size(), 2);
        QCOMPARE(snap.edges.size(), 1);
        QCOMPARE(snap.edges[0].kind, QString("relay_virtual"));
        QCOMPARE(snap.edges[0].via_relay, QString("96.9.226.98:20999"));
        const auto p = findNode(snap, "peer:" + r.addr);
        QCOMPARE(p.kind, QString("relay_virtual"));
        QCOMPARE(p.bucket, QString("warm"));
    }

    void hint_only_target_is_disconnected_relay_candidate() {
        const QString target = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const auto snap = NodePoller::BuildTopologySnapshot(
            {}, {}, {hint(target, "173.249.200.59:20999")}, {});

        QCOMPARE(snap.nodes.size(), 2);
        QCOMPARE(snap.edges.size(), 1);
        const auto h = findNode(snap, "hint:" + target);
        QCOMPARE(h.kind, QString("hint"));
        QCOMPARE(h.bucket, QString("relay_candidate"));
        QVERIFY(!h.connected);
        QCOMPARE(snap.edges[0].kind, QString("hint"));
        QCOMPARE(snap.edges[0].via_relay, QString("173.249.200.59:20999"));
    }
};

QTEST_MAIN(TestTopologySnapshot)
#include "test_topology_snapshot.moc"
