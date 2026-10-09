#pragma once
namespace dinero {
namespace {
const std::array<const char*,8> OrchardBoundMethods{
    "wallet.orchard.createaccount","wallet.orchard.getnewaddress",
    "wallet.orchard.getbalance","wallet.orchard.listoperations",
    "wallet.orchard.queuespend","wallet.orchard.finishspend",
    "wallet.orchard.queueshield","wallet.orchard.finishshield"};
din::Json OrchardBindingRequest(const char* name="canonical-recovery") {
    din::Json p;p["wallet_name"]=name;return p;
}
din::Json OrchardBoundShape(const std::string& method) {
    if(method.ends_with("shield"))return ShieldRpcShape();
    if(method.ends_with("spend"))return SpendRpcShape();
    return OrchardOperationListRequest();
}
}
TEST(OrchardRpcBinding, EveryPublicMethodRequiresWellFormedBinding) {
    RegisterOrchardAccountRpc();ExecutionContext context;
    for(const auto name:OrchardBoundMethods) {
        const auto* handler=g_rpcRegistry.lookup(name);ASSERT_NE(handler,nullptr);
        auto params=OrchardBoundShape(name);const auto missing=(*handler)(context,params);
        EXPECT_EQ(missing["error_code"].asString(),"wallet_binding_required");
        for(const auto& value:std::vector<din::Json>{din::Json{},din::Json(true),din::Json(3),
                din::Json(""),din::Json(std::string(63,'a')),din::Json(std::string(64,'A')),
                din::Json(std::string(63,'a')+'\0')}) {
            params["wallet_binding"]=value;const auto refused=(*handler)(context,params);
            EXPECT_EQ(refused["error_code"].asString(),"wallet_binding_invalid");
            EXPECT_FALSE(refused.isMember("operation_id"));EXPECT_FALSE(refused.isMember("address"));
        }
    }
    const auto* bootstrap=g_rpcRegistry.lookup("wallet.orchard.getwalletbinding");ASSERT_NE(bootstrap,nullptr);
    EXPECT_EQ((*bootstrap)(context,din::Json{})["error_code"].asString(),"wallet_binding_invalid");
    EXPECT_EQ((*bootstrap)(context,OrchardBindingRequest(""))["error_code"].asString(),"wallet_binding_invalid");
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardRpcBinding, ExplicitNameAndStableReadOnlyBinding) {
    OrchardBalanceFixture f;RegisterOrchardAccountRpc();auto context=f.RequestContext();context.walletName.clear();
    const auto* bootstrap=g_rpcRegistry.lookup("wallet.orchard.getwalletbinding");ASSERT_NE(bootstrap,nullptr);
    const auto before=f.Snapshot();const auto binding=(*bootstrap)(context,OrchardBindingRequest());
    ASSERT_FALSE(binding.isMember("error"))<<binding.toStyledString();ASSERT_EQ(binding["wallet_binding"].asString().size(),64u);
    EXPECT_EQ(binding["wallet_name"].asString(),"canonical-recovery");
    EXPECT_EQ((*bootstrap)(context,OrchardBindingRequest()),binding);EXPECT_EQ(f.Snapshot(),before);
    const auto wrong=(*bootstrap)(context,OrchardBindingRequest("different-wallet"));
    EXPECT_EQ(wrong["error_code"].asString(),"wallet_binding_mismatch");EXPECT_FALSE(wrong.isMember("wallet_binding"));
    auto params=OrchardOperationListRequest();params["wallet_binding"]=binding["wallet_binding"];
    const auto* balance=g_rpcRegistry.lookup("wallet.orchard.getbalance");ASSERT_NE(balance,nullptr);
    EXPECT_EQ((*balance)(context,params),f.Balance());EXPECT_EQ(f.Snapshot(),before);
    sqlite3* db=nullptr;{auto use=WalletService::AcquireWalletUse(f.wallet);db=use->Wallet().AcquireDatabaseLease()->Database();}
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ && table && std::string_view(table)=="wallet_meta"?SQLITE_DENY:SQLITE_OK;
    },nullptr),SQLITE_OK);
    const auto unavailable=(*bootstrap)(context,OrchardBindingRequest());
    EXPECT_TRUE(unavailable.isMember("error"));EXPECT_FALSE(unavailable.isMember("wallet_binding"));
    EXPECT_TRUE((*balance)(context,params).isMember("error"));
    ASSERT_EQ(sqlite3_set_authorizer(db,nullptr,nullptr),SQLITE_OK);
    EXPECT_EQ((*bootstrap)(context,OrchardBindingRequest()),binding);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardRpcBinding, ReopenAndWalletSwitchRefuseEveryOldPublicRequestBeforeEffects) {
    OrchardBalanceFixture f;RegisterOrchardAccountRpc();auto context=f.RequestContext();context.walletName.clear();
    const auto original=rpc_context_wallet_orchard_getwalletbinding(context,OrchardBindingRequest());
    ASSERT_FALSE(original.isMember("error"));const auto old=original["wallet_binding"];
    const auto unchanged=f.Snapshot();
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    const auto reopened=rpc_context_wallet_orchard_getwalletbinding(context,OrchardBindingRequest());
    ASSERT_FALSE(reopened.isMember("error"));EXPECT_NE(reopened["wallet_binding"],old);
    const auto refuse=[&] {
        const auto before=f.Snapshot();
        for(const auto name:OrchardBoundMethods) {
            const auto* handler=g_rpcRegistry.lookup(name);ASSERT_NE(handler,nullptr);
            auto params=OrchardBoundShape(name);params["wallet_binding"]=old;
            const auto result=(*handler)(context,params);
            EXPECT_EQ(result["error_code"].asString(),"wallet_binding_mismatch")<<name<<result.toStyledString();
            EXPECT_FALSE(result.isMember("operation_id"));EXPECT_FALSE(result.isMember("address"));
        }
        EXPECT_EQ(f.Snapshot(),before);
    };
    ASSERT_NO_FATAL_FAILURE(refuse());EXPECT_EQ(f.Snapshot(),unchanged);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    auto params=OrchardOperationListRequest();params["wallet_binding"]=reopened["wallet_binding"];
    const auto* balance=g_rpcRegistry.lookup("wallet.orchard.getbalance");ASSERT_NE(balance,nullptr);
    EXPECT_EQ((*balance)(context,params),f.Balance());ASSERT_NO_FATAL_FAILURE(refuse());
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().create("binding-other");}
    ASSERT_NO_FATAL_FAILURE(refuse());
    EXPECT_EQ(rpc_context_wallet_orchard_getwalletbinding(context,OrchardBindingRequest())["error_code"].asString(),"wallet_binding_mismatch");
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.Snapshot(),unchanged);
}
TEST(OrchardRpcBinding, DifferentManagersBindSameDatabaseAndSessionDifferently) {
    OrchardBalanceFixture f;const auto path=f.f.path/"binding-managers";
    {WalletManager initialize(path,f.f.logger.get());initialize.create("same");}
    std::array<uint8_t,32> first{},identity{};uint64_t session=0;
    {WalletManager manager(path,f.f.logger.get());manager.open("same");auto lease=manager.AcquireDatabaseLease();
     first=lease->RpcBinding();identity=lease->ReadDeliveryIdentity();session=lease->Session();
     EXPECT_EQ(lease->RpcBinding(),first);}
    {WalletManager manager(path,f.f.logger.get());manager.open("same");auto lease=manager.AcquireDatabaseLease();
     EXPECT_EQ(lease->Session(),session);EXPECT_EQ(lease->ReadDeliveryIdentity(),identity);
     EXPECT_NE(lease->RpcBinding(),first);}
}
#endif
}
