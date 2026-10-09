#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QSignalSpy>
#include "rpc_reply_test_access.h"
#include <memory>
class RpcErrorEnvelopeTest:public QObject {
    Q_OBJECT
private Q_SLOTS:
    void structuredFailureIsDeliveredOnceAndNeverAsResult() {
        std::unique_ptr<RpcClient> rpc(RpcReplyTestAccess::Make());
        QSignalSpy details(rpc.get(),&RpcClient::rpcErrorDetailed),legacy(rpc.get(),&RpcClient::rpcError),success(rpc.get(),&RpcClient::rpcResult);
        const QJsonObject data{{"orchard",QJsonObject{{"error_code","proof_not_ready"},{"proof_state","running"},{"reservation_retained",true}}}};
        const QJsonObject envelope{{"error",QJsonObject{{"code",-32603},{"message","pending"},{"data",data}}},{"result",QJsonObject{{"admitted",true}}}};
        QVERIFY(!RpcReplyTestAccess::Deliver(*rpc,"opaque-wallet-tag",QJsonDocument(envelope).toJson()));
        QCOMPARE(details.size(),1);QCOMPARE(legacy.size(),1);QCOMPARE(success.size(),0);
        QCOMPARE(details[0][0].toString(),QString("opaque-wallet-tag"));QCOMPARE(details[0][1].toInt(),-32603);
        QCOMPARE(details[0][2].toString(),QString("pending"));QCOMPARE(details[0][3].value<QJsonValue>().toObject(),data);
    }
    void legacyMalformedAndNullResponsesRemainDistinct() {
        std::unique_ptr<RpcClient> rpc(RpcReplyTestAccess::Make());
        QSignalSpy details(rpc.get(),&RpcClient::rpcErrorDetailed),legacy(rpc.get(),&RpcClient::rpcError),success(rpc.get(),&RpcClient::rpcResult);
        const QList<QByteArray> errors{R"({"error":{"code":-1,"message":"old"}})",R"({"error":"bad","result":{"admitted":true}})","{",R"({"jsonrpc":"2.0"})"};
        for(const auto& body:errors)QVERIFY(!RpcReplyTestAccess::Deliver(*rpc,"tag",body));
        QCOMPARE(details.size(),4);QCOMPARE(legacy.size(),4);QCOMPARE(success.size(),0);
        QVERIFY(details[0][3].value<QJsonValue>().isUndefined());
        QVERIFY(RpcReplyTestAccess::Deliver(*rpc,"gettxout",R"({"error":null,"result":null})"));
        QCOMPARE(success.size(),1);QVERIFY(success[0][1].value<QJsonValue>().isNull());QCOMPARE(details.size(),4);
    }
    void detailsCannotOverrideMessageOrBecomeSuccess() {
        std::unique_ptr<RpcClient> rpc(RpcReplyTestAccess::Make());
        QSignalSpy details(rpc.get(),&RpcClient::rpcErrorDetailed),success(rpc.get(),&RpcClient::rpcResult);
        QVERIFY(!RpcReplyTestAccess::Deliver(*rpc,"tag",R"({"error":{"code":-5,"message":"refused","data":{"result":{"admitted":true},"message":"paid"}}})"));
        QCOMPARE(details.size(),1);QCOMPARE(success.size(),0);QCOMPARE(details[0][2].toString(),QString("refused"));
    }
};
QTEST_GUILESS_MAIN(RpcErrorEnvelopeTest)
#include "test_rpc_error_envelope.moc"
