#pragma once
#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QFile>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <functional>

class NodeSupervisor : public QObject {
    Q_OBJECT
public:
    explicit NodeSupervisor(QObject* parent=nullptr);
    void setPaths(QString dinerodPath, QString dataDir);
    void setPorts(int rpc=20998, int p2p=20999, int ws=18332);

    void ensureRunning();

signals:
    void nodeReady();   // fire when RPC answers; GUI can now open WS
    void logLine(QString);

private:
    QString dinerodPath, dataDir, cookiePath;
    int rpcPort=20998, p2pPort=20999, wsPort=18332;
    QProcess* proc;
    QNetworkAccessManager* nam;

    void startDaemon();
    void pollForCookieThenRpc();
    void tryRpcGetBlockCount(std::function<void(bool)> cb);

    void probeTcp(const QString& host, int port, std::function<void(bool)> cb);
};
