#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include "rpcclient.h"

// One-shot HTTP server: records the request body and answers with a fixed result.
class CannedRpcServer : public QObject {
public:
    QTcpServer server;
    QByteArray lastBody;
    explicit CannedRpcServer(const QByteArray& resultJson) {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this, resultJson]() {
            QTcpSocket* s = server.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [this, s, resultJson]() {
                const QByteArray all = s->readAll();
                const int split = all.indexOf("\r\n\r\n");
                lastBody = split >= 0 ? all.mid(split + 4) : all;
                const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":" + resultJson + "}";
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                         QByteArray::number(body.size()) + "\r\n\r\n" + body);
                s->disconnectFromHost();
            });
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())); }
};

class RpcReplyAliasTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void replyArrivesUnderAliasAndRequestIsClean() {
        CannedRpcServer server(R"([{"type":"mined","amount":100.0}])");
        RpcClient rpc;
        rpc.setEndpoint(server.url());
        QSignalSpy results(&rpc, &RpcClient::rpcResult);
        rpc.callNamedAs("wallet.listtransactions", QJsonObject{{"type", "mined"}}, "overview.miningrewards");
        QVERIFY(results.wait(5000));
        QCOMPARE(results.at(0).at(0).toString(), QString("overview.miningrewards"));
        QCOMPARE(results.at(0).at(1).toJsonValue().toArray().size(), 1);
        const QJsonObject sent = QJsonDocument::fromJson(server.lastBody).object();
        QCOMPARE(sent.value("method").toString(), QString("wallet.listtransactions"));
        QVERIFY(!sent.contains("__replyAs"));
        QCOMPARE(sent.value("params").toObject().value("type").toString(), QString("mined"));
    }
    void plainCallsKeepTheirMethodName() {
        CannedRpcServer server("[]");
        RpcClient rpc;
        rpc.setEndpoint(server.url());
        QSignalSpy results(&rpc, &RpcClient::rpcResult);
        rpc.callNamed("wallet.listtransactions", QJsonObject{{"type", "all"}});
        QVERIFY(results.wait(5000));
        QCOMPARE(results.at(0).at(0).toString(), QString("wallet.listtransactions"));
    }
};
QTEST_GUILESS_MAIN(RpcReplyAliasTest)
#include "test_rpc_reply_alias.moc"
