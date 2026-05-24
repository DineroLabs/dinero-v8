// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "rpcclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

namespace dinero::qt::dashboard {

namespace {
QString fleetNameFor(const QString& addr) {
    if (addr.startsWith("172.93.160.131")) return "LA";
    if (addr.startsWith("173.249.195.59")) return "VA";
    if (addr.startsWith("72.18.214.120"))  return "MO";
    if (addr.startsWith("96.9.226.98"))    return "CN";
    return {};
}
}  // namespace

NodePoller::NodePoller(RpcClient* rpc, QObject* parent)
    : QObject(parent), rpc_(rpc) {
    timer_.setInterval(interval_ms_);
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &NodePoller::tick);

    // CONNECT HOOK:
    //
    // RpcClient uses TWO separate signals (not a single combined signal):
    //   rpcResult(const QString& method, const QJsonValue& result)   — success
    //   rpcError(const QString& method, int code, const QString& message) — failure
    //
    // We bridge both into onRpcResponse: success passes empty error string;
    // failure passes the message and ignores the integer code.
    if (rpc_) {
        connect(rpc_, &RpcClient::rpcResult,
                this, [this](const QString& method, const QJsonValue& result) {
            onRpcResponse(method, result, QString{});
        });
        connect(rpc_, &RpcClient::rpcError,
                this, [this](const QString& method, int /*code*/,
                             const QString& message) {
            onRpcResponse(method, QJsonValue{}, message);
        });
    }
}

void NodePoller::setIntervalMs(int ms) {
    interval_ms_ = ms;
    timer_.setInterval(ms);
}

void NodePoller::start() {
    timer_.start();
    tick();  // immediate first poll
}

void NodePoller::stop() {
    timer_.stop();
}

bool NodePoller::isRunning() const {
    return timer_.isActive();
}

void NodePoller::tick() {
    if (!rpc_) return;
    pending_peers_.clear();
    rpc_->call("getnetworkinfo");
    rpc_->call("getblockchaininfo");
    rpc_->call("getpeerinfo");
    rpc_->call("getmempoolinfo");
    rpc_->call("getmininginfo");
}

void NodePoller::onRpcResponse(const QString& method,
                               const QJsonValue& result,
                               const QString& error) {
    if (!error.isEmpty()) {
        noteFailure();
        return;
    }
    noteSuccess();

    if      (method == "getnetworkinfo")    parseNetworkInfo(result);
    else if (method == "getblockchaininfo") parseChainInfo(result);
    else if (method == "getpeerinfo")       parsePeers(result);
    else if (method == "getmempoolinfo")    parseMempool(result);
    else if (method == "getmininginfo")     parseMining(result);
}

void NodePoller::parseNetworkInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_identity_.subversion = obj.value("subversion").toString();
    pending_identity_.version    = obj.value("version").toInt();
    pending_identity_.services   = static_cast<quint64>(
        obj.value("localservices").toString().toULongLong(nullptr, 16));
    pending_identity_.node_id_hex = obj.value("localnodeid").toString();

    const auto local_addrs = obj.value("localaddresses").toArray();
    if (!local_addrs.isEmpty()) {
        const auto a = local_addrs.first().toObject();
        pending_identity_.local_addr = a.value("address").toString();
        pending_identity_.local_port = static_cast<quint16>(
            a.value("port").toInt());
    }

    if (obj.contains("relay_active")) {
        pending_identity_.is_relay_active = obj.value("relay_active").toBool();
    }
    if (obj.contains("registrants")) {
        pending_identity_.registrants_count = obj.value("registrants").toInt();
    }
    if (obj.contains("grace_pending")) {
        pending_identity_.grace_count = obj.value("grace_pending").toInt();
    }

    const bool listening = obj.value("localrelay").toBool();
    pending_identity_.reachability =
        listening ? NodeIdentity::DIRECT : NodeIdentity::UNREACHABLE;

    Q_EMIT identityUpdated(pending_identity_);
}

void NodePoller::parseChainInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.our_height = static_cast<qint64>(
        obj.value("blocks").toDouble());

    const QString bitsHex = obj.value("bits").toString();
    pending_chain_.difficulty_compact = bitsHex.isEmpty()
        ? 0u
        : bitsHex.toUInt(nullptr, 16);

    Q_EMIT chainInfoUpdated(pending_chain_);
}

void NodePoller::parsePeers(const QJsonValue& result) {
    const auto arr = result.toArray();
    pending_peers_.clear();
    pending_peers_.reserve(arr.size());

    QVector<qint64> heights;
    heights.reserve(arr.size());

    for (const auto& v : arr) {
        const auto p = v.toObject();
        PeerRow r;
        r.addr             = p.value("addr").toString();
        r.via_relay        = r.addr.startsWith("relay:");
        r.is_inbound       = p.value("inbound").toBool();
        r.fleet_name       = fleetNameFor(r.addr);
        r.height           = static_cast<qint64>(
            p.value("synced_blocks").toDouble(-1));
        r.ping_ms          = static_cast<qint64>(
            p.value("pingtime").toDouble(-1) * 1000.0);
        r.quality_score    = p.value("quality_score").toInt(-1);
        r.handshake_complete = p.value("identity_proven").toBool(true);
        r.services         = static_cast<quint64>(
            p.value("services").toString().toULongLong(nullptr, 16));
        r.subversion       = p.value("subver").toString();
        r.bytes_sent       = static_cast<qint64>(
            p.value("bytessent").toDouble(0));
        r.bytes_recv       = static_cast<qint64>(
            p.value("bytesrecv").toDouble(0));

        if (r.height > 0) heights.push_back(r.height);
        pending_peers_.push_back(r);
    }

    if (!heights.isEmpty()) {
        pending_chain_.peer_heights = heights;
        std::sort(heights.begin(), heights.end());
        pending_chain_.max_peer_height = heights.back();
        // mode (consensus)
        qint64 best = heights.first();
        int best_count = 1, run = 1;
        for (int i = 1; i < heights.size(); ++i) {
            if (heights[i] == heights[i - 1]) { ++run; }
            else { run = 1; }
            if (run > best_count) { best_count = run; best = heights[i]; }
        }
        pending_chain_.net_consensus_height = best;
        Q_EMIT chainInfoUpdated(pending_chain_);
    }

    Q_EMIT peersUpdated(pending_peers_);
}

void NodePoller::parseMempool(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.mempool_tx_count = obj.value("size").toInt();
    pending_chain_.mempool_bytes    = static_cast<qint64>(
        obj.value("bytes").toDouble());
    Q_EMIT chainInfoUpdated(pending_chain_);
}

void NodePoller::parseMining(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_identity_.is_mining = obj.value("active").toBool();
    pending_identity_.shares_per_min =
        obj.value("shares_per_minute").toDouble(0.0);
    pending_identity_.mining_destination =
        obj.value("address").toString();
    Q_EMIT identityUpdated(pending_identity_);
}

void NodePoller::noteFailure() {
    ++consecutive_failures_;
    if (consecutive_failures_ >= 3 && !degraded_) {
        degraded_ = true;
        setIntervalMs(30000);
        Q_EMIT daemonStateChanged(false);
    }
}

void NodePoller::noteSuccess() {
    consecutive_failures_ = 0;
    if (degraded_) {
        degraded_ = false;
        setIntervalMs(5000);
        Q_EMIT daemonStateChanged(true);
    }
}

}  // namespace dinero::qt::dashboard
