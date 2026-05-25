// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "rpcclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace dinero::qt::dashboard {

namespace {
QString fleetNameFor(const QString& addr) {
    if (addr.startsWith("172.93.160.131")) return "LA";
    if (addr.startsWith("173.249.195.59")) return "VA";
    if (addr.startsWith("72.18.214.120"))  return "MO";
    if (addr.startsWith("96.9.226.98"))    return "CN";
    return {};
}

QString scoreLabel(double total) {
    if (total < 2.0) return "just observing";
    if (total < 4.0) return "consuming responsibly";
    if (total < 6.0) return "pulling your weight";
    if (total < 8.0) return "you're carrying real weight";
    return "you're load-bearing for the network";
}

double linearCap(double value, double cap_at_one) {
    if (cap_at_one <= 0.0) return 0.0;
    return std::min(1.0, std::max(0.0, value / cap_at_one));
}
}  // namespace

// static
DecentralizationScore NodePoller::ComputeDecentralizationScore(
        const ScoreInputs& in) {
    DecentralizationScore s;

    s.breakdown.reachable = in.reachable_with_inbound ? 1.0 : 0.0;
    s.breakdown.relay_active = in.relay_active_with_registrants ? 2.0 : 0.0;

    constexpr qint64 kThirtyDaysSec = 30LL * 24 * 3600;
    s.breakdown.uptime =
        linearCap(static_cast<double>(in.uptime_seconds), kThirtyDaysSec) * 1.5;

    s.breakdown.peer_diversity =
        linearCap(static_cast<double>(in.unique_peer_subnets_slash16), 8.0) * 1.5;

    double traffic_log = 0.0;
    if (in.bytes_relayed_24h > 0) {
        traffic_log = std::log10(static_cast<double>(in.bytes_relayed_24h));
    }
    s.breakdown.traffic = linearCap(traffic_log, 9.0) * 1.0;

    double mining_ratio = 0.0;
    if (in.fleet_hashrate_hps > 0.0) {
        mining_ratio = in.local_hashrate_hps / in.fleet_hashrate_hps;
    }
    s.breakdown.mining = linearCap(mining_ratio, 1.0) * 1.5;

    s.breakdown.gossip_reach =
        linearCap(static_cast<double>(in.peers_who_learned_via_gossip), 32.0) * 1.5;

    s.total = s.breakdown.reachable
            + s.breakdown.relay_active
            + s.breakdown.uptime
            + s.breakdown.peer_diversity
            + s.breakdown.traffic
            + s.breakdown.mining
            + s.breakdown.gossip_reach;
    if (s.total < 0.0)  s.total = 0.0;
    if (s.total > 10.0) s.total = 10.0;

    s.label = scoreLabel(s.total);
    return s;
}

void NodePoller::pushSparklineSample(QVector<qint64>* buf, qint64 sample) {
    if (!buf) return;
    buf->append(sample < 0 ? 0 : sample);
    while (buf->size() > kSparklineCapacity) {
        buf->removeFirst();
    }
}

