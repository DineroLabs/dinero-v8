#pragma once
#include <array>
#include <limits>
#include "rpc/orchard_account_rpc.h"
#include "wallet/address.h"
#include "wallet/p2mr_address.h"
namespace dinero {
namespace {
din::Json SpendRpcShape(){
    din::Json p;p["account"]=Json::UInt64(3);p["expected_revision"]=Json::UInt64(1);
    p["request_id"]="01"+std::string(62,'0');p["fee_una"]=Json::UInt64(100000);
    p["payments"]=din::arr();p["outputs"]=din::arr();din::Json recipient;
    recipient["address"]="address";recipient["amount_una"]=Json::UInt64(200000);p["payments"].append(recipient);return p;
}
void SpendRpcRefused(const din::Json& result){EXPECT_TRUE(result.isMember("error"));EXPECT_EQ(result.size(),1u);}
}
TEST(OrchardSpendRpc, MalformedRequestsRefuseBeforeServices){
    // Healthy Base58 payload coverage also runs with the backend disabled.

    for(size_t size:{size_t(21),size_t(33),size_t(78)})for(size_t zeros:{size_t(0),size_t(1),size_t(3)}){
        std::vector<uint8_t> payload(size);for(size_t i=zeros;i<size;++i)payload[i]=uint8_t(17+i*37);
        const auto address=Address::encodeBase58Check(payload);ASSERT_FALSE(address.empty());
        std::vector<uint8_t> decoded;ASSERT_TRUE(Address::decodeBase58Check(address,decoded));EXPECT_EQ(decoded,payload);
    }
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(Address::decodeBase58Check("1111111111111111111114oLvT2",decoded));EXPECT_EQ(decoded,std::vector<uint8_t>(21,0));
    ASSERT_TRUE(Address::decodeBase58Check("1BoatSLRHtKNngkdXEeobR76b53LETtpyT",decoded));
    EXPECT_EQ(util::hex(decoded),"007680adec8eabcabac676be9e83854ade0bd22cdb");
    EXPECT_FALSE(Address::decodeBase58Check("not an address",decoded));

    ExecutionContext context;std::vector<din::Json> invalid{din::arr(),din::Json{},din::Json("request")};
    for(const auto& field:{"account","expected_revision","fee_una"}){
        for(const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("1")}){auto p=SpendRpcShape();p[field]=value;invalid.push_back(p);}
    }
    auto p=SpendRpcShape();p["account"]=Json::UInt64(0x80000000ULL);invalid.push_back(p);
    p=SpendRpcShape();p["expected_revision"]=Json::UInt64(0);invalid.push_back(p);
    for(const std::string& id:std::vector<std::string>{"",std::string(64,'0'),std::string(64,'g'),std::string(63,'a'),std::string(65,'a'),std::string(63,'a')+'\0'}){p=SpendRpcShape();p["request_id"]=id;invalid.push_back(p);}
    p=SpendRpcShape();p["extra"]=true;invalid.push_back(p);p=SpendRpcShape();p.removeMember("outputs");invalid.push_back(p);
    p=SpendRpcShape();p["payments"]=din::arr();invalid.push_back(p);p=SpendRpcShape();p["outputs"]=din::Json("array");invalid.push_back(p);
    p=SpendRpcShape();while(p["payments"].size()<9)p["payments"].append(p["payments"][0]);invalid.push_back(p);
    p=SpendRpcShape();while(p["outputs"].size()<1025)p["outputs"].append(p["payments"][0]);invalid.push_back(p);
    for(const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0),din::Json(1.0),din::Json(false),din::Json("200000")}){p=SpendRpcShape();p["payments"][0]["amount_una"]=value;invalid.push_back(p);}
    for(const auto& memo:std::vector<din::Json>{din::Json(1),din::Json("g0"),din::Json("0"),din::Json(std::string(1026,'0')),din::Json(std::string(1,'\0'))}){p=SpendRpcShape();p["payments"][0]["memo_hex"]=memo;invalid.push_back(p);}
    for(const auto& address:std::vector<din::Json>{din::Json(1),din::Json(""),din::Json(std::string(257,'a')),din::Json(std::string("abc\0def",7))}){p=SpendRpcShape();p["payments"][0]["address"]=address;invalid.push_back(p);}
    p=SpendRpcShape();p["payments"][0]["unknown"]=1;invalid.push_back(p);
    p=SpendRpcShape();p["outputs"].append(p["payments"][0]);p["outputs"][0]["memo_hex"]="00";invalid.push_back(p);
    for(const auto& request:invalid){const auto result=rpc_context_wallet_orchard_queuespend(context,request);SpendRpcRefused(result);
        EXPECT_NE(result["error"].asString(),"Daemon services unavailable");EXPECT_NE(result["error"].asString(),"Orchard wallet backend unavailable");}
}
TEST(OrchardSpendRpc, RegistryAndBackendOrServiceAbsenceRefuse){
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.queuespend");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,SpendRpcShape());SpendRpcRefused(result);
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#else
    EXPECT_EQ(result["error"].asString(),"Daemon services unavailable");
#endif
}

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardSpendRpcFixture : OrchardSpendRequestFixture {
    din::Json RequestJson(){auto p=SpendRpcShape();p["expected_revision"]=Json::UInt64(Account(3).revision);
        p["payments"][0]["address"]=AccountKeys(17).Receiver(orchard::WalletScope::External,{}).EncodeAddress(orchard::WalletNetwork::Regtest);return p;}
    std::string TransparentAddress(const std::string& hrp="rdin"){return bech32::Encode(hrp,1,std::vector<uint8_t>(f.script.begin()+2,f.script.end()),bech32::Encoding::BECH32M);}
    din::Json CallSpend(const din::Json& p){return rpc_context_wallet_orchard_queuespend(RequestContext(),p);}
    auto FinishSpend(std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
        auto use=WalletService::AcquireWalletUse(wallet);auto& jobs=use->OrchardProofs();
        OrchardAdmissionFixture::Require(ServiceProofTerminal(jobs,Operation(1))==wallet::OrchardProofJobs::State::Succeeded);
        auto captured=Read(jobs);OrchardAdmissionFixture::Require(bool(captured.proof));
        const auto envelope=orchard::TransactionEnvelope::Create(0,{},std::vector<orchard::TransparentOutput>(outputs.begin(),outputs.end()),fee,captured.proof->Bytes());
        const auto authorization=AuthorizeAtSelectedTip(envelope);const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        const auto finalized=wallet::OrchardAccountDelivery::FinalizeCatalogProofForReplay(use->Wallet(),Session(),{Domain(),102,3},**view,Operation(1),authorization,jobs);
        OrchardAdmissionFixture::Require(finalized.retired_job);return authorization;
    }
};
}
TEST(OrchardSpendRpc, ActualRegistryTransferRetryReadyAndReopenPreserveRequest){
    OrchardSpendRpcFixture f;auto params=f.RequestJson();params["payments"][0]["memo_hex"]="0100ff";
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.queuespend");ASSERT_NE(method,nullptr);
    const auto first=(*method)(f.RequestContext(),params);ASSERT_FALSE(first.isMember("error"))<<first["error"].asString();
    EXPECT_TRUE(first["proof_queued"].asBool());EXPECT_FALSE(first["existing_request"].asBool());EXPECT_FALSE(first["archived"].asBool());EXPECT_EQ(first["durable_state"].asString(),"reserved");EXPECT_EQ(first["operation_id"],params["request_id"]);
    const auto reserved=f.Snapshot();const auto retry=f.CallSpend(params);ASSERT_FALSE(retry.isMember("error"));EXPECT_FALSE(retry["proof_queued"].asBool());EXPECT_TRUE(retry["existing_request"].asBool());EXPECT_EQ(retry["account_revision"],first["account_revision"]);EXPECT_EQ(f.Snapshot(),reserved);
    auto changed=params;changed["payments"][0]["memo_hex"]="0100fe";SpendRpcRefused(f.CallSpend(changed));EXPECT_EQ(f.Snapshot(),reserved);
    const auto authorization=f.FinishSpend({},100000);const auto ready=f.Snapshot();
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto reopened=f.CallSpend(params);ASSERT_FALSE(reopened.isMember("error"));EXPECT_EQ(reopened["durable_state"].asString(),"signed");EXPECT_FALSE(reopened["proof_queued"].asBool());EXPECT_EQ(f.Snapshot(),ready);
    (void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),200000u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),200000u);
    // Mining records a completed observation; explicit archival is a separate
    // authenticated owner transition. Preserve both contracts in this RPC case.
    const auto observed=f.Snapshot();const auto current=f.CallSpend(params);
    ASSERT_FALSE(current.isMember("error"));EXPECT_FALSE(current["archived"].asBool());
    EXPECT_TRUE(current["existing_request"].asBool());EXPECT_FALSE(current["proof_queued"].asBool());
    EXPECT_EQ(f.Snapshot(),observed);f.ArchiveCompleted();
    const auto completed=f.Snapshot();const auto archived=f.CallSpend(params);ASSERT_FALSE(archived.isMember("error"));EXPECT_TRUE(archived["archived"].asBool());EXPECT_TRUE(archived["existing_request"].asBool());EXPECT_FALSE(archived["proof_queued"].asBool());EXPECT_EQ(f.Snapshot(),completed);
}
TEST(OrchardSpendRpc, ActualUnshieldAndMixedRecipientsRetainExactOutputAmounts){
    for(const bool mixed:{false,true}){
        OrchardSpendRpcFixture f;auto params=f.RequestJson();
        if(!mixed)params["payments"]=din::arr();else params["payments"][0]["amount_una"]=Json::UInt64(100000);
        const uint64_t amount=mixed?200000:400000;din::Json output;output["address"]=f.TransparentAddress();output["amount_una"]=Json::UInt64(amount);params["outputs"].append(output);
        const auto result=f.CallSpend(params);ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();EXPECT_TRUE(result["proof_queued"].asBool());
        const std::vector<orchard::TransparentOutput> outputs{{amount,f.f.script}};const auto authorization=f.FinishSpend(outputs,100000);
        const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto id=body.GetTxid().AsUint256();(void)f.Mine(body);ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
        const auto coin=f.f.db.getCoin(id,0);ASSERT_TRUE(coin.ok());EXPECT_EQ(coin->amount,amount);EXPECT_EQ(coin->script_pubkey,util::hex(f.f.script));
        EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),mixed?100000u:0u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),mixed?100000u:0u);
    }
}
TEST(OrchardSpendRpc, TransparentDestinationTypesBindExactOrderedScripts){
    OrchardSpendRpcFixture f;auto params=f.RequestJson();params["payments"]=din::arr();params["fee_una"]=Json::UInt64(1000);
    std::vector<std::pair<std::string,std::vector<uint8_t>>> destinations;std::vector<uint8_t> hash(20,0x31);
    for(const uint8_t version:{AddressVersion::DINERO_TESTNET_P2PKH,AddressVersion::DINERO_TESTNET_P2SH}){
        std::vector<uint8_t> payload{version};payload.insert(payload.end(),hash.begin(),hash.end());
        std::vector<uint8_t> script=version==AddressVersion::DINERO_TESTNET_P2PKH?std::vector<uint8_t>{0x76,0xa9,0x14}:std::vector<uint8_t>{0xa9,0x14};
        script.insert(script.end(),hash.begin(),hash.end());if(version==AddressVersion::DINERO_TESTNET_P2PKH)script.insert(script.end(),{0x88,0xac});else script.push_back(0x87);
        destinations.push_back({Address::encodeBase58Check(payload),script});
    }
    for(const auto [version,size]:std::array<std::pair<int,size_t>,4>{{{0,20},{0,32},{1,32},{3,32}}}){
        std::vector<uint8_t> program(size,uint8_t(0x40+version));std::vector<uint8_t> script{uint8_t(version==0?0:0x50+version),uint8_t(size)};script.insert(script.end(),program.begin(),program.end());
        destinations.push_back({bech32::Encode("rdin",version,program,version==0?bech32::Encoding::BECH32:bech32::Encoding::BECH32M),script});
    }
    std::vector<orchard::TransparentOutput> outputs;
    for(const auto& [address,script]:destinations){din::Json output;output["address"]=address;output["amount_una"]=Json::UInt64(100);params["outputs"].append(output);outputs.push_back({100,script});}
    const auto result=f.CallSpend(params);ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();const auto before=f.Snapshot();
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();const auto exact=f.Invoke(jobs,{},outputs,1000,params["expected_revision"].asUInt64());EXPECT_TRUE(exact->existing_request);EXPECT_FALSE(exact->enqueued);EXPECT_EQ(f.Snapshot(),before);
    auto changed=params;changed["outputs"][0]=params["outputs"][1];changed["outputs"][1]=params["outputs"][0];SpendRpcRefused(f.CallSpend(changed));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(ServiceProofTerminal(jobs,f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);
}
TEST(OrchardSpendRpc, WrongNetworkAmountOwnerAndChangedRetryRefuseWithoutMutation){
    OrchardSpendRpcFixture f;const auto valid=f.RequestJson();const auto initial=f.Snapshot();std::vector<din::Json> invalid;
    for(const auto network:{orchard::WalletNetwork::Mainnet,orchard::WalletNetwork::Testnet}){auto p=valid;p["payments"][0]["address"]=f.AccountKeys(17).Receiver(orchard::WalletScope::External,{}).EncodeAddress(network);invalid.push_back(p);}
    for(const auto& address:std::vector<std::string>{f.TransparentAddress("din"),f.TransparentAddress("tdin"),"not-an-address",bech32::Encode("rdin",2,std::vector<uint8_t>(32,1),bech32::Encoding::BECH32M)}){
        auto p=valid;p["payments"]=din::arr();din::Json output;output["address"]=address;output["amount_una"]=Json::UInt64(100);p["outputs"].append(output);invalid.push_back(p);
    }
    for(const uint8_t version:{AddressVersion::DINERO_P2PKH,AddressVersion::MAINNET_P2PKH}){
        std::vector<uint8_t> payload(21,0x31);payload[0]=version;auto p=valid;p["payments"]=din::arr();din::Json output;output["address"]=Address::encodeBase58Check(payload);output["amount_una"]=Json::UInt64(100);p["outputs"].append(output);invalid.push_back(p);
    }
    auto p=valid;p["fee_una"]=Json::UInt64(std::numeric_limits<uint64_t>::max());invalid.push_back(p);p=valid;p["payments"][0]["amount_una"]=Json::UInt64(orchard::kMaxMoneyUna);invalid.push_back(p);
    p=valid;p["expected_revision"]=Json::UInt64(valid["expected_revision"].asUInt64()+1);invalid.push_back(p);p=valid;p["account"]=Json::UInt64(29);invalid.push_back(p);
    for(const auto& request:invalid){SpendRpcRefused(f.CallSpend(request));EXPECT_EQ(f.Snapshot(),initial);}
    auto context=f.RequestContext();context.walletName="another-wallet";SpendRpcRefused(rpc_context_wallet_orchard_queuespend(context,valid));EXPECT_EQ(f.Snapshot(),initial);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().lockWallet();SpendRpcRefused(f.CallSpend(valid));use->Wallet().unlockWallet("canonical-fixture-pass",0);}EXPECT_EQ(f.Snapshot(),initial);
    auto first=f.CallSpend(valid);ASSERT_FALSE(first.isMember("error"));const auto reserved=f.Snapshot();p=valid;p["fee_una"]=Json::UInt64(99999);SpendRpcRefused(f.CallSpend(p));EXPECT_EQ(f.Snapshot(),reserved);
    auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_EQ(ServiceProofTerminal(use->OrchardProofs(),f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);
}
TEST(OrchardSpendRpc, RequiredWritesCommitAndIncompleteCatalogLeaveNoRequest){
    OrchardSpendRpcFixture f;const auto params=f.RequestJson();const auto before=f.Snapshot();auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    f.Sql("CREATE TRIGGER refuse_rpc_request BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'request refusal'); END");SpendRpcRefused(f.CallSpend(params));f.Sql("DROP TRIGGER refuse_rpc_request");EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(f.Operation(1)));
    bool seen=false;sqlite3_commit_hook(f.Database(),[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);const auto denied=f.CallSpend(params);sqlite3_commit_hook(f.Database(),nullptr,nullptr);EXPECT_TRUE(seen);SpendRpcRefused(denied);EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(f.Operation(1)));
    f.Sql("CREATE TEMP TABLE saved_rpc_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");const auto missing=f.Snapshot();SpendRpcRefused(f.CallSpend(params));EXPECT_EQ(f.Snapshot(),missing);EXPECT_FALSE(jobs.Query(f.Operation(1)));f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_rpc_owner; DROP TABLE saved_rpc_owner");EXPECT_EQ(f.Snapshot(),before);
    const auto accepted=f.CallSpend(params);ASSERT_FALSE(accepted.isMember("error"))<<accepted["error"].asString();EXPECT_TRUE(accepted["proof_queued"].asBool());EXPECT_EQ(ServiceProofTerminal(jobs,f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);
}
#endif
} // namespace dinero
