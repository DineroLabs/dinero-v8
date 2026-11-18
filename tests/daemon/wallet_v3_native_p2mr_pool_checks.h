#pragma once
// v8.2.5 (#939 follow-up): more P2MR addresses after native v3 enrollment.
// Orchard-origin only; uses the native v3 route helpers (NativeIssue,
// ExpectNativeReceiptsCurrent, NativeState, RestartNativeWallet,
// NativeTestCount) and FillNativeP2MRTargetsV3.

#include "rpc/v7_pq_handlers.h"
#include "address/addr_codec.h"

namespace dinero {

namespace {
int P2MRPoolRows(CanonicalRecoveryFixture& f,const std::string& where="1") {
    auto use=WalletService::AcquireWalletUse(f.wallet);
    return NativeTestCount(use->Wallet(),std::string("SELECT count(*) FROM native_v3_p2mr_pool WHERE ")+where);
}
bool P2MRTargetOpen(CanonicalRecoveryFixture& f) {
    auto use=WalletService::AcquireWalletUse(f.wallet);
    return !use->Wallet().AcquireDatabaseLease()->NativeP2MRTargetMissing().empty();
}
void Deliver(CanonicalRecoveryFixture& f) {
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
}
} // namespace

// A wallet created and enrolled on v3 gets a sealed P2MR pool with its first
// coverage: P2MR addresses are issued from it (RPC and PQ RPC), the receipts
// stay certified, funds paid to a pooled P2MR address are delivered, and the
// issued state survives a restart.
TEST(WalletV3NativeP2MRPool, CreatedWalletIssuesFundedP2MRFromPool) {
    CanonicalRecoveryFixture f(false,false);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-p2mr-pool-index");
    Deliver(f);
    ASSERT_EQ(NativeState(f),WalletManager::NativeV3State::Complete);
    EXPECT_EQ(P2MRPoolRows(f,"issued=0"),WalletManager::kNativeV3ReceivePool);
    EXPECT_FALSE(P2MRTargetOpen(f));
    ExpectNativeReceiptsCurrent(f,102);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto first=NativeIssue(request,"p2mr","savings");
    ASSERT_TRUE(first.isMember("address")) << first.toStyledString();
    EXPECT_TRUE(first["address"].asString().starts_with("rdin1r"));
    ExpectNativeReceiptsCurrent(f,102);
    // The PQ RPC serves the same pool, and refuses explicit derivation.
    registerV7PqWalletMethods();
    const auto* pq=g_rpcRegistry.lookup("wallet.getnewp2mraddress");
    ASSERT_NE(pq,nullptr);
    // Only the pool's own derivation is accepted: another account or an
    // unissued index is refused; the issued index is returned unchanged.
    din::Json other_account;other_account["account"]=1;
    EXPECT_EQ((*pq)(request,other_account)["error_code"].asString(),"wallet_native_p2mr_pool_only");
    din::Json unissued_index;unissued_index["address_index"]=19;
    EXPECT_EQ((*pq)(request,unissued_index)["error_code"].asString(),"wallet_native_p2mr_pool_only");
    {
        const auto decoded_path=NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
            "SELECT idx FROM native_v3_p2mr_pool WHERE address='"+first["address"].asString()+"'");
        din::Json same;same["address_index"]=decoded_path;same["account"]=0;same["leaf_index"]=0;
        same["hrp"]="rdin";
        const auto again=(*pq)(request,same);
        EXPECT_EQ(again["address"].asString(),first["address"].asString()) << again.toStyledString();
    }
    din::Json labelled;labelled["label"]="rent";
    const auto second=(*pq)(request,labelled);
    ASSERT_TRUE(second.isMember("address")) << second.toStyledString();
    EXPECT_NE(second["address"].asString(),first["address"].asString());
    EXPECT_EQ(second["leaf_index"].asInt64(),0);
    EXPECT_TRUE(second["derivation_path"].asString().starts_with("m/88'/1448'/0'/0/"));
    EXPECT_EQ(P2MRPoolRows(f,"issued=1 AND generation=0 AND label IN ('savings','rent')"),2);
    {
        // listp2mraddresses shows issued pool addresses, never unissued ones.
        const auto* list=g_rpcRegistry.lookup("wallet.listp2mraddresses");
        ASSERT_NE(list,nullptr);
        const auto listed=(*list)(request,din::obj());
        ASSERT_FALSE(listed.isMember("error")) << listed.toStyledString();
        std::set<std::string> shown;
        for(const auto& row:listed["addresses"])shown.insert(row["address"].asString());
        EXPECT_TRUE(shown.contains(first["address"].asString()));
        EXPECT_TRUE(shown.contains(second["address"].asString()));
        EXPECT_EQ(shown.size(),size_t(P2MRPoolRows(f,"issued=1")+0)) << listed.toStyledString();
    }
    ExpectNativeReceiptsCurrent(f,102);

