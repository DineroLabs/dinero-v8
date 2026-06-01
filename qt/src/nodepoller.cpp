// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nodepoller.h"
#include "rpcclient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

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

QString peerBucket(int quality_score) {
    if (quality_score < 0) return {};
    if (quality_score >= 70) return QStringLiteral("hot");
    if (quality_score >= 40) return QStringLiteral("warm");
    return QStringLiteral("demote");
}

QString compactNodeId(const QString& hex) {
    if (hex.isEmpty()) return QStringLiteral("unknown");
    if (hex.size() <= 12) return hex;
    return hex.left(6) + QStringLiteral("…") + hex.right(4);
}

QString labelForPeer(const PeerRow& peer) {
    if (!peer.fleet_name.isEmpty()) return peer.fleet_name;
    if (peer.via_relay) return QStringLiteral("relay peer");
    return peer.addr;
}

QString relayEndpointFromVirtualAddr(const QString& addr) {
    static const QString prefix = QStringLiteral("relay:in:");
    if (!addr.startsWith(prefix)) return {};
    QString body = addr.mid(prefix.size());
    if (body.endsWith(QStringLiteral(":0"))) {
        body.chop(2);
    }
    const int circuit_sep = body.lastIndexOf(':');
    if (circuit_sep <= 0) return {};
    return body.left(circuit_sep);
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

// static
QVector<HintRow> NodePoller::ParseRelayHintsList(const QJsonObject& response) {
    QVector<HintRow> out;
    const auto targets = response.value("targets").toArray();
    for (const auto& tv : targets) {
        const auto t = tv.toObject();
        const auto target_hex = t.value("target_node_id_hex").toString();
        for (const auto& ev : t.value("endpoints").toArray()) {
            const auto e = ev.toObject();
            HintRow r;
            r.target_node_id_hex = target_hex;
            r.net = e.value("net").toString();
            const auto addr = e.value("addr").toString();
            const auto port = e.value("port").toInt();
            r.endpoint = addr.isEmpty()
                ? QStringLiteral("(no addr)")
                : QStringLiteral("%1:%2").arg(addr).arg(port);
            r.age_seconds = static_cast<qint64>(
                e.value("age_seconds").toDouble(0.0));
            r.dial_failures = e.value("dial_failures").toInt(0);
            r.near_eviction = e.value("near_eviction").toBool(false);
            out.append(r);
        }
    }
    return out;
}

// static
TopologySnapshot NodePoller::BuildTopologySnapshot(
        const NodeIdentity& identity,
        const QVector<PeerRow>& peers,
        const QVector<HintRow>& hints,
        const DynamicP2POverview& /*overview*/) {
    TopologySnapshot snapshot;

    TopologyNode self;
    self.id = QStringLiteral("self");
    self.label = identity.node_id_hex.isEmpty()
        ? QStringLiteral("This node")
        : QStringLiteral("This node · %1").arg(compactNodeId(identity.node_id_hex));
    self.endpoint = identity.local_addr.isEmpty()
        ? (identity.local_port > 0
              ? QStringLiteral(":%1").arg(identity.local_port)
              : QString{})
        : QStringLiteral("%1:%2").arg(identity.local_addr).arg(identity.local_port);
    self.kind = QStringLiteral("self");
    self.connected = true;
    snapshot.nodes.push_back(self);

    QSet<QString> hint_target_nodes;
    for (const auto& peer : peers) {
        TopologyNode node;
        node.id = QStringLiteral("peer:%1").arg(peer.addr);
        node.label = labelForPeer(peer);
        node.endpoint = peer.addr;
        node.kind = peer.via_relay
            ? QStringLiteral("relay_virtual")
            : (peer.fleet_name.isEmpty() ? QStringLiteral("direct")
                                         : QStringLiteral("fleet"));
        node.bucket = peerBucket(peer.quality_score);
        node.quality_score = peer.quality_score;
        node.connected = true;
        snapshot.nodes.push_back(node);

        TopologyEdge edge;
        edge.from_id = QStringLiteral("self");
        edge.to_id = node.id;
        edge.kind = peer.via_relay ? QStringLiteral("relay_virtual")
                                   : QStringLiteral("direct");
        edge.via_relay = peer.relay_via_addr.isEmpty()
            ? relayEndpointFromVirtualAddr(peer.addr)
            : peer.relay_via_addr;
        snapshot.edges.push_back(edge);
    }

    QVector<HintRow> sorted_hints = hints;
    std::sort(sorted_hints.begin(), sorted_hints.end(),
              [](const HintRow& a, const HintRow& b) {
        if (a.target_node_id_hex != b.target_node_id_hex) {
            return a.target_node_id_hex < b.target_node_id_hex;
        }
        return a.endpoint < b.endpoint;
    });
    for (const auto& hint : sorted_hints) {
        if (hint.target_node_id_hex.isEmpty()) continue;
        const QString node_id = QStringLiteral("hint:%1").arg(hint.target_node_id_hex);
        if (!hint_target_nodes.contains(node_id)) {
            TopologyNode node;
            node.id = node_id;
            node.label = QStringLiteral("hint %1")
                .arg(compactNodeId(hint.target_node_id_hex));
            node.endpoint = hint.endpoint;
            node.kind = QStringLiteral("hint");
            node.bucket = QStringLiteral("relay_candidate");
            node.quality_score = -1;
            node.connected = false;
            snapshot.nodes.push_back(node);
            hint_target_nodes.insert(node_id);
        }

        TopologyEdge edge;
        edge.from_id = QStringLiteral("self");
        edge.to_id = node_id;
        edge.kind = QStringLiteral("hint");
        edge.via_relay = hint.endpoint;
        snapshot.edges.push_back(edge);
    }

    return snapshot;
}

void NodePoller::pushSparklineSample(QVector<qint64>* buf, qint64 sample) {
    if (!buf) return;
    buf->append(sample < 0 ? 0 : sample);
    while (buf->size() > kSparklineCapacity) {
        buf->removeFirst();
    }
}

void NodePoller::AccumulateLongerWindow(LongerWindowAccumulator& acc,
                                        qint64 sample) {
    acc.partial_minute_sum += sample;
    if (++acc.partial_minute_ticks >= 12) {  // 12 × 5s = 1min
        acc.minute_buffer.append(acc.partial_minute_sum);
        while (acc.minute_buffer.size() > 60) acc.minute_buffer.removeFirst();
        acc.partial_hour_sum += acc.partial_minute_sum;
        acc.partial_minute_sum = 0;
        acc.partial_minute_ticks = 0;
        if (++acc.partial_hour_minutes >= 60) {
            acc.hour_buffer.append(acc.partial_hour_sum);
            while (acc.hour_buffer.size() > 24) acc.hour_buffer.removeFirst();
            acc.partial_hour_sum = 0;
            acc.partial_hour_minutes = 0;
        }
    }
}

qint64 NodePoller::AverageOverWindow(const QVector<qint64>& buf) {
    if (buf.isEmpty()) return 0;
    qint64 s = 0;
    for (auto v : buf) s += v;
    return s / buf.size();
}

void NodePoller::emitContributionAndScore() {
    ContributionStats stats;
    stats.blocks_served_24h   = pending_blocks_served_24h_;
    stats.bytes_relayed_24h   = pending_bytes_relayed_24h_;
    stats.registrants_active  = pending_registrants_active_;
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
    in.bytes_relayed_24h = pending_bytes_relayed_24h_;
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
    rpc_->call("relay_hints.list");
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
    else if (method == "relay_hints.list")  parseHints(result);
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
        // Phase 2b — real 24h counters replace Phase 2a placeholders and
        // the bytes_relayed linear extrapolation.
        pending_blocks_served_24h_ =
            static_cast<qint64>(relay.value("blocks_served_24h").toDouble(0.0));
        pending_bytes_relayed_24h_ =
            static_cast<qint64>(relay.value("bytes_relayed_24h").toDouble(0.0));
    }
    if (obj.contains("registrants")) {
        pending_identity_.registrants_count = obj.value("registrants").toInt();
    }
    // Phase 2b — reuse the already-parsed registrants_count rather than
    // adding a duplicate relay.registrants_count parse path: the daemon
    // emits registrants at the top level, not nested inside relay.
    pending_registrants_active_ = pending_identity_.registrants_count;
    if (obj.contains("grace_pending")) {
        pending_identity_.grace_count = obj.value("grace_pending").toInt();
    }

    // Reachability — mirror the daemon's computed verdict (same fields
    // `node.status` uses) so the dashboard and `dinero-cli node.status` agree,
    // one source of truth. DIRECT iff the daemon computed direct_reachable.
    // BEHIND_RELAY uses the daemon's relay_fallback_eligible (network_active &&
    // listening && !direct) — NOT bare "listen", which would falsely claim
    // relay reachability for any listening node with no relay path. Falls back
    // to listen/localrelay on daemons predating relay_fallback_eligible.
    const bool direct_ok = obj.value("direct_reachable").toBool(false);
    const bool relay_eligible = obj.value("relay_fallback_eligible").toBool(
        obj.value("listen").toBool(obj.value("localrelay").toBool(false)));
    pending_identity_.reachability =
        direct_ok ? NodeIdentity::DIRECT
                  : (relay_eligible ? NodeIdentity::BEHIND_RELAY
                                    : NodeIdentity::UNREACHABLE);

    Q_EMIT identityUpdated(pending_identity_);
    emitContributionAndScore();
    emitTopologySnapshot();
}

