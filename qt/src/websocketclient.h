#pragma once

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QUrl>
#include <QTimer>
#include <QSet>
#include <atomic>

class WebSocketClient : public QObject {
    Q_OBJECT

public:
    explicit WebSocketClient(const QString& serverUrl = "ws://127.0.0.1:20997", QObject* parent = nullptr);
    ~WebSocketClient();

    // Connection management
    void connectToServer();
    void disconnectFromServer();
    bool isConnected() const;

    // Server configuration
    void setServerUrl(const QString& serverUrl);
    QString serverUrl() const { return m_serverUrl; }

    // Authentication
    void setDatadir(const QString& datadir);
    bool loadCookie();

    // Auto-reconnect configuration
    void setAutoReconnect(bool enable);
    bool isAutoReconnectEnabled() const;

    // Subscription management
    void subscribe(const QString& topic);
    void unsubscribe(const QString& topic);

Q_SIGNALS:
    // Connection signals
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void reconnecting(int attemptNumber, int delayMs);

    // Subscription topic signals
    void newBlockReceived(const QJsonObject& blockData);
    void newTransactionReceived(const QJsonObject& txData);
    void miningInfoReceived(const QJsonObject& miningData);
    void networkInfoReceived(const QJsonObject& networkData);
    void mempoolUpdateReceived(const QJsonObject& mempoolData);
    void syncProgressReceived(const QJsonObject& syncData);

    // Generic subscription notification
    void subscriptionEvent(const QString& topic, const QJsonObject& data);

private Q_SLOTS:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);
    void onPingTimeout();
    void onReconnectTimeout();

private:
    void sendJsonRpc(const QString& method, const QJsonObject& params, const QString& id = QString());
    void handleSubscriptionNotification(const QJsonObject& message);
    void handleEventNotification(const QJsonObject& message);
    void startPingTimer();
    void stopPingTimer();
    void scheduleReconnect();
    void resetReconnectState();
    void resubscribeAll();

    QWebSocket m_webSocket;
    QString m_serverUrl;
    QString m_datadir;
    QString m_cookieToken;
    std::atomic<bool> m_connected{false};
    std::atomic<int> m_requestId{1};

    // Auto-reconnect state
    bool m_autoReconnect{true};
    bool m_manualDisconnect{false};
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts{0};
    static constexpr int MAX_RECONNECT_DELAY_MS = 30000;   // 30 seconds max
    static constexpr int INITIAL_RECONNECT_DELAY_MS = 1000; // 1 second initial

    // Topic subscription state (for auto-resubscribe on reconnect)
    QSet<QString> m_subscribedTopics;

    // Ping/pong mechanism to keep connection alive
    QTimer* m_pingTimer;
    static constexpr int PING_INTERVAL_MS = 30000;  // 30 seconds
};