    // Funds to a pooled P2MR address are delivered by ordinary v3 delivery.
    f.receive_address=first["address"].asString();
    const auto paid=f.MineToWallet();
    Deliver(f);
    ExpectNativeReceiptsCurrent(f,103);
    {
        // P2MR coins carry their script (the address column stays empty for
        // witness v3, as on every route).
        const auto decoded=wallet::DecodeP2MRAddress(first["address"].asString());
        ASSERT_TRUE(decoded);
        const auto spk=wallet::BuildP2MRScriptPubKey(decoded->merkle_root);
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(NativeTestCount(use->Wallet(),
            "SELECT count(*) FROM utxos WHERE lower(script_pubkey)='"+util::hex(spk)+"'"),1);
    }
    EXPECT_GT(paid,0u);

    RestartNativeWallet(f,v3);
    Deliver(f);
    EXPECT_EQ(P2MRPoolRows(f,"issued=1"),P2MRPoolRows(f,"issued=1 AND address IN ('"+
        first["address"].asString()+"','"+second["address"].asString()+"')"));
    const auto third=NativeIssue(request,"p2mr");
    ASSERT_TRUE(third.isMember("address")) << third.toStyledString();
    EXPECT_NE(third["address"].asString(),first["address"].asString());
    EXPECT_NE(third["address"].asString(),second["address"].asString());
}

// Sierra's shape: native-complete with only its creation-time P2MR address
// and no P2MR pool. The worker runs a P2MR-only extension: the existing key
// is adopted as issued, the next indices are filled, the receipts reseal,
// and P2MR issuance then works. The pool is never re-entered while sealed.
TEST(WalletV3NativeP2MRPool, EnrolledWalletWithoutPoolExtendsP2MROnly) {
    CanonicalRecoveryFixture f(false,false);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-p2mr-sierra-index");
    Deliver(f);
    ASSERT_EQ(NativeState(f),WalletManager::NativeV3State::Complete);
    int taproot_generations=0;
    {
        // Forget the P2MR pool bookkeeping (unguarded tables: the receipts
        // stay sealed), leaving every P2MR key and watch script in place.
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_EQ(sqlite3_exec(lease->Database(),
            "DROP TABLE native_v3_p2mr_pool;DROP TABLE IF EXISTS native_v3_p2mr_target;",
            nullptr,nullptr,nullptr),SQLITE_OK);
        taproot_generations=NativeTestCount(use->Wallet(),"SELECT MAX(generation) FROM native_v3_address_pool");
    }
    ExpectNativeReceiptsCurrent(f,102);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto before=NativeIssue(request,"p2mr");
    EXPECT_EQ(before["error_code"].asString(),"wallet_native_address_pool_extending") << before.toStyledString();
    Deliver(f);   // extension (P2MR only) + fill + coverage + reconcile
    ASSERT_EQ(NativeState(f),WalletManager::NativeV3State::Complete);
    EXPECT_FALSE(P2MRTargetOpen(f));
    EXPECT_EQ(P2MRPoolRows(f,"generation=0 AND issued=1"),WalletManager::kNativeV3ReceivePool)
        << "every pre-existing P2MR key (the enrollment pool) is adopted as issued";
    EXPECT_EQ(P2MRPoolRows(f,"issued=0"),WalletManager::kNativeV3ReceivePool);
    EXPECT_EQ(P2MRPoolRows(f,"issued=0 AND generation="+std::to_string(taproot_generations+1)),
        WalletManager::kNativeV3ReceivePool);
    ExpectNativeReceiptsCurrent(f,102);
    const auto after=NativeIssue(request,"p2mr");
    ASSERT_TRUE(after.isMember("address")) << after.toStyledString();
    ExpectNativeReceiptsCurrent(f,102);
}

