#pragma once
#include "../../qt/tests/orchard_backend_contract_adapter.h"
#include <sstream>
namespace dinero {
namespace {
namespace QtContract = OrchardBackendContractTest;
din::Json QtContractJson(const std::string& wire) {
    Json::CharReaderBuilder builder;din::Json value;std::string errors;std::istringstream stream(wire);
    if(!Json::parseFromStream(builder,stream,&value,&errors))throw std::runtime_error("Qt contract JSON parse failed: "+errors);
    return value;
}
din::Json QtContractParse(const std::string& kind,const din::Json& value,const std::string& context="") {
    return QtContractJson(QtContract::Parse(kind,value.toStyledString(),context));
}
din::Json QtContractCall(const ExecutionContext& ctx,const QtContract::Call& request,const std::string& binding) {
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup(request.method);
    if(!method)throw std::runtime_error("Qt contract method missing: "+request.method);
    auto params=QtContractJson(request.params);params["wallet_binding"]=binding;return (*method)(ctx,params);
}
std::string QtContractBinding(const ExecutionContext& ctx) {
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.getwalletbinding");
    if(!method)throw std::runtime_error("Qt binding method missing");
    auto params=din::obj();params["wallet_name"]=ctx.walletName;
    const auto parsed=QtContractParse("binding",(*method)(ctx,params),ctx.walletName);
    if(!parsed["accepted"].asBool())throw std::runtime_error("Qt rejected actual wallet binding");
    return parsed["binding"].asString();
}
}
TEST(OrchardQtContract, MissingBackendAndOwnerRefuseThroughActualParsers) {
    ExecutionContext ctx;const std::string binding(64,'1');
    const std::vector<std::pair<std::string,QtContract::Call>> reads{
        {"balance",QtContract::Account("wallet.orchard.getbalance",3)},
        {"operations",QtContract::Account("wallet.orchard.listoperations",3)},
        {"received",QtContract::Received(3)},
        {"accounts",{"wallet.orchard.listaccounts","{}"}}};
    for(const auto& [parser,call]:reads) {
        const auto reply=QtContractCall(ctx,call,binding);ASSERT_TRUE(reply.isMember("error"));
        EXPECT_FALSE(QtContractParse(parser,reply)["accepted"].asBool());
    }
    const auto* activation=g_rpcRegistry.lookup("orchard.getactivationstatus");ASSERT_NE(activation,nullptr);
    EXPECT_FALSE(QtContractParse("activation",(*activation)(ctx,din::obj()),"regtest")["accepted"].asBool());
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardQtContract, ActualCatalogBalanceHistoryAndStaleWalletBinding) {
    OrchardFinishRpcFixture f;const auto ctx=f.RequestContext();const auto binding=QtContractBinding(ctx);const auto before=f.Snapshot();
    const auto call=[&](const std::string& kind,const QtContract::Call& request){return QtContractParse(kind,QtContractCall(ctx,request,binding));};
    const auto accounts=call("accounts",{"wallet.orchard.listaccounts","{}"});ASSERT_TRUE(accounts["accepted"].asBool());
    ASSERT_EQ(accounts["ids"].size(),2u);EXPECT_EQ(accounts["ids"][0].asUInt64(),3u);EXPECT_EQ(accounts["ids"][1].asUInt64(),17u);
    const auto balance=call("balance",QtContract::Account("wallet.orchard.getbalance",3));ASSERT_TRUE(balance["accepted"].asBool());
    EXPECT_EQ(balance["confirmed"].asUInt64(),500000u);EXPECT_EQ(balance["reserved"].asUInt64(),0u);EXPECT_TRUE(balance["caught_up"].asBool());
    const auto history=call("received",QtContract::Received(3));ASSERT_TRUE(history["accepted"].asBool());ASSERT_EQ(history["rows"].size(),1u);
    EXPECT_EQ(history["rows"][0]["amount"].asUInt64(),500000u);EXPECT_EQ(history["rows"][0]["scope"].asString(),"external");
    const auto activation=QtContractParse("activation",ReadActivation(f.context),"regtest");ASSERT_TRUE(activation["accepted"].asBool());EXPECT_TRUE(activation["active"].asBool());
    EXPECT_FALSE(QtContractParse("activation",ReadActivation(f.context),"mainnet")["accepted"].asBool());EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto stale=QtContractCall(f.RequestContext(),QtContract::Received(3),binding);
    EXPECT_EQ(stale["error_code"].asString(),"wallet_binding_mismatch");EXPECT_FALSE(QtContractParse("received",stale)["accepted"].asBool());
    const auto fresh=QtContractBinding(f.RequestContext());EXPECT_NE(fresh,binding);
    EXPECT_EQ(QtContractParse("received",QtContractCall(f.RequestContext(),QtContract::Received(3),fresh)),history);EXPECT_EQ(f.Snapshot(),before);
}
namespace {
void ExerciseQtContractSpend(bool withdraw) {
    OrchardFinishRpcFixture f;const auto ctx=f.RequestContext();const auto binding=QtContractBinding(ctx);
    const auto operation=f.Operation(1);const auto id=util::hex(std::vector<unsigned char>(operation.begin(),operation.end()));const auto address=withdraw?f.TransparentAddress():f.AccountKeys(17).Receiver(orchard::WalletScope::External,{}).EncodeAddress(orchard::WalletNetwork::Regtest);
    const auto request=QtContract::Payment(false,withdraw,3,f.Account(3).revision,id,address,withdraw?400000:200000,100000);
    const auto queued=QtContractParse("queue",QtContractCall(ctx,request,binding));ASSERT_TRUE(queued["accepted"].asBool());EXPECT_EQ(queued["id"].asString(),id);EXPECT_TRUE(queued["queued"].asBool());
    const auto reserved=f.Snapshot();const auto retry=QtContractParse("queue",QtContractCall(ctx,request,binding));ASSERT_TRUE(retry["accepted"].asBool());EXPECT_TRUE(retry["existing"].asBool());EXPECT_EQ(f.Snapshot(),reserved);
    const auto list=QtContract::Account("wallet.orchard.listoperations",3);
    const auto pending=QtContractParse("operations",QtContractCall(ctx,list,binding));ASSERT_TRUE(pending["accepted"].asBool());ASSERT_EQ(pending["rows"].size(),1u);
    EXPECT_EQ(pending["rows"][0]["completion"].asString(),"wallet.orchard.finishspend");EXPECT_TRUE(pending["rows"][0]["outcome"].asString().empty());
    f.CompleteProof();const auto finish=QtContract::Finish(false,3,id);EXPECT_EQ(QtContractJson(finish.params).size(),2u);
    const auto submitted=QtContractParse("finish",QtContractCall(ctx,finish,binding));ASSERT_TRUE(submitted["accepted"].asBool());EXPECT_TRUE(submitted["admitted"].asBool());EXPECT_EQ(f.broadcasts,1u);
    const auto ready=f.Snapshot();const auto repeated=QtContractParse("finish",QtContractCall(ctx,finish,binding));ASSERT_TRUE(repeated["accepted"].asBool());EXPECT_TRUE(repeated["already"].asBool());EXPECT_EQ(repeated["txid"],submitted["txid"]);EXPECT_EQ(f.Snapshot(),ready);EXPECT_EQ(f.broadcasts,1u);
    f.MineEmpty();const auto lag=QtContractParse("operations",QtContractCall(ctx,list,binding));ASSERT_TRUE(lag["accepted"].asBool());EXPECT_TRUE(lag["syncing"].asBool());EXPECT_TRUE(lag["rows"][0]["outcome"].asString().empty());
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed=QtContractParse("operations",QtContractCall(ctx,list,binding));ASSERT_TRUE(confirmed["accepted"].asBool());EXPECT_EQ(confirmed["rows"][0]["outcome"].asString(),"confirmed");EXPECT_EQ(confirmed["rows"][0]["txid"],submitted["txid"]);
    const auto balance=QtContractParse("balance",QtContractCall(ctx,QtContract::Account("wallet.orchard.getbalance",3),binding));ASSERT_TRUE(balance["accepted"].asBool());EXPECT_EQ(balance["confirmed"].asUInt64(),withdraw?0u:200000u);
    const auto history=QtContractParse("received",QtContractCall(ctx,QtContract::Received(3),binding));ASSERT_TRUE(history["accepted"].asBool());EXPECT_EQ(history["total"].asUInt64(),withdraw?1u:2u);EXPECT_EQ(history["rows"][0]["amount"].asUInt64(),500000u);
    if(!withdraw){EXPECT_EQ(history["rows"][1]["scope"].asString(),"internal");EXPECT_EQ(history["rows"][1]["amount"].asUInt64(),200000u);}
    else {const auto coin=f.f.db.getCoin(uint256::FromHexUnsafe(submitted["txid"].asString()),0);ASSERT_TRUE(coin.ok());EXPECT_EQ(coin->amount,400000u);}
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone=QtContractParse("operations",QtContractCall(ctx,list,binding));ASSERT_TRUE(undone["accepted"].asBool());EXPECT_TRUE(undone["rows"][0]["outcome"].asString().empty());
    const auto after_undo=QtContractParse("received",QtContractCall(ctx,QtContract::Received(3),binding));ASSERT_TRUE(after_undo["accepted"].asBool());EXPECT_EQ(after_undo["total"].asUInt64(),1u);
}
}
TEST(OrchardQtContract, TransferRequestsAdmissionHistoryAndReorg) { ASSERT_NO_FATAL_FAILURE(ExerciseQtContractSpend(false)); }
TEST(OrchardQtContract, UnshieldRequestsAdmissionHistoryAndReorg) { ASSERT_NO_FATAL_FAILURE(ExerciseQtContractSpend(true)); }
TEST(OrchardQtContract, ShieldRequestStoredIdFinishAndIncomingReceipt) {
    ShieldRpcFixture f;const auto binding=QtContractBinding(f.execution);const auto original=f.Request();
    const auto id=original["request_id"].asString();const auto request=QtContract::Payment(true,false,3,f.Account(3).revision,id,original["payments"][0]["address"].asString(),20000,10000);
    EXPECT_FALSE(QtContractJson(request.params).isMember("outputs"));const auto queued=QtContractParse("queue",QtContractCall(f.execution,request,binding));ASSERT_TRUE(queued["accepted"].asBool());EXPECT_EQ(queued["id"].asString(),id);
    f.Complete();const auto finish=QtContract::Finish(true,3,id);EXPECT_EQ(QtContractJson(finish.params).size(),2u);
    const auto submitted=QtContractParse("finish",QtContractCall(f.execution,finish,binding));ASSERT_TRUE(submitted["accepted"].asBool());EXPECT_TRUE(submitted["admitted"].asBool());EXPECT_EQ(f.broadcasts,1u);
    const auto stored=f.Snapshot();const auto retry=QtContractParse("finish",QtContractCall(f.execution,finish,binding));ASSERT_TRUE(retry["accepted"].asBool());EXPECT_TRUE(retry["already"].asBool());EXPECT_EQ(retry["txid"],submitted["txid"]);EXPECT_EQ(f.Snapshot(),stored);EXPECT_EQ(f.broadcasts,1u);
    const auto body=f.Account(3).account.Operations().Entries().at(orchard::Hash{81}).transaction;ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(body))));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto history=QtContractParse("received",QtContractCall(f.execution,QtContract::Received(17),binding));ASSERT_TRUE(history["accepted"].asBool());ASSERT_EQ(history["rows"].size(),1u);EXPECT_EQ(history["rows"][0]["amount"].asUInt64(),20000u);EXPECT_EQ(history["rows"][0]["scope"].asString(),"external");EXPECT_EQ(history["rows"][0]["txid"],submitted["txid"]);
}
#endif
}
