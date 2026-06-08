#include "aitools.h"
#include "rpcclient.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

AiTools::AiTools(RpcClient* rpc, QObject* parent)
    : QObject(parent)
    , rpc_(rpc)
{
    connect(rpc_, &RpcClient::rpcResult, this, &AiTools::onRpcResult);
}

// ---------------------------------------------------------------------------
// Tool definitions for Claude
// ---------------------------------------------------------------------------

static QJsonObject makeToolDef(const QString& name, const QString& desc,
                                const QJsonObject& props = {},
                                const QJsonArray& required = {})
{
    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = props;
    if (!required.isEmpty()) schema["required"] = required;

    QJsonObject tool;
    tool["name"] = name;
    tool["description"] = desc;
    tool["input_schema"] = schema;
    return tool;
}

QJsonArray AiTools::toolDefinitions() const
{
    QJsonArray tools;

    tools.append(makeToolDef(
        "get_node_status",
        "Get current node synchronization status including block height, "
        "headers, sync percentage, chain name, and daemon version."));

    tools.append(makeToolDef(
        "get_wallet_summary",
        "Get wallet balance breakdown: confirmed, unconfirmed, and immature "
        "balances in DIN, plus wallet name and encryption status."));

    tools.append(makeToolDef(
        "get_mining_status",
        "Get mining status: whether mining is active, current hashrate, "
        "thread count, blocks found, and network difficulty."));

    tools.append(makeToolDef(
        "get_peer_info",
        "Get list of connected P2P peers with IP address, latency, "
        "version string, and sync height."));

    tools.append(makeToolDef(
        "get_mempool_info",
        "Get mempool statistics: transaction count, total size in bytes, "
        "and minimum relay fee."));

    QJsonObject countProp;
    countProp["type"] = "integer";
    countProp["description"] = "Number of recent transactions to return (default 10, max 50)";
    QJsonObject txProps;
    txProps["count"] = countProp;

    tools.append(makeToolDef(
        "get_recent_transactions",
        "Get recent wallet transactions with amounts, confirmations, "
        "direction (sent/received/mined), and timestamps.",
        txProps));

    tools.append(makeToolDef(
        "get_network_health",
        "Get synthesized network health report: sync status, peer quality, "
        "mempool pressure, and overall health assessment with any issues."));

    tools.append(makeToolDef(
        "get_fleet_status",
        "Get live status of the current Dinero fleet (Mac, LA, SJ, NA, EU1) "
        "including height, peers, balance, consensus alignment, "
        "and privacy lane status. Returns data from the fleet dashboard."));

    QJsonObject targetProp;
    targetProp["type"] = "integer";
    targetProp["description"] = "Target number of blocks for confirmation (default 6)";
    QJsonObject feeProps;
    feeProps["conf_target"] = targetProp;

    tools.append(makeToolDef(
        "estimate_fee",
        "Estimate transaction fee rate for a target confirmation time.",
        feeProps));

    return tools;
}

// ---------------------------------------------------------------------------
// Tool execution
// ---------------------------------------------------------------------------

void AiTools::executeTool(const QString& name, const QJsonObject& input,
                           std::function<void(const QString&)> callback)
{
    if (name == "get_node_status")          return getNodeStatus(input, callback);
    if (name == "get_wallet_summary")       return getWalletSummary(input, callback);
    if (name == "get_mining_status")        return getMiningStatus(input, callback);
    if (name == "get_peer_info")            return getPeerInfo(input, callback);
    if (name == "get_mempool_info")         return getMempoolInfo(input, callback);
    if (name == "get_recent_transactions")  return getRecentTransactions(input, callback);
    if (name == "get_network_health")       return getNetworkHealth(input, callback);
    if (name == "estimate_fee")             return estimateFee(input, callback);
    if (name == "get_fleet_status")         return getFleetStatus(input, callback);

    // Unknown tool
    QJsonObject err;
    err["error"] = QString("Unknown tool: %1").arg(name);
    callback(QJsonDocument(err).toJson(QJsonDocument::Compact));
}

// ---------------------------------------------------------------------------
// RPC result dispatcher
// ---------------------------------------------------------------------------