void NodePoller::emitContributionAndScore() {
    ContributionStats stats;
    stats.circuits_active     = 0;  // Phase 2b: real counter from daemon
    stats.blocks_served_today = 0;  // Phase 2b: real counter from daemon
    stats.hints_sent          = relay_hints_sent_;
    stats.peers_via_gossip    = relay_hints_received_relay_;
    stats.bytes_in_rate    = bytes_in_buffer_.isEmpty()  ? 0 : bytes_in_buffer_.last();
    stats.bytes_out_rate   = bytes_out_buffer_.isEmpty() ? 0 : bytes_out_buffer_.last();
    stats.relay_bytes_rate = relay_bytes_buffer_.isEmpty() ? 0 : relay_bytes_buffer_.last();
    Q_EMIT contributionStatsUpdated(stats);

    ScoreInputs in;
    in.reachable_with_inbound =
        (pending_identity_.reachability == NodeIdentity::DIRECT)
        && (pending_peers_.size() > 0);
    in.relay_active_with_registrants =
        pending_identity_.is_relay_active
        && (pending_identity_.registrants_count > 0);
    in.uptime_seconds = pending_identity_.uptime.count();
    QVector<QString> subnets;
    for (const auto& r : pending_peers_) {
        const auto dot1 = r.addr.indexOf('.');
        if (dot1 > 0) {
            const auto dot2 = r.addr.indexOf('.', dot1 + 1);
            if (dot2 > 0) {
                const auto sub16 = r.addr.left(dot2);
                if (!subnets.contains(sub16)) subnets.append(sub16);
            }
        }
    }
    in.unique_peer_subnets_slash16 = subnets.size();
    qint64 sum_relay = 0;
    for (auto v : relay_bytes_buffer_) sum_relay += v;
    // 5min × 288 = 24h linear extrapolation (Phase 2a approximation).
    in.bytes_relayed_24h = sum_relay * 288;
    in.local_hashrate_hps = pending_identity_.shares_per_min * 1'000'000.0;
    in.fleet_hashrate_hps = pending_chain_.network_hashrate_hps;
    in.peers_who_learned_via_gossip = relay_hints_received_relay_;
    Q_EMIT decentralizationScoreUpdated(ComputeDecentralizationScore(in));
}

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
    rpc_->call("mining.status");
    rpc_->call("dynamic_p2p.observe");
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
    else if (method == "mining.status")     parseMining(result);
    else if (method == "dynamic_p2p.observe") parseDynamicP2POverview(result);
}

void NodePoller::parseNetworkInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_identity_.subversion = obj.value("subversion").toString();
    pending_identity_.version    = obj.value("version").toInt();
    // node_id_hex was added to getnetworkinfo in dinero-v8 PR #140. Empty
    // string on daemons predating that PR — Identity section renders "—".
    pending_identity_.node_id_hex = obj.value("node_id_hex").toString();

    // localaddresses is empty for NAT'd home nodes — fall back to listen_port.
    const auto local_addrs = obj.value("localaddresses").toArray();
    if (!local_addrs.isEmpty()) {
        const auto a = local_addrs.first().toObject();
        pending_identity_.local_addr = a.value("address").toString();
        pending_identity_.local_port = static_cast<quint16>(
            a.value("port").toInt());
    } else if (obj.contains("listen_port")) {
        pending_identity_.local_addr.clear();
        pending_identity_.local_port = static_cast<quint16>(
            obj.value("listen_port").toInt());
    }

    // This daemon nests relay state under getnetworkinfo.relay = { active,
    // local, mode, fallback_eligible }. Bitcoin-Core convention of flat
    // relay_active doesn't apply.
    if (obj.contains("relay")) {
        const auto relay = obj.value("relay").toObject();
        pending_identity_.is_relay_active = relay.value("active").toBool(false);
        const auto hints = relay.value("hints").toObject();
        relay_hints_sent_           = hints.value("received_self").toInt(0);
        relay_hints_received_relay_ = hints.value("received_relay").toInt(0);
    }
    if (obj.contains("registrants")) {
        pending_identity_.registrants_count = obj.value("registrants").toInt();
    }
    if (obj.contains("grace_pending")) {
        pending_identity_.grace_count = obj.value("grace_pending").toInt();
    }

    // Reachability: direct_reachable (true = NAT open, listener confirmed),
    // listen (true = we accept inbound), else UNREACHABLE.
    const bool direct_ok = obj.value("direct_reachable").toBool(false);
    const bool listening = obj.value("listen").toBool(
                               obj.value("localrelay").toBool(false));
    pending_identity_.reachability =
        direct_ok ? NodeIdentity::DIRECT
                  : (listening ? NodeIdentity::BEHIND_RELAY
                               : NodeIdentity::UNREACHABLE);

    Q_EMIT identityUpdated(pending_identity_);
    emitContributionAndScore();
}

