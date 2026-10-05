#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct WalletDetachedReadTestAccess {
    // Same-thread observation only; no racing access or synchronization control.
    static bool Released(const WalletManager& wallet) {
        return wallet.database_leases_==0&&wallet.recovery_seeds_==0;
    }
};
TEST(OrchardDetachedRead, RestoresRealFundedAccountAfterReleasingItsOwners) {
    OrchardCreationFixture fixture;
    ASSERT_FALSE(fixture.Call().isMember("error"));
    const auto keys=fixture.AccountKeys();const auto [body,bundle]=fixture.Shield(keys);
    (void)fixture.Mine(body);fixture.ReplayAccount();
    const auto before=fixture.Account();ASSERT_EQ(before.account.Scan().BalanceUna(),500000u);
    const auto saved=fixture.Snapshot();
    const auto replay=fixture.f.service->getRuntimeAccountReplay();ASSERT_TRUE(replay.ok());
    auto use=WalletService::AcquireWalletUse(fixture.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    auto point=(*replay)->Point((*replay)->Head());const auto original=point.lookups.origin;size_t calls=0;
    point.lookups.origin=[&](uint32_t height,const uint256& hash,const orchard::Hash& txid){
        ++calls;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        return original(height,hash,txid);
    };
    const auto restored=wallet::OrchardAccountDelivery::Read(wallet,session,{fixture.Domain(),102,3},point);
    EXPECT_GT(calls,0u);EXPECT_EQ(restored.revision,before.revision);
    EXPECT_EQ(OrchardCreationBytes(restored.account),OrchardCreationBytes(before.account));
    EXPECT_EQ(fixture.Snapshot(),saved);EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
    const auto replay_read=wallet::OrchardAccountDelivery::ReadForReplay(wallet,session,{fixture.Domain(),102,3},**replay);
    EXPECT_EQ(replay_read.revision,before.revision);
    EXPECT_EQ(OrchardCreationBytes(replay_read.account),OrchardCreationBytes(before.account));
}
TEST(OrchardDetachedRead, FinalAuthenticationReadRefusalReturnsNoStaleResult) {
    OrchardCreationFixture fixture;ASSERT_FALSE(fixture.Call().isMember("error"));
    const auto keys=fixture.AccountKeys();const auto [body,bundle]=fixture.Shield(keys);
    (void)fixture.Mine(body);fixture.ReplayAccount();const auto saved=fixture.Snapshot();
    const auto replay=fixture.f.service->getRuntimeAccountReplay();ASSERT_TRUE(replay.ok());
    auto use=WalletService::AcquireWalletUse(fixture.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    struct ResetAuthorizer {sqlite3* db;~ResetAuthorizer(){sqlite3_set_authorizer(db,nullptr,nullptr);}} reset{db};
    auto point=(*replay)->Point((*replay)->Head());const auto original=point.lookups.origin;bool called=false;
    point.lookups.origin=[&](uint32_t height,const uint256& hash,const orchard::Hash& txid){
        called=true;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        const auto result=original(height,hash,txid);
        EXPECT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"?SQLITE_DENY:SQLITE_OK;
        },nullptr),SQLITE_OK);
        return result;
    };
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::Read(wallet,session,{fixture.Domain(),102,3},point),std::runtime_error);
    EXPECT_TRUE(called);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
    ASSERT_EQ(sqlite3_set_authorizer(db,nullptr,nullptr),SQLITE_OK);EXPECT_EQ(fixture.Snapshot(),saved);
    const auto retried=wallet::OrchardAccountDelivery::ReadForReplay(wallet,session,{fixture.Domain(),102,3},**replay);
    EXPECT_EQ(retried.account.Scan().BalanceUna(),500000u);EXPECT_EQ(fixture.Snapshot(),saved);
}
TEST(OrchardDetachedRead, SessionAndBorrowedTransactionRefuseBeforeRestoration) {
    OrchardCreationFixture fixture;ASSERT_FALSE(fixture.Call().isMember("error"));const auto saved=fixture.Snapshot();
    const auto replay=fixture.f.service->getRuntimeAccountReplay();ASSERT_TRUE(replay.ok());
    auto use=WalletService::AcquireWalletUse(fixture.wallet);auto& wallet=use->Wallet();
    auto lease=wallet.AcquireDatabaseLease();const auto session=lease->Session();auto* db=lease->Database();
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadForReplay(wallet,session+1,{fixture.Domain(),102,3},**replay),std::runtime_error);
    ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadForReplay(wallet,session,{fixture.Domain(),102,3},**replay),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    lease.reset();EXPECT_EQ(fixture.Snapshot(),saved);
}
TEST(OrchardDetachedRead, MissingPersistentIdentityNeverEnrollsAReplacement) {
    OrchardCreationFixture fixture;ASSERT_FALSE(fixture.Call().isMember("error"));
    const auto replay=fixture.f.service->getRuntimeAccountReplay();ASSERT_TRUE(replay.ok());
    fixture.Sql("UPDATE wallet_meta SET runtime_delivery_id=NULL WHERE id=1");
    const auto missing=fixture.Snapshot();
    auto use=WalletService::AcquireWalletUse(fixture.wallet);auto& wallet=use->Wallet();
    const auto session=wallet.AcquireDatabaseLease()->Session();
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadForReplay(wallet,session,{fixture.Domain(),102,3},**replay),std::runtime_error);
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::Read(wallet,session,{fixture.Domain(),102,3},(*replay)->Point({})),std::runtime_error);
    EXPECT_EQ(fixture.Snapshot(),missing);
    auto lease=wallet.AcquireDatabaseLease();
    struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
    ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&q.p,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(q.p),SQLITE_ROW);EXPECT_EQ(sqlite3_column_type(q.p,0),SQLITE_NULL);
    EXPECT_EQ(sqlite3_step(q.p),SQLITE_DONE);
}
} // namespace dinero
#endif
