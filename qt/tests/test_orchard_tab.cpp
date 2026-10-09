// The Orchard preview screens, offline: no node. Replies are injected through
// RpcClient's signals under the exact tags the screen sent (requestSent).
#include <QtTest/QtTest>

#include <QApplication>
#include <QComboBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QTableWidget>

#include "chromestyle.h"
#include "orchardwidget.h"
#include "rpcclient.h"
#include "rpc_reply_test_access.h"
#include <memory>

namespace {
template <typename T>
T* Find(QWidget* w, const char* name) {
    auto* c = w->findChild<T*>(name);
    if (!c) qFatal("missing widget %s", name);
    return c;
}
QJsonValue J(const char* json) { return QJsonDocument::fromJson(json).object(); }

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

struct Sent { QString method; QJsonObject params; QString tag; };
}  // namespace

class OrchardTabTest : public QObject {
    Q_OBJECT
    OrchardWidget* tab_ = nullptr;
    QVector<Sent> sent_;

    Sent last(const QString& method) {
        for (int i = sent_.size() - 1; i >= 0; --i) if (sent_[i].method == method) return sent_[i];
        qFatal("no %s request sent", qPrintable(method));
        return {};
    }
    void envelopeError(const Sent& s,const QJsonObject& detail,const QString& message="refused") {
        std::unique_ptr<RpcClient> rpc(RpcReplyTestAccess::Make());
        int deliveries=0;
        connect(rpc.get(),&RpcClient::rpcErrorDetailed,this,[&](const QString& tag,int code,const QString& text,const QJsonValue& data){
            ++deliveries;
            QVERIFY(QMetaObject::invokeMethod(tab_,"onRpcErrorDetailed",Qt::DirectConnection,
                Q_ARG(QString,tag),Q_ARG(int,code),Q_ARG(QString,text),Q_ARG(QJsonValue,data)));
        });
        QSignalSpy results(rpc.get(),&RpcClient::rpcResult);
        const QJsonObject wire{{"error",QJsonObject{{"code",-32603},{"message",message},{"data",QJsonObject{{"orchard",detail}}}}}};
        QVERIFY(!RpcReplyTestAccess::Deliver(*rpc,s.tag,QJsonDocument(wire).toJson()));
        QCOMPARE(deliveries,1);QCOMPARE(results.size(),0);
    }
    void rawReply(const Sent& s,const QJsonValue& value) {
        QVERIFY(QMetaObject::invokeMethod(tab_,"onRpcResult",Qt::DirectConnection,Q_ARG(QString,s.tag),Q_ARG(QJsonValue,value)));
    }
    // Positive payment fixtures explicitly take identity from the request the
    // widget actually emitted. Negative payload tests use rawReply directly.
    QJsonObject paymentFixture(const Sent& s,QJsonObject value) {
        value["operation_id"]=s.params.value("request_id");value["account"]=s.params.value("account");
        if (s.method=="wallet.orchard.queuespend" || s.method=="wallet.orchard.queueshield") {
            value["account_revision"]=5;
            if(!value.contains("proof_queued"))value["proof_queued"]=value.value("durable_state").toString()=="reserved";
            if(!value.contains("existing_request"))value["existing_request"]=false;
            if(!value.contains("archived"))value["archived"]=false;
        } else {
            if(!value.contains("already_in_mempool"))value["already_in_mempool"]=false;
            if(!value.contains("submission_code"))value["submission_code"]=value.value("admitted").toBool()?"accepted":"rejected";
            if(!value.contains("submission_message"))value["submission_message"]="";
        }
        return value;
    }
    void reply(const Sent& s, const char* json) {
        auto value=J(json).toObject();
        if(value.contains("durable_state") && !value.contains("error"))value=paymentFixture(s,value);
        rawReply(s,value);
    }
    QJsonObject activationFixture() {
        return {{"network","regtest"},{"activation_state","active"},{"activation_height",10},{"branch_id",42},
            {"tip_height",20},{"tip_hash",QString(64,'d')},{"next_block_height",21},
            {"rule_active_at_tip",true},{"rule_active_for_next_block",true},{"wallet_backend_compiled",true},{"storage_mode","full"}};
    }
    QJsonObject balanceFixture() {
        return {{"account",0},{"account_revision",5},{"account_sequence",3},{"captured_source_sequence",3},
            {"account_digest",QString(64,'c')},{"captured_source_digest",QString(64,'c')},
            {"checkpoint_height",20},{"checkpoint_hash",QString(64,'d')},
            {"confirmed_una",0},{"reserved_confirmed_una",0},{"unreserved_confirmed_una",0},{"account_caught_up_to_captured_source",true}};
    }
    QJsonObject storedSnapshot(const QString& method={}) {
        QJsonObject row{{"operation_id",QString(64,'b')},{"durable_state","signed"},{"txid",QString(64,'f')},{"chain_observation",QJsonValue(QJsonValue::Null)}};
        if(!method.isEmpty())row["completion_method"]=method;
        return {{"account",0},{"account_revision",5},{"account_sequence",3},{"captured_source_sequence",3},
            {"account_digest",QString(64,'c')},{"captured_source_digest",QString(64,'c')},{"operations",QJsonArray{row}}};
    }
    QJsonObject accountsFixture(const QList<int>& ids={0}) {
        QJsonArray rows;
        for(int id:ids)rows.append(QJsonObject{{"account",id},{"account_revision",5},{"account_sequence",3},
            {"account_digest",QString(64,'c')},{"checkpoint_height",20},{"checkpoint_hash",QString(64,'d')}});
        return {{"captured_source_sequence",3},{"captured_source_digest",QString(64,'c')},{"accounts",rows}};
    }
    void bind(const QString& wallet) {
        rawReply(last("getblockchaininfo"),QJsonObject{{"chain","regtest"}});
        const auto s=last("wallet.orchard.getwalletbinding");
        const QJsonValue value=QJsonObject{{"wallet_name",wallet},{"wallet_binding",QString(64,'a')}};
        QVERIFY(QMetaObject::invokeMethod(tab_,"onRpcResult",Qt::DirectConnection,Q_ARG(QString,s.tag),Q_ARG(QJsonValue,value)));
        rawReply(last("wallet.orchard.listaccounts"),accountsFixture());
        rawReply(last("orchard.getactivationstatus"),activationFixture());
    }
    // Wallet "alice" on regtest, unlocked, with an Orchard account at revision 5.
    void ready() {
        tab_->setChain("regtest");
        tab_->setWalletScope("alice");
        bind("alice");
        tab_->setWalletUnlocked(true);
        reply(last("wallet.orchard.listoperations"),
              R"({"account":0,"account_revision":5,"account_sequence":3,"captured_source_sequence":3,"account_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","captured_source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","operations":[]})");
    }
    void fillSend() {
        Find<QComboBox>(tab_, "orchardMode")->setCurrentIndex(1);  // send privately
        Find<QLineEdit>(tab_, "orchardRecipient")->setText("rdinorch1qqqqrecipient");
        Find<QLineEdit>(tab_, "orchardAmount")->setText("1.5");
    }
    void confirmNextDialog() {
        QTimer::singleShot(100, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                for (auto* b : box->buttons()) if (box->buttonRole(b) == QMessageBox::AcceptRole) return b->click();
        });
    }

