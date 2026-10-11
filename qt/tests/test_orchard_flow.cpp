// Orchard payment flow and RPC contract (no widgets, no node).
#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QSet>

#include "orchardflow.h"

using namespace OrchardContract;
using namespace OrchardFlow;

namespace {
QJsonValue J(const char* json) { return QJsonDocument::fromJson(json).object(); }

Request Spend() {
    Request r;
    r.kind = Request::Kind::Spend;
    r.account = 0;
    r.requestId = QString(64,'a');  // deterministic fixture identity
    r.expectedRevision = 3;
    r.payments = {{"rdinorch1abc", 150000000, {}}};
    r.feeUna = 10000;
    return r;
}
QJsonObject OperationSnapshot() {
    QJsonObject op{{"operation_id",QString(64,'a')},{"durable_state","signed"},
        {"txid",QString(64,'f')},{"chain_observation",QJsonValue(QJsonValue::Null)}};
    return {{"account",0},{"account_revision",5},{"account_sequence",10},
        {"captured_source_sequence",10},{"account_digest",QString(64,'c')},
        {"captured_source_digest",QString(64,'c')},{"operations",QJsonArray{op}}};
}
QJsonObject QueueResponse() {
    return {{"operation_id",QString(64,'a')},{"account",0},{"account_revision",5},
        {"durable_state","reserved"},{"proof_queued",true},{"existing_request",false},{"archived",false}};
}
QJsonObject FinishResponse() {
    return {{"operation_id",QString(64,'a')},{"account",0},{"durable_state","signed"},
        {"txid",QString(64,'f')},{"admitted",true},{"already_in_mempool",false},
        {"submission_code","accepted"},{"submission_message",""}};
}
QJsonObject BalanceSnapshot() {
    return {{"account",0},{"account_revision",5},{"account_sequence",10},{"captured_source_sequence",10},
        {"account_digest",QString(64,'c')},{"captured_source_digest",QString(64,'c')},
        {"checkpoint_height",20},{"checkpoint_hash",QString(64,'d')},
        {"confirmed_una",250},{"reserved_confirmed_una",50},{"unreserved_confirmed_una",200},
        {"account_caught_up_to_captured_source",true}};
}

QJsonObject ReceivedPage(int account=0,int offset=0,int total=101,int revision=5) {
    QJsonArray rows;
    for(int i=offset;i<qMin(offset+100,total);++i)rows.append(QJsonObject{
        {"txid",QString(64,'a')},{"action_index",i},{"scope",i==100?"internal":"external"},
        {"amount_una",100000000},{"recipient_hex",QString(86,'b')},{"memo_hex",QString(1024,'0')},
        {"height",20},{"block_hash",QString(64,'d')}});
    return {{"account",account},{"account_revision",revision},{"account_sequence",3},{"captured_source_sequence",3},
        {"account_digest",QString(64,'c')},{"captured_source_digest",QString(64,'c')},
        {"checkpoint_height",20},{"checkpoint_hash",QString(64,'d')},{"account_caught_up_to_captured_source",true},
        {"history_complete",true},{"offset",offset},{"limit",100},{"total_count",total},
        {"next_offset",offset+rows.size()<total?QJsonValue(offset+rows.size()):QJsonValue(QJsonValue::Null)},
        {"received",rows}};
}

QJsonObject ActivationSnapshot() {
    return {{"network","regtest"},{"activation_state","active"},{"activation_height",10},{"branch_id",42},
        {"tip_height",20},{"tip_hash",QString(64,'d')},{"next_block_height",21},
        {"rule_active_at_tip",true},{"rule_active_for_next_block",true},
        {"wallet_backend_compiled",true},{"storage_mode","full"}};
}
}  // namespace

class OrchardFlowTest : public QObject {
    Q_OBJECT
private Q_SLOTS:

