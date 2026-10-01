#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "wallet/orchard_ownership_inventory.h"
#include <cstring>
namespace dinero {
namespace {
using OwnershipInventory=wallet::OrchardOwnershipInventory;
struct InventoryTestTransaction {
    sqlite3* db;bool committed=false;
    explicit InventoryTestTransaction(sqlite3* p):db(p){OrchardAdmissionFixture::Require(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);}
    ~InventoryTestTransaction(){if(!committed)EXPECT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);}
    void Commit(){OrchardAdmissionFixture::Require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);committed=true;}
};
struct OwnershipInventoryFixture : OrchardCatalogRecoveryFixture {
    template<class F> auto WithDatabase(F&& function){
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());return function(lease->Database(),seed->Bytes());
    }
    auto Read(){return WithDatabase([](sqlite3* db,std::span<const uint8_t> seed){
        InventoryTestTransaction tx(db);auto result=OwnershipInventory::Read(db,seed);
        EXPECT_FALSE(sqlite3_get_autocommit(db));return result;
    });}
    // A real plan over the existing fixture's mature coin. This tests durable
    // reservation ownership, not a new ordinary-wallet coin selection RPC.
    auto Reserve(uint32_t number,uint8_t tag){
        auto account=Account(number);const auto keys=AccountKeys(number);
        const OutPoint point(f.blocks[1].vtx.front().GetTxid(),0);
        const auto& coin=f.replay->ProvenUtxos().at(point);
        orchard::ResolvedInput input{};std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
        input.output_index=0;input.sequence=UINT32_MAX;input.amount_una=coin.value.GetUna();input.script_pub_key=f.script;
        const std::vector<orchard::WalletPayment> payments{{500000,keys.Receiver(orchard::WalletScope::External,{})}};
        const std::vector<orchard::TransparentOutput> outputs{{input.amount_una-600000,input.script_pub_key}};
        const std::vector<orchard::ResolvedInput> inputs{input};
        const auto signing=orchard::SigningContext::Create(Domain(),0,inputs,outputs,100000);
        auto plan=orchard::WalletBundlePlan::PrepareShield(keys,payments);orchard::Hash operation{tag};
        return account.account.Reserve(operation,plan.Intent(signing));
    }
};
}
TEST(OrchardOwnershipInventory, GeneratedEmptyAndNonconsecutiveEncryptedReopen){
    OwnershipInventoryFixture f;const auto before=f.Snapshot();ASSERT_EQ(f.StorageTables(),0);
    const auto empty=f.Read();EXPECT_TRUE(empty.catalog.generated);EXPECT_TRUE(empty.accounts.empty());EXPECT_TRUE(empty.current_inputs.empty());
    EXPECT_EQ(empty.current_rows,0u);EXPECT_EQ(empty.retained_rows,0u);EXPECT_EQ(f.StorageTables(),0);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TABLE orchard_wallet_retained(wallet_id BLOB,account INTEGER,revision INTEGER,sealed BLOB)");
    EXPECT_THROW((void)f.Read(),std::runtime_error);
    f.Sql("DROP TABLE orchard_wallet_retained");
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));ASSERT_FALSE(f.Issue(17).isMember("error"));
    const auto owned=f.Read();ASSERT_EQ(owned.accounts.size(),2u);EXPECT_EQ(owned.accounts[0].entry.account,3u);EXPECT_EQ(owned.accounts[1].entry.account,17u);
    EXPECT_EQ(owned.current_rows,2u);EXPECT_GT(owned.retained_rows,0u);EXPECT_FALSE(owned.accounts[1].retained_accounts.empty());EXPECT_TRUE(owned.current_inputs.empty());
    const auto stable=f.Snapshot();
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_THROW((void)f.Read(),std::runtime_error);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    const auto reopened=f.Read();EXPECT_EQ(reopened.catalog,owned.catalog);EXPECT_EQ(reopened.wallet_id,owned.wallet_id);EXPECT_EQ(reopened.retained_rows,owned.retained_rows);EXPECT_EQ(f.Snapshot(),stable);
}
TEST(OrchardOwnershipInventory, MissingDeclaredForeignAndMalformedRowsRefuseWholeRead){
    OwnershipInventoryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));ASSERT_FALSE(f.Issue(17).isMember("error"));const auto before=f.Snapshot();
    for(const auto* sql:{"DELETE FROM orchard_wallet_snapshots WHERE account=17",
        "INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) SELECT randomblob(32),account,revision,sealed FROM orchard_wallet_snapshots LIMIT 1",
        "UPDATE orchard_wallet_retained SET sealed=zeroblob(length(sealed))",
        "UPDATE orchard_wallet_retained SET account=-1",
        "DELETE FROM settings WHERE key='orchard_account_catalog_v1'"}){
        f.WithDatabase([&](sqlite3* db,std::span<const uint8_t> seed){InventoryTestTransaction tx(db);
            ASSERT_EQ(sqlite3_exec(db,sql,nullptr,nullptr,nullptr),SQLITE_OK);
            EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);
            EXPECT_FALSE(sqlite3_get_autocommit(db));
        });EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read().accounts.size(),2u);
    }
}
TEST(OrchardOwnershipInventory, AuthenticatedMalformedRetainedRefusesSelectedCatalogCaller){
    OwnershipInventoryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    f.WithDatabase([&](sqlite3* db,std::span<const uint8_t> seed){InventoryTestTransaction tx(db);
        const auto before=OwnershipInventory::Read(db,seed);const auto identity=before.accounts[1].identity;
        orchard::WalletSnapshotStore store(db,identity,seed);auto saved=store.Read();OrchardAdmissionFixture::Require(bool(saved));
        const std::array<uint8_t,8> malformed{'b','a','d','-','a','c','c','t'};
        const auto bad=store.StageReplaceRetaining(saved->revision,orchard::WalletStateBytes(malformed));
        (void)store.StageReplaceRetaining(bad,saved->state); // Valid current owner, invalid authenticated older payload.
        EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);tx.Commit();
    });
    const auto before=f.Snapshot();EXPECT_THROW((void)f.Read(),std::runtime_error);
    EXPECT_THROW((void)f.Owned(),std::runtime_error);
    EXPECT_TRUE(f.Issue(3).isMember("error"));EXPECT_TRUE(f.Call(29).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardOwnershipInventory, CurrentTransparentInputsAndCrossAccountConflict){
    OwnershipInventoryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto first=f.Reserve(3,31),second=f.Reserve(17,32);const auto before=f.Snapshot();
    f.WithDatabase([&](sqlite3* db,std::span<const uint8_t> seed){InventoryTestTransaction tx(db);
        const auto initial=OwnershipInventory::Read(db,seed);
        orchard::WalletSnapshotStore a(db,initial.accounts[0].identity,seed),b(db,initial.accounts[1].identity,seed);
        (void)a.StageReplaceRetaining(initial.accounts[0].current.revision,first.Encode());
        const auto owned=OwnershipInventory::Read(db,seed);ASSERT_EQ(owned.current_inputs.size(),1u);
        EXPECT_EQ(owned.current_inputs[0].account,3u);EXPECT_EQ(owned.current_inputs[0].operation,orchard::Hash{31});
        EXPECT_EQ(owned.current_inputs[0].input.amount_una,first.Operations().Entries().at(orchard::Hash{31}).inputs[0].amount_una);
        (void)b.StageReplaceRetaining(initial.accounts[1].current.revision,second.Encode());
        EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
    });EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.Read().current_inputs.empty());
}
TEST(OrchardOwnershipInventory, ReadErrorsAndInterruptedTerminalStepPreserveCaller){
    OwnershipInventoryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Issue(3).isMember("error"));const auto before=f.Snapshot();
    f.WithDatabase([&](sqlite3* db,std::span<const uint8_t> seed){
        EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);EXPECT_TRUE(sqlite3_get_autocommit(db));
        InventoryTestTransaction tx(db);
        struct Cleanup{sqlite3* db;~Cleanup(){sqlite3_set_authorizer(db,nullptr,nullptr);sqlite3_trace_v2(db,0,nullptr,nullptr);}} cleanup{db};
        sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
        EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_FALSE(sqlite3_get_autocommit(db));
        struct Interrupt{sqlite3* db;bool hit=false;} interrupt{db};
        sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* context,void* statement,void*){
            auto& state=*static_cast<Interrupt*>(context);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if(!state.hit&&sql&&std::strstr(sql,"FROM orchard_wallet_retained ORDER BY")){state.hit=true;sqlite3_interrupt(state.db);}return 0;},&interrupt);
        EXPECT_THROW((void)OwnershipInventory::Read(db,seed),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupt.hit);EXPECT_FALSE(sqlite3_get_autocommit(db));
        EXPECT_EQ(OwnershipInventory::Read(db,seed).accounts.size(),1u);
    });EXPECT_EQ(f.Snapshot(),before);
}
} // namespace dinero
#endif
