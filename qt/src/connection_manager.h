#pragma once

#include <QObject>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QJsonObject>
#include <functional>

/**
 * @brief Bulletproof connection manager for Dinero daemon
 * 
 * Handles:
 * - Automatic reconnection with exponential backoff
 * - Health checks every 5 seconds
 * - RPC call queuing when disconnected
 * - Clear state transitions
 * - Error recovery
 */
class ConnectionManager : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY(ConnectionManager)

public:
    enum ConnectionState {
        Disconnected,    // No connection to daemon
        Connecting,      // Attempting to connect
        Connected,       // Connected and verified
        Reconnecting,    // Lost connection, attempting to reconnect
        Failed          // Connection failed permanently (user action needed)
    };
    Q_ENUM(ConnectionState)

    explicit ConnectionManager(QObject* parent = nullptr);
    ~ConnectionManager();

    // Current state
    ConnectionState state() const { return state_; }
    QString stateString() const;
    bool isConnected() const { return state_ == Connected; }
    
    // Connection info
    QString daemonUrl() const { return daemonUrl_; }
    QString cookiePath() const { return cookiePath_; }
    
    // Configuration
    void setDaemonUrl(const QString& url);
    void setCookiePath(const QString& path);
    void setHealthCheckInterval(int ms) { healthCheckInterval_ = ms; }
    void setMaxRetries(int retries) { maxRetries_ = retries; }
    
    // Connection control
    void connectToDaemon();
    void disconnectFromDaemon();
    void reconnect();
    
    // RPC calls (automatically queued if disconnected)
    void call(const QString& method, const QJsonObject& params = QJsonObject{},
              std::function<void(const QJsonObject&)> onSuccess = nullptr,
              std::function<void(const QString&)> onError = nullptr);
    void callNamed(const QString& method, const QJsonObject& params,
                   std::function<void(const QJsonObject&)> onSuccess = nullptr,
                   std::function<void(const QString&)> onError = nullptr);

Q_SIGNALS:
    // State changes
    void stateChanged(ConnectionState newState, ConnectionState oldState);
    void connected();
    void disconnected();
    void reconnecting(int attempt, int maxAttempts);
    
    // RPC results
    void rpcSuccess(QString method, QJsonObject result);
    void rpcError(QString method, QString error);
    
    // Status updates
    void statusMessage(QString message, QString level); // level: info/warning/error
    void blockchainSynced(int blocks, int headers);
    void peerCountChanged(int count);

private Q_SLOTS:
    void onHealthCheckTimer();
    void processRpcQueue();

private:
    struct RpcCall {
        QString method;
        QJsonObject params;
        bool namedParams = false;
        std::function<void(const QJsonObject&)> onSuccess;
        std::function<void(const QString&)> onError;
        int attempts = 0;
    };

    void setState(ConnectionState newState);
    void scheduleHealthCheck();
    void performHealthCheck();
    void executeRpcCall(const RpcCall& call);
    QString readCookie();
    int calculateBackoff(int attempt) const;

    ConnectionState state_;
    QString daemonUrl_;
    QString cookiePath_;
    
    QNetworkAccessManager* nam_;
    QTimer* healthCheckTimer_;
    QTimer* reconnectTimer_;
    QTimer* queueProcessTimer_;
    
    QList<RpcCall> rpcQueue_;
    bool processingQueue_;
    
    int healthCheckInterval_;
    int maxRetries_;
    int currentRetryAttempt_;
    int consecutiveFailures_;
    
    QPointer<QNetworkReply> activeHealthCheck_;
};
