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
#include <algorithm>
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
    // Requests to the wallet (reads and payments). Looking a transaction up
    // on the chain for the details panel is not one of them.
    // wallet.orchard.* requests sent since `from`.
    int walletReads(qsizetype from) const {
        int n=0;for(qsizetype i=from;i<sent_.size();++i)if(sent_[i].method.startsWith("wallet.orchard."))++n;return n;
    }
    qsizetype walletRequests() const {
        qsizetype n=0;for(const auto& r:sent_)if(r.method!="gettransaction")++n;return n;
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
            {"rule_active_at_tip",true},{"rule_active_for_next_block",true},{"wallet_backend_compiled",true},{"storage_mode","full"},
            {"node_supported",true},{"node_notice",""},{"storage_upgrade_refused",false},{"storage_upgrade_reason","none"}};
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

    void transientOperationsErrorClearsAfterRecovery() {
        ready();
        QMetaObject::invokeMethod(tab_, "onPoll");
        rawReply(last("wallet.orchard.listoperations"),
                 QJsonObject{{"error", "Wallet recovery seed unavailable"}});
        auto* status = Find<QLabel>(tab_, "orchardStatus");
        QCOMPARE(status->text(), QString("Wallet recovery seed unavailable"));
        QVERIFY(status->isVisible());

        QMetaObject::invokeMethod(tab_, "onPoll");
        rawReply(last("wallet.orchard.listoperations"), storedSnapshot());
        QVERIFY(!status->isVisible());
    }


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
    // The page read the wallet every three seconds on whichever tab was
    // showing. Hidden, it reads nothing; shown again, it catches up at once;
    // a payment on its way is followed all the same.
    void hiddenPageReadsNothingUnlessAPaymentIsOnItsWay() {
        ready();
        const auto orchardReads=[&](int from){
            int n=0;for(int i=from;i<sent_.size();++i)if(sent_[i].method.startsWith("wallet.orchard."))++n;return n;};
        tab_->hide();
        int mark=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(sent_.size(),mark);

        mark=sent_.size();tab_->show();
        QVERIFY2(orchardReads(mark)>0,"shown again: one refresh without waiting for the tick");

        // A queued payment: its proof is collected by the page's own "finish" call.
        fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        reply(last("wallet.orchard.queuespend"),R"({"durable_state":"reserved","proof_queued":true})");
        tab_->hide();
        mark=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");
        bool finishing=false;
        for(int i=mark;i<sent_.size();++i)finishing=finishing||sent_[i].method=="wallet.orchard.finishspend";
        QVERIFY2(finishing,"a hidden page still finishes a payment that is on its way");
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
    void originalOrchardAddressAppearsFromAccountDiscovery() {
        tab_->setWalletScope("Hotel");tab_->setWalletUnlocked(true);
        rawReply(last("getblockchaininfo"),QJsonObject{{"chain","regtest"}});
        rawReply(last("wallet.orchard.getwalletbinding"),QJsonObject{
            {"wallet_name","Hotel"},{"wallet_binding",QString(64,'a')}});
        auto catalog=accountsFixture();
        auto accounts=catalog["accounts"].toArray();
        auto account=accounts[0].toObject();
        account["first_receive_address"]="rdinorch1originalreceiver";
        accounts[0]=account;catalog["accounts"]=accounts;
        rawReply(last("wallet.orchard.listaccounts"),catalog);
        auto* address=Find<QLineEdit>(tab_,"orchardReceiveAddress");
        QCOMPARE(address->text(),QString("rdinorch1originalreceiver"));
        QVERIFY(address->isVisible());
        QCOMPARE(std::count_if(sent_.begin(),sent_.end(),[](const Sent& call) {
            return call.method=="wallet.orchard.getnewaddress";
        }),0);
    }
    void failedAccountDiscoveryNeverEnablesCreationOrUsesOldBalance() {
        ready();rawReply(last("wallet.orchard.listaccounts"),QJsonObject{{"error","catalog missing"}});
        QVERIFY(Find<QPushButton>(tab_,"orchardCreateAccount")->isVisible());
        QVERIFY(!Find<QPushButton>(tab_,"orchardCreateAccount")->isEnabled());
        QVERIFY(!Find<QPushButton>(tab_,"orchardNewAddress")->isEnabled());
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        const auto count=sent_.size();QMetaObject::invokeMethod(tab_,"onCreateAccount");QCOMPARE(sent_.size(),count);
        const auto catalogCalls=[&] {
            return std::count_if(sent_.begin(),sent_.end(),[](const Sent& call) {
                return call.method=="wallet.orchard.listaccounts";
            });
        };
        const auto catalogCount=catalogCalls();
        QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(catalogCalls(),catalogCount);
        tab_->setWalletUnlocked(false);tab_->setWalletUnlocked(true);
        QCOMPARE(catalogCalls(),catalogCount+1);
        rawReply(last("wallet.orchard.listaccounts"),accountsFixture({}));
        QVERIFY(Find<QPushButton>(tab_,"orchardCreateAccount")->isEnabled());
        QCOMPARE(Find<QComboBox>(tab_,"orchardAccountSelector")->count(),0);
    }
    void missingSnapshotHistoryKeepsSetupDiscoverableUntilVerified() {
        tab_->setWalletScope("golf");tab_->setWalletUnlocked(true);
        rawReply(last("getblockchaininfo"),QJsonObject{{"chain","regtest"}});
        rawReply(last("wallet.orchard.getwalletbinding"),QJsonObject{
            {"wallet_name","golf"},{"wallet_binding",QString(64,'a')}});
        rawReply(last("orchard.getactivationstatus"),activationFixture());
        auto* setup=Find<QPushButton>(tab_,"orchardCreateAccount");
        QVERIFY(setup->isVisible());QVERIFY(!setup->isEnabled());
        rawReply(last("wallet.orchard.listaccounts"),
            QJsonObject{{"error","Authenticated Orchard replay source unavailable"}});
        QVERIFY(setup->isVisible());QVERIFY(!setup->isEnabled());
        QVERIFY(Find<QLabel>(tab_,"orchardAccountStatus")->text().contains("Waiting for verified Orchard history"));
        QMetaObject::invokeMethod(tab_,"onPoll");
        tab_->setWalletUnlocked(false);tab_->setWalletUnlocked(true);
        rawReply(last("wallet.orchard.listaccounts"),accountsFixture({}));
        QVERIFY(setup->isVisible());QVERIFY(setup->isEnabled());
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
        ready();const auto before=sent_.size();
        tab_->setTipHeight(21);  // a block arrived: read
        const auto first=sent_.size();QVERIFY(first>before);
        tab_->setTipHeight(22);  // another, with the replies still pending
        QCOMPARE(walletReads(first),0);
    }
    // Measured on a v8.2.5 desktop (2026-11-24): the page read the wallet four
    // times every three seconds, each read held the wallet's database lease on
    // the node, and the node's own delivery of new blocks waited behind them,
    // so the Orchard account fell further behind the chain while the page was
    // open. An account's data changes with a block: that is when it is read.
    void theWalletIsReadWhenABlockArrivesNotOnEveryTick() {
        ready();
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        rawReply(last("wallet.orchard.listreceived"),ReceivedPage(0,0,0));
        rawReply(last("orchard.getactivationstatus"),activationFixture());
        tab_->setTipHeight(20);
        // Bring every reply in, so nothing is pending and nothing is refused.
        reply(last("wallet.orchard.listoperations"),
              R"({"account":0,"account_revision":5,"account_sequence":3,"captured_source_sequence":3,"account_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","captured_source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","operations":[]})");
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        rawReply(last("orchard.getactivationstatus"),activationFixture());

        // Twenty ticks of the page timer (a minute of the old polling) with no
        // new block: not one read.
        int mark=sent_.size();
        for(int i=0;i<20;++i)QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(walletReads(mark),0);

        // A block: the account is read once.
        mark=sent_.size();tab_->setTipHeight(21);
        QStringList methods;for(int i=mark;i<sent_.size();++i)methods<<sent_[i].method;
        QVERIFY2(methods.contains("wallet.orchard.getbalance") && methods.contains("wallet.orchard.listoperations"),qPrintable(methods.join(",")));
        QCOMPARE(methods.count("wallet.orchard.getbalance"),1);
        // The same height reported again changes nothing.
        mark=sent_.size();tab_->setTipHeight(21);QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(walletReads(mark),0);
    }
    // A payment that was submitted is confirmed by a block. It is not asked
    // about on every tick while it waits.
    void aSubmittedPaymentIsFollowedByBlocksNotByTicks() {
        ready();fillSend();confirmNextDialog();Find<QPushButton>(tab_,"orchardReview")->click();
        reply(last("wallet.orchard.queuespend"),R"({"durable_state":"signed"})");
        reply(last("wallet.orchard.finishspend"),R"({"durable_state":"signed","txid":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","admitted":true})");
        QCOMPARE(*tab_->paymentState(),OrchardFlow::State::Submitted);
        tab_->setTipHeight(30);
        // The operations list answers (the payment is in it, unconfirmed), so no read is pending.
        const auto id=last("wallet.orchard.queuespend").params.value("request_id").toString();
        auto listed=storedSnapshot("wallet.orchard.finishspend");auto rows=listed["operations"].toArray();
        auto row=rows[0].toObject();row["operation_id"]=id;rows[0]=row;listed["operations"]=rows;
        rawReply(last("wallet.orchard.listoperations"),listed);
        QCOMPARE(*tab_->paymentState(),OrchardFlow::State::Submitted);
        int mark=sent_.size();
        for(int i=0;i<10;++i)QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(walletReads(mark),0);
        // The next block is what brings the next read (the balance's reply is in; the others are still pending).
        auto balance=balanceFixture();balance["account_revision"]=6;balance["account_sequence"]=4;balance["captured_source_sequence"]=4;
        rawReply(last("wallet.orchard.getbalance"),balance);
        mark=sent_.size();tab_->setTipHeight(31);
        QVERIFY(walletReads(mark)>0);
    }
    void accountDiscoveryIsNotRepeatedOnEveryPollOrUnchangedUnlock() {
        ready();rawReply(last("wallet.orchard.listaccounts"),accountsFixture());
        const auto catalogCalls=[&] {
            return std::count_if(sent_.begin(),sent_.end(),[](const Sent& call) {
                return call.method=="wallet.orchard.listaccounts";
            });
        };
        const auto before=catalogCalls();
        tab_->setWalletUnlocked(true);
        QMetaObject::invokeMethod(tab_,"onPoll");
        QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(catalogCalls(),before);
        tab_->setWalletUnlocked(false);tab_->setWalletUnlocked(true);
        QCOMPARE(catalogCalls(),before+1);
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
    void refusedStorageUpgradeWarnsAndDisablesPayment() {
        ready();fillSend();QVERIFY(Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        auto status=activationFixture();status["storage_upgrade_refused"]=true;
        status["storage_upgrade_reason"]="insufficient_disk";
        status["node_supported"]=false;
        status["node_notice"]="This full node cannot follow Orchard activation because its storage upgrade was refused. Free space and restart.";
        rawReply(last("orchard.getactivationstatus"),status);
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
        QVERIFY(Find<QLabel>(tab_,"orchardIntro")->text().contains("storage upgrade was refused"));
        QVERIFY(Find<QLabel>(tab_,"orchardIntro")->text().contains("Free space"));
    }
    void wrongAccountAndStaleBalancesChangeNothing() {
        ready();const auto call=last("wallet.orchard.getbalance");auto x=balanceFixture();x["account"]=1;rawReply(call,x);
        QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        x=balanceFixture();x["account_revision"]=4;rawReply(call,x);QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("Unknown"));
        x=balanceFixture();x["account_revision"]=6;rawReply(call,x);QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("0.00000000 DIN confirmed"));
        // An older reply arriving late neither replaces nor degrades the newer figure.
        rawReply(call,balanceFixture());QCOMPARE(Find<QLabel>(tab_,"orchardBalance")->text(),QString("0.00000000 DIN confirmed"));
    }
    // Owner, 2026-11-23: after a shield the page showed "25 DIN", then
    // "Unknown" while the wallet caught up with new blocks (a miner's wallet
    // is behind for a moment after every block). A balance that was read is
    // kept and shown with its block; only a balance never read is Unknown.
    void refusedReadKeepsTheLastBalanceWithItsBlock() {
        ready();
        QSignalSpy changed(tab_,&OrchardWidget::balanceChanged);
        auto x=balanceFixture();x["confirmed_una"]=qint64(2500000000);x["unreserved_confirmed_una"]=qint64(2500000000);
        rawReply(last("wallet.orchard.getbalance"),x);
        auto* label=Find<QLabel>(tab_,"orchardBalance");
        QCOMPARE(label->text(),QString("25.00000000 DIN confirmed"));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(changed.last().at(0).toBool(),true);QCOMPARE(changed.last().at(1).toULongLong(),quint64(2500000000));
        QCOMPARE(changed.last().at(2).toBool(),true);QCOMPARE(changed.last().at(3).toULongLong(),quint64(20));

        // The node refuses the next read (it is delivering a block).
        QMetaObject::invokeMethod(tab_,"onPoll");
        rawReply(last("wallet.orchard.getbalance"),QJsonObject{{"error","Orchard replay source unavailable"}});
        QCOMPARE(label->text(),QString::fromUtf8("25.00000000 DIN at block 20 — updating"));
        QVERIFY(label->toolTip().contains("block 20"));
        QCOMPARE(changed.last().at(0).toBool(),true);QCOMPARE(changed.last().at(2).toBool(),false);

        // It answers again, one block later.
        QMetaObject::invokeMethod(tab_,"onPoll");
        x["checkpoint_height"]=21;rawReply(last("wallet.orchard.getbalance"),x);
        QCOMPARE(label->text(),QString("25.00000000 DIN confirmed"));

        // Another wallet: the previous wallet's money is never shown for it.
        tab_->setWalletScope("bob");
        QCOMPARE(label->text(),QString("Unknown"));
        QCOMPARE(changed.last().at(0).toBool(),false);
    }
    // The Wallet page shows the Orchard balance. While it is on screen the
    // balance is read for it although the Orchard page is hidden: that one
    // read, slowly, and nothing else.
    void walletPageGetsTheBalanceWhileThisPageIsHidden() {
        ready();rawReply(last("wallet.orchard.getbalance"),balanceFixture());tab_->hide();
        int mark=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(sent_.size(),mark);
        tab_->setBalanceWanted(true);
        QStringList methods;for(int i=mark;i<sent_.size();++i)methods<<sent_[i].method;
        QCOMPARE(methods,QStringList{"wallet.orchard.getbalance"});
        rawReply(last("wallet.orchard.getbalance"),balanceFixture());
        mark=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");QMetaObject::invokeMethod(tab_,"onPoll");
        QCOMPARE(sent_.size(),mark);  // not again within the slow interval
        tab_->setBalanceWanted(false);
        mark=sent_.size();QMetaObject::invokeMethod(tab_,"onPoll");QCOMPARE(sent_.size(),mark);
    }
    // Owner, 2026-11-23: "when I press Your operations I should be able to
    // see details". Selecting a row shows what the node saved and asks the
    // chain for the transaction.
    void selectingAnOperationShowsItsDetails() {
        ready();
        auto snapshot=storedSnapshot("wallet.orchard.finishshield");
        auto rows=snapshot["operations"].toArray();auto row=rows[0].toObject();
        row["chain_observation"]=QJsonObject{{"height",18},{"outcome","confirmed"},{"block_hash",QString(64,'e')},{"transaction_id",QString(64,'f')}};
        rows[0]=row;snapshot["operations"]=rows;
        rawReply(last("wallet.orchard.listoperations"),snapshot);
        auto* table=Find<QTableWidget>(tab_,"orchardOperationsTable");auto* detail=Find<QLabel>(tab_,"orchardOperationDetail");
        QCOMPARE(table->rowCount(),1);
        QVERIFY(detail->text().contains("Select an operation"));
        table->selectRow(0);
        QVERIFY2(detail->text().contains("Kind: Shield"),qPrintable(detail->text()));
        QVERIFY(detail->text().contains("State: Confirmed in block 18"));
        QVERIFY(detail->text().contains("Operation: "+QString(64,'b')));
        QVERIFY(detail->text().contains("Transaction: "+QString(64,'f')));
        QVERIFY(detail->text().contains("reading"));
        const auto asked=last("gettransaction");
        QCOMPARE(asked.params.value("txid").toString(),QString(64,'f'));

        rawReply(asked,QJsonObject{{"txid",QString(64,'f')},{"status","confirmed"},{"blockheight",18},{"blockhash",QString(64,'e')},
            {"confirmations",3},{"orchard_value_direction","into_orchard"},{"orchard_value_din","25.00000000"},{"fee_din","0.00010000"},
            {"total_input_value_din","100.00155000"},{"input_count",1},{"total_output_value_din","75.00145000"},{"output_count",1},{"size",9360}});
        const QString text=detail->text();
        QVERIFY2(text.contains("Moved into Orchard: 25.00000000 DIN"),qPrintable(text));
        QVERIFY(text.contains("Network fee: 0.00010000 DIN"));
        QVERIFY(text.contains("Confirmations: 3"));
        QVERIFY(text.contains("Block: 18 ("+QString(64,'e')+")"));
        QVERIFY(text.contains("Public coins spent: 100.00155000 DIN (1 coin)"));
        QVERIFY(text.contains("Public coins created: 75.00145000 DIN (1 coin, your change)"));
        QVERIFY2(detail->minimumHeight()>=detail->fontMetrics().lineSpacing()*int(text.count('\n')+1),"every detail line has room");
        // A settled transaction is not asked for again when the list refreshes.
        const int mark=sent_.size();
        QMetaObject::invokeMethod(tab_,"onPoll");rawReply(last("wallet.orchard.listoperations"),snapshot);
        for(int i=mark;i<sent_.size();++i)QVERIFY(sent_[i].method!="gettransaction");
        QVERIFY(detail->text().contains("Moved into Orchard"));
    }
    void operationDetailsSayWhenTheNodeCannotLookTheTransactionUp() {
        ready();rawReply(last("wallet.orchard.listoperations"),storedSnapshot("wallet.orchard.finishspend"));
        Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
        auto* detail=Find<QLabel>(tab_,"orchardOperationDetail");
        QVERIFY(detail->text().contains("Kind: Payment from Orchard"));
        QVERIFY(detail->text().contains("State: Signed and sent, not confirmed yet"));
        rawReply(last("gettransaction"),QJsonObject{{"txid",QString(64,'f')},{"error","Transaction not found in blockchain"},{"status","not_found"}});
        QVERIFY2(detail->text().contains("could not find the transaction"),qPrintable(detail->text()));
    }
    void displayWordingAcrossPages() {
        using namespace OrchardDisplay;
        // Wallet page.
        auto w=walletPrivate(true,std::nullopt,0.0,false);
        QCOMPARE(w.orchardText,QString::fromUtf8("Reading…"));QVERIFY(!w.orchardKnown);QCOMPARE(w.countedDin,0.0);QVERIFY(!w.showRetired);
        w=walletPrivate(true,KnownBalance{2500000000ULL,165213,true,true},0.0,false);
        QCOMPARE(w.orchardText,QString("25.00000000 DIN"));QCOMPARE(w.countedDin,25.0);QVERIFY(w.orchardKnown);
        w=walletPrivate(true,KnownBalance{2500000000ULL,165213,true,false},3.5,true);
        QCOMPARE(w.orchardText,QString("25.00000000 DIN (block 165213)"));QCOMPARE(w.countedDin,25.0);
        QVERIFY(w.showRetired);QCOMPARE(w.retiredText,QString("3.50000000 DIN (retired, locked)"));
        w=walletPrivate(false,std::nullopt,0.0,false);QCOMPARE(w.orchardText,QString::fromUtf8("—"));
        QCOMPARE(walletPrivate(true,KnownBalance{123456789012345ULL,1,true,true},0,false).orchardText,QString("1,234,567.89012345 DIN"));
        // History page.
        QCOMPARE(historyFooter(200,200,"All"),QString("Showing the newest 200 transactions. Older ones exist: use Show older."));
        QCOMPARE(historyFooter(1,200,"Shield"),QString("Showing all 1 shield transactions."));
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
            const auto read=last("wallet.orchard.listoperations");const auto count=walletRequests();
            rawReply(read,storedSnapshot(method));QCOMPARE(walletRequests(),count); // discovery itself never submits
            QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());
            Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
            auto* resume=Find<QPushButton>(tab_,"orchardResume");QVERIFY(resume->isEnabled());
            confirmNextDialog();resume->click();const auto finish=last(method);
            QCOMPARE(walletRequests(),count+1);QCOMPARE(finish.params.size(),3);
            QCOMPARE(finish.params.value("account").toInteger(),qint64(0));QCOMPARE(finish.params.value("request_id").toString(),QString(64,'b'));
            QCOMPARE(finish.params.value("wallet_binding").toString(),QString(64,'a'));
            QVERIFY(!finish.params.contains("payments"));QVERIFY(!finish.params.contains("outputs"));QVERIFY(!finish.params.contains("fee_una"));
            QVERIFY(!resume->isEnabled());
        }
    }
    void legacyAndMalformedDiscoveryCannotEnableAnotherPayment() {
        ready();fillSend();const auto read=last("wallet.orchard.listoperations");const auto count=walletRequests();
        rawReply(read,storedSnapshot());Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());QVERIFY(!Find<QPushButton>(tab_,"orchardResume")->isEnabled());
        rawReply(read,storedSnapshot("sendtoaddress"));
        QVERIFY(!Find<QPushButton>(tab_,"orchardReview")->isEnabled());QVERIFY(!Find<QPushButton>(tab_,"orchardResume")->isEnabled());
        QMetaObject::invokeMethod(tab_,"onResumeSelected",Qt::DirectConnection);QCOMPARE(walletRequests(),count);
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

        // A confirmed shield, selected: its details and the kept balance.
        tab_->setWalletScope("alice");bind("alice");tab_->setWalletUnlocked(true);
        auto balance=balanceFixture();balance["confirmed_una"]=qint64(2500000000);balance["unreserved_confirmed_una"]=qint64(2500000000);
        rawReply(last("wallet.orchard.getbalance"),balance);
        auto snapshot=storedSnapshot("wallet.orchard.finishshield");
        auto rows=snapshot["operations"].toArray();auto row=rows[0].toObject();
        row["chain_observation"]=QJsonObject{{"height",18},{"outcome","confirmed"},{"block_hash",QString(64,'e')},{"transaction_id",QString(64,'f')}};
        rows[0]=row;snapshot["operations"]=rows;
        rawReply(last("wallet.orchard.listoperations"),snapshot);
        Find<QTableWidget>(tab_,"orchardOperationsTable")->selectRow(0);
        rawReply(last("gettransaction"),QJsonObject{{"txid",QString(64,'f')},{"status","confirmed"},{"blockheight",18},{"blockhash",QString(64,'e')},
            {"confirmations",280},{"orchard_value_direction","into_orchard"},{"orchard_value_din","25.00000000"},{"fee_din","0.00010000"},
            {"total_input_value_din","100.00155000"},{"input_count",1},{"total_output_value_din","75.00145000"},{"output_count",1},{"size",9360}});
        QMetaObject::invokeMethod(tab_,"onPoll");
        rawReply(last("wallet.orchard.getbalance"),QJsonObject{{"error","Orchard replay source unavailable"}});
        tab_->resize(1440, 1250);
        QApplication::processEvents();
        QVERIFY(tab_->grab().save(dir + "/orchard-operation-detail.png"));
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
