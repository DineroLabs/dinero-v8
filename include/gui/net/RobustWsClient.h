#pragma once
#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>
#include <QJsonObject>
#include <optional>

class RobustWsClient : public QObject {
    Q_OBJECT
public:
    explicit RobustWsClient(QObject* parent = nullptr);
    void connectTo(const QUrl& url);
    void close();                    // user-initiated close
    void setThrottleMs(int ms);      // default 250ms

signals:
    void connected();
    void disconnected();
    void tipUpdated(int height);     // throttled
    void miningInfoUpdated(QJsonObject info); // throttled
    void errorText(QString);

private:
    void scheduleReconnect();
    void openNow();
    void handleMessage(const QString& msg);
    void emitThrottled();

    QWebSocket ws_;
    QUrl url_;
    QTimer reconnect_;     // exponential backoff
    int attempt_ = 0;
    int baseMs_ = 500;     // 0.5s
    int maxMs_  = 30000;   // 30s
    bool userClose_ = false;

    // throttling
    int throttleMs_ = 250;
    QTimer throttleTimer_;
    bool havePending_ = false;
    std::optional<int> pendingHeight_;
    std::optional<QJsonObject> pendingMining_;

    // heartbeat
    QTimer ping_;
    QElapsedTimer lastPong_;
};
