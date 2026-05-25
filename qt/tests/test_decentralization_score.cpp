// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "dashboardtypes.h"

#include <QtTest/QtTest>

using dinero::qt::dashboard::DecentralizationScore;
using dinero::qt::dashboard::NodePoller;

namespace {
NodePoller::ScoreInputs zeroInputs() { return {}; }
}  // namespace

class TestDecentralizationScore : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void all_zero_yields_zero_and_just_observing() {
        const auto s = NodePoller::ComputeDecentralizationScore(zeroInputs());
        QCOMPARE(s.total, 0.0);
        QCOMPARE(s.label, QString("just observing"));
    }

    void reachable_alone_is_one() {
        auto in = zeroInputs();
        in.reachable_with_inbound = true;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.reachable, 1.0);
        QCOMPARE(s.total, 1.0);
    }

    void relay_active_is_worth_two() {
        auto in = zeroInputs();
        in.relay_active_with_registrants = true;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.relay_active, 2.0);
    }

    void uptime_30_days_is_full_one_point_five() {
        auto in = zeroInputs();
        in.uptime_seconds = 30LL * 24 * 3600;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.uptime, 1.5);
    }

    void uptime_caps_at_full_for_long_runs() {
        auto in = zeroInputs();
        in.uptime_seconds = 365LL * 24 * 3600;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.uptime, 1.5);
    }

    void eight_subnets_is_full_peer_diversity() {
        auto in = zeroInputs();
        in.unique_peer_subnets_slash16 = 8;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.peer_diversity, 1.5);
    }

    void one_gigabyte_relayed_is_full_traffic_score() {
        auto in = zeroInputs();
        in.bytes_relayed_24h = 1'000'000'000;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.traffic, 1.0);
    }

    void mining_ratio_caps_at_one_full_weight() {
        auto in = zeroInputs();
        in.local_hashrate_hps = 1e9;
        in.fleet_hashrate_hps = 1e8;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.breakdown.mining, 1.5);
    }

    void all_max_inputs_yields_ten_and_load_bearing() {
        NodePoller::ScoreInputs in;
        in.reachable_with_inbound = true;
        in.relay_active_with_registrants = true;
        in.uptime_seconds = 30LL * 24 * 3600;
        in.unique_peer_subnets_slash16 = 8;
        in.bytes_relayed_24h = 1'000'000'000;
        in.local_hashrate_hps = 1.0;
        in.fleet_hashrate_hps = 1.0;
        in.peers_who_learned_via_gossip = 32;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.total, 10.0);
        QCOMPARE(s.label, QString("you're load-bearing for the network"));
    }

    void score_clamps_to_ten_max() {
        NodePoller::ScoreInputs in;
        in.reachable_with_inbound = true;
        in.relay_active_with_registrants = true;
        in.uptime_seconds = 365LL * 24 * 3600;
        in.unique_peer_subnets_slash16 = 100;
        in.bytes_relayed_24h = qint64(1e15);
        in.local_hashrate_hps = 1e15;
        in.fleet_hashrate_hps = 1.0;
        in.peers_who_learned_via_gossip = 10'000;
        const auto s = NodePoller::ComputeDecentralizationScore(in);
        QCOMPARE(s.total, 10.0);
    }

    void label_buckets_match_spec() {
        auto in = zeroInputs();
        in.reachable_with_inbound = true;
        QCOMPARE(NodePoller::ComputeDecentralizationScore(in).label,
                 QString("just observing"));

        in.relay_active_with_registrants = true;
        QCOMPARE(NodePoller::ComputeDecentralizationScore(in).label,
                 QString("consuming responsibly"));
    }
};

QTEST_MAIN(TestDecentralizationScore)
#include "test_decentralization_score.moc"
