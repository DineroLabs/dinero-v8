#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QWebSocket>
#include <functional>
#include "gui-desktop/utils/net_defaults.h"

/**
 * RpcClient - Professional RPC client for Dinero Desktop
 * 
 * Features:
 * - Direct integration with our 14 production RPC methods
 * - Automatic cookie authentication
 * - WebSocket events for real-time updates
 * - Professional error handling
 * - Connection state management
 * - Schema compliance validation (din.rpc.v1, schema_rev:1)
 */
class RpcClient : public QObject {
    Q_OBJECT

public:
    struct RpcResponse {
        bool success;
        QJsonObject result;
        QString error;
        int errorCode;
        QString rpcSchema;
        int schemaRev;
        
        bool isSchemaCompliant() const {
            return rpcSchema == "din.rpc.v1" && schemaRev == 1;
        }
    };

    struct BlockchainInfo {
        int blocks;
        int headers;
        QString bestBlockHash;
        double difficulty;
        bool synced;
        QString network;
    };

    struct NetworkInfo {
        int connections;
        QString network;
        QString version;
        bool networkActive;
    };

    struct MempoolInfo {
        int size;
        int bytes;
        double minFee;
        double maxFee;
    };

    struct WalletBalance {
        QString balanceDIN;
        qint64 balanceUna;
        QString confirmedDIN;
        qint64 confirmedUna;
        QString unconfirmedDIN;
        qint64 unconfirmedUna;
    };

    explicit RpcClient(QObject *parent = nullptr);
    ~RpcClient();

    // Connection management
    bool testConnection();
    bool isConnected() const { return m_connected; }
    void setNodeInfo(const QString &rpcUrl, const QString &cookiePath);

    // === Core RPC Methods (our 14 production methods) ===
    
    // Blockchain info
    void getBlockchainInfo(std::function<void(const BlockchainInfo&)> callback);
    void getNetworkInfo(std::function<void(const NetworkInfo&)> callback);
    void getMempoolInfo(std::function<void(const MempoolInfo&)> callback);

    // Wallet methods
    void getNewAddress(const QString &label, std::function<void(const QString&)> callback);
    void getBalance(std::function<void(const WalletBalance&)> callback);
    void listUnspent(std::function<void(const QJsonArray&)> callback);
    void listTransactions(int limit, std::function<void(const QJsonArray&)> callback);
    void validateAddress(const QString &address, std::function<void(const QJsonObject&)> callback);

    // PSBT workflow
    void psbtCreate(const QJsonObject &outputs, std::function<void(const QString&, const QString&)> callback);
    void psbtFund(const QString &psbt, std::function<void(const QString&, qint64)> callback);
    void psbtSign(const QString &psbt, std::function<void(const QString&, bool)> callback);
    void psbtSubmit(const QString &psbt, bool allowMainnet, std::function<void(const QString&)> callback);

    // Explorer methods
    void getRawTransaction(const QString &txid, bool decode, std::function<void(const QJsonObject&)> callback);
    void getTxOut(const QString &txid, int vout, std::function<void(const QJsonObject&)> callback);
    void getRawMempool(bool verbose, std::function<void(const QJsonValue&)> callback);

    // Mining methods (existing)
    void miningStart(int threads, const QString& address, std::function<void(bool)> callback);
    void miningStop(std::function<void(bool)> callback);
    void miningStatus(std::function<void(const QJsonObject&)> callback);
    
    // New mining methods for wallet integration
    void miningGenerateNewAddress(std::function<void(const QString&, const QString&)> callback);
    void miningGetAddress(std::function<void(const QString&)> callback);
    void miningSetAddress(const QString& address, std::function<void(bool)> callback);
    void getWalletInfo(std::function<void(const QJsonObject&)> callback);

    // Event subscription
    void subscribeToEvents(const QStringList &topics, std::function<void(const QString&, const QString&)> callback);
    void unsubscribeFromEvents(std::function<void(bool)> callback);
    
    // Generic RPC call (for events.subscribe/unsubscribe)
    void call(const QString &method, const QJsonObject &params, std::function<void(QJsonValue, QString)> callback);
    
    // Network switching support
    bool waitForHealth(Network n, int timeoutMs = 10000);
    bool bootstrapAuthFromCookieAndMint();
    void resetAuth();
    void setCookiePath(const QString& path);
    void setBaseUrl(const QUrl& url);
    void unsubscribeAll();
    bool httpHeadOk(const QUrl& url);
    
    // Network state integration
    void updateForNetwork(Network network);
    Network detectNetworkFromDaemon();
    bool isConnectedToNetwork(Network network) const;

signals:
    void connected();
    void disconnected();
    void connectionError(const QString &error);
    
    // Network state signals
    void networkDetected(Network detectedNetwork);
    void networkMismatch(Network expected, Network detected);
    
    // Real-time events
    void newBlockReceived(const QString &hash, int height);
    void walletTransactionReceived(const QString &txid, double amount);
    void mempoolTransactionReceived(const QString &txid);
    void miningUpdateReceived(bool active, double hashrate);

private slots:
    void onNetworkReplyFinished();
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketMessageReceived(const QString &message);
    void checkConnection();

private:
    void makeRpcCall(const QString &method, const QJsonValue &params, 
                     std::function<void(const RpcResponse&)> callback);
    
    void loadConfiguration();
    void setupWebSocket();
    void loadOrCreateBearerToken();
    bool ensureBearerTokenReady();
    bool mintTokenFromCookie();
    void createBearerToken();
    QString loadStoredBearerToken();
    void storeBearerToken(const QString& token);
    QString determineCookiePath();
    
public:
    QString loadCookie();  // Make public for AppEvents
    void connectWebSocket(const QString& wsUrl, const QString& sessionId);
    void processWebSocketEvent(const QJsonObject &event);
    
    // NEW: Auth management
    void setBearer(const QString& token);
    void setDaemonCookiePath(const QString& cookiePath);  // Set cookie path from daemon launcher
    bool hasAuth() const;
    QString getAuthDebugInfo() const;  // Debug helper
    
    QNetworkAccessManager *m_networkManager;
    QWebSocket *m_webSocket;
    QTimer *m_connectionTimer;
    
    QString m_rpcUrl;
    QString m_cookiePath;
    QString m_cookieAuth;
    QString m_bearerToken;
    bool m_bootstrapInFlight{false};
    int m_currentRpcPort{NetDefaults::RPC};  // Use unified port
    QString m_lastError;
    Network m_expectedNetwork{Network::Regtest};  // Track expected network
    
    bool m_connected;
    QString m_wsSessionId;
    
    // Request tracking
    int m_nextRequestId;
    QHash<QNetworkReply*, std::function<void(const RpcResponse&)>> m_pendingRequests;
};
