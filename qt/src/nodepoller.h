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

    void parseNetworkInfo(const QJsonValue& result);
    void parseChainInfo(const QJsonValue& result);
    void parsePeers(const QJsonValue& result);
    void parseDynamicP2POverview(const QJsonValue& result);
    void parseMempool(const QJsonValue& result);
    void parseMining(const QJsonValue& result);
    void noteFailure();
    void noteSuccess();
};

}  // namespace dinero::qt::dashboard