private Q_SLOTS:
    void init() {
        tab_ = new OrchardWidget(nullptr);  // passive transport: no sockets or IPC
        sent_.clear();
        connect(tab_, &OrchardWidget::requestSent, this,
                [this](const QString& m, const QJsonObject& p, const QString& t) { sent_.append({m, p, t}); });
        tab_->setChain("regtest");
        tab_->resize(1440, 1200);
        tab_->show();
    }
    void cleanup() { delete tab_; tab_ = nullptr; }


    void structuredEnvelopeKeepsProofProgressAndBindingRefusal() {
        ready();fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        const auto queue=last("wallet.orchard.queuespend");
        reply(queue,R"({"durable_state":"reserved","proof_queued":true})");
        QMetaObject::invokeMethod(tab_,"onPoll");const auto finish=last("wallet.orchard.finishspend");
        envelopeError(finish,{{"error_code","proof_not_ready"},{"proof_state","running"},{"reservation_retained",true}});
        QCOMPARE(*tab_->paymentState(),OrchardFlow::State::Proving);
        envelopeError(finish,{{"error_code","wallet_binding_mismatch"}});
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        const auto before=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");QCOMPARE(sent_.size(),before);
    }
    void structuredEnvelopeHistoryAndPreviousWalletRemainScoped() {
        ready();const auto old=last("wallet.orchard.listreceived");
        envelopeError(old,{{"error_code","history_incomplete"}});
        QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("History incomplete"));
        tab_->setWalletScope("bob");bind("bob");const auto before=sent_.size();
        envelopeError(old,{{"error_code","wallet_binding_mismatch"}});
        QCOMPARE(sent_.size(),before);
        const auto fresh=last("wallet.orchard.listreceived");
        envelopeError(fresh,{{"error_code","stale_account_revision"}});
        QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("History changed"));
    }
    void historyPagesKeepRevisionAndRejectLatePriorPage() {
        ready();const auto first=last("wallet.orchard.listreceived");QCOMPARE(first.params["wallet_binding"].toString(),QString(64,'a'));QVERIFY(!first.params.contains("request_id"));
        rawReply(first,ReceivedPage());auto* table=Find<QTableWidget>(tab_,"orchardHistoryTable");QCOMPARE(table->rowCount(),100);
        QCOMPARE(table->item(0,0)->text(),QString("Incoming"));QCOMPARE(table->item(0,1)->text(),QString("1.00000000"));
        auto* next=Find<QPushButton>(tab_,"orchardHistoryNext");QVERIFY(next->isEnabled());next->click();const auto second=last("wallet.orchard.listreceived");
        QCOMPARE(second.params["offset"].toInt(),100);QCOMPARE(second.params["expected_revision"].toInt(),5);QVERIFY(second.tag!=first.tag);
        rawReply(first,ReceivedPage());QVERIFY(!next->isEnabled());QVERIFY(table->isHidden());
        rawReply(second,ReceivedPage(0,100));QCOMPARE(table->rowCount(),1);QCOMPARE(table->item(0,0)->text(),QString("Change"));QVERIFY(!next->isEnabled());
        auto* previous=Find<QPushButton>(tab_,"orchardHistoryPrevious");QVERIFY(previous->isEnabled());previous->click();const auto back=last("wallet.orchard.listreceived");
        QCOMPARE(back.params["offset"].toInt(),0);QCOMPARE(back.params["expected_revision"].toInt(),5);
        rawReply(back,QJsonObject{{"error","changed"},{"error_code","stale_account_revision"}});
        QCOMPARE(table->rowCount(),0);QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("History changed"));
        Find<QPushButton>(tab_,"orchardHistoryRefresh")->click();QVERIFY(!last("wallet.orchard.listreceived").params.contains("expected_revision"));
    }
    void historyWrongWalletAndAccountCannotPopulateTheView() {
        ready();const auto old=last("wallet.orchard.listreceived");tab_->setWalletScope("bob");bind("bob");const auto fresh=last("wallet.orchard.listreceived");
        auto* table=Find<QTableWidget>(tab_,"orchardHistoryTable");rawReply(old,ReceivedPage());QCOMPARE(table->rowCount(),0);
        rawReply(fresh,ReceivedPage(17));QCOMPARE(table->rowCount(),0);QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("unavailable"));
        Find<QPushButton>(tab_,"orchardHistoryRefresh")->click();rawReply(last("wallet.orchard.listreceived"),ReceivedPage(0,0,0));
        QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("No receipts at this checkpoint"));
    }
    void historyMalformedLateRowOrLegacyGapShowsNoPartialList() {
        ready();auto bad=ReceivedPage();auto rows=bad["received"].toArray();auto late=rows[99].toObject();late["amount_una"]="100000000";rows[99]=late;bad["received"]=rows;
        rawReply(last("wallet.orchard.listreceived"),bad);QCOMPARE(Find<QTableWidget>(tab_,"orchardHistoryTable")->rowCount(),0);
        QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("unavailable"));
        Find<QPushButton>(tab_,"orchardHistoryRefresh")->click();rawReply(last("wallet.orchard.listreceived"),QJsonObject{{"error","missing history"},{"error_code","history_incomplete"}});
        QCOMPARE(Find<QTableWidget>(tab_,"orchardHistoryTable")->rowCount(),0);QVERIFY(Find<QLabel>(tab_,"orchardHistoryStatus")->text().contains("History incomplete"));
    }

    void accountDiscoveryPrecedesReadsAndSelectsActualIds() {
        tab_->setWalletScope("alice");tab_->setWalletUnlocked(true);
        rawReply(last("getblockchaininfo"),QJsonObject{{"chain","regtest"}});
        rawReply(last("wallet.orchard.getwalletbinding"),QJsonObject{{"wallet_name","alice"},{"wallet_binding",QString(64,'a')}});
        for(const auto& call:sent_)QVERIFY(call.method!="wallet.orchard.listoperations" && call.method!="wallet.orchard.getbalance");
        const auto catalog=last("wallet.orchard.listaccounts");QCOMPARE(catalog.params.size(),1); // binding only
        rawReply(catalog,accountsFixture({3,17}));
        auto* selector=Find<QComboBox>(tab_,"orchardAccountSelector");QCOMPARE(selector->count(),2);
        QCOMPARE(last("wallet.orchard.listoperations").params["account"].toInt(),3);
        const auto oldBalance=last("wallet.orchard.getbalance");const auto oldOps=last("wallet.orchard.listoperations");const auto generation=tab_->generation();
        selector->setCurrentIndex(1);QCOMPARE(tab_->generation(),generation+1);
        QCOMPARE(last("wallet.orchard.listoperations").params["account"].toInt(),17);
        auto b=balanceFixture();b["account"]=3;rawReply(oldBalance,b);rawReply(oldOps,storedSnapshot());
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        b["account"]=17;rawReply(last("wallet.orchard.getbalance"),b);
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("0.00000000 DIN confirmed"));
    }
    void failedAccountDiscoveryNeverEnablesCreationOrUsesOldBalance() {
        ready();rawReply(last("wallet.orchard.listaccounts"),QJsonObject{{"error","catalog missing"}});
        QVERIFY(!Find<QPushButton>(tab_,"orchardCreateAccount")->isEnabled());
        QVERIFY(!Find<QPushButton>(tab_,"orchardNewAddress")->isEnabled());
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        const auto count=sent_.size();QMetaObject::invokeMethod(tab_,"onCreateAccount");QCOMPARE(sent_.size(),count);
        QMetaObject::invokeMethod(tab_,"onPoll");rawReply(last("wallet.orchard.listaccounts"),accountsFixture({}));
        QVERIFY(Find<QPushButton>(tab_,"orchardCreateAccount")->isEnabled());
        QCOMPARE(Find<QComboBox>(tab_,"orchardAccountSelector")->count(),0);
    }
    void activePaymentCannotBeRetargetedToAnotherAccount() {
        ready();rawReply(last("wallet.orchard.listaccounts"),accountsFixture({0,17}));fillSend();
        confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        const auto queued=last("wallet.orchard.queuespend");QCOMPARE(queued.params["account"].toInt(),0);
        auto* selector=Find<QComboBox>(tab_,"orchardAccountSelector");QVERIFY(!selector->isEnabled());
        const auto generation=tab_->generation();const auto count=sent_.size();selector->setCurrentIndex(1);
        QCOMPARE(selector->currentIndex(),0);QCOMPARE(tab_->generation(),generation);QCOMPARE(sent_.size(),count);
        reply(queued,R"({"durable_state":"reserved"})");
        QVERIFY(!selector->isEnabled());
    }
    void noWalletEffectsBeforeBindingAndStaleBindingStopsRequests() {
        tab_->setWalletScope("alice");tab_->setWalletUnlocked(true);
        QCOMPARE(sent_.size(),3); // binding, activation and generation-tagged network reads only
        QCOMPARE(sent_[0].method,QString("wallet.orchard.getwalletbinding"));
        QCOMPARE(sent_[1].method,QString("orchard.getactivationstatus"));
        QCOMPARE(sent_[2].method,QString("getblockchaininfo"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardCreateAccount")->isEnabled());
        bind("alice");
        const auto read=last("wallet.orchard.listoperations");
        QCOMPARE(read.params.value("wallet_binding").toString(),QString(64,'a'));
        reply(read,R"({"error":"Changed","error_code":"wallet_binding_mismatch"})");
        const auto count=sent_.size();QMetaObject::invokeMethod(tab_,"onNewAddress");
        QCOMPARE(sent_.size(),count);
    }
    void walletSwitchDuringReviewDoesNotRetargetPayment() {
        ready();fillSend();
        QTimer::singleShot(100,[this] {
            tab_->setWalletScope("bob");bind("bob");
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                for(auto* b:box->buttons())if(box->buttonRole(b)==QMessageBox::AcceptRole){b->click();return;}
        });
        Find<QPushButton>(tab_,"orchardReview")->click();
        for(const auto& s:sent_)QVERIFY(s.method!="wallet.orchard.queuespend");
    }
    void rejectedPaymentRetainsIdentityAndBlocksAnotherPayment() {
        ready();fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        const auto queue=last("wallet.orchard.queuespend");
        reply(queue,R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed"})");
        const auto finish=last("wallet.orchard.finishspend");
        reply(finish,R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":false})");
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        QVERIFY(Find<QPushButton>(tab_,"orchardRetry")->isVisible());
        Find<QPushButton>(tab_,"orchardRetry")->click();
        QCOMPARE(last("wallet.orchard.finishspend").params,finish.params);
    }
    void periodicReadsDoNotPileUpWhenRepliesArePending() {
        ready();const auto before=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");
        const auto first=sent_.size();QVERIFY(first>before);
        QMetaObject::invokeMethod(tab_,"onPoll");QCOMPARE(sent_.size(),first);
    }
    void malformedOperationsDoNotEstablishAnAccount() {
        tab_->setChain("regtest");tab_->setWalletScope("alice");bind("alice");tab_->setWalletUnlocked(true);
        const auto call=last("wallet.orchard.listoperations");
        reply(call,R"({"account":0,"account_revision":5,"account_sequence":3,"captured_source_sequence":3})");
        fillSend();QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        QCOMPARE(Find<QTableWidget>(tab_,"orchardOperationsTable")->rowCount(),0);
    }
    void wrongAccountOperationsDoNotEstablishAnAccount() {
        tab_->setChain("regtest");tab_->setWalletScope("alice");bind("alice");tab_->setWalletUnlocked(true);
        const QJsonValue value=QJsonObject{{"account",1},{"account_revision",5},{"account_sequence",3},
            {"captured_source_sequence",3},{"account_digest",QString(64,'c')},
            {"captured_source_digest",QString(64,'c')},{"operations",QJsonArray{}}};
        const auto call=last("wallet.orchard.listoperations");
        QVERIFY(QMetaObject::invokeMethod(tab_,"onRpcResult",Qt::DirectConnection,Q_ARG(QString,call.tag),Q_ARG(QJsonValue,value)));
        fillSend();QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
    }
    void malformedFinishCannotMarkPaymentSubmitted() {
        ready();fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        const auto queue=last("wallet.orchard.queuespend");reply(queue,R"({"durable_state":"signed"})");
        const auto finish=last("wallet.orchard.finishspend");
        auto value=paymentFixture(finish,QJsonObject{{"durable_state","signed"},{"txid",QString(64,'f')},{"admitted",true}});
        value["admitted"]="true";rawReply(finish,value);
        QCOMPARE(*tab_->paymentState(),OrchardFlow::State::NeedsRetry);
        QVERIFY(!Find<QLabel>(tab_,"orchardPaymentState")->text().startsWith("Submitted"));
    }
    void invalidActivationCannotEnablePayment() {
        ready();fillSend();QVERIFY(Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        auto x=activationFixture();x["network"]="mainnet";rawReply(last("orchard.getactivationstatus"),x);
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        rawReply(last("orchard.getactivationstatus"),activationFixture());QVERIFY(Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        x=activationFixture();x.remove("tip_hash");rawReply(last("orchard.getactivationstatus"),x);
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
    }
    void wrongAccountAndStaleBalancesStayUnknown() {
        ready();const auto call=last("wallet.orchard.getbalance");auto x=balanceFixture();x["account"]=1;rawReply(call,x);
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        x=balanceFixture();x["account_revision"]=4;rawReply(call,x);QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        x=balanceFixture();x["account_revision"]=6;rawReply(call,x);QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("0.00000000 DIN confirmed"));
        rawReply(call,balanceFixture());QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
    }
    void wrongAccountAddressDoesNotReplaceReceiveAddress() {
        ready();QMetaObject::invokeMethod(tab_,"onNewAddress");const auto call=last("wallet.orchard.getnewaddress");
        QJsonObject x{{"account",0},{"revision",6},{"address","rdinorch1fixture"}};rawReply(call,x);
        QCOMPARE(Find<QLineEdit>(tab_,"orchardReceiveAddress")->text(),QString("rdinorch1fixture"));
        x["account"]=1;x["revision"]=7;x["address"]="rdinorch1other";rawReply(call,x);
        QCOMPARE(Find<QLineEdit>(tab_,"orchardReceiveAddress")->text(),QString("rdinorch1fixture"));
        x["account"]=0;x["revision"]=5;rawReply(call,x);
        QCOMPARE(Find<QLineEdit>(tab_,"orchardReceiveAddress")->text(),QString("rdinorch1fixture"));
    }
    void connectionChangeRevokesBindingAndDropsPreviousReplies() {
        ready();fillSend();const auto oldBinding=last("wallet.orchard.getwalletbinding"),oldBalance=last("wallet.orchard.getbalance"),oldActivation=last("orchard.getactivationstatus"),oldChain=last("getblockchaininfo");
        const auto count=sent_.size();const auto generation=tab_->generation();
        QVERIFY(QMetaObject::invokeMethod(tab_,"onConnectionContextChanged",Qt::DirectConnection));
        QCOMPARE(tab_->generation(),generation+1);QCOMPARE(sent_.size(),count);
        rawReply(oldBinding,QJsonObject{{"wallet_name","alice"},{"wallet_binding",QString(64,'a')}});
        rawReply(oldBalance,balanceFixture());rawReply(oldActivation,activationFixture());rawReply(oldChain,QJsonObject{{"chain","regtest"}});
        tab_->setWalletUnlocked(true);QMetaObject::invokeMethod(tab_,"onPoll");QMetaObject::invokeMethod(tab_,"onNewAddress");
        QCOMPARE(sent_.size(),count);QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());QVERIFY(!Find<QPushButton>(tab_,"orchardNewAddress")->isEnabled());
    }
    void connectionChangeCannotRetargetPendingPayment() {
        ready();fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();const auto queue=last("wallet.orchard.queuespend");
        const auto count=sent_.size();QMetaObject::invokeMethod(tab_,"onConnectionContextChanged",Qt::DirectConnection);
        reply(queue,R"({"durable_state":"signed"})");QMetaObject::invokeMethod(tab_,"onRetry");QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(sent_.size(),count);QVERIFY(tab_->paymentState().has_value());
        QVERIFY(Find<QLabel>(tab_,"orchardPaymentState")->text().contains("Connection changed"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardRetry")->isVisible());
    }
    void reopenedWalletWaitsForDiscoveryAndResumesOnlyById() {
        for(const auto& method:QStringList{"wallet.orchard.finishshield","wallet.orchard.finishspend"}) {
            ready();fillSend();tab_->setWalletScope("alice");bind("alice");tab_->setWalletUnlocked(true);
            QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
            const auto read=last("wallet.orchard.listoperations");const auto count=sent_.size();
            rawReply(read,storedSnapshot(method));QCOMPARE(sent_.size(),count); // discovery itself never submits
            QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
            Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
            auto* resume=Find<QPushButton>(tab_,"orchardResume");QVERIFY(resume->isEnabled());
            confirmNextDialog();resume->click();const auto finish=last(method);
            QCOMPARE(sent_.size(),count+1);QCOMPARE(finish.params.size(),3);
            QCOMPARE(finish.params.value("account").toInteger(),qint64(0));QCOMPARE(finish.params.value("request_id").toString(),QString(64,'b'));
            QCOMPARE(finish.params.value("wallet_binding").toString(),QString(64,'a'));
            QVERIFY(!finish.params.contains("payments"));QVERIFY(!finish.params.contains("outputs"));QVERIFY(!finish.params.contains("fee_una"));
            QVERIFY(!resume->isEnabled());
        }
    }
    void legacyAndMalformedDiscoveryCannotEnableAnotherPayment() {
        ready();fillSend();const auto read=last("wallet.orchard.listoperations");const auto count=sent_.size();
        rawReply(read,storedSnapshot());Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());QVERIFY(!Find<QPushButton>(tab_,"orchardResume")->isEnabled());
        rawReply(read,storedSnapshot("sendtoaddress"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());QVERIFY(!Find<QPushButton>(tab_,"orchardResume")->isEnabled());
        QMetaObject::invokeMethod(tab_,"onResumeSelected",Qt::DirectConnection);QCOMPARE(sent_.size(),count);
    }
    void previousWalletDiscoveryCannotOfferResume() {
        ready();const auto old=last("wallet.orchard.listoperations");tab_->setWalletScope("bob");bind("bob");tab_->setWalletUnlocked(true);
        const auto count=sent_.size();rawReply(old,storedSnapshot("wallet.orchard.finishspend"));
        QMetaObject::invokeMethod(tab_,"onResumeSelected",Qt::DirectConnection);
        QCOMPARE(sent_.size(),count);QVERIFY(!Find<QPushButton>(tab_,"orchardResume")->isEnabled());QVERIFY(!tab_->paymentState());
    }
    void everyButtonHasHoverTextAndTheAppLook() {
        for (auto* b : tab_->findChildren<QPushButton*>()) {
            QVERIFY2(!b->toolTip().trimmed().isEmpty(), qPrintable("no hover text on " + b->text()));
            QVERIFY2(b->styleSheet().contains("border-radius: 7px"), qPrintable("not a chrome button: " + b->text()));
        }
    }

    void balanceIsUnknownNeverZero() {
        ready();
        auto* balance = Find<QLabel>(tab_, "orchardBalance");
        QCOMPARE(balance->text(), QString("Unknown"));
        reply(last("wallet.orchard.getbalance"), R"({"error":"Method not found"})");
        QCOMPARE(balance->text(), QString("Unknown"));
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        QCOMPARE(balance->text(), QString("0.00000000 DIN confirmed"));  // only a reported zero shows as zero
    }

    void repliesForAPreviousWalletAreIgnored() {
        tab_->setChain("regtest");
        tab_->setWalletScope("alice");
        bind("alice");
        tab_->setWalletUnlocked(true);
        const Sent aliceOps = last("wallet.orchard.listoperations");
        tab_->setWalletScope("bob");
        bind("bob");
        // Bob's authenticated catalog already contains account 0. Its saved
        // operations are still unknown; Alice's late reply cannot satisfy them.
        const QString beforeHint = Find<QLabel>(tab_, "orchardFormHint")->text();
        QVERIFY2(beforeHint.contains("wait for saved-payment discovery"),
                 "Bob's account must await its own saved-payment discovery");
        QVERIFY(!Find<QPushButton>(tab_, "orchardReview")->isEnabled());
        const int requestsBeforeLateReply = sent_.size();
        reply(aliceOps, R"({"account":0,"account_revision":9,"account_sequence":1,"captured_source_sequence":1,"account_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","captured_source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","operations":[]})");
        QCOMPARE(sent_.size(), requestsBeforeLateReply);
        fillSend();
        QCOMPARE(Find<QLabel>(tab_, "orchardFormHint")->text(), beforeHint);
        QVERIFY2(Find<QLabel>(tab_, "orchardFormHint")->text().contains("wait for saved-payment discovery"),
                 "alice's reply must not mark bob's account as ready");
        QVERIFY(!Find<QPushButton>(tab_, "orchardReview")->isEnabled());
    }

    void aPaymentMovesThroughDistinctStates() {
        ready();
        fillSend();
        QVERIFY(Find<QPushButton>(tab_, "orchardReview")->isEnabled());
        confirmNextDialog();
        Find<QPushButton>(tab_, "orchardReview")->click();
        const Sent queue = last("wallet.orchard.queuespend");
        QCOMPARE(queue.params["expected_revision"].toInt(), 5);
        QCOMPARE(queue.params["payments"].toArray()[0].toObject()["amount_una"].toVariant().toULongLong(), 150000000ULL);
        QVERIFY(!Find<QPushButton>(tab_, "orchardReview")->isEnabled());  // one payment at a time
        reply(queue, R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"reserved","proof_queued":true})");
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::Queued);
        QMetaObject::invokeMethod(tab_, "onPoll");
        reply(last("wallet.orchard.finishspend"), R"({"error":"Proof pending","error_code":"proof_not_ready","proof_state":"running","reservation_retained":true})");
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::Proving);
        QCOMPARE(Find<QLabel>(tab_, "orchardPaymentState")->text(), QString("Building proof"));
        QMetaObject::invokeMethod(tab_, "onPoll");
        reply(last("wallet.orchard.finishspend"),
              R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":true})");
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::Submitted);
        QVERIFY(Find<QLabel>(tab_, "orchardPaymentState")->text().startsWith("Submitted"));
        QMetaObject::invokeMethod(tab_, "onPoll");
        auto confirmation=J(
              R"({"account":0,"account_revision":6,"account_sequence":4,"captured_source_sequence":4,"account_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","captured_source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","operations":[
                 {"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","chain_observation":
                  {"outcome":"confirmed","height":77,"block_hash":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","transaction_id":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}}]})").toObject();
        auto observations=confirmation.value("operations").toArray();
        auto own=observations[0].toObject();own["operation_id"]=queue.params.value("request_id");
        observations[0]=own;confirmation["operations"]=observations;
        rawReply(last("wallet.orchard.listoperations"),confirmation);
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::Confirmed);
        QVERIFY(Find<QPushButton>(tab_, "orchardReview")->isEnabled());  // next payment allowed
    }

    void aRefusedSubmissionIsShownAsRejectedNotPaid() {
        ready();
        fillSend();
        confirmNextDialog();
        Find<QPushButton>(tab_, "orchardReview")->click();
        reply(last("wallet.orchard.queuespend"), R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed"})");
        reply(last("wallet.orchard.finishspend"),
              R"({"operation_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":false,"already_in_mempool":false,
                 "submission_code":"insufficient-fee","submission_message":"fee too low"})");
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::Rejected);
        QCOMPARE(Find<QLabel>(tab_, "orchardPaymentState")->text(), QString("Rejected"));
    }

    void retrySendsTheIdenticalRequest() {
        ready();
        fillSend();
        confirmNextDialog();
        Find<QPushButton>(tab_, "orchardReview")->click();
        const Sent first = last("wallet.orchard.queuespend");
        reply(first,R"({"error":"Connection reset"})");  // reply may have been lost
        QCOMPARE(*tab_->paymentState(), OrchardFlow::State::NeedsRetry);
        auto* retry = Find<QPushButton>(tab_, "orchardRetry");
        QVERIFY(retry->isVisible());
        const int before = sent_.size();
        retry->click();
        QCOMPARE(sent_.size(), before + 1);
        const Sent again = last("wallet.orchard.queuespend");
        QCOMPARE(again.params, first.params);  // same id, recipients and fee: never a second payment
    }

    void shotsForReview() {
        const QString dir = qEnvironmentVariable("ORCHARD_TAB_SHOTS");
        if (dir.isEmpty()) QSKIP("set ORCHARD_TAB_SHOTS=<dir> to save pictures");
        tab_->setStyleSheet(appPageStyle());
        tab_->resize(1440, 1000);
        ready();
        auto history=ReceivedPage(0,0,2);auto notes=history["received"].toArray();auto change=notes[1].toObject();change["scope"]="internal";notes[1]=change;history["received"]=notes;
        rawReply(last("wallet.orchard.listreceived"),history);
        reply(last("wallet.orchard.getbalance"), R"({"error":"Method not found"})");
        QVERIFY(tab_->grab().save(dir + "/orchard-ready.png"));
        fillSend();
        confirmNextDialog();
        Find<QPushButton>(tab_, "orchardReview")->click();
        reply(last("wallet.orchard.queuespend"), R"({"operation_id":"a1b2c3d4e5f6a7b8","durable_state":"reserved"})");
        QMetaObject::invokeMethod(tab_, "onPoll");
        reply(last("wallet.orchard.finishspend"), R"({"error":"Proof pending","error_code":"proof_not_ready","proof_state":"running","reservation_retained":true})");
        QApplication::processEvents();
        QVERIFY(tab_->grab().save(dir + "/orchard-proving.png"));
    }

    void noPrivateCovenantsAndNoInventedActivation() {
        ready();
        const QRegularExpression countdown("activat\\w*\\s+(at|in|on)\\s+\\d|countdown|block\\s+\\d+", QRegularExpression::CaseInsensitiveOption);
        for (auto* w : tab_->findChildren<QWidget*>()) {
            QString text;
            if (auto* l = qobject_cast<QLabel*>(w)) text = l->text();
            if (auto* b = qobject_cast<QPushButton*>(w)) text = b->text() + " " + b->toolTip();
            QVERIFY2(!text.contains("covenant", Qt::CaseInsensitive), qPrintable(text));
            QVERIFY2(!countdown.match(text).hasMatch(), qPrintable(text));
        }
    }
};

QTEST_MAIN(OrchardTabTest)
#include "test_orchard_tab.moc"
