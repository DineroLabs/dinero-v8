// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using dinero::qt::dashboard::NodePoller;
using dinero::qt::dashboard::NodeIdentity;
using dinero::qt::dashboard::ChainInfo;
using dinero::qt::dashboard::PeerRow;

class TestablePoller : public NodePoller {
public:
    TestablePoller() : NodePoller(nullptr) {}

    // Feed a canned response directly into the public onRpcResponse slot.
    void feed(const QString& method, const QJsonValue& result) {
        QMetaObject::invokeMethod(this, "onRpcResponse",
            Q_ARG(QString, method),
            Q_ARG(QJsonValue, result),
            Q_ARG(QString, QString()));
    }
};

class TestNodePoller : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parses_network_info_into_identity() {
        TestablePoller p;
        QSignalSpy spy(&p, &NodePoller::identityUpdated);

        QJsonObject ni;
        ni["subversion"]    = "/dinerod:b06ec828/";
        ni["version"]       = 80000;
        ni["localservices"] = "20000000";
        ni["localnodeid"]   = "fd4fc04df38bacbf72d4ecae451d1589570bcaba";
        ni["localrelay"]    = true;
        ni["direct_reachable"] = true;
        ni["listen"]        = true;
        QJsonArray la;
        QJsonObject one;
        one["address"] = "162.200.227.214";
        one["port"]    = 20999;
        la.append(one);
        ni["localaddresses"] = la;

        p.feed("getnetworkinfo", ni);
        QCOMPARE(spy.count(), 1);
        const auto id = spy.at(0).at(0).value<NodeIdentity>();
        QCOMPARE(id.subversion, QString("/dinerod:b06ec828/"));
        QCOMPARE(id.version, 80000);
        QCOMPARE(id.local_addr, QString("162.200.227.214"));
        QCOMPARE(id.local_port, quint16(20999));
        QCOMPARE(int(id.reachability), int(NodeIdentity::DIRECT));
    }

    void parses_peers_height_consensus_via_mode() {
        TestablePoller p;
        QSignalSpy spy_chain(&p, &NodePoller::chainInfoUpdated);
        QSignalSpy spy_peers(&p, &NodePoller::peersUpdated);

        QJsonArray peers;
        auto mk = [](const QString& a, qint64 h, int q) {
            QJsonObject o;
            o["addr"]          = a;
            o["synced_blocks"] = double(h);
            o["quality_score"] = q;
            o["inbound"]       = false;
            return o;
        };
        peers.append(mk("172.93.160.131:20999", 27402, 92));
        peers.append(mk("173.249.195.59:20999", 27402, 88));
        peers.append(mk("72.18.214.120:20999",  27402, 75));
        peers.append(mk("96.9.226.98:20999",    27401, 44));

        p.feed("getpeerinfo", peers);
        QVERIFY(spy_peers.count() >= 1);
        QVERIFY(spy_chain.count() >= 1);

        const auto rows = spy_peers.last().at(0).value<QVector<PeerRow>>();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].fleet_name, QString("LA"));
        QCOMPARE(rows[1].fleet_name, QString("VA"));
        QCOMPARE(rows[2].fleet_name, QString("MO"));
        QCOMPARE(rows[3].fleet_name, QString("CN"));
        QCOMPARE(rows[0].quality_score, 92);

        const auto ci = spy_chain.last().at(0).value<ChainInfo>();
        QCOMPARE(ci.net_consensus_height, qint64(27402));
        QCOMPARE(ci.max_peer_height,      qint64(27402));
    }

    void degraded_after_three_consecutive_failures() {
        TestablePoller p;
        QSignalSpy spy(&p, &NodePoller::daemonStateChanged);

        auto fail = [&] {
            QMetaObject::invokeMethod(&p, "onRpcResponse",
                Q_ARG(QString, "getnetworkinfo"),
                Q_ARG(QJsonValue, QJsonValue()),
                Q_ARG(QString, QString("connection refused")));
        };

        fail();
        QCOMPARE(spy.count(), 0);
        fail();
        QCOMPARE(spy.count(), 0);
        fail();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toBool(), false);
    }
};

QTEST_MAIN(TestNodePoller)
#include "test_node_poller.moc"
