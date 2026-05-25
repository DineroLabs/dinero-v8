// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using dinero::qt::dashboard::HintRow;
using dinero::qt::dashboard::NodePoller;

namespace {
QJsonObject parseJson(const char* s) {
    return QJsonDocument::fromJson(s).object();
}
}

class TestRelayHintsParser : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void empty_response_yields_empty_vector() {
        auto obj = parseJson(R"({
            "rpc_schema": "din.rpc.v1",
            "targets": [],
            "total_targets": 0,
            "ttl_seconds": 900,
            "max_failures": 3
        })");
        const auto rows = NodePoller::ParseRelayHintsList(obj);
        QCOMPARE(rows.size(), 0);
    }

    void single_entry_round_trips_fields() {
        auto obj = parseJson(R"({
            "rpc_schema": "din.rpc.v1",
            "targets": [
                {
                    "target_node_id_hex": "0102030405060708090a0102030405060708090a",
                    "endpoints": [
                        {
                            "net": "ipv4",
                            "addr": "203.0.113.7",
                            "port": 20999,
                            "age_seconds": 47,
                            "dial_failures": 0,
                            "near_eviction": false
                        }
                    ]
                }
            ],
            "total_targets": 1,
            "ttl_seconds": 900,
            "max_failures": 3
        })");
        const auto rows = NodePoller::ParseRelayHintsList(obj);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].target_node_id_hex,
                 QString("0102030405060708090a0102030405060708090a"));
        QCOMPARE(rows[0].endpoint, QString("203.0.113.7:20999"));
        QCOMPARE(rows[0].net, QString("ipv4"));
        QCOMPARE(rows[0].age_seconds, qint64(47));
        QCOMPARE(rows[0].dial_failures, 0);
        QCOMPARE(rows[0].near_eviction, false);
    }

    void target_with_multiple_endpoints_yields_one_row_per_endpoint() {
        auto obj = parseJson(R"({
            "rpc_schema": "din.rpc.v1",
            "targets": [
                {
                    "target_node_id_hex": "aa",
                    "endpoints": [
                        {"net": "ipv4", "addr": "1.1.1.1", "port": 1, "age_seconds": 1, "dial_failures": 0, "near_eviction": false},
                        {"net": "ipv4", "addr": "2.2.2.2", "port": 2, "age_seconds": 2, "dial_failures": 0, "near_eviction": false}
                    ]
                }
            ],
            "total_targets": 1,
            "ttl_seconds": 900,
            "max_failures": 3
        })");
        const auto rows = NodePoller::ParseRelayHintsList(obj);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].endpoint, QString("1.1.1.1:1"));
        QCOMPARE(rows[1].endpoint, QString("2.2.2.2:2"));
    }
};

QTEST_MAIN(TestRelayHintsParser)
#include "test_relay_hints_parser.moc"
