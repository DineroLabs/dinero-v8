#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrl>
#include <QNetworkReply>

class WebSocketClient : public QObject {
    Q_OBJECT

public:
    enum ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting
    };

    explicit WebSocketClient(QObject* parent = nullptr);
    ~WebSocketClient();

    // Connection management
    void connectToServer(const QString& host = "127.0.0.1", int port = 20999);
    void disconnect();
    void reconnect();
    
    // Subscription management
    void subscribeToNewBlocks();
    void subscribeToNewTransactions();
    void subscribeToMiningRewards();
    void subscribeToBalanceUpdates();
    void unsubscribeFromAll();
    
    // State
    ConnectionState getConnectionState() const { return m_connectionState; }
    bool isConnected() const { return m_connectionState == Connected; }
    
    // Configuration
    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }
    void setReconnectInterval(int intervalMs) { m_reconnectInterval = intervalMs; }
    void setCookieAuth(const QString& cookie) { m_authCookie = cookie; }

signals:
    // Connection events
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void reconnecting();
    
    // Blockchain events
    void newBlockReceived(const QJsonObject& blockData);
    void newTransactionReceived(const QJsonObject& txData);
    void balanceUpdated(double newBalance, double unconfirmed, double immature);
    void miningRewardReceived(double amount, int blockHeight);
    void confirmationUpdate(const QString& txid, int confirmations);
    
    // Status events
    void subscriptionConfirmed(const QString& eventType);
    void subscriptionError(const QString& eventType, const QString& error);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);
    void onSslErrors(const QList<QSslError>& errors);
    void onReconnectTimer();

private:
    // Core components
    QWebSocket* m_webSocket;
    QTimer* m_reconnectTimer;
    QTimer* m_heartbeatTimer;
    
    // Connection state
    ConnectionState m_connectionState = Disconnected;
    QString m_serverHost = "127.0.0.1";
    int m_serverPort = 20999;
    QString m_authCookie;
    
    // Configuration
    bool m_autoReconnect = true;
    int m_reconnectInterval = 5000; // 5 seconds
    int m_heartbeatInterval = 30000; // 30 seconds
    int m_reconnectAttempts = 0;
    int m_maxReconnectAttempts = 10;
    
    // Subscription tracking
    QStringList m_activeSubscriptions;
    
    // Helper methods
    void setState(ConnectionState newState);
    void sendMessage(const QJsonObject& message);
    void sendSubscriptionRequest(const QString& eventType, const QJsonObject& params = {});
    void handleIncomingMessage(const QJsonObject& message);
    void handleSubscriptionResponse(const QJsonObject& message);
    void handleEventNotification(const QJsonObject& message);
    void handleHeartbeat();
    void startHeartbeat();
    void stopHeartbeat();
    void resubscribeAll();
    
    // Message builders
    QJsonObject buildSubscriptionMessage(const QString& eventType, const QJsonObject& params = {});
    QJsonObject buildUnsubscribeMessage(const QString& eventType);
    QJsonObject buildHeartbeatMessage();
};
