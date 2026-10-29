#pragma once
din::Json rpc_context_wallet_restore(const ExecutionContext&,const din::Json&);
namespace dinero {
TEST(WalletRestorePreflight, UnavailableSnapshotHistoryPreservesExistingWalletBeforeRestore) {
    WalletIndexOwnerFixture f;
    auto wallet=std::make_shared<WalletService>();ASSERT_TRUE(wallet->Init(f.context));f.context.wallet=wallet;
    {
        auto use=WalletService::AcquireWalletUse(wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.source);
        use->Wallet().setUTXOIndex(&index->Index());
        use->Wallet().create("funded-original");
        ASSERT_TRUE(use->Wallet().addUTXO(std::string(64,'a'),0,12345,"sentinel","5120",1,false));
    }
    // Model the exact source mismatch: selected snapshot chain is ahead of the
    // durable archival tip. No claim is made that these fixture headers validate.
    CBlockIndex selected(SelectedGenesis().header,7);
    ShieldedStateStartupTestAccess::Select(*f.source,selected);
    {
        auto index=ChainstateService::AcquireWalletIndexUse(f.source);
        index->Index().ApplyAtomically([&]{
            if(!index->Index().SetMetadata("wallet_snapshot_recovery_base_height","6"))
                throw std::runtime_error("fixture recovery base");
        });
        auto use=WalletService::AcquireWalletUse(wallet);
        use->Wallet().setSetting("assumeutxo_wallet_recovery_base","6");
    }
    std::string recovery_error;
    EXPECT_FALSE(wallet->RecoverActiveWalletFromSnapshotIfNeeded(&recovery_error));
    EXPECT_FALSE(recovery_error.empty());
    ExecutionContext request;request.daemon=&f.context;
    auto params=din::obj();params["name"]="restore-candidate";
    params["mnemonic"]="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    params["rescan"]=true;
    const auto reply=rpc_context_wallet_restore(request,params);
    EXPECT_TRUE(reply.isMember("error"));
    EXPECT_TRUE(reply.isMember("recovery_required")&&reply["recovery_required"].asBool());
    {
        auto use=WalletService::AcquireWalletUse(wallet);
        EXPECT_EQ(use->Wallet().getCurrentWalletName(),"funded-original");
        EXPECT_FALSE(use->Wallet().exists("restore-candidate"));
        EXPECT_EQ(use->Wallet().getTransactionHistory().size(),0u);
        auto lease=use->Wallet().AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),"SELECT amount FROM utxos",-1,&q,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(q),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int64(q,0),12345);
        EXPECT_EQ(sqlite3_step(q),SQLITE_DONE);sqlite3_finalize(q);
    }
    wallet->Stop();f.context.wallet.reset();
}
} // namespace dinero
