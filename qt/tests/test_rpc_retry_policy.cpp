#include <QtTest/QtTest>
#include <QJsonArray>
#include "rpcretrypolicy.h"

class RpcRetryPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void orchardWalletMethodsRequireExplicitRetryWithOrWithoutAliases() {
        for (const auto& method:{"wallet.orchard.getwalletbinding","wallet.orchard.createaccount",
             "wallet.orchard.getnewaddress","wallet.orchard.queueshield","wallet.orchard.finishshield",
             "wallet.orchard.queuespend","wallet.orchard.finishspend","wallet.orchard.getbalance",
             "wallet.orchard.listoperations"}) {
            QJsonObject request{{"method",method}};
            QVERIFY2(RpcRetryPolicy::RequiresExplicitRetry(request),method);
            request["__replyAs"]="orchardtab|finish|616c696365|7|request";
            QVERIFY2(RpcRetryPolicy::RequiresExplicitRetry(request),method);
            request["__replyAs"]="getblockchaininfo";
            QVERIFY2(RpcRetryPolicy::RequiresExplicitRetry(request),method);
        }
    }
    void existingFundMovingMethodsCannotBeHiddenByAReplyAlias() {
        for (const auto& method:{"wallet.shield","wallet.unshield","wallet.transfer",
             "wallet.sendtoaddress","sendtoaddress","sendrawtransaction",
             "wallet.sendrawtransaction","wallet.covenant.create"}) {
            QJsonObject request{{"method",method},{"__replyAs","overview.refresh"}};
            QVERIFY2(RpcRetryPolicy::RequiresExplicitRetry(request),method);
        }
    }
    void readClassificationUsesTheWireMethodNotAnEffectLookingAlias() {
        QJsonObject request{{"method","getblockchaininfo"},{"__replyAs","wallet.orchard.finishspend"}};
        QVERIFY(!RpcRetryPolicy::RequiresExplicitRetry(request));
        request["method"]="orchard.getactivationstatus";
        QVERIFY(!RpcRetryPolicy::RequiresExplicitRetry(request));
    }
    void malformedMethodNeverAuthorizesAutomaticReplay() {
        QVERIFY(RpcRetryPolicy::RequiresExplicitRetry(QJsonObject{}));
        for (const auto& value:QJsonArray{QJsonValue(QJsonValue::Null),true,1,""})
            QVERIFY(RpcRetryPolicy::RequiresExplicitRetry(QJsonObject{{"method",value}}));
    }
};
QTEST_GUILESS_MAIN(RpcRetryPolicyTest)
#include "test_rpc_retry_policy.moc"
