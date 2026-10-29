#pragma once
namespace dinero {
namespace {
struct VaultRuntimeReset {
    VaultRuntimeReset(){vault::ShutdownVaultRuntime();}
    ~VaultRuntimeReset(){vault::ShutdownVaultRuntime();}
};
vault::VaultRuntimeConfig VaultOwnerConfig() {
    vault::VaultRuntimeConfig cfg;cfg.enabled=true;
    cfg.block_hash_at_height=[](uint64_t){std::array<uint8_t,32> hash{};hash.fill(1);return hash;};
    cfg.tx_included_at=[](const auto&,uint32_t,uint64_t,const auto&){return true;};return cfg;
}
std::string VaultOwnerAddress(const std::vector<uint8_t>& script) {
    return bech32::Encode(HrpForActiveNetworkRef(),1,
        std::vector<uint8_t>(script.begin()+2,script.end()),bech32::Encoding::BECH32M);
}
}
TEST(VaultRuntimeOwner, RetainedServiceAndRpcAcquisitionSurviveDetach) {
    VaultRuntimeReset reset;ExecutionContext context;din::Json params;
    EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_TRUE(din::rpc_vault_metrics(context,params).isMember("error"));
    vault::InitializeVaultRuntime(VaultOwnerConfig());auto retained=din::GetVaultService();ASSERT_TRUE(retained);
    std::weak_ptr<vault::VaultService> previous=retained;
    EXPECT_FALSE(din::rpc_vault_metrics(context,params).isMember("error"));
    vault::ShutdownVaultRuntime();EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_TRUE(din::rpc_vault_metrics(context,params).isMember("error"));
    EXPECT_FALSE(previous.expired());EXPECT_EQ(retained->accountCount(),0u);
    vault::InitializeVaultRuntime(VaultOwnerConfig());const auto replacement=din::GetVaultService();
    ASSERT_TRUE(replacement);EXPECT_NE(replacement.get(),retained.get());
    retained.reset();EXPECT_TRUE(previous.expired());
    EXPECT_FALSE(din::rpc_vault_metrics(context,params).isMember("error"));
}
TEST(VaultRuntimeOwner, OperatorBindingRejectsInvalidReplacement) {
    VaultRuntimeReset reset;vault::InitializeVaultRuntime(VaultOwnerConfig());
    std::vector<uint8_t> script(34,7);script[0]=0x51;script[1]=32;
    const auto address=VaultOwnerAddress(script);std::string error;
    ASSERT_TRUE(vault::SetVaultOperator(address,"fixture-owner",&error));
    EXPECT_EQ(vault::GetVaultOperator().address,address);
    EXPECT_FALSE(vault::SetVaultOperator("invalid-address","other",&error));
    EXPECT_EQ(vault::GetVaultOperator().address,address);EXPECT_EQ(vault::GetVaultOperator().account,"fixture-owner");
    EXPECT_TRUE(vault::SetVaultOperator("","disabled",&error));
    EXPECT_TRUE(vault::GetVaultOperator().address.empty());
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
vault::VaultRuntimeConfig VaultOwnerChainConfig(VaultObservationFixture& f) {
    auto cfg=VaultOwnerConfig();cfg.operator_address=VaultOwnerAddress(f.f.script);cfg.default_account="fixture-owner";
    cfg.block_hash_at_height=vault::MakeChainstateBlockHashClosure(f.context);
    cfg.tx_included_at=vault::MakeChainstateTxIncludedClosure(f.context);return cfg;
}
}
TEST(VaultRuntimeOwner, CanonicalHistoricalCoinAndActualRpcObservation) {
    VaultObservationFixture f;f.ArchiveHistorical(1);VaultRuntimeReset reset;
    vault::InitializeVaultRuntime(VaultOwnerChainConfig(f));const auto owner=vault::GetVaultRuntimeService();ASSERT_TRUE(owner);
    const auto txid=f.f.blocks[1].vtx.front().GetTxid().AsUint256();
    uint64_t amount=0,height=0;std::array<uint8_t,32> hash{};std::string error;
    ASSERT_TRUE(vault::VerifyOperatorDeposit(owner,VaultRawHash(txid),0,amount,height,hash,error))<<error;
    EXPECT_EQ(amount,f.f.blocks[1].vtx.front().vout.front().value.GetUna());EXPECT_EQ(height,1u);
    EXPECT_EQ(hash,VaultRawHash(f.f.blocks[1].GetHash()));
    const auto historical=f.f.service->getCanonicalOutputInclusion(txid,0,1);
    ASSERT_TRUE(historical.ok());ASSERT_TRUE(historical->transparent_amount);
    EXPECT_EQ(*historical->transparent_amount,amount);EXPECT_EQ(historical->script_pub_key,f.f.script);
    EXPECT_TRUE(historical->MatchesTransparent(amount,f.f.script));
    EXPECT_FALSE(historical->MatchesTransparent(amount+1,f.f.script));
    auto other_script=f.f.script;other_script.back()^=1;
    EXPECT_FALSE(historical->MatchesTransparent(amount,other_script));
    auto unavailable=*historical;unavailable.transparent_amount.reset();
    EXPECT_FALSE(unavailable.MatchesTransparent(amount,f.f.script));
    unavailable=*historical;unavailable.included=false;
    EXPECT_FALSE(unavailable.MatchesTransparent(amount,f.f.script));
    din::Json object;object["txid"]=txid.GetHex();object["vout"]=0;object["account_id"]="fixture-owner";
    auto params=din::arr();params.append(object);ExecutionContext context;context.daemon=&f.context;
    const auto result=din::rpc_vault_observe(context,params);ASSERT_FALSE(result.isMember("error"));
    EXPECT_EQ(result["status"].asString(),"observed");
    vault::NotifyVaultTipConnected(101);
    EXPECT_EQ(owner->accountConfirmed(vault::AccountId{"fixture-owner"}),amount);
    const auto seq=owner->ledgerNextSeq();
    ASSERT_FALSE(din::rpc_vault_observe(context,params).isMember("error"));
    vault::NotifyVaultTipConnected(101);EXPECT_EQ(owner->ledgerNextSeq(),seq);
    // A real canonical shield spends the historical coin and creates typed
    // transparent change paying this same operator script.
    const auto [body,bundle]=f.Shield(f.Keys());const auto stored=f.Mine(body);
    amount=71;height=72;hash.fill(73);const auto untouched=hash;
    EXPECT_FALSE(vault::VerifyOperatorDeposit(owner,VaultRawHash(txid),0,amount,height,hash,error));
    EXPECT_EQ(amount,71u);EXPECT_EQ(height,72u);EXPECT_EQ(hash,untouched);
    ASSERT_TRUE(vault::VerifyOperatorDeposit(owner,VaultRawHash(body.GetTxid().AsUint256()),0,
        amount,height,hash,error))<<error;
    EXPECT_EQ(amount,body.OutputCoin(0,102).value.GetUna());EXPECT_EQ(height,102u);
    EXPECT_EQ(hash,VaultRawHash(stored->Orchard().Header().GetHash()));
    const auto typed=f.f.service->getCanonicalOutputInclusion(body.GetTxid().AsUint256(),0,102);
    ASSERT_TRUE(typed.ok());ASSERT_TRUE(typed->transparent_amount);
    EXPECT_EQ(*typed->transparent_amount,amount);EXPECT_EQ(typed->script_pub_key,f.f.script);
    EXPECT_TRUE(typed->MatchesTransparent(amount,f.f.script));
    EXPECT_FALSE(typed->MatchesTransparent(amount+1,f.f.script));
    EXPECT_FALSE(typed->MatchesTransparent(amount,other_script));
    const auto absent=f.f.service->getCanonicalOutputInclusion(body.GetTxid().AsUint256(),body.OutputCount(),102);
    ASSERT_TRUE(absent.ok());EXPECT_FALSE(absent->included);EXPECT_FALSE(absent->transparent_amount);
    EXPECT_TRUE(absent->script_pub_key.empty());EXPECT_FALSE(absent->MatchesTransparent(amount,f.f.script));
    EXPECT_EQ(owner->ledgerNextSeq(),seq);
}
TEST(VaultRuntimeOwner, UnavailableSourceAndChangedOwnerPreserveOutputs) {
    VaultObservationFixture f;f.ArchiveHistorical(1);VaultRuntimeReset reset;
    vault::InitializeVaultRuntime(VaultOwnerChainConfig(f));auto owner=vault::GetVaultRuntimeService();ASSERT_TRUE(owner);
    const auto txid=VaultRawHash(f.f.blocks[1].vtx.front().GetTxid().AsUint256());
    uint64_t amount=71,height=72;std::array<uint8_t,32> hash{};hash.fill(73);const auto original=hash;std::string error;
    f.f.db.close();
    EXPECT_FALSE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error));
    EXPECT_EQ(amount,71u);EXPECT_EQ(height,72u);EXPECT_EQ(hash,original);EXPECT_EQ(owner->accountCount(),0u);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    EXPECT_TRUE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error))<<error;
    vault::ShutdownVaultRuntime();vault::InitializeVaultRuntime(VaultOwnerChainConfig(f));
    amount=71;height=72;hash=original;
    EXPECT_FALSE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error));
    EXPECT_EQ(amount,71u);EXPECT_EQ(height,72u);EXPECT_EQ(hash,original);EXPECT_EQ(owner->accountCount(),0u);
    owner=vault::GetVaultRuntimeService();f.wallet->Stop();f.f.ingress->Stop();f.f.service->Stop();
    EXPECT_FALSE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error));
    EXPECT_EQ(amount,71u);EXPECT_EQ(height,72u);EXPECT_EQ(hash,original);
}
#endif
} // namespace dinero