// Crash points of the cross-store fill: after the durable intent (no key
// row yet) and after the key-store write (no adoption yet). Coverage refuses
// meanwhile; the next worker pass repairs and finishes, and the wallet
// reseals with exactly the owed pool.
TEST(WalletV3NativeP2MRPool, InterruptedFillRepairsAndReseals) {
    for(const bool store_written:{false,true}) {
        SCOPED_TRACE(store_written?"after key-store write":"after intent");
        CanonicalRecoveryFixture f(false,false);
        (void)f.MineToWallet();
        NativeV3IndexScope v3(f,store_written?"v3-p2mr-crash-store":"v3-p2mr-crash-intent");
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        int64_t owed=0;
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            ASSERT_FALSE(lease->BeginNativeTransparentMigrationV3(index->Index(),session).empty());
            const auto missing=lease->NativeP2MRTargetMissing();
            ASSERT_EQ(missing.size(),size_t(WalletManager::kNativeV3ReceivePool));
            owed=missing.front();
            auto seed=lease->CopyRecoverySeed(session);
            lease->BeginP2MRIssuance(*seed,"m/88'/1448'/0'/0/"+std::to_string(owed),0);
            // A complete-state wallet never accepts an issuance intent.
        }
        if(store_written) {
            namespace v7rpc=dinero::rpc::v7;
            v7rpc::GetNewP2MRAddressParams p{};
            p.wallet_id=1;p.hrp=HrpForActiveNetworkRef();p.address_index=int(owed);
            auto bip32=use->Wallet().DeriveV7Bip32Material(0,0,uint32_t(owed));
            auto master=use->Wallet().GetV7PqMasterKey();
            ASSERT_TRUE(bip32);ASSERT_TRUE(master);
            p.bip32_priv=bip32->private_key;p.bip32_chain=bip32->chain_code;
            std::memcpy(p.master_key.data(),master->data(),32);
            wallet::V7P2MRStore store;
            ASSERT_EQ(store.Open(use->Wallet().GetV7P2MRStorePath()),wallet::V7P2MRStore::OpenResult::Ok);
            ASSERT_EQ(v7rpc::GetNewP2MRAddress(store,p).status,v7rpc::HandlerStatus::Ok);
        }
        // Coverage must not seal anything while the fill is owed.
        const auto coverage=f.f.service->getRuntimeWalletCoverageV3(use->Wallet(),session,index->Index());
        ASSERT_TRUE(coverage.ok());
        EXPECT_THROW(RuntimeOrdinaryDelivery::ReconcileNativeTransparentCoverageV3(
            use->Wallet(),index->Index(),**coverage,[&](const auto& commit){
                auto selected=f.f.service->AcquireBlockIngressActivationLock();commit();}),
            std::runtime_error);
        EXPECT_FALSE(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),index->Index(),session));
        use.reset();index.reset();
        Deliver(f);
        ASSERT_EQ(NativeState(f),WalletManager::NativeV3State::Complete);
        EXPECT_FALSE(P2MRTargetOpen(f));
        EXPECT_EQ(P2MRPoolRows(f,"issued=0 AND generation=0"),WalletManager::kNativeV3ReceivePool);
        EXPECT_EQ(P2MRPoolRows(f,"idx="+std::to_string(owed)),1);
        ExpectNativeReceiptsCurrent(f,102);
    }
}