void AiTools::onRpcResult(const QString& method, const QJsonValue& result)
{
    auto it = pendingCallbacks_.find(method);
    if (it != pendingCallbacks_.end()) {
        auto cb = it.value().onResult;
        pendingCallbacks_.erase(it);
        if (cb) cb(result);
    }
}

// Helper: fire an RPC call and register a callback
void AiTools::getNodeStatus(const QJsonObject&, std::function<void(const QString&)> cb)
{
    int cached = cachedPeerCount_;
    PendingCallback pc;
    pc.onResult = [cb, cached](const QJsonValue& result) {
        auto obj = result.toObject();
        QJsonObject out;
        int blocks = obj["blocks"].toInt();
        int headers = obj["headers"].toInt();
        out["height"] = blocks;
        out["headers"] = headers;
        double syncPct = (headers > 0) ? (100.0 * blocks / headers) : 0.0;
        out["sync_percent"] = QString::number(syncPct, 'f', 1).toDouble();
        out["chain"] = obj["chain"].toString();
        out["version"] = obj["version"].toString();
        out["difficulty"] = obj["difficulty"];
        out["connections"] = (cached >= 0) ? cached : obj["connections"].toInt();
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("blockchain.getinfo", pc);
    rpc_->getInfo();
}

void AiTools::getWalletSummary(const QJsonObject&, std::function<void(const QString&)> cb)
{
    PendingCallback pc;
    pc.onResult = [cb](const QJsonValue& result) {
        auto obj = result.toObject();
        QJsonObject out;
        out["confirmed"] = obj["confirmed"].toDouble();
        out["unconfirmed"] = obj["unconfirmed"].toDouble();
        out["immature"] = obj["immature"].toDouble();
        out["total"] = obj["total"].toDouble();
        out["wallet_name"] = obj["wallet_name"].toString();
        out["encrypted"] = obj["encrypted"].toBool();
        out["locked"] = obj["locked"].toBool();
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("wallet.getbalance", pc);
    rpc_->getBalance();
}

void AiTools::getMiningStatus(const QJsonObject&, std::function<void(const QString&)> cb)
{
    PendingCallback pc;
    pc.onResult = [cb](const QJsonValue& result) {
        auto obj = result.toObject();
        QJsonObject out;
        out["active"] = obj["mining"].toBool();
        out["hashrate_hps"] = obj["hashrate"].toDouble();
        out["threads"] = obj["threads"].toInt();
        out["blocks_found"] = obj["blocks"].toInt();
        out["difficulty"] = obj["difficulty"];
        out["network_hashrate"] = obj["networkhashps"].toDouble();
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("blockchain.getmininginfo", pc);
    rpc_->getMiningInfo();
}

void AiTools::getPeerInfo(const QJsonObject&, std::function<void(const QString&)> cb)
{
    PendingCallback pc;
    pc.onResult = [cb](const QJsonValue& result) {
        auto arr = result.toArray();
        QJsonArray peers;
        for (const auto& p : arr) {
            auto peer = p.toObject();
            QJsonObject out;
            out["addr"] = peer["addr"].toString();
            out["latency_ms"] = peer["pingtime"].toDouble() * 1000.0;
            out["version"] = peer["subver"].toString();
            out["best_height"] = peer["synced_headers"].toInt();
            out["inbound"] = peer["inbound"].toBool();
            peers.append(out);
        }
        QJsonObject result_obj;
        result_obj["peer_count"] = peers.size();
        result_obj["peers"] = peers;
        cb(QJsonDocument(result_obj).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("getpeerinfo", pc);
    rpc_->getPeerInfo();
}

void AiTools::getMempoolInfo(const QJsonObject&, std::function<void(const QString&)> cb)
{
    PendingCallback pc;
    pc.onResult = [cb](const QJsonValue& result) {
        auto obj = result.toObject();
        QJsonObject out;
        out["tx_count"] = obj["size"].toInt();
        out["size_bytes"] = obj["bytes"].toInt();
        out["min_relay_fee"] = obj["mempoolminfee"].toDouble();
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("mempool.getinfo", pc);
    rpc_->getMempoolInfo();
}

void AiTools::getRecentTransactions(const QJsonObject& input,
                                     std::function<void(const QString&)> cb)
{
    int count = input["count"].toInt(10);
    if (count > 50) count = 50;
    if (count < 1) count = 1;

    PendingCallback pc;
    pc.onResult = [cb, count](const QJsonValue& result) {
        auto arr = result.toArray();
        QJsonArray txs;
        int limit = qMin(count, arr.size());
        for (int i = 0; i < limit; ++i) {
            auto tx = arr[i].toObject();
            QJsonObject out;
            out["txid"] = tx["txid"].toString();
            out["amount"] = tx["amount"].toDouble();
            out["confirmations"] = tx["confirmations"].toInt();
            out["category"] = tx["category"].toString(); // send, receive, generate
            out["time"] = tx["time"].toInt();
            out["address"] = tx["address"].toString();
            txs.append(out);
        }
        QJsonObject result_obj;
        result_obj["count"] = txs.size();
        result_obj["transactions"] = txs;
        cb(QJsonDocument(result_obj).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("wallet.listtransactions", pc);
    rpc_->call("wallet.listtransactions", QJsonArray{count});
}

void AiTools::getNetworkHealth(const QJsonObject&, std::function<void(const QString&)> cb)
{
    // This tool synthesizes from multiple data points.
    // We call getInfo which gives us most of what we need.
    PendingCallback pc;
    int cached = cachedPeerCount_;
    pc.onResult = [cb, cached](const QJsonValue& result) {
        auto obj = result.toObject();
        int blocks = obj["blocks"].toInt();
        int headers = obj["headers"].toInt();
        int connections = (cached >= 0) ? cached : obj["connections"].toInt();
        double syncPct = (headers > 0) ? (100.0 * blocks / headers) : 0.0;

        QJsonObject out;
        QJsonArray issues;

        // Sync assessment
        if (syncPct >= 99.9) {
            out["sync_status"] = "fully_synced";
        } else if (syncPct > 95.0) {
            out["sync_status"] = "nearly_synced";
        } else {
            out["sync_status"] = "syncing";
            issues.append(QString("Node is syncing: %1%").arg(syncPct, 0, 'f', 1));
        }
        out["sync_percent"] = syncPct;

        // Peer assessment
        if (connections == 0) {
            out["peer_health"] = "no_peers";
            issues.append("No peers connected — node is isolated");
        } else if (connections < 3) {
            out["peer_health"] = "low";
            issues.append(QString("Only %1 peer(s) — consider adding seed nodes").arg(connections));
        } else {
            out["peer_health"] = "healthy";
        }
        out["peer_count"] = connections;

        // Overall
        if (issues.isEmpty()) {
            out["overall"] = "healthy";
        } else if (connections == 0 || syncPct < 50.0) {
            out["overall"] = "critical";
        } else {
            out["overall"] = "degraded";
        }
        out["issues"] = issues;

        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("blockchain.getinfo", pc);
    rpc_->getInfo();
}

void AiTools::estimateFee(const QJsonObject& input, std::function<void(const QString&)> cb)
{
    int target = input["conf_target"].toInt(6);

    PendingCallback pc;
    pc.onResult = [cb, target](const QJsonValue& result) {
        auto obj = result.toObject();
        QJsonObject out;
        out["fee_rate_per_kb"] = obj["feerate"].toDouble();
        out["conf_target"] = target;
        out["errors"] = obj["errors"];
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    };
    pendingCallbacks_.insert("wallet.estimatefee", pc);
    rpc_->estimateSmartFee(target);
}

void AiTools::getFleetStatus(const QJsonObject&, std::function<void(const QString&)> cb)
{
    if (!fleetNam_)
        fleetNam_ = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl("http://localhost:3848/api/fleet")};
    req.setTransferTimeout(5000);
    auto* reply = fleetNam_->get(req);

    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QJsonObject err;
            err["error"] = "Fleet dashboard not running. Start it with: node fleet.js (in dinero-mcp directory)";
            cb(QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) {
            QJsonObject err;
            err["error"] = "Invalid fleet response";
            cb(QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }

        QJsonArray nodes = doc.array();
        QJsonObject out;
        QJsonArray nodeList;
        int online = 0;
        int maxHeight = 0;

        for (const auto& n : nodes) {
            auto node = n.toObject();
            if (node["online"].toBool()) {
                online++;
                int h = node["height"].toInt();
                if (h > maxHeight) maxHeight = h;
            }
            nodeList.append(node);
        }

        out["nodes"] = nodeList;
        out["online_count"] = online;
        out["total_count"] = nodes.size();
        out["best_height"] = maxHeight;
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    });
}

// ---------------------------------------------------------------------------
// Context builder — snapshot for system prompt
// ---------------------------------------------------------------------------

void AiTools::buildContext(std::function<void(const QString&)> callback)
{
    // Try fleet + dashboard APIs first, fall back to direct RPC
    buildContextFromFleet(callback);
}

void AiTools::buildContextFromFleet(std::function<void(const QString&)> callback)
{
    if (!fleetNam_)
        fleetNam_ = new QNetworkAccessManager(this);

    // Fire both fleet (:3848) and dashboard (:3847) in parallel
    auto* context = new QJsonObject();
    auto* remaining = new int(2);
    auto* anySuccess = new bool(false);

    auto finalize = [this, context, remaining, anySuccess, callback]() {
        if (--(*remaining) > 0) return; // wait for both
        if (!*anySuccess) {
            // Both fleet and dashboard failed — fall back to direct RPC
            delete context;
            delete remaining;
            delete anySuccess;
            qDebug() << "Fleet + Dashboard unavailable, falling back to direct RPC";
            buildContextFromRpc(callback);
            return;
        }
        QString json = QJsonDocument(*context).toJson(QJsonDocument::Compact);
        delete context;
        delete remaining;
        delete anySuccess;
        callback(json);
    };

    // 1. Fleet API — all 5 nodes
    {
        QNetworkRequest req{QUrl("http://localhost:3848/api/fleet")};
        req.setTransferTimeout(5000);
        auto* reply = fleetNam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [context, anySuccess, reply, finalize]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) { finalize(); return; }

            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) { finalize(); return; }

            *anySuccess = true;
            QJsonArray nodes = doc.array();
            QJsonArray fleet;
            int online = 0, maxHeight = 0;
            bool aligned = true;
            int firstH = -1;

            for (const auto& n : nodes) {
                auto node = n.toObject();
                if (node["online"].toBool()) {
                    online++;
                    int h = node["height"].toInt();
                    if (h > maxHeight) maxHeight = h;
                    if (firstH < 0) firstH = h;
                    else if (h != firstH) aligned = false;
                }
                fleet.append(node);
            }

            (*context)["fleet_nodes"] = fleet;
            (*context)["nodes_online"] = online;
            (*context)["nodes_total"] = nodes.size();
            (*context)["best_height"] = maxHeight;
            (*context)["consensus_aligned"] = aligned;
            finalize();
        });
    }

    // 2. Dashboard API — local wallet details (status + balance + mining)
    {
        // Chain 3 calls sequentially via the dashboard POST API
        auto* dashData = new QJsonObject();
        auto* dashRemaining = new int(3);

        auto dashFinalize = [context, anySuccess, dashData, dashRemaining, finalize]() {
            if (--(*dashRemaining) > 0) return;
            if (!dashData->isEmpty()) {
                *anySuccess = true;
                // Merge dashboard data into context
                for (auto it = dashData->begin(); it != dashData->end(); ++it)
                    (*context)[it.key()] = it.value();
            }
            delete dashData;
            delete dashRemaining;
            finalize();
        };

        auto postDashboard = [this, dashData, dashFinalize](const QString& action,
                std::function<void(const QJsonObject&)> handler) {
            QNetworkRequest req{QUrl("http://localhost:3847/api/" + action)};
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            req.setTransferTimeout(5000);
            auto* reply = fleetNam_->post(req, QByteArray("{}"));
            connect(reply, &QNetworkReply::finished, this, [reply, handler, dashFinalize]() {
                reply->deleteLater();
                if (reply->error() == QNetworkReply::NoError) {
                    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                    if (doc.isObject() && doc.object().contains("result"))
                        handler(doc.object()["result"].toObject());
                }
                dashFinalize();
            });
        };

        postDashboard("status", [dashData](const QJsonObject& r) {
            (*dashData)["local_height"] = r["current_height"];
            (*dashData)["ct_pool_size"] = r["ct_output_pool_size"];
            (*dashData)["privacy_status"] = r["privacy_lane_status"];
            (*dashData)["rings_required"] = r["rings_required"];
            (*dashData)["ring_size"] = r["ring_size"];
            (*dashData)["key_images"] = r["key_images_tracked"];
        });

        postDashboard("balance", [dashData](const QJsonObject& r) {
            (*dashData)["balance_spendable"] = r["spendable"];
            (*dashData)["balance_confidential"] = r["confidential"];
            (*dashData)["balance_immature"] = r["immature"];
            (*dashData)["balance_total"] = r["total"];
            (*dashData)["utxo_count"] = r["utxo_count"];
        });

        postDashboard("mining", [dashData](const QJsonObject& r) {
            (*dashData)["mining_active"] = r["mining"];
            (*dashData)["hashrate"] = r["hashrate"];
            (*dashData)["difficulty"] = r["difficulty"];
            (*dashData)["network_hashrate"] = r["networkhashps"];
        });
    }
}

// If neither fleet nor dashboard are running, fall back to direct RPC
// (This is the old approach — kept as safety net)

void AiTools::buildContextFromRpc(std::function<void(const QString&)> callback)
{
    if (activeBuild_) {
        delete activeBuild_;
    }

    activeBuild_ = new ContextBuild();
    activeBuild_->remaining = 3;
    activeBuild_->callback = callback;

    // 1. Blockchain info
    {
        PendingCallback pc;
        pc.onResult = [this](const QJsonValue& result) {
            if (!activeBuild_) return;
            auto obj = result.toObject();
            activeBuild_->data["height"] = obj["blocks"].toInt();
            activeBuild_->data["headers"] = obj["headers"].toInt();
            int h = obj["headers"].toInt();
            int b = obj["blocks"].toInt();
            activeBuild_->data["sync_percent"] = (h > 0) ? (100.0 * b / h) : 0.0;
            activeBuild_->data["chain"] = obj["chain"].toString();
            activeBuild_->data["connections"] = (cachedPeerCount_ >= 0)
                ? cachedPeerCount_ : obj["connections"].toInt();
            activeBuild_->data["version"] = obj["version"].toString();

            if (--activeBuild_->remaining == 0) {
                auto cb = activeBuild_->callback;
                QString json = QJsonDocument(activeBuild_->data).toJson(QJsonDocument::Compact);
                delete activeBuild_;
                activeBuild_ = nullptr;
                cb(json);
            }
        };
        pendingCallbacks_.insert("blockchain.getinfo", pc);
        rpc_->getInfo();
    }

    // 2. Wallet balance
    {
        PendingCallback pc;
        pc.onResult = [this](const QJsonValue& result) {
            if (!activeBuild_) return;
            auto obj = result.toObject();
            activeBuild_->data["balance_confirmed"] = obj["confirmed"].toDouble();
            activeBuild_->data["balance_unconfirmed"] = obj["unconfirmed"].toDouble();
            activeBuild_->data["balance_immature"] = obj["immature"].toDouble();

            if (--activeBuild_->remaining == 0) {
                auto cb = activeBuild_->callback;
                QString json = QJsonDocument(activeBuild_->data).toJson(QJsonDocument::Compact);
                delete activeBuild_;
                activeBuild_ = nullptr;
                cb(json);
            }
        };
        pendingCallbacks_.insert("wallet.getbalance", pc);
        rpc_->getBalance();
    }

    // 3. Mining info
    {
        PendingCallback pc;
        pc.onResult = [this](const QJsonValue& result) {
            if (!activeBuild_) return;
            auto obj = result.toObject();
            activeBuild_->data["mining_active"] = obj["mining"].toBool();
            activeBuild_->data["hashrate"] = obj["hashrate"].toDouble();
            activeBuild_->data["difficulty"] = obj["difficulty"];

            if (--activeBuild_->remaining == 0) {
                auto cb = activeBuild_->callback;
                QString json = QJsonDocument(activeBuild_->data).toJson(QJsonDocument::Compact);
                delete activeBuild_;
                activeBuild_ = nullptr;
                cb(json);
            }
        };
        pendingCallbacks_.insert("blockchain.getmininginfo", pc);
        rpc_->getMiningInfo();
    }
}
