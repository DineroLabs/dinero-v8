#pragma once
#include <QObject>
#include <QString>

class LiveWsClient : public QObject {
    Q_OBJECT

public:
    explicit LiveWsClient(QObject* parent = nullptr);
    
    void configure(const QString& url, int port);
    bool isConnected() const;
    
public slots:
    void connectToServer();
    void disconnectFromServer();

signals:
    void connected();
    void disconnected();

private:
    QString m_url;
    int m_port = 0;
    bool m_connected;
};
