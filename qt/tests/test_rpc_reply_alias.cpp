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
    QList<QByteArray> bodies;
    explicit CannedRpcServer(const QByteArray& resultJson, bool isError = false) {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this, resultJson, isError]() {
            QTcpSocket* s = server.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [this, s, resultJson, isError]() {
                const QByteArray all = s->readAll();
                const int split = all.indexOf("\r\n\r\n");
                lastBody = split >= 0 ? all.mid(split + 4) : all;
                bodies.append(lastBody);
                const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"" +
                    (isError ? QByteArray("error") : QByteArray("result")) + "\":" + resultJson + "}";
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

    void positionalCallsForTheSameAddressKeepSeparateReplyRoutes() {
        CannedRpcServer server(R"({"address":"din1pSAME","transactions":[]})");
        RpcClient rpc;
        rpc.setEndpoint(server.url());
        QSignalSpy results(&rpc, &RpcClient::rpcResult);
        const QString address = QStringLiteral("din1pSAME");
        rpc.callAs("blockchain.getaddresshistory", QJsonArray{address, 200},
                   "pool.earnings.history.current");
        rpc.call("blockchain.getaddresshistory", QJsonArray{address, 50});
        rpc.callAs("blockchain.getaddresshistory", QJsonArray{address, 200, 799},
                   "pool.earnings.history.next");
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 5000);
        QStringList routes;
        for (const auto& result : results) {
            routes.append(result.at(0).toString());
            QCOMPARE(result.at(1).toJsonValue().toObject().value("address").toString(), address);
        }
        QVERIFY(routes.contains(QStringLiteral("blockchain.getaddresshistory")));
        QVERIFY(routes.contains(QStringLiteral("pool.earnings.history.current")));
        QVERIFY(routes.contains(QStringLiteral("pool.earnings.history.next")));
        QCOMPARE(server.bodies.size(), 3);
        for (const QByteArray& body : server.bodies) {
            const QJsonObject sent = QJsonDocument::fromJson(body).object();
            QCOMPARE(sent.value("method").toString(), QStringLiteral("blockchain.getaddresshistory"));
            QVERIFY(!sent.contains("__replyAs"));
            QCOMPARE(sent.value("params").toArray().at(0).toString(), address);
        }
    }

    void balanceCallAlsoKeepsExplorerReplyOutOfPoolRoute() {
        CannedRpcServer server(R"({"address":"din1pSAME","balance":500})");
        RpcClient rpc;
        rpc.setEndpoint(server.url());
        QSignalSpy results(&rpc, &RpcClient::rpcResult);
        const QString address = QStringLiteral("din1pSAME");
        rpc.callAs("blockchain.getaddressbalance", QJsonArray{address},
                   "pool.earnings.balance.current");
        rpc.call("blockchain.getaddressbalance", QJsonArray{address});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 5000);
        QStringList routes;
        for (const auto& result : results) routes.append(result.at(0).toString());
        QVERIFY(routes.contains(QStringLiteral("blockchain.getaddressbalance")));
        QVERIFY(routes.contains(QStringLiteral("pool.earnings.balance.current")));
        for (const QByteArray& body : server.bodies) {
            const QJsonObject sent = QJsonDocument::fromJson(body).object();
            QCOMPARE(sent.value("params").toArray().at(0).toString(), address);
            QVERIFY(!sent.contains("__replyAs"));
        }
    }

    void positionalAliasAlsoRoutesRpcErrors() {
        CannedRpcServer server(R"({"code":-8,"message":"bad anchor"})", true);
        RpcClient rpc;
        rpc.setEndpoint(server.url());
        QSignalSpy errors(&rpc, &RpcClient::rpcError);
        rpc.callAs("blockchain.getaddresshistory", QJsonArray{"din1pSAME", 200, 799},
                   "pool.earnings.history.failed-page");
        QVERIFY(errors.wait(5000));
        QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("pool.earnings.history.failed-page"));
        QCOMPARE(errors.at(0).at(1).toInt(), -8);
        const QJsonObject sent = QJsonDocument::fromJson(server.lastBody).object();
        QCOMPARE(sent.value("method").toString(), QStringLiteral("blockchain.getaddresshistory"));
        QVERIFY(!sent.contains("__replyAs"));
    }
};
QTEST_GUILESS_MAIN(RpcReplyAliasTest)
#include "test_rpc_reply_alias.moc"