// A restored, unencrypted wallet has no PQ master on this node: its pool is
// Taproot-only, P2MR is refused honestly and never makes the pool "low", so
// the worker does not loop extending it.
TEST(WalletV3NativeP2MRPool, NoPqMasterKeepsTaprootPoolAndRefusesP2MR) {
    CanonicalRecoveryFixture f(true,true);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-p2mr-nomaster-index");
    ExecutionContext ctx;ctx.daemon=&f.context;
    din::Json params(Json::arrayValue);
    params.append("restored-nomaster");
    params.append("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about");
    params.append("");params.append("");
    const auto restored=rpc_context_wallet_restore(ctx,params);
    ASSERT_FALSE(restored.isMember("error")) << restored.toStyledString();
    Deliver(f);
    ASSERT_EQ(NativeState(f),WalletManager::NativeV3State::Complete);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        ASSERT_FALSE(use->Wallet().GetV7PqMasterKey().has_value());
        EXPECT_FALSE(use->Wallet().AcquireDatabaseLease()->NativeV3PoolNeedsExtension());
    }
    ctx.walletName="restored-nomaster";
    const auto p2mr=NativeIssue(ctx,"p2mr");
    EXPECT_EQ(p2mr["error_code"].asString(),"wallet_native_p2mr_unavailable_on_v3") << p2mr.toStyledString();
    const auto taproot=NativeIssue(ctx);
    EXPECT_TRUE(taproot.isMember("address")) << taproot.toStyledString();
    Deliver(f);
    EXPECT_EQ(NativeState(f),WalletManager::NativeV3State::Complete) << "no P2MR extension loop";
}

// A MATURE P2MR coin on a native v3 wallet (unshielded to a pooled P2MR
// address): the balance counts it and a native send either spends it
// correctly or leaves it out cleanly; it never fails the send.
TEST(WalletV3NativeP2MRPool, MatureP2MRCoinIsCountedAndSendable) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-p2mr-spend-index");
    Deliver(f);
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    Deliver(f);
    const auto p2mr=NativeIssue(ctx,"p2mr");
    ASSERT_TRUE(p2mr.isMember("address")) << p2mr.toStyledString();
    din::Json request,output;
    request["account"]=Json::UInt64(3);request["expected_revision"]=Json::UInt64(f.Account().revision);
    std::vector<uint8_t> id(32);id[0]=0x95;request["request_id"]=util::hex(id);
    request["fee_una"]=Json::UInt64(10000);request["payments"]=din::arr();request["outputs"]=din::arr();
    output["address"]=p2mr["address"];output["amount_una"]=Json::UInt64(300000);request["outputs"].append(output);
    ASSERT_FALSE(rpc_context_wallet_orchard_queuespend(ctx,request).isMember("error"));
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
    }
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
    const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
    ASSERT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
    const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
    ASSERT_TRUE(bool(pooled));ASSERT_TRUE(f.Mine(pooled->tx));
    Deliver(f);
    const auto funded=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(funded.isMember("error")) << funded.toStyledString();
    EXPECT_NEAR(funded["spendable"].asDouble()*1e8,300000.0,0.5) << "the P2MR coin is counted: " << funded.toStyledString();
    din::Json payment=din::obj();
    payment["address"]=OrchardMiningPayout;payment["amount_una"]=Json::UInt64(100000);payment["fee_rate"]=1;
    const auto sent=rpc_context_wallet_sendtoaddress(ctx,payment);
    if(sent.isMember("txid")) {
        const auto tx=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(sent["txid"].asString()));
        ASSERT_TRUE(bool(tx)) << "a signed P2MR spend is admitted";
        ASSERT_TRUE(f.Mine(tx->tx));
        Deliver(f);
        const auto after=rpc_context_wallet_getbalance(ctx,din::obj());
        ASSERT_FALSE(after.isMember("error")) << after.toStyledString();
        EXPECT_LT(after["spendable"].asDouble()*1e8,200000.0) << after.toStyledString();
    } else {
        ADD_FAILURE() << "native send with only a P2MR coin failed: " << sent.toStyledString();
    }
    f.context.tx_ingress=nullptr;
}

} // namespace dinero
