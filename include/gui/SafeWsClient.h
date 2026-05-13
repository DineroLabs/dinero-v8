#pragma once
#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QWebSocket>
#include <memory>

class SafeWsClient : public QObject {
    Q_OBJECT
public:
    explicit SafeWsClient(const QUrl& url, QObject* parent=nullptr);

    void start();              // begin / connect
    void stop();               // stop and close
    void sendText(const QString& msg);
    bool isConnected() const;  // check connection status

signals:
    void connected();
    void disconnected();
    void message(const QString& text);
    void status(const QString& msg);

private slots:
    void openOnce();           // internal: guarded open()

private:
    void makeSocket();         // (re)create socket object cleanly
    void scheduleReconnect(int ms);
    void teardownSocket();     // close & deleteLater

    QUrl url_;
    std::unique_ptr<QWebSocket> ws_;
    QTimer reconnect_;         // managed backoff
    bool started_ = false;
};