void NodePoller::parseChainInfo(const QJsonValue& result) {
    const auto obj = result.toObject();
    pending_chain_.our_height = static_cast<qint64>(
        obj.value("blocks").toDouble());

    pending_chain_.difficulty = obj.value("difficulty").toDouble(0.0);
    if (pending_chain_.net_consensus_height <= 0 && pending_chain_.our_height > 0) {
        pending_chain_.net_consensus_height = pending_chain_.our_height;
    }
    if (pending_chain_.max_peer_height <= 0 && pending_chain_.our_height > 0) {
        pending_chain_.max_peer_height = pending_chain_.our_height;
    }

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
        r.relay_via_addr   = p.value("relay_via_addr").toString();
        r.is_inbound       = p.value("inbound").toBool();
        r.fleet_name       = fleetNameFor(r.addr);
        r.height = static_cast<qint64>(
            p.value("observed_sync_height").toDouble(
                p.value("synced_blocks").toDouble(
                    p.value("bestknownheight").toDouble(
                        p.value("startingheight").toDouble(-1)))));
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
    emitTopologySnapshot();

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
    AccumulateLongerWindow(bytes_in_long_,    delta_recv);
    AccumulateLongerWindow(bytes_out_long_,   delta_sent);
    AccumulateLongerWindow(relay_bytes_long_, delta_relay);
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
    pending_chain_.has_median_fee = obj.contains("median_fee_rate");
    pending_chain_.median_fee_una_per_vbyte =
        obj.value("median_fee_rate").toDouble(0.0);
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
    pending_dynamic_p2p_ = o;
    Q_EMIT dynamicP2POverviewUpdated(o);
    emitTopologySnapshot();
}

void NodePoller::parseHints(const QJsonValue& result) {
    pending_hints_ = ParseRelayHintsList(result.toObject());
    Q_EMIT hintsUpdated(pending_hints_);
    emitTopologySnapshot();
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

void NodePoller::emitTopologySnapshot() {
    Q_EMIT topologyUpdated(
        BuildTopologySnapshot(pending_identity_, pending_peers_,
                              pending_hints_, pending_dynamic_p2p_));
}

}  // namespace dinero::qt::dashboard
