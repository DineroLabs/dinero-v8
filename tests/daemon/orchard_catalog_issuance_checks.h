#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardCatalogIssuance, NonconsecutiveOwnersReopenAndOnlyRequestedCounterAdvances){
    OrchardCreationFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto catalog=f.Catalog();const auto before=f.Account(3);const auto other=f.Account(17);
    const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    const auto issued=f.Issue(3);ASSERT_FALSE(issued.isMember("error"))<<issued["error"].asString();
    EXPECT_EQ(issued["address"].asString(),expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(issued["revision"].asUInt64(),before.revision+1);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),OrchardCreationBytes(other.account));EXPECT_EQ(f.Account(17).revision,other.revision);
    EXPECT_EQ(f.Catalog(),catalog);EXPECT_EQ(f.Account(3).account.Delivery(),before.account.Delivery());
    const auto unchanged=f.Snapshot();EXPECT_TRUE(f.Issue(29).isMember("error"));EXPECT_EQ(f.Snapshot(),unchanged);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_TRUE(f.Issue(17).isMember("error"));
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto reopened=f.Issue(17);ASSERT_FALSE(reopened.isMember("error"));
    const auto next=other.account.IssueReceiver(orchard::WalletScope::External);
    EXPECT_EQ(reopened["address"].asString(),next.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(f.Catalog(),catalog);EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
}
TEST(OrchardCatalogIssuance, OtherOwnerAndRequiredHistoryRefuseWithoutIssuance){
    OrchardCreationFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto catalog=f.Setting("orchard_account_catalog_v1");
    f.Sql("CREATE TEMP TABLE saved_catalog_other AS SELECT * FROM orchard_wallet_snapshots WHERE account=17");
    const auto refuse=[&](){const auto bytes=f.Snapshot();const auto result=f.Issue(3);EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("address"));EXPECT_FALSE(result.isMember("revision"));EXPECT_EQ(f.Snapshot(),bytes);EXPECT_EQ(f.Setting("orchard_account_catalog_v1"),catalog);};
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE account=17");refuse();
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_catalog_other");
    f.Sql("UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=17");refuse();
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE account=17; INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_catalog_other; DROP TABLE saved_catalog_other");
    f.Sql("INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) SELECT zeroblob(32),account,revision,sealed FROM orchard_wallet_snapshots WHERE account=17");refuse();
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE wallet_id=zeroblob(32)");
    ASSERT_FALSE(f.Issue(17).isMember("error"));
    f.Sql("CREATE TEMP TABLE saved_catalog_retained AS SELECT * FROM orchard_wallet_retained WHERE account=17");
    f.Sql("UPDATE orchard_wallet_retained SET sealed=zeroblob(length(sealed)) WHERE account=17");refuse();
    f.Sql("DELETE FROM orchard_wallet_retained WHERE account=17; INSERT INTO orchard_wallet_retained SELECT * FROM saved_catalog_retained; DROP TABLE saved_catalog_retained");
    f.ReplayAccount(17);const auto parent=f.Account(17).account.ParentSnapshotRevision();ASSERT_GT(parent,0u);
    f.Sql("CREATE TEMP TABLE saved_catalog_parent AS SELECT * FROM orchard_wallet_retained WHERE account=17 AND revision="+std::to_string(parent));
    f.Sql("DELETE FROM orchard_wallet_retained WHERE account=17 AND revision="+std::to_string(parent));refuse();
    f.Sql("INSERT INTO orchard_wallet_retained SELECT * FROM saved_catalog_parent; DROP TABLE saved_catalog_parent");
    ASSERT_FALSE(f.Issue(3).isMember("error"));EXPECT_EQ(f.Setting("orchard_account_catalog_v1"),catalog);
}
TEST(OrchardCatalogIssuance, ReadCommitAndUnknownInventoryRefuseWithoutEffects){
    OrchardCreationFixture f;const auto empty=f.Snapshot();EXPECT_TRUE(f.Issue(3).isMember("error"));EXPECT_EQ(f.Snapshot(),empty);
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));const auto established=f.Snapshot();
    auto use=WalletService::AcquireWalletUse(f.wallet);sqlite3* db;{auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    for(const char* denied_table:{"settings","orchard_wallet_snapshots","orchard_wallet_retained"}){
        sqlite3_set_authorizer(db,[](void* arg,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string_view(table)==static_cast<const char*>(arg)?SQLITE_DENY:SQLITE_OK;
        },const_cast<char*>(denied_table));
        const auto denied=f.Issue(3);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),established);
    }
    struct Interrupt{sqlite3* db;bool seen=false;} interrupt{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* arg,void* stmt,void*){
        auto& state=*static_cast<Interrupt*>(arg);const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
        if(!state.seen&&sql&&std::string_view(sql)=="SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY account"){
            state.seen=true;sqlite3_interrupt(state.db);
        }return 0;
    },&interrupt);
    const auto interrupted=f.Issue(3);sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupt.seen);EXPECT_TRUE(interrupted.isMember("error"));EXPECT_EQ(f.Snapshot(),established);
    bool commit_seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&commit_seen);
    const auto denied=f.Issue(3);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(commit_seen);EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),established);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");
    EXPECT_THROW(wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(use->Wallet(),session,{f.Domain(),102,3},**view,orchard::WalletScope::External),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Snapshot(),established);
    const auto catalog=f.Setting("orchard_account_catalog_v1");f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");
    auto before=f.Snapshot();EXPECT_TRUE(f.Issue(3).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().setSetting("orchard_account_catalog_v1","00");before=f.Snapshot();EXPECT_TRUE(f.Issue(3).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().setSetting("orchard_account_catalog_v1",catalog);ASSERT_FALSE(f.Issue(3).isMember("error"));
    use->Wallet().createFromBip39("unknown-issuance","abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    auto c=f.RequestContext();c.walletName="unknown-issuance";before=f.Snapshot();
    EXPECT_TRUE(rpc_context_wallet_orchard_getnewaddress(c,OrchardCreationRequest()).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
}
} // namespace dinero
#endif
