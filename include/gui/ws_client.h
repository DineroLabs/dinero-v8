#pragma once
#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QUrl>

class LiveWsClient : public QObject {
    Q_OBJECT
public:
    explicit LiveWsClient(QObject* parent = nullptr);
    void configure(const QString& cookiePath, int wsPort);
    void start();
    bool isConnected() const { return socket_.state() == QAbstractSocket::ConnectedState; }

signals:
    void connected();
    void disconnected();
    void messageReceived(QByteArray payload);

public slots:
    void subscribe(const QByteArray& jsonSub);  // e.g. R"({"op":"sub","topics":["blocks"]})"

private:
    void open();
    void scheduleReconnect();
    void sendPing();

    QWebSocket socket_;
    QTimer reconnect_;
    QTimer ping_;
    QUrl url_;
    QByteArray authHeader_;
    int backoffMs_ = 250;
};
