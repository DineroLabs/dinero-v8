#pragma once
#include "../../qt/tests/orchard_http_transport_adapter.h"
#include "daemon/http_rpc_server.h"
#include "daemon/rpc_auth.h"
#include <fstream>
#include <utility>
extern std::string g_data_dir;
// Adds an explicit happens-before edge before inspecting/mutating fixture state.
// No synchronization is disabled or removed.
struct HttpRpcDrainTestAccess {
    static void Wait(HttpRpcServer& server) {
        std::unique_lock<std::mutex> lock(server.connections_mutex_);
        if(!server.connections_drained_.wait_for(lock,std::chrono::seconds(15),[&]{return server.client_sockets_.empty();}))
            throw std::runtime_error("HTTP handlers did not drain");
    }
};
din::Json rpc_context_getblockchaininfo(const ExecutionContext&,const din::Json&);
namespace dinero {
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
namespace HttpTest=OrchardHttpTransportTest;
std::string HttpIsolation() {
    if(::testing::GTEST_FLAG(filter)!="OrchardHttpTransport.*")throw std::runtime_error("requires exact isolated test filter");
    return HttpTest::ValidateEnvironment();
}
// Keep even the server's unauthenticated discovery route inside this temporary
// datadir. Restore only after the server and all handlers have stopped.
struct IsolatedHttpDataDirectory {
    std::string previous;
    explicit IsolatedHttpDataDirectory(std::string directory) : previous(std::move(g_data_dir)) {
        g_data_dir=std::move(directory);
    }
    IsolatedHttpDataDirectory(const IsolatedHttpDataDirectory&)=delete;
    ~IsolatedHttpDataDirectory(){g_data_dir.swap(previous);}
};
struct OrchardHttpFixture {
    // Lifetime order: client destroyed, server drained/stopped, then caller's backend fixture.
    std::string home,datadir;
    IsolatedHttpDataDirectory legacy_data;
    std::shared_ptr<RpcAuth> auth;
    HttpRpcServer server;
    std::unique_ptr<HttpTest::Client> client;
    unsigned calls=0;
    static constexpr uint16_t port=39713;
    explicit OrchardHttpFixture(DaemonContext& context,const std::string& isolated)
        :home(isolated),datadir((std::filesystem::path(home)/
#ifdef __APPLE__
            "Library/Application Support/Dinero"
#else
            ".dinero"
#endif
          ).string()),legacy_data(datadir),auth(std::make_shared<RpcAuth>(datadir)),server("127.0.0.1",port) {
        if(HttpIsolation()!=home)throw std::runtime_error("changed isolation");
        RegisterOrchardAccountRpc();
        g_rpcRegistry.registerHandler("blockchain.getblockchaininfo",rpc_context_getblockchaininfo,RegisterMode::Overwrite,"http-test-production-handler");
        g_rpcRegistry.registerAlias("getblockchaininfo","blockchain.getblockchaininfo");
        if(!auth->generate_cookie())throw std::runtime_error("cannot create isolated authentication");
        std::ofstream info(std::filesystem::path(datadir)/"serverinfo.json",std::ios::trunc);
        info<<"{\"rpc\":{\"host\":\"127.0.0.1\",\"port\":"<<port<<",\"tls\":false}}";info.close();
        if(!info)throw std::runtime_error("cannot persist isolated discovery");
        server.set_auth(auth);server.set_dev_mode(false);server.set_rpc_registry(&g_rpcRegistry);server.set_daemon_context(&context);
        server.start(); // bind our listener first; collision fails, never probe another listener
        try {client=std::make_unique<HttpTest::Client>(home,datadir,port);} catch(...) {server.stop();throw;}
    }
    ~OrchardHttpFixture(){client.reset();server.stop();}
    din::Json Raw(const QtContract::Call& request,const std::string& binding="") {
        auto params=QtContractJson(request.params);if(!binding.empty())params["wallet_binding"]=binding;
        const auto tag="orchard-http-"+std::to_string(++calls);
        const auto result=QtContractJson(client->Call(request.method,params.toStyledString(),tag));
        HttpRpcDrainTestAccess::Wait(server);
        if(result["tag"].asString()!=tag)throw std::runtime_error("wrong actual RpcClient reply route");
        return result;
    }
    din::Json Read(const std::string& kind,const QtContract::Call& call,const std::string& binding,const std::string& context="") {
        const auto reply=Raw(call,binding);if(!reply["ok"].asBool())throw std::runtime_error("actual HTTP call failed: "+reply.toStyledString());
        return QtContractParse(kind,reply["value"],context);
    }
    std::string Binding(const std::string& wallet="canonical-recovery") {
        auto p=din::obj();p["wallet_name"]=wallet;const auto value=Read("binding",{"wallet.orchard.getwalletbinding",p.toStyledString()},"",wallet);
        if(!value["accepted"].asBool())throw std::runtime_error("actual HTTP binding rejected by Qt");return value["binding"].asString();
    }
};
}
TEST(OrchardHttpTransport, ActualWidgetReadsAndStaleBindingEnvelope) {
    const auto home=HttpIsolation();OrchardFinishRpcFixture f;OrchardHttpFixture http(f.context,home);
    const auto binding=http.Binding();const auto before=f.Snapshot();
    const auto widget=QtContractJson(http.client->ReadWidget("canonical-recovery"));HttpRpcDrainTestAccess::Wait(http.server);
    ASSERT_EQ(widget["accounts"].size(),2u);EXPECT_EQ(widget["accounts"][0].asUInt64(),3u);EXPECT_EQ(widget["accounts"][1].asUInt64(),17u);
    EXPECT_EQ(widget["history_rows"].asUInt64(),1u);EXPECT_EQ(widget["history_amount"].asString(),"0.00500000");
    EXPECT_EQ(widget["balance"].asString(),"0.00500000 DIN confirmed");
    EXPECT_EQ(widget["history_type"].asString(),"Incoming");EXPECT_TRUE(widget["receive_enabled"].asBool());
    EXPECT_EQ(widget["activation"].asString(),"Orchard rules are active. Wallet synchronization is still required.");EXPECT_FALSE(widget["payment_present"].asBool());EXPECT_EQ(widget["pending"].asUInt64(),0u);EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    for(const auto& method:std::vector<std::string>{"wallet.orchard.getbalance","orchard.getbalance"}) {
        const auto stale=http.Raw(QtContract::Account(method,3),binding);EXPECT_FALSE(stale["ok"].asBool());
        EXPECT_EQ(stale["data"]["orchard"]["error_code"].asString(),"wallet_binding_mismatch");EXPECT_EQ(f.Snapshot(),before);
    }
    const auto fresh=http.Binding();EXPECT_NE(fresh,binding);
    const auto balance=http.Read("balance",QtContract::Account("wallet.orchard.getbalance",3),fresh);
    EXPECT_TRUE(balance["accepted"].asBool());EXPECT_EQ(balance["confirmed"].asUInt64(),500000u);
}
namespace {
void ExerciseHttpSpend(bool withdraw) {
    const auto home=HttpIsolation();OrchardFinishRpcFixture f;OrchardHttpFixture http(f.context,home);const auto binding=http.Binding();
    const auto operation=f.Operation(1);const auto id=util::hex(std::vector<unsigned char>(operation.begin(),operation.end()));
    const auto address=withdraw?f.TransparentAddress():f.AccountKeys(17).Receiver(orchard::WalletScope::External,{}).EncodeAddress(orchard::WalletNetwork::Regtest);
    const auto request=QtContract::Payment(false,withdraw,3,f.Account(3).revision,id,address,withdraw?400000:200000,100000);
    const auto queued=http.Read("queue",request,binding);ASSERT_TRUE(queued["accepted"].asBool());EXPECT_EQ(queued["id"].asString(),id);
    const auto reserved=f.Snapshot();const auto retry=http.Read("queue",request,binding);ASSERT_TRUE(retry["accepted"].asBool());EXPECT_TRUE(retry["existing"].asBool());EXPECT_EQ(f.Snapshot(),reserved);
    f.CompleteProof();const auto finish=QtContract::Finish(false,3,id);ASSERT_EQ(QtContractJson(finish.params).size(),2u);
    const auto submitted=http.Read("finish",finish,binding);ASSERT_TRUE(submitted["accepted"].asBool());EXPECT_TRUE(submitted["admitted"].asBool());EXPECT_EQ(f.broadcasts,1u);
    const auto stored=f.Snapshot();const auto repeated=http.Read("finish",finish,binding);ASSERT_TRUE(repeated["accepted"].asBool());EXPECT_TRUE(repeated["already"].asBool());EXPECT_EQ(repeated["txid"],submitted["txid"]);EXPECT_EQ(f.Snapshot(),stored);EXPECT_EQ(f.broadcasts,1u);
    f.MineEmpty();ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto list=QtContract::Account("wallet.orchard.listoperations",3);const auto confirmed=http.Read("operations",list,binding);
    ASSERT_TRUE(confirmed["accepted"].asBool());ASSERT_EQ(confirmed["rows"].size(),1u);EXPECT_EQ(confirmed["rows"][0]["outcome"].asString(),"confirmed");
    EXPECT_EQ(confirmed["rows"][0]["txid"],submitted["txid"]);
    const auto balance=http.Read("balance",QtContract::Account("wallet.orchard.getbalance",3),binding);ASSERT_TRUE(balance["accepted"].asBool());EXPECT_EQ(balance["confirmed"].asUInt64(),withdraw?0u:200000u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone=http.Read("operations",list,binding);ASSERT_TRUE(undone["accepted"].asBool());EXPECT_TRUE(undone["rows"][0]["outcome"].asString().empty());
}
}
namespace {
// A separate normal fixture for widget-generated random request IDs. The older
// fixed-ID fixtures and all their assertions remain unchanged.
struct HttpWidgetSpendFixture : OrchardSpendRpcFixture {
    unsigned broadcasts=0;bool committed_ready=false;
    std::vector<uint8_t> broadcast_body;
    HttpWidgetSpendFixture(){
        context.tx_ingress=f.ingress.get();
        f.ingress->mempool().setTxBroadcastCallback([this](const uint256& txid){
            ++broadcasts;const auto account=Account(3);
            const auto& entries=account.account.Operations().Entries();
            OrchardAdmissionFixture::Require(entries.size()==1);
            const auto pooled=f.ingress->mempool().getMempoolEntry(txid);
            OrchardAdmissionFixture::Require(bool(pooled));broadcast_body=pooled->tx.Serialize();
            const auto& entry=entries.begin()->second;
            committed_ready=sqlite3_get_autocommit(Database()) &&
                entry.phase==wallet::OrchardOperationQueue::Phase::Ready && entry.transaction==broadcast_body;
        });
    }
    ~HttpWidgetSpendFixture(){f.ingress->mempool().setTxBroadcastCallback({});context.tx_ingress=nullptr;}
};
void ExpectOneWidgetRequest(const din::Json& snapshot,const std::string& id) {
    unsigned queues=0;
    for(const auto& call:snapshot["calls"]){const auto method=call["method"].asString();
        if(method=="wallet.orchard.queuespend"){
            ++queues;EXPECT_EQ(call["params"]["request_id"].asString(),id);
        }
        if(method=="wallet.orchard.finishspend"){
            EXPECT_EQ(call["params"]["request_id"].asString(),id);
            EXPECT_EQ(call["params"]["account"].asUInt64(),3u);
        }
    }
    EXPECT_EQ(queues,1u);EXPECT_EQ(snapshot["pending"].asUInt64(),0u);
}
unsigned WidgetFinishCalls(const din::Json& snapshot) {
    unsigned count=0;
    for(const auto& call:snapshot["calls"]){const auto method=call["method"].asString();
        if(method=="wallet.orchard.finishspend" || method=="wallet.orchard.finishshield")++count;
    }
    return count;
}
void ExerciseHttpWidgetSpend(bool withdraw) {
    const auto home=HttpIsolation();HttpWidgetSpendFixture f;OrchardHttpFixture http(f.context,home);
    const auto address=withdraw?f.TransparentAddress():f.AccountKeys(17).Receiver(orchard::WalletScope::External,{}).EncodeAddress(orchard::WalletNetwork::Regtest);
    const auto started=QtContractJson(http.client->BeginWidgetPayment("canonical-recovery",withdraw?"unshield":"send",address,withdraw?400000:200000,100000));
    HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_NE(started["state"].asString(),"Confirmed");
    const auto initial=f.Account(3);ASSERT_EQ(initial.account.Operations().Entries().size(),1u);
    const auto operation=initial.account.Operations().Entries().begin()->first;
    const auto id=util::hex(std::vector<uint8_t>(operation.begin(),operation.end()));
    const auto submitted=QtContractJson(http.client->WaitWidgetPayment("submitted"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_EQ(submitted["label"].asString(),"Submitted — waiting for a block");ExpectOneWidgetRequest(submitted,id);
    EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.committed_ready);ASSERT_FALSE(f.broadcast_body.empty());
    f.MineEmpty();ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed=QtContractJson(http.client->WaitWidgetPayment("confirmed"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_EQ(confirmed["label"].asString(),"Confirmed");ExpectOneWidgetRequest(confirmed,id);EXPECT_EQ(f.broadcasts,1u);
    EXPECT_EQ(confirmed["balance"].asString(),withdraw?"0.00000000 DIN confirmed":"0.00200000 DIN confirmed");
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone=QtContractJson(http.client->WaitWidgetPayment("unconfirmed"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_NE(undone["label"].asString(),"Confirmed");ExpectOneWidgetRequest(undone,id);EXPECT_EQ(f.broadcasts,1u);
    // Reads revoke confirmation without granting another submission.
    EXPECT_TRUE(undone["retry_available"].asBool());
    EXPECT_EQ(WidgetFinishCalls(undone),WidgetFinishCalls(confirmed));
    const auto retained=f.Account(3).account.Operations().Entries().at(operation).transaction;
    const auto originalBody=f.broadcast_body;
    EXPECT_EQ(retained,originalBody);
    const auto retried=QtContractJson(http.client->RetryWidgetPayment());HttpRpcDrainTestAccess::Wait(http.server);
    ExpectOneWidgetRequest(retried,id);EXPECT_EQ(f.broadcasts,2u);
    EXPECT_EQ(WidgetFinishCalls(retried),WidgetFinishCalls(undone)+1);
    EXPECT_EQ(f.broadcast_body,originalBody);
    const auto afterRetry=f.Account(3);ASSERT_EQ(afterRetry.account.Operations().Entries().size(),1u);
    EXPECT_EQ(afterRetry.account.Operations().Entries().at(operation).transaction,originalBody);
}
}
TEST(OrchardHttpTransport, TransferOverAuthenticatedHttp) {
    ASSERT_NO_FATAL_FAILURE(ExerciseHttpSpend(false));
    ASSERT_NO_FATAL_FAILURE(ExerciseHttpWidgetSpend(false));
}
TEST(OrchardHttpTransport, UnshieldOverAuthenticatedHttp) {
    ASSERT_NO_FATAL_FAILURE(ExerciseHttpSpend(true));
    ASSERT_NO_FATAL_FAILURE(ExerciseHttpWidgetSpend(true));
}

namespace {
struct HttpWidgetShieldFixture : ShieldReservationFixture {
    unsigned broadcasts=0;bool ready_at_broadcast=false,autocommit_at_broadcast=false;
    std::vector<uint8_t> broadcast_body;
    HttpWidgetShieldFixture(){
        context.tx_ingress=f.ingress.get();
        f.ingress->mempool().setTxBroadcastCallback([this](const uint256& txid){
            ++broadcasts;autocommit_at_broadcast=sqlite3_get_autocommit(wallet->get().getCurrentDatabase());
            const auto current=Account(3);const auto& entries=current.account.Operations().Entries();
            Need(entries.size()==1);
            const auto pooled=f.ingress->mempool().getMempoolEntry(txid);Need(bool(pooled));
            broadcast_body=pooled->tx.Serialize();const auto& entry=entries.begin()->second;
            ready_at_broadcast=entry.phase==wallet::OrchardOperationQueue::Phase::Ready &&
                entry.transaction==broadcast_body && entry.shield_request.has_value();
        });
    }
    ~HttpWidgetShieldFixture(){f.ingress->mempool().setTxBroadcastCallback({});context.tx_ingress=nullptr;}
};
void ExpectOneWidgetShieldRequest(const din::Json& snapshot,const std::string& id) {
    unsigned queues=0,finishes=0;
    for(const auto& call:snapshot["calls"]){const auto method=call["method"].asString();
        if(method=="wallet.orchard.queueshield"){
            ++queues;EXPECT_EQ(call["params"]["request_id"].asString(),id);
            EXPECT_EQ(call["params"]["account"].asUInt64(),3u);
            EXPECT_EQ(call["params"]["fee_una"].asUInt64(),10000u);
            ASSERT_EQ(call["params"]["payments"].size(),1u);
            EXPECT_EQ(call["params"]["payments"][0]["amount_una"].asUInt64(),20000u);
        }
        if(method=="wallet.orchard.finishshield"){
            ++finishes;EXPECT_EQ(call["params"]["request_id"].asString(),id);
            EXPECT_EQ(call["params"]["account"].asUInt64(),3u);
            EXPECT_FALSE(call["params"].isMember("payments"));
            EXPECT_FALSE(call["params"].isMember("fee_una"));
        }
        EXPECT_NE(method,"wallet.orchard.queuespend");EXPECT_NE(method,"wallet.orchard.finishspend");
    }
    EXPECT_EQ(queues,1u);EXPECT_GE(finishes,1u);EXPECT_EQ(snapshot["pending"].asUInt64(),0u);
}
void ExerciseHttpWidgetShield() {
    const auto home=HttpIsolation();HttpWidgetShieldFixture f;OrchardHttpFixture http(f.context,home);
    const auto address=f.Payments()[0].recipient.EncodeAddress(orchard::WalletNetwork::Regtest);
    const auto started=QtContractJson(http.client->BeginWidgetPayment(f.execution.walletName,"shield",address,20000,10000));
    HttpRpcDrainTestAccess::Wait(http.server);EXPECT_NE(started["state"].asString(),"Confirmed");
    const auto initial=f.Account(3);ASSERT_EQ(initial.account.Operations().Entries().size(),1u);
    const auto operation=initial.account.Operations().Entries().begin()->first;
    const auto id=util::hex(std::vector<uint8_t>(operation.begin(),operation.end()));
    const auto submitted=QtContractJson(http.client->WaitWidgetPayment("submitted"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_EQ(submitted["label"].asString(),"Submitted — waiting for a block");ExpectOneWidgetShieldRequest(submitted,id);
    EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);
    ASSERT_FALSE(f.broadcast_body.empty());
    ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(f.broadcast_body))));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed=QtContractJson(http.client->WaitWidgetPayment("confirmed"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_EQ(confirmed["label"].asString(),"Confirmed");ExpectOneWidgetShieldRequest(confirmed,id);
    EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    const auto binding=http.Binding(f.execution.walletName);
    const auto history=http.Read("received",QtContract::Received(17),binding);
    ASSERT_TRUE(history["accepted"].asBool());ASSERT_EQ(history["rows"].size(),1u);
    EXPECT_EQ(history["rows"][0]["amount"].asUInt64(),20000u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone=QtContractJson(http.client->WaitWidgetPayment("unconfirmed"));HttpRpcDrainTestAccess::Wait(http.server);
    EXPECT_NE(undone["label"].asString(),"Confirmed");ExpectOneWidgetShieldRequest(undone,id);
    EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),0u);
    EXPECT_TRUE(undone["retry_available"].asBool());
    EXPECT_EQ(WidgetFinishCalls(undone),WidgetFinishCalls(confirmed));
    const auto retained=f.Account(3).account.Operations().Entries().at(operation).transaction;
    const auto originalBody=f.broadcast_body;
    EXPECT_EQ(retained,originalBody);
    const auto retried=QtContractJson(http.client->RetryWidgetPayment());HttpRpcDrainTestAccess::Wait(http.server);
    ExpectOneWidgetShieldRequest(retried,id);EXPECT_EQ(f.broadcasts,2u);
    EXPECT_EQ(WidgetFinishCalls(retried),WidgetFinishCalls(undone)+1);
    EXPECT_EQ(f.broadcast_body,originalBody);
    const auto afterRetry=f.Account(3);ASSERT_EQ(afterRetry.account.Operations().Entries().size(),1u);
    EXPECT_EQ(afterRetry.account.Operations().Entries().at(operation).transaction,originalBody);
}
}
TEST(OrchardHttpTransport, ShieldStoredIdFinishAndIncomingReceipt) {
    {

    const auto home=HttpIsolation();ShieldRpcFixture f;OrchardHttpFixture http(f.context,home);const auto binding=http.Binding(f.execution.walletName);
    const auto original=f.Request();const auto id=original["request_id"].asString();
    const auto request=QtContract::Payment(true,false,3,f.Account(3).revision,id,original["payments"][0]["address"].asString(),20000,10000);
    const auto queued=http.Read("queue",request,binding);ASSERT_TRUE(queued["accepted"].asBool());EXPECT_EQ(queued["id"].asString(),id);
    f.Complete();const auto finish=QtContract::Finish(true,3,id);ASSERT_EQ(QtContractJson(finish.params).size(),2u);
    const auto submitted=http.Read("finish",finish,binding);ASSERT_TRUE(submitted["accepted"].asBool());EXPECT_TRUE(submitted["admitted"].asBool());EXPECT_EQ(f.broadcasts,1u);
    const auto stored=f.Snapshot();const auto again=http.Read("finish",finish,binding);ASSERT_TRUE(again["accepted"].asBool());EXPECT_TRUE(again["already"].asBool());EXPECT_EQ(again["txid"],submitted["txid"]);EXPECT_EQ(f.Snapshot(),stored);EXPECT_EQ(f.broadcasts,1u);
    const auto body=f.Account(3).account.Operations().Entries().at(orchard::Hash{81}).transaction;
    ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(body))));ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto history=http.Read("received",QtContract::Received(17),binding);ASSERT_TRUE(history["accepted"].asBool());ASSERT_EQ(history["rows"].size(),1u);
    EXPECT_EQ(history["rows"][0]["amount"].asUInt64(),20000u);EXPECT_EQ(history["rows"][0]["txid"],submitted["txid"]);
    }
    ASSERT_NO_FATAL_FAILURE(ExerciseHttpWidgetShield());
}
#endif
}
