#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>

class QNetworkAccessManager;
class RpcClient;

class AiTools : public QObject {
    Q_OBJECT
public:
    explicit AiTools(RpcClient* rpc, QObject* parent = nullptr);

    // Tool definitions for Claude API
    QJsonArray toolDefinitions() const;

    // Execute a tool by name, callback receives result JSON string
    void executeTool(const QString& name, const QJsonObject& input,
                     std::function<void(const QString& resultJson)> callback);

    // Build context snapshot for system prompt — fetches from fleet API first,
    // falls back to direct RPC if fleet server isn't running
    void buildContext(std::function<void(const QString& contextJson)> callback);

    // Cached peer count (fed from MainWindow's getpeerinfo handler)
    void setCachedPeerCount(int count) { cachedPeerCount_ = count; }

private Q_SLOTS:
    void onRpcResult(const QString& method, const QJsonValue& result);

private:
    void buildContextFromFleet(std::function<void(const QString&)> callback);
    void buildContextFromRpc(std::function<void(const QString&)> callback);
    void getNodeStatus(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getWalletSummary(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getMiningStatus(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getPeerInfo(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getMempoolInfo(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getRecentTransactions(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getNetworkHealth(const QJsonObject& input, std::function<void(const QString&)> cb);
    void estimateFee(const QJsonObject& input, std::function<void(const QString&)> cb);
    void getFleetStatus(const QJsonObject& input, std::function<void(const QString&)> cb);

    RpcClient* rpc_;
    QNetworkAccessManager* fleetNam_ = nullptr;

    // Pending RPC callbacks: method → list of callbacks
    // (allows multiple outstanding calls to different methods)
    struct PendingCallback {
        std::function<void(const QJsonValue&)> onResult;
    };
    QMultiMap<QString, PendingCallback> pendingCallbacks_;

    // Context builder state
    struct ContextBuild {
        QJsonObject data;
        int remaining = 0;
        std::function<void(const QString&)> callback;
    };
    ContextBuild* activeBuild_ = nullptr;
    int cachedPeerCount_ = -1; // -1 = not yet known
};