    void receivedPagesValidateCompleteTypedMetadataAndEveryRow() {
        const auto good=ReceivedPage();const auto page=ParseReceived(good);QVERIFY(page);QCOMPARE(page->notes.size(),100);QCOMPARE(*page->nextOffset,quint64(100));
        QCOMPARE(page->notes[0].scope,QString("external"));QCOMPARE(page->notes[0].amount,quint64(100000000));
        const auto last=ParseReceived(ReceivedPage(0,100));QVERIFY(last);QCOMPARE(last->notes[0].scope,QString("internal"));QVERIFY(!last->nextOffset);
        QVERIFY(ParseReceived(ReceivedPage(0,101)));QVERIFY(ParseReceived(ReceivedPage(0,0,0)));
        for(const auto& field:QStringList{"account","account_revision","account_sequence","captured_source_sequence","checkpoint_height","offset","limit","total_count"}) {
            for(const auto& badValue:QList<QJsonValue>{QJsonValue(-1),QJsonValue(0.5),QJsonValue(true),QJsonValue("1"),QJsonValue()}) {
                auto bad=good;bad[field]=badValue;QVERIFY(!ParseReceived(bad));
            }
        }
        for(const auto& field:QStringList{"amount_una","action_index","height"}) {
            auto bad=good;auto rows=bad["received"].toArray();auto late=rows[99].toObject();late[field]="1";rows[99]=late;bad["received"]=rows;QVERIFY(!ParseReceived(bad));
        }
        for(const auto& field:QStringList{"txid","block_hash","recipient_hex","memo_hex","scope"}) {
            auto bad=good;auto rows=bad["received"].toArray();auto late=rows[99].toObject();late[field]="invalid";rows[99]=late;bad["received"]=rows;QVERIFY(!ParseReceived(bad));
        }
        auto bad=good;auto rows=bad["received"].toArray();rows[99]=rows[0];bad["received"]=rows;QVERIFY(!ParseReceived(bad));
        bad=good;bad["history_complete"]=false;QVERIFY(!ParseReceived(bad));bad=good;bad["account_caught_up_to_captured_source"]=false;QVERIFY(!ParseReceived(bad));
        bad=good;bad["next_offset"]=99;QVERIFY(!ParseReceived(bad));bad=good;bad.remove("next_offset");QVERIFY(!ParseReceived(bad));
        bad=good;rows=bad["received"].toArray();rows.removeLast();bad["received"]=rows;QVERIFY(!ParseReceived(bad));
        bad=good;bad["error"]="refused";QVERIFY(!ParseReceived(bad));
    }
    void receivedRequestsBindPagesToRevisionAndExposeLag() {
        const auto first=ReceivedParams(17);QCOMPARE(first.size(),3);QCOMPARE(first["account"].toInt(),17);QVERIFY(!first.contains("expected_revision"));
        const auto second=ReceivedParams(17,100,9);QCOMPARE(second.size(),4);QCOMPARE(second["expected_revision"].toInt(),9);QCOMPARE(second["offset"].toInt(),100);
        auto lag=ReceivedPage(17);lag["captured_source_sequence"]=4;lag["captured_source_digest"]=QString(64,'e');lag["account_caught_up_to_captured_source"]=false;
        const auto parsed=ParseReceived(lag);QVERIFY(parsed);QVERIFY(!parsed->caughtUp);
        lag["account_caught_up_to_captured_source"]=true;QVERIFY(!ParseReceived(lag));
        auto single=ReceivedPage(0,100);auto singleRows=single["received"].toArray();auto singleNote=singleRows[0].toObject();singleNote["block_hash"]=QString(64,'e');singleRows[0]=singleNote;single["received"]=singleRows;QVERIFY(!ParseReceived(single));
        auto bad=ReceivedPage();auto rows=bad["received"].toArray();auto last=rows[99].toObject();last["block_hash"]=QString(64,'e');rows[99]=last;bad["received"]=rows;QVERIFY(!ParseReceived(bad));
    }
    void accountCatalogAcceptsNonconsecutiveIdsAndRejectsMalformedLateRows() {
        const auto entry=[](int id){return QJsonObject{{"account",id},{"account_revision",5},{"account_sequence",10},
            {"account_digest",QString(64,'c')},{"checkpoint_height",20},{"checkpoint_hash",QString(64,'d')}};};
        QJsonObject good{{"captured_source_sequence",10},{"captured_source_digest",QString(64,'c')},
            {"accounts",QJsonArray{entry(3),entry(17)}}};
        const auto parsed=ParseAccounts(good);QVERIFY(parsed);QCOMPARE(parsed->accounts.size(),2);
        QCOMPARE(parsed->accounts[0].account,quint64(3));QCOMPARE(parsed->accounts[1].account,quint64(17));
        for(const auto& field:QStringList{"account","account_revision","account_sequence","checkpoint_height"}) {
            for(const auto& invalid:QList<QJsonValue>{QJsonValue(-1),QJsonValue(0.5),QJsonValue(true),QJsonValue("5"),QJsonValue()}) {
                auto bad=good;auto rows=bad["accounts"].toArray();auto row=rows[1].toObject();row[field]=invalid;rows[1]=row;bad["accounts"]=rows;QVERIFY(!ParseAccounts(bad));
            }
        }
        auto bad=good;bad["accounts"]=QJsonArray{entry(3),entry(3)};QVERIFY(!ParseAccounts(bad));
        auto row=entry(17);row["account_digest"]=QString(64,'e');bad=good;bad["accounts"]=QJsonArray{entry(3),row};QVERIFY(!ParseAccounts(bad));
        row=entry(17);row["account_sequence"]=11;bad["accounts"]=QJsonArray{entry(3),row};QVERIFY(!ParseAccounts(bad));
        bad=good;bad["accounts"]=QJsonArray{};QVERIFY(ParseAccounts(bad));
        QJsonArray overflow;for(int i=0;i<1025;++i)overflow.append(entry(i));bad["accounts"]=overflow;QVERIFY(!ParseAccounts(bad));
        bad=good;bad["error"]="catalog unavailable";QVERIFY(!ParseAccounts(bad));
    }
    void storedCompletionMethodsAreAnAllowlist() {
        auto value=OperationSnapshot();auto rows=value["operations"].toArray();auto row=rows[0].toObject();
        QVERIFY(ParseOperations(value)); // older visible intent, no inferred method
        for(const auto& method:QStringList{kFinishShield,kFinishSpend}) {
            row["completion_method"]=method;rows[0]=row;value["operations"]=rows;
            const auto parsed=ParseOperations(value);QVERIFY(parsed);QCOMPARE(parsed->operations[0].completionMethod,method);
        }
        for(const auto& bad:QList<QJsonValue>{QJsonValue(),QJsonValue(true),QJsonValue("wallet.orchard.queuespend"),QJsonValue("sendtoaddress")}) {
            row["completion_method"]=bad;rows[0]=row;value["operations"]=rows;QVERIFY(!ParseOperations(value));
        }
    }
    void resumedPaymentHasOnlyACompletionSelector() {
        for(const auto& method:QStringList{kFinishShield,kFinishSpend}) {
            auto snapshot=OperationSnapshot();auto rows=snapshot["operations"].toArray();auto row=rows[0].toObject();
            row["completion_method"]=method;rows[0]=row;snapshot["operations"]=rows;
            auto ops=ParseOperations(snapshot);QVERIFY(ops);auto pay=OrchardPayment::Resume(ops->account,ops->operations[0],ops->accountRevision);QVERIFY(pay);
            QVERIFY(!pay->canQueue());QVERIFY_EXCEPTION_THROWN(pay->request(),std::logic_error);
            QCOMPARE(pay->requestId(),QString(64,'a'));QCOMPARE(pay->finishMethod(),method);
            const auto selector=pay->finishParams();QCOMPARE(selector.size(),2);QCOMPARE(selector["account"].toInteger(),qint64(0));QCOMPARE(selector["request_id"].toString(),QString(64,'a'));
            pay->adoptRevision(99);QCOMPARE(pay->finishParams(),selector);
            pay->onQueueReply(QueueResponse());QVERIFY(!pay->canQueue());QCOMPARE(pay->finishParams(),selector);
            pay->onFinishReply(FinishResponse());QCOMPARE(pay->state(),State::Submitted);QCOMPARE(pay->txid(),QString(64,'f'));
        }
    }
    void incompleteOrObservedOperationCannotBeResumed() {
        auto value=OperationSnapshot();const auto ops=ParseOperations(value);QVERIFY(ops);auto op=ops->operations[0];
        QVERIFY(!OrchardPayment::Resume(0,op,5));op.completionMethod=kFinishSpend;
        QVERIFY(!OrchardPayment::Resume(0,op,0));QVERIFY(!OrchardPayment::Resume(kMaxAccount+1,op,5));
        op.outcome="confirmed";QVERIFY(!OrchardPayment::Resume(0,op,5));op.outcome="conflicted";QVERIFY(!OrchardPayment::Resume(0,op,5));
        op.outcome.clear();op.operationId=QString(64,'0');QVERIFY(!OrchardPayment::Resume(0,op,5));
    }

