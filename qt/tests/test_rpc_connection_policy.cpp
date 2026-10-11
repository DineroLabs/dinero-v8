#include <QtTest/QtTest>
#include "rpcconnectionpolicy.h"
#include "rpcretrypolicy.h"

class RpcConnectionPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void onlyTheDispatchContextCanPublishAReply() {
        const QUrl a("http://node-a.invalid/"),b("http://node-b.invalid/");
        QVERIFY(RpcConnectionPolicy::MatchesOrigin("one",a,"one",a));
        QVERIFY(!RpcConnectionPolicy::MatchesOrigin("",a,"",a));
        QVERIFY(!RpcConnectionPolicy::MatchesOrigin("one",a,"two",a));
        QVERIFY(!RpcConnectionPolicy::MatchesOrigin("one",a,"one",b));
        // Returning to the same URL does not revive the old local lifetime.
        QVERIFY(!RpcConnectionPolicy::MatchesOrigin("one",a,"three",a));
    }
    void boundRequestsRefuseRedirectsWithoutAuthorizingReplay() {
        const QUrl a("http://node-a.invalid/"),b("http://node-b.invalid/");
        const QJsonObject request{{"method","wallet.orchard.finishspend"},{"__replyAs","overview"}};
        const bool bound=RpcRetryPolicy::RequiresExplicitRetry(request);QVERIFY(bound);
        QVERIFY(RpcConnectionPolicy::AcceptsResponse("one",a,"one",a,bound,a,200));
        QVERIFY(!RpcConnectionPolicy::AcceptsResponse("one",a,"one",a,bound,b,200));
        for(int status:{301,302,303,307,308})QVERIFY(!RpcConnectionPolicy::AcceptsResponse("one",a,"one",a,bound,a,status));
        QVERIFY(!RpcConnectionPolicy::AcceptsResponse("one",a,"two",a,bound,a,200));
    }
    void ordinaryReadsKeepRedirectPolicyButNeverCrossLocalLifetimes() {
        const QUrl a("http://node-a.invalid/"),b("http://node-b.invalid/");
        QVERIFY(RpcConnectionPolicy::AcceptsResponse("one",a,"one",a,false,b,200));
        QVERIFY(!RpcConnectionPolicy::AcceptsResponse("one",a,"two",b,false,b,200));
    }
};
QTEST_GUILESS_MAIN(RpcConnectionPolicyTest)
#include "test_rpc_connection_policy.moc"
