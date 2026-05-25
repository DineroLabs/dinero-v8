// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dashboardactioncontroller.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

using dinero::qt::dashboard::DashboardActionController;
using dinero::qt::dashboard::HintRow;
using dinero::qt::dashboard::PeerRow;

class TestDashboardActions : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void ban_guardrails_reject_relay_and_hostnames() {
        QVERIFY(!DashboardActionController::isBannableEndpoint("relay:in:abc"));
        QVERIFY(!DashboardActionController::isBannableEndpoint("la.dinerolabs.org:20999"));
        QVERIFY(DashboardActionController::isBannableEndpoint("172.93.160.131:20999"));
        QVERIFY(DashboardActionController::isBannableEndpoint("[2606:4700:4700::1111]:20999"));
        QCOMPARE(DashboardActionController::hostForBan("172.93.160.131:20999"),
                 QStringLiteral("172.93.160.131"));
        QCOMPARE(DashboardActionController::hostForBan("[2606:4700:4700::1111]:20999"),
                 QStringLiteral("2606:4700:4700::1111"));
    }

    void peer_details_json_has_action_payload() {
        PeerRow p;
        p.addr = "1.2.3.4:20999";
        p.via_relay = true;
        p.relay_via_addr = "172.93.160.131:20999";
        p.quality_score = 88;
        const auto obj = DashboardActionController::peerDetailsJson(p);
        QCOMPARE(obj.value("addr").toString(), QStringLiteral("1.2.3.4:20999"));
        QCOMPARE(obj.value("via_relay").toBool(), true);
        QCOMPARE(obj.value("relay_via_addr").toString(),
                 QStringLiteral("172.93.160.131:20999"));
        QCOMPARE(obj.value("quality_score").toInt(), 88);
    }

    void non_destructive_actions_dispatch_expected_rpc_params() {
        DashboardActionController c(nullptr, nullptr);
        QSignalSpy spy(&c, &DashboardActionController::rpcDispatched);
        c.tryDirectReconnect("1.2.3.4:20999");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("addnode"));

        HintRow h;
        h.target_node_id_hex = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        h.endpoint = QStringLiteral("172.93.160.131:20999");
        c.dialRelayHint(h);
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("relayhints.dial"));
        QCOMPARE(args.at(2).toBool(), true);
        const auto params = args.at(1).value<QJsonValue>().toObject();
        QCOMPARE(params.value("target_node_id_hex").toString(), h.target_node_id_hex);
        QCOMPARE(params.value("relay_endpoint").toString(), h.endpoint);
        QCOMPARE(params.value("dry_run").toBool(), false);
    }

    void destructive_actions_require_confirmation() {
        DashboardActionController c(nullptr, nullptr);
        QSignalSpy spy(&c, &DashboardActionController::rpcDispatched);

        c.setConfirmCallbackForTest([](QWidget*, const QString&, const QString&) {
            return false;
        });
        c.disconnectPeer("1.2.3.4:20999");
        QCOMPARE(spy.count(), 0);

        c.setConfirmCallbackForTest([](QWidget*, const QString&, const QString&) {
            return true;
        });
        c.disconnectPeer("1.2.3.4:20999");
        QCOMPARE(spy.count(), 1);
        auto args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("disconnectnode"));
        QCOMPARE(args.at(2).toBool(), false);
        QCOMPARE(args.at(1).value<QJsonValue>().toArray().at(0).toString(),
                 QStringLiteral("1.2.3.4:20999"));

        c.banPeer("1.2.3.4:20999", 3600);
        QCOMPARE(spy.count(), 1);
        args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("setban"));
        const auto params = args.at(1).value<QJsonValue>().toArray();
        QCOMPARE(params.at(0).toString(), QStringLiteral("1.2.3.4"));
        QCOMPARE(params.at(1).toString(), QStringLiteral("add"));
        QCOMPARE(params.at(2).toInt(), 3600);
    }
};

QTEST_MAIN(TestDashboardActions)
#include "test_dashboard_actions.moc"
