#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

class RpcClient;

class AppEvents : public QObject {
    Q_OBJECT

public:
    explicit AppEvents(RpcClient* rpcClient, QObject* parent = nullptr);
    
    void start();  // Subscribe and connect
    void stop();   // Unsubscribe and disconnect
    
    bool isLive() const { return m_isLive; }

signals:
    // Real-time event signals
    void newBlock(int height, const QString& hash);
    void miningUpdate(double hashrate, int threads);
    void walletTx(const QString& txid, qint64 amountSat);
    void mempoolTx(const QString& txid);
    
    // Connection status
    void connectionStatusChanged(bool isLive);

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketMessage(const QString& message);
    void scheduleReconnect();

private:
    void subscribeToEvents();
    void openWebSocket(const QString& url);
    void setLive(bool live);
    void showError(const QString& error);
    
    RpcClient* m_rpcClient;
    QWebSocket m_webSocket;
    QTimer m_reconnectTimer;
    
    QString m_sessionId;
    QString m_wsUrl;
    QString m_cookie;
    
    bool m_isLive;
    int m_reconnectDelay;  // Exponential backoff: 1s→2→4→...≤30s
    
    static constexpr int MAX_RECONNECT_DELAY = 30000; // 30 seconds
};
