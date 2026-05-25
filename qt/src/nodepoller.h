// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"

#include <QObject>
#include <QTimer>

class RpcClient;

namespace dinero::qt::dashboard {

// Drives 5-second polling of getnetworkinfo / getpeerinfo /
// getblockchaininfo / getrelayinfo / getmempoolinfo and emits typed
// updates. Sections subscribe to the signals they need.
//
// Owns no UI. Safe to live anywhere; we instantiate one per
// MyNodeDashboard.
class NodePoller : public QObject {
    Q_OBJECT

public:
    explicit NodePoller(RpcClient* rpc, QObject* parent = nullptr);

    // Cadence is a constructor-time default; tests can override.
    void setIntervalMs(int ms);

    // Inputs to the decentralization score (separated from poll state
    // so tests can drive the formula deterministically). All values
    // are observable signals from the polled RPCs + LocalMiningProvider.
    struct ScoreInputs {
        bool   reachable_with_inbound{false};
        bool   relay_active_with_registrants{false};
        qint64 uptime_seconds{0};
        int    unique_peer_subnets_slash16{0};
        qint64 bytes_relayed_24h{0};
        double local_hashrate_hps{0.0};
        double fleet_hashrate_hps{0.0};
        int    peers_who_learned_via_gossip{0};
    };

    static DecentralizationScore ComputeDecentralizationScore(
        const ScoreInputs& inputs);

    // Start/stop the timer. Start triggers an immediate first poll.
    void start();
    void stop();
    bool isRunning() const;

Q_SIGNALS:
    void identityUpdated(const NodeIdentity& identity);
    void chainInfoUpdated(const ChainInfo& info);
    void peersUpdated(const QVector<PeerRow>& peers);
    void daemonStateChanged(bool reachable);
    void dynamicP2POverviewUpdated(const DynamicP2POverview& overview);
    void contributionStatsUpdated(const ContributionStats& stats);
    void decentralizationScoreUpdated(const DecentralizationScore& score);

public Q_SLOTS:
    // Public so tests can feed canned responses without a real RpcClient.
    void onRpcResponse(const QString& method,
                       const QJsonValue& result,
                       const QString& error);

private Q_SLOTS:
    void tick();

private:
    RpcClient* rpc_{nullptr};
    QTimer     timer_;
    int        interval_ms_{5000};

    // Coalescing state — we accumulate per-RPC parses and emit the
    // relevant signal as each arrives.
    NodeIdentity pending_identity_;
    ChainInfo    pending_chain_;
    QVector<PeerRow> pending_peers_;

    // Daemon-reachable state machine. degraded == 3 consecutive RPC
    // failures.
    int  consecutive_failures_{0};
    bool degraded_{false};

    // Rolling 5-minute sparkline buffers (1 sample per 5s poll tick).
    static constexpr int kSparklineCapacity = 60;
    QVector<qint64> bytes_in_buffer_;
    QVector<qint64> bytes_out_buffer_;
    QVector<qint64> relay_bytes_buffer_;

    qint64 last_bytes_sent_total_{0};
    qint64 last_bytes_recv_total_{0};
    qint64 last_relay_bytes_total_{0};
    bool   have_baseline_{false};

    int    relay_hints_sent_{0};
    int    relay_hints_received_relay_{0};

    void parseNetworkInfo(const QJsonValue& result);
    void parseChainInfo(const QJsonValue& result);
    void parsePeers(const QJsonValue& result);
    void parseDynamicP2POverview(const QJsonValue& result);
    void parseMempool(const QJsonValue& result);
    void parseMining(const QJsonValue& result);
    void noteFailure();
    void noteSuccess();
    void pushSparklineSample(QVector<qint64>* buf, qint64 sample);
    void emitContributionAndScore();
};

}  // namespace dinero::qt::dashboard