void NodePoller::parseChainInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.our_height = static_cast<qint64>(
        obj.value("blocks").toDouble());

    // This daemon returns difficulty as a decimal double (1310.72), not the
    // Bitcoin nBits compact-form hex string. Round to a uint for display;
    // tiny precision loss is acceptable at the dashboard granularity.
    pending_chain_.difficulty_compact = static_cast<quint32>(
        obj.value("difficulty").toDouble(0.0));

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
        // ping_ms + quality_score added in dinero-v8 PR #140. Daemons
        // predating that PR omit the fields; -1 / -1 keeps the dashboard's
        // "—" / "○" rendering for the un-upgraded case.
        if (p.contains("ping_ms")) {
            r.ping_ms = static_cast<qint64>(p.value("ping_ms").toInt(-1));
        } else if (p.contains("pingtime")) {
            // Bitcoin-Core fallback path (seconds → ms)
            r.ping_ms = static_cast<qint64>(p.value("pingtime").toDouble(-1) * 1000.0);
        } else {
            r.ping_ms = -1;
        }
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

    // Phase 2a: accumulate bytes for sparkline + per-tick rate stats.
    qint64 cur_sent_total = 0;
    qint64 cur_recv_total = 0;
    qint64 cur_relay_total = 0;
    for (const auto& r : pending_peers_) {
        cur_sent_total += r.bytes_sent;
        cur_recv_total += r.bytes_recv;
        if (r.via_relay) cur_relay_total += r.bytes_sent + r.bytes_recv;
    }
    if (!have_baseline_) {
        last_bytes_sent_total_  = cur_sent_total;
        last_bytes_recv_total_  = cur_recv_total;
        last_relay_bytes_total_ = cur_relay_total;
        have_baseline_ = true;
    }
    const qint64 delta_recv  = std::max<qint64>(0, cur_recv_total  - last_bytes_recv_total_);
    const qint64 delta_sent  = std::max<qint64>(0, cur_sent_total  - last_bytes_sent_total_);
    const qint64 delta_relay = std::max<qint64>(0, cur_relay_total - last_relay_bytes_total_);
    pushSparklineSample(&bytes_in_buffer_,    delta_recv);
    pushSparklineSample(&bytes_out_buffer_,   delta_sent);
    pushSparklineSample(&relay_bytes_buffer_, delta_relay);
    last_bytes_sent_total_  = cur_sent_total;
    last_bytes_recv_total_  = cur_recv_total;
    last_relay_bytes_total_ = cur_relay_total;

    emitContributionAndScore();
}

void NodePoller::parseMempool(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.mempool_tx_count = obj.value("size").toInt();
    pending_chain_.mempool_bytes    = static_cast<qint64>(
        obj.value("bytes").toDouble());
    Q_EMIT chainInfoUpdated(pending_chain_);
}

void NodePoller::parseMining(const QJsonValue& result) {
    // This daemon's mining state lives in the mining.status RPC, NOT in
    // getmininginfo (which exposes the obsolete setgenerate CPU-miner flag).
    // Stratum server is a separate binary (dinero-stratum), so daemon-side
    // mining.status reflects ONLY in-daemon mining — UI-driven Stratum
    // mining won't appear here. That's a Phase 2 follow-up.
    const auto obj = result.toObject();
    pending_identity_.is_mining = obj.value("mining").toBool(false);
    // hashrate is hashes/second; convert to a "shares_per_min"-like display
    // value by treating it as MH/s. Real shares/min requires Stratum.
    const double hps = obj.value("hashrate").toDouble(0.0);
    pending_identity_.shares_per_min = hps / 1'000'000.0;  // MH/s
    if (obj.contains("address") && !obj.value("address").isNull()) {
        pending_identity_.mining_destination =
            obj.value("address").toString();
    }
    pending_chain_.network_hashrate_hps =
        obj.value("networkhashps").toDouble(0.0);
    Q_EMIT identityUpdated(pending_identity_);
}

void NodePoller::parseDynamicP2POverview(const QJsonValue& result) {
    const auto obj = result.toObject();
    DynamicP2POverview o;
    o.enabled = obj.value("enabled").toBool(false);
    o.mode    = obj.value("mode").toString();
    const auto governor = obj.value("governor").toObject();
    if (!governor.isEmpty()) {
        o.connected_outbound = governor.value("connected_outbound").toInt(0);
        o.hot_peers          = governor.value("hot_peers").toArray().size();
        o.warm_candidates    = governor.value("warm_candidates").toArray().size();
        o.relay_registration_candidates =
            governor.value("relay_registration_candidates").toArray().size();
        o.demote_candidates  = governor.value("demote_candidates").toArray().size();
    }
    Q_EMIT dynamicP2POverviewUpdated(o);
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
