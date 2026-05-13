#pragma once
#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QTimer>
#include <QSet>
#include <QJsonArray>

struct WsConfig {
    QString dataDir;                 // e.g. /Users/.../Dinero/data/mainnet
    QString rpcHost = "127.0.0.1";
    int     rpcPort = 20998;
    QString bindHost = "127.0.0.1";
    quint16 bindPort = 22999;
    int     intervalMs = 2000;       // broadcast cadence
};

class WsNotifier : public QObject {
    Q_OBJECT
public:
    explicit WsNotifier(const WsConfig& cfg, QObject* parent=nullptr);
    
    // Phase 1: Bind immediately (listen-only mode)
    bool listenOnly();
    
    // Phase 2: Activate RPC polling and timers (after node is ready)
    void activate();
    
    // Legacy method (calls both phases)
    bool start();
    
    void stop();
    bool isListening() const;

signals:
    void logMsg(const QString&);

private slots:
    void tick();
    void onNewConnection();
    void onClosed();

private:
    QByteArray rpc(const QString& method, const QJsonArray& params = {});
    QString cookie() const;
    bool updateState();              // returns true if changed
    QJsonObject snapshot() const;
    void broadcast(const QJsonObject& msg);

private:
    WsConfig cfg_;
    QWebSocketServer server_;
    QSet<QWebSocket*> clients_;
    QTimer timer_;
    bool activated_{false};          // tracks if activate() was called
    // cached
    int height_ = -1;
    int headers_ = -1;
    int mempool_ = -1;
    bool mining_ = false;
    double hashrate_ = 0.0;
};
