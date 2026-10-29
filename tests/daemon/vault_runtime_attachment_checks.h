#pragma once
#include "vault_operator_binding_checks.h"
namespace dinero {
namespace {
struct VaultRuntimeAttachmentFixture {
    std::unique_ptr<VaultStateStoreFixture> f;
    VaultRuntimeAttachmentFixture() {
        SelectParams(Chain::REGTEST);f=std::make_unique<VaultStateStoreFixture>();
        f->domain.network=static_cast<uint8_t>(GetActiveChain());uint256 genesis;
        if(!uint256::FromHex(Params().genesis_hash,genesis))throw std::runtime_error("fixture genesis");
        std::copy(genesis.begin(),genesis.end(),f->domain.genesis.begin());
        f->config.operator_binding=BindingForVaultFixture();
    }
    ExecutionContext Context() {
        ExecutionContext ctx;ctx.daemon=&f->wallet.context;ctx.logger=&f->wallet.logger;ctx.walletName="owner";return ctx;
    }
    WalletSigningIdentity Selected() {
        auto use=WalletService::AcquireWalletUse(f->wallet.service);
        return CaptureWalletSigningIdentity(use->Wallet(),"owner");
    }
    vault::VaultRuntimeConfig Config() {
        auto config=VaultOwnerConfig();config.capture_tip=f->Capture();return config;
    }
    void Open(const vault::VaultIdentity& id) {
        vault::OpenExistingVaultRuntime(Config(),Context(),f->wallet.service,Selected(),id);
    }
    // Runtime teardown must precede the fixture's context/service teardown,
    // including when a fatal assertion returns from a case early.
    ~VaultRuntimeAttachmentFixture(){vault::ShutdownVaultRuntime();}
};
}
TEST(VaultRuntimeAttachment, AuthenticatesExistingBindingBeforeRuntimePublication) {
    VaultRuntimeReset reset;VaultRuntimeAttachmentFixture fixture;auto& f=*fixture.f;
    auto bound=f.Create();f.Fund(bound.service);const auto rows=f.Rows();
    const auto saved=vault::EncodeVaultState(bound.service->captureState());fixture.Open(bound.identity);
    auto service=vault::GetVaultRuntimeService();ASSERT_TRUE(service);
    EXPECT_EQ(vault::EncodeVaultState(service->captureState()),saved);EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(vault::GetVaultOperator().address,VaultOwnerAddress(f.config.operator_binding->script_pub_key));
    EXPECT_EQ(vault::GetVaultOperator().account,f.config.operator_binding->account);
    std::string error;EXPECT_FALSE(vault::SetVaultOperator("","replacement",&error));
    EXPECT_FALSE(error.empty());EXPECT_EQ(service->operatorBinding(),f.config.operator_binding);
    EXPECT_THROW(fixture.Open(bound.identity),std::runtime_error);
    EXPECT_EQ(vault::GetVaultRuntimeService(),service);EXPECT_EQ(f.Rows(),rows);
    vault::ShutdownVaultRuntime();EXPECT_FALSE(vault::GetVaultRuntimeService());
    fixture.Open(bound.identity);EXPECT_NE(vault::GetVaultRuntimeService(),service);
    EXPECT_EQ(vault::EncodeVaultState(vault::GetVaultRuntimeService()->captureState()),saved);
}
TEST(VaultRuntimeAttachment, MissingHistoricalAndMismatchedOwnersRefuse) {
    VaultRuntimeReset reset;VaultRuntimeAttachmentFixture fixture;auto& f=*fixture.f;
    auto bound=f.Create();const auto binding=f.config.operator_binding;f.config.operator_binding.reset();auto historical=f.Create();f.config.operator_binding=binding;
    const auto rows=f.Rows();auto missing=bound.identity;missing.back()^=1;
    EXPECT_THROW(fixture.Open(missing),std::exception);
    EXPECT_THROW(fixture.Open(historical.identity),std::exception);
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_EQ(f.Rows(),rows);
    auto config=fixture.Config();config.default_account="different-account";
    EXPECT_THROW(vault::OpenExistingVaultRuntime(config,fixture.Context(),f.wallet.service,fixture.Selected(),bound.identity),std::exception);
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_EQ(f.Rows(),rows);
    auto stale=fixture.Selected();++stale.session;
    EXPECT_THROW(vault::OpenExistingVaultRuntime(fixture.Config(),fixture.Context(),f.wallet.service,stale,bound.identity),std::exception);
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_EQ(f.Rows(),rows);
    fixture.Open(bound.identity);EXPECT_EQ(vault::GetVaultRuntimeService()->operatorBinding(),binding);
}
TEST(VaultRuntimeAttachment, CloseDetachesAcquisitionAndClosesRetainedDispatch) {
    VaultRuntimeReset reset;VaultRuntimeAttachmentFixture fixture;auto& f=*fixture.f;
    auto bound=f.Create();f.Fund(bound.service);fixture.Open(bound.identity);
    auto retained=vault::GetVaultRuntimeService();ASSERT_TRUE(retained);
    vault::WithdrawalPaymentTerms terms{1,100,"runtime-lifetime-fixture"};
    const auto request=retained->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,BindingForVaultFixture().script_pub_key,terms);
    vault::CloseVaultRuntimeDispatch();EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_TRUE(vault::GetVaultOperator().address.empty());
    EXPECT_THROW(retained->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalSigning>(retained->withdrawalState(request)));
    {auto use=WalletService::AcquireWalletUse(f.wallet.service);EXPECT_TRUE(use->Wallet().getPendingPayments().empty());}
    EXPECT_THROW(fixture.Open(bound.identity),std::runtime_error);
    vault::ShutdownVaultRuntime();EXPECT_FALSE(vault::GetVaultRuntimeService());
    f.wallet.service->Stop();
    EXPECT_THROW(retained->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalSigning>(retained->withdrawalState(request)));
}
TEST(VaultRuntimeAttachment, ActualOpenRpcSelectsExistingWalletWithoutCreatingOrReplacing) {
    VaultRuntimeReset reset;VaultRuntimeAttachmentFixture fixture;auto& f=*fixture.f;
    auto bound=f.Create();const auto rows=f.Rows();
    static constexpr char digits[]="0123456789abcdef";std::string identity;
    for(auto v:bound.identity){identity.push_back(digits[v>>4]);identity.push_back(digits[v&15]);}
    din::Json request(::Json::arrayValue);din::Json object;object["vault_id"]=identity;request.append(object);
    auto wrong=fixture.Context();wrong.walletName="different-wallet";
    EXPECT_TRUE(din::rpc_vault_open(wrong,request).isMember("error"));EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_EQ(f.Rows(),rows);
    auto malformed=request;malformed[0]["vault_id"]="not-an-identity";
    EXPECT_TRUE(din::rpc_vault_open(fixture.Context(),malformed).isMember("error"));EXPECT_EQ(f.Rows(),rows);
    const auto result=din::rpc_vault_open(fixture.Context(),request);ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    EXPECT_TRUE(result["attached"].asBool());EXPECT_EQ(result["vault_id"].asString(),identity);EXPECT_EQ(result["wallet"].asString(),"owner");
    auto service=vault::GetVaultRuntimeService();ASSERT_TRUE(service);EXPECT_EQ(service->operatorBinding(),f.config.operator_binding);EXPECT_EQ(f.Rows(),rows);
    // This fixture has no canonical source. Attachment acknowledges ownership
    // only; the actual production reader refuses an unavailable chain view.
    const auto state=vault::EncodeVaultState(service->captureState());EXPECT_THROW(service->tipChanged(20),std::runtime_error);
    EXPECT_EQ(vault::EncodeVaultState(service->captureState()),state);EXPECT_EQ(f.Rows(),rows);
    EXPECT_TRUE(din::rpc_vault_open(fixture.Context(),request).isMember("error"));EXPECT_EQ(vault::GetVaultRuntimeService(),service);
}
} // namespace dinero