    void requestIdsAreFreshNonzero32Bytes() {
        QSet<QString> seen;
        for (int i = 0; i < 200; ++i) {
            const QString id = NewRequestId();
            QCOMPARE(id.size(), 64);
            QVERIFY(id != QString(64, '0'));
            QVERIFY(!seen.contains(id));
            seen.insert(id);
        }
    }

    void paramsMatchTheNodesExactFieldSets() {
        auto spend = Spend();
        spend.outputs = {{"rdin1qxyz", 5, {}}};
        const auto p = QueueParams(spend);
        const QStringList keys = p.keys();
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()),
                 (QSet<QString>{"account", "request_id", "expected_revision", "payments", "outputs", "fee_una"}));
        auto shield = Spend();
        shield.kind = Request::Kind::Shield;
        const auto s = QueueParams(shield);
        QVERIFY2(!s.contains("outputs"), "queueshield rejects an outputs field");
        QCOMPARE(s.keys().size(), 5);
        QCOMPARE(QueueMethod(shield), kQueueShield);
        QCOMPARE(FinishMethod(spend), kFinishSpend);
        // Amounts are exact integers in una.
        QCOMPARE(p["payments"].toArray()[0].toObject()["amount_una"].toVariant().toULongLong(), 150000000ULL);
        QVERIFY(!p["payments"].toArray()[0].toObject().contains("memo_hex"));
    }

    void retryResendsTheIdenticalRequest() {
        OrchardPayment pay(Spend());
        const auto first = QueueParams(pay.request());
        pay.onQueueReply(J(R"({"error":"Daemon services unavailable"})"));
        QCOMPARE(pay.state(), State::NeedsRetry);
        QCOMPARE(QueueParams(pay.request()), first);  // same id, recipients, fee: no second payment
        pay.adoptRevision(4);
        QCOMPARE(pay.request().expectedRevision, 4ULL);
        QCOMPARE(QueueParams(pay.request())["request_id"], first["request_id"]);
    }

    void aFinishedProofIsNotAPayment() {
        OrchardPayment pay(Spend());
        pay.onQueueReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"reserved","proof_queued":true,"account":0,"account_revision":5,"existing_request":false,"archived":false})"));
        QCOMPARE(pay.state(), State::Queued);
        pay.onFinishReply(J(R"({"error":"Proof pending","error_code":"proof_not_ready","proof_state":"running","reservation_retained":true})"));
        QCOMPARE(pay.state(), State::Proving);
        // Signed, but the mempool refused it: the payment did not happen.
        pay.onFinishReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":false,"already_in_mempool":false,"submission_code":"insufficient-fee","submission_message":"fee too low","account":0})"));
        QCOMPARE(pay.state(), State::Rejected);
        QCOMPARE(pay.detail(), QString("fee too low"));
        QVERIFY(!Label(State::Signed).contains("Sent") && Label(State::Signed).contains("not sent"));
    }

    void submittedThenConfirmedOnlyByTheChainObservation() {
        OrchardPayment pay(Spend());
        pay.onQueueReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"reserved","account":0,"account_revision":5,"proof_queued":true,"existing_request":false,"archived":false})"));
        pay.onFinishReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":true,"account":0,"already_in_mempool":false,"submission_code":"accepted","submission_message":""})"));
        QCOMPARE(pay.state(), State::Submitted);
        OperationsReply none;
        none.operations = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "signed", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "", 0}};
        pay.onOperations(none);
        QCOMPARE(pay.state(), State::Submitted);  // no observation yet: still not confirmed
        OperationsReply mined;
        mined.operations = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "signed", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "confirmed", 1234}};
        pay.onOperations(mined);
        QCOMPARE(pay.state(), State::Confirmed);
        // A stale finish reply cannot move it; a newer checkpoint can.
        pay.onFinishReply(J(R"({"error":"anything"})"));
        QCOMPARE(pay.state(), State::Confirmed);
    }

    void alreadyInMempoolCountsAsSubmittedAndConflictAsRejected() {
        OrchardPayment a(Spend());
        a.onFinishReply(J(R"({"durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":false,"already_in_mempool":true,"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","account":0,"submission_code":"rejected","submission_message":""})"));
        QCOMPARE(a.state(), State::Submitted);
        OperationsReply c;
        c.operations = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "signed", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "conflicted", 9}};
        a.onOperations(c);
        QCOMPARE(a.state(), State::Rejected);
    }

    void aLostReplyIsRecoveredFromTheOperationsList() {
        OrchardPayment pay(Spend());
        pay.onQueueReply(J(R"({"error":"connection reset"})"));
        QCOMPARE(pay.state(), State::NeedsRetry);
        // operationId unknown on this client; nothing to match yet, stays retryable.
        OperationsReply ops;
        ops.operations = {{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "reserved", "", "", 0}};
        pay.onOperations(ops);
        QCOMPARE(pay.state(), State::NeedsRetry);
    }

    void repliesForAnotherWalletOrGenerationAreRecognised() {
        const auto tag = ReplyTag::Make("queue", QString::fromUtf8("Wallet \xC3\xA9|1"), 7);
        const auto parsed = ReplyTag::Parse(tag);
        QVERIFY(parsed);
        QCOMPARE(parsed->wallet, QString::fromUtf8("Wallet \xC3\xA9|1"));  // names with | survive
        QCOMPARE(parsed->generation, 7ULL);
        QCOMPARE(parsed->method, QString("queue"));
        QVERIFY(!ReplyTag::Parse("swaptab.list"));
    }

    void accountRepliesRequireTypedIdentityAndIssuanceFields() {
        QJsonObject good{{"account",0},{"revision",5},{"address","rdinorch1fixture"},{"requires_sync",true}};
        QVERIFY(ParseAccount(good,true));good.remove("requires_sync");QVERIFY(ParseAccount(good));QVERIFY(!ParseAccount(good,true));
        for(const auto& field:QStringList{"account","revision","address"}){auto x=good;x.remove(field);QVERIFY(!ParseAccount(x));}
        for(const auto& value:QVector<QJsonValue>{QJsonValue(-1),QJsonValue(1.5),QJsonValue("1"),QJsonValue(true)}){
            auto x=good;x["account"]=value;QVERIFY(!ParseAccount(x));x=good;x["revision"]=value;QVERIFY(!ParseAccount(x));
        }
        auto x=good;x["account"]=2147483648.0;QVERIFY(!ParseAccount(x));x=good;x["revision"]=0;QVERIFY(!ParseAccount(x));
        x=good;x["address"]="";QVERIFY(!ParseAccount(x));x=good;x["requires_sync"]="true";QVERIFY(!ParseAccount(x));
    }
    void balanceRepliesRequireCompleteConsistentCheckpoints() {
        const auto good=BalanceSnapshot();QVERIFY(ParseBalance(good));
        for(const auto& field:good.keys()){auto x=good;x.remove(field);QVERIFY2(!ParseBalance(x),qPrintable(field));}
        for(const auto& field:QStringList{"account","account_revision","account_sequence","captured_source_sequence","checkpoint_height"}){
            auto x=good;x[field]="0";QVERIFY(!ParseBalance(x));x[field]=1.5;QVERIFY(!ParseBalance(x));
        }
        auto x=good;x["account"]=2147483648.0;QVERIFY(!ParseBalance(x));x=good;x["checkpoint_height"]=4294967296.0;QVERIFY(!ParseBalance(x));
        x=good;x["account_digest"]=QString(64,'e');QVERIFY(!ParseBalance(x));
        x=good;x["account_sequence"]=11;QVERIFY(!ParseBalance(x));x=good;x["account_caught_up_to_captured_source"]=false;QVERIFY(!ParseBalance(x));
        x=good;x["account_sequence"]=9;QVERIFY(!ParseBalance(x));x["account_caught_up_to_captured_source"]=false;QVERIFY(ParseBalance(x));
        x=good;x["checkpoint_hash"]="bad";QVERIFY(!ParseBalance(x));x=good;x["unreserved_confirmed_una"]=201;QVERIFY(!ParseBalance(x));
    }
    void activationRepliesRequireNetworkAndConsistentBoundaryFacts() {
        const auto good=ActivationSnapshot();QVERIFY(ParseActivation(good,"regtest"));QVERIFY(!ParseActivation(good,"mainnet"));
        for(const auto& field:good.keys()){auto x=good;x.remove(field);QVERIFY2(!ParseActivation(x,"regtest"),qPrintable(field));}
        auto x=good;x["activation_height"]=21;x["activation_state"]="scheduled";x["rule_active_at_tip"]=false;
        const auto boundary=ParseActivation(x,"regtest");QVERIFY(boundary);QVERIFY(!boundary->activeAtTip);QVERIFY(boundary->activeForNext);
        x["rule_active_at_tip"]=true;QVERIFY(!ParseActivation(x,"regtest"));x=good;x["next_block_height"]=22;QVERIFY(!ParseActivation(x,"regtest"));
        x=good;x["branch_id"]=0;QVERIFY(!ParseActivation(x,"regtest"));x=good;x["wallet_backend_compiled"]="true";QVERIFY(!ParseActivation(x,"regtest"));
        x=good;x["activation_state"]="unscheduled";x["activation_height"]=QJsonValue(QJsonValue::Null);x["branch_id"]=QJsonValue(QJsonValue::Null);
        x["rule_active_at_tip"]=false;x["rule_active_for_next_block"]=false;QVERIFY(ParseActivation(x,"regtest"));
        x.remove("branch_id");QVERIFY(!ParseActivation(x,"regtest"));
    }
    void amountsAreExactUna() {
        QCOMPARE(*ParseDin("1"), 100000000ULL);
        QCOMPARE(*ParseDin("0.00000001"), 1ULL);
        QCOMPARE(*ParseDin("12.5"), 1250000000ULL);
        QCOMPARE(*ParseDin(".5"), 50000000ULL);
        QVERIFY(!ParseDin("0"));
        QVERIFY(!ParseDin("1.000000001"));
        QVERIFY(!ParseDin("-1"));
        QVERIFY(!ParseDin("1e8"));
        QVERIFY(!ParseDin("1.2.3"));
        QVERIFY(!ParseDin("999999999999"));
        QCOMPARE(FormatDin(1250000001ULL), QString("12.50000001"));
    }

    void balanceIsUnknownUnlessWellFormed() {
        QVERIFY(!ParseBalance(J(R"({"error":"Method not found"})")));
        QVERIFY(!ParseBalance(J(R"({})")));
        QVERIFY(!ParseBalance(J(R"({"spendable_una":0})")));
        QVERIFY(!ParseBalance(J(R"({"confirmed_una":0,"reserved_confirmed_una":1,"unreserved_confirmed_una":0,"account_caught_up_to_captured_source":true})")));
        auto zeroReply=BalanceSnapshot();zeroReply["confirmed_una"]=0;zeroReply["reserved_confirmed_una"]=0;zeroReply["unreserved_confirmed_una"]=0;
        zeroReply["account_sequence"]=9;zeroReply["account_caught_up_to_captured_source"]=false;
        const auto zero=ParseBalance(zeroReply);
        QVERIFY(zero);QCOMPARE(zero->confirmed,0ULL);QVERIFY(!zero->caughtUp);
        const auto value=ParseBalance(BalanceSnapshot());
        QVERIFY(value);QCOMPARE(value->unreserved,200ULL);
        QVERIFY(!ReadUna(QJsonValue(-1)));QVERIFY(!ReadUna(QJsonValue(1.5)));QVERIFY(!ReadUna(QJsonValue("1")));
    }
    void newerCheckpointWithdrawsConfirmationAndOlderReplyCannotRestoreIt() {
        OrchardPayment pay(Spend());
        pay.onQueueReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","account":0,"account_revision":5,"proof_queued":false,"existing_request":false,"archived":false})"));
        OperationsReply mined;mined.accountRevision=7;mined.operations={{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","signed","ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","confirmed",10}};
        pay.onOperations(mined);QCOMPARE(pay.state(),State::Confirmed);
        auto rollback=mined;rollback.accountRevision=8;rollback.operations[0].outcome.clear();
        pay.onOperations(rollback);QCOMPARE(pay.state(),State::Signed);
        pay.onOperations(mined);QCOMPARE(pay.state(),State::Signed);
    }
    void rejectedSignedPaymentRetriesSameIdAndArchiveDoesNot() {
        auto r=Spend();OrchardPayment pay(r);
        pay.onFinishReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":false,"account":0,"already_in_mempool":false,"submission_code":"rejected","submission_message":""})"));
        QCOMPARE(pay.state(),State::Rejected);QVERIFY(pay.canRetry());QVERIFY(!IsFinal(pay.state()));
        QCOMPARE(FinishParams(pay.request()),FinishParams(r));
        pay.onFinishReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":true,"account":0,"already_in_mempool":false,"submission_code":"accepted","submission_message":""})"));
        QCOMPARE(pay.state(),State::Submitted);
        OrchardPayment archived(r);archived.onQueueReply(J(R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","archived":true,"account":0,"account_revision":5,"proof_queued":false,"existing_request":true})"));
        QCOMPARE(archived.state(),State::Archived);QVERIFY(!archived.canRetry());
    }
    void paymentReplyTagsCarryRequestIdentity() {
        const auto id=NewRequestId();const auto tag=ReplyTag::Parse(ReplyTag::Make("finish","alice",2,id));
        QVERIFY(tag);QCOMPARE(tag->requestId,id);
        QVERIFY(!ReplyTag::Parse("orchardtab|finish|616c696365|2|bad"));
    }
    void bindingAndStoredCompletionMatchNode() {
        const QString binding(64,'a');
        QVERIFY(ParseBinding(QJsonObject{{"wallet_name","alice"},{"wallet_binding",binding}},"alice"));
        QVERIFY(!ParseBinding(QJsonObject{{"wallet_name","bob"},{"wallet_binding",binding}},"alice"));
        QVERIFY(!ValidBinding(QString(64,'A')));QVERIFY(!ValidBinding(QString(64,'0')));
        const auto r=Spend();const auto p=FinishParams(r);
        QCOMPARE(p.size(),2);QCOMPARE(p.value("request_id").toString(),r.requestId);
        QVERIFY(!p.contains("payments"));QVERIFY(!p.contains("fee_una"));
    }
    void proofProgressRequiresStructuredRunningState() {
        QVERIFY(!IsProofNotReady(J(R"({"error":"proof is not available"})")));
        QVERIFY(!IsProofNotReady(J(R"({"error_code":"proof_not_ready","proof_state":"failed","reservation_retained":true})")));
        QVERIFY(IsProofNotReady(J(R"({"error_code":"proof_not_ready","proof_state":"queued","reservation_retained":true})")));
    }

    void addressKindsFollowTheChain() {
        QVERIFY(LooksLikeOrchardAddress("dinorch1qqq", "main"));
        QVERIFY(!LooksLikeOrchardAddress("rdinorch1qqq", "main"));
        QVERIFY(LooksLikeOrchardAddress("RDINORCH1QQQ", "regtest"));
        QVERIFY(LooksLikeTransparentAddress("din1pxyz", "mainnet"));
        QVERIFY(!LooksLikeTransparentAddress("dinorch1qqq", "mainnet"));
    }

    void paymentRepliesRequireCompleteTypedFields() {
        const auto queue=QueueResponse(),finish=FinishResponse();QVERIFY(ParseQueue(queue));QVERIFY(ParseFinish(finish));
        for (const auto& field:queue.keys()) {auto bad=queue;bad.remove(field);QVERIFY2(!ParseQueue(bad),qPrintable(field));}
        for (const auto& field:finish.keys()) {auto bad=finish;bad.remove(field);QVERIFY2(!ParseFinish(bad),qPrintable(field));}
        for (const auto& field:{"proof_queued","existing_request","archived"}) {
            auto bad=queue;bad[field]="true";QVERIFY(!ParseQueue(bad));
        }
        for (const auto& field:{"admitted","already_in_mempool"}) {
            auto bad=finish;bad[field]=1;QVERIFY(!ParseFinish(bad));
        }
        auto bad=queue;bad["account_revision"]=-1;QVERIFY(!ParseQueue(bad));
        bad=queue;bad["durable_state"]="future";QVERIFY(!ParseQueue(bad));
        bad=finish;bad["txid"]="ff";QVERIFY(!ParseFinish(bad));
        bad=finish;bad["account_revision"]=1.5;QVERIFY(!ParseFinish(bad));
        bad=finish;bad["account_revision"]=9;QVERIFY(ParseFinish(bad));
    }
    void mismatchedPaymentRepliesCannotPublishIdentityOrSubmission() {
        for (const auto& field:{"operation_id","account"}) {
            OrchardPayment pay(Spend());auto queue=QueueResponse();
            queue[field]=QString(field)=="account"?QJsonValue(1):QJsonValue(QString(64,'b'));
            pay.onQueueReply(queue);QCOMPARE(pay.state(),State::NeedsRetry);QVERIFY(pay.operationId().isEmpty());
            auto finish=FinishResponse();finish[field]=queue[field];
            pay.onFinishReply(finish);QCOMPARE(pay.state(),State::NeedsRetry);QVERIFY(pay.txid().isEmpty());
            QCOMPARE(pay.request().requestId,QString(64,'a'));
        }
    }
    void signedTransactionIdentityCannotChangeOnRetry() {
        OrchardPayment pay(Spend());auto finish=FinishResponse();finish["admitted"]=false;
        pay.onFinishReply(finish);QCOMPARE(pay.state(),State::Rejected);QCOMPARE(pay.txid(),QString(64,'f'));
        auto different=FinishResponse();different["txid"]=QString(64,'e');pay.onFinishReply(different);
        QCOMPARE(pay.state(),State::NeedsRetry);QCOMPARE(pay.txid(),QString(64,'f'));
        pay.onFinishReply(FinishResponse());QCOMPARE(pay.state(),State::Submitted);QCOMPARE(pay.txid(),QString(64,'f'));
    }

    void operationMetadataMustBePresentAndExactlyTyped() {
        const auto good=OperationSnapshot();QVERIFY(ParseOperations(good));
        for (const auto& field:good.keys()) { auto bad=good;bad.remove(field);QVERIFY2(!ParseOperations(bad),qPrintable(field)); }
        for (const auto& field:{"account","account_revision","account_sequence","captured_source_sequence"}) {
            for (const auto& value:QJsonArray{-1,1.5,"5",true,QJsonValue(QJsonValue::Null),9007199254740992.0}) {
                auto bad=good;bad[field]=value;QVERIFY2(!ParseOperations(bad),field);
            }
        }
        auto bad=good;bad["account"]=2147483648.0;QVERIFY(!ParseOperations(bad));
        bad=good;bad["account_sequence"]=11;QVERIFY(!ParseOperations(bad));
        bad=good;bad["captured_source_digest"]=QString(64,'d');QVERIFY(!ParseOperations(bad));
        bad=good;bad["operations"]=QJsonObject{};QVERIFY(!ParseOperations(bad));
        auto empty=good;empty["operations"]=QJsonArray{};QVERIFY(ParseOperations(empty));
        QVERIFY(ParseOperations(empty)->operations.isEmpty());
    }
    void malformedLateOperationNeverReturnsAPrefix() {
        const auto good=OperationSnapshot();const auto original=good["operations"].toArray()[0].toObject();
        for (const auto& field:original.keys()) {
            auto late=original;late["operation_id"]=QString(64,'b');late.remove(field);
            auto bad=good;bad["operations"]=QJsonArray{original,late};QVERIFY2(!ParseOperations(bad),qPrintable(field));
        }
        auto bad=good;bad["operations"]=QJsonArray{original,original};QVERIFY(!ParseOperations(bad));
        for (const auto& value:QJsonArray{0,"operation",QJsonValue(QJsonValue::Null)}) {
            bad=good;bad["operations"]=QJsonArray{original,value};QVERIFY(!ParseOperations(bad));
        }
        auto invalid=original;invalid["durable_state"]="future";bad["operations"]=QJsonArray{invalid};QVERIFY(!ParseOperations(bad));
        invalid=original;invalid["operation_id"]=QString(64,'0');bad["operations"]=QJsonArray{invalid};QVERIFY(!ParseOperations(bad));
    }
    void confirmationRequiresTheSignedTransactionsIdentity() {
        auto good=OperationSnapshot();auto op=good["operations"].toArray()[0].toObject();
        QJsonObject observation{{"outcome","confirmed"},{"height",77},{"block_hash",QString(64,'b')},{"transaction_id",QString(64,'f')}};
        op["chain_observation"]=observation;good["operations"]=QJsonArray{op};QVERIFY(ParseOperations(good));
        for (const auto& field:observation.keys()) {
            auto broken=observation;broken.remove(field);auto late=op;late["chain_observation"]=broken;
            auto bad=good;bad["operations"]=QJsonArray{late};QVERIFY2(!ParseOperations(bad),qPrintable(field));
        }
        auto broken=observation;broken["transaction_id"]=QString(64,'e');op["chain_observation"]=broken;
        good["operations"]=QJsonArray{op};QVERIFY(!ParseOperations(good));
        broken=observation;broken["height"]=4294967296.0;op["chain_observation"]=broken;
        good["operations"]=QJsonArray{op};QVERIFY(!ParseOperations(good));
    }
    void conflictingTransactionIsNeverPresentedAsOwnSignedTransaction() {
        auto good=OperationSnapshot();auto op=good["operations"].toArray()[0].toObject();
        op["durable_state"]="reserved";op.remove("txid");
        QJsonObject observation{{"outcome","conflicted"},{"height",77},{"block_hash",QString(64,'b')},{"transaction_id",QString(64,'e')}};
        op["chain_observation"]=observation;good["operations"]=QJsonArray{op};
        const auto parsed=ParseOperations(good);QVERIFY(parsed);QVERIFY(parsed->operations[0].txid.isEmpty());
        QCOMPARE(parsed->operations[0].outcome,QString("conflicted"));
        observation["outcome"]="confirmed";op["chain_observation"]=observation;good["operations"]=QJsonArray{op};QVERIFY(!ParseOperations(good));
    }

    void operationsReportSyncingSeparately() {
        const auto r = ParseOperations(J(R"({"account":0,"account_revision":5,"account_sequence":10,"captured_source_sequence":12,"account_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","captured_source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
            "operations":[{"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","chain_observation":null}]})"));
        QVERIFY(r);
        QVERIFY(r->syncing());
        QCOMPARE(r->operations.size(), 1);
        QVERIFY(r->operations[0].outcome.isEmpty());
    }
};

QTEST_MAIN(OrchardFlowTest)
#include "test_orchard_flow.moc"
