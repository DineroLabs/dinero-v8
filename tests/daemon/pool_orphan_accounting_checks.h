#pragma once
#include "pool/pool_manager.h"
#include <sqlite3.h>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <memory>
#include <cstring>

namespace dinero::pool {
struct PoolOrphanAccountingTestAccess {
    static sqlite3* Database(PoolDB& db) {return db.db_;}
};
}
namespace pool_orphan_accounting_checks {
using namespace dinero::pool;
using Rows=std::vector<std::vector<std::string>>;
void Sql(sqlite3* db,const std::string& sql) {
    char* error=nullptr;int rc=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&error);
    std::string message=error?error:"";sqlite3_free(error);
    if(rc!=SQLITE_OK)throw std::runtime_error("fixture SQL: "+message);
}
Rows Read(sqlite3* db,const std::string& sql) {
    sqlite3_stmt* raw=nullptr;
    if(sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("fixture read prepare");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(raw,sqlite3_finalize);
    Rows rows;int rc;
    while((rc=sqlite3_step(raw))==SQLITE_ROW) {
        std::vector<std::string> row;
        for(int c=0;c<sqlite3_column_count(raw);++c) {
            auto type=sqlite3_column_type(raw,c);const auto* data=sqlite3_column_blob(raw,c);int size=sqlite3_column_bytes(raw,c);
            row.push_back(std::to_string(type)+":"+(data?std::string(static_cast<const char*>(data),size):std::string{}));
        }
        rows.push_back(std::move(row));
    }
    if(rc!=SQLITE_DONE)throw std::runtime_error("fixture read incomplete");
    return rows;
}
struct Fixture {
    std::filesystem::path root;
    std::unique_ptr<PoolManager> manager;
    const std::string hash=std::string(64,'a');
    Fixture() {
        root=std::filesystem::temp_directory_path()/("pool_orphan_accounting_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("fixture directory");
        open();
        auto& db=manager->getDatabase();
        for(const auto& w:{"a","b","c","d"})db.getOrCreateWorker(w,"address-"+std::string(w));
        if(!db.addWorkerPending("a",100)||!db.addWorkerPending("b",10)||!db.addWorkerPending("c",9)||!db.addWorkerPending("d",8))throw std::runtime_error("fixture balances");
        PoolBlock block;block.block_hash=hash;block.height=7;block.finder_worker="a";block.finder_address="address-a";block.found_at=123;
        if(!db.insertBlock(block))throw std::runtime_error("fixture block");
        const uint64_t amounts[]={40,20,5,7};
        for(int i=0;i<4;++i) {
            Payout payout;payout.block_id=block.block_id;payout.worker_id=std::string(1,'a'+i);payout.wallet_address="address-"+payout.worker_id;
            payout.amount=amounts[i];payout.status=static_cast<PayoutStatus>(i);payout.calculated_at=456;
            if(!db.insertPayout(payout))throw std::runtime_error("fixture payout");
        }
        Sql(raw(),"UPDATE payouts SET txid='original-tx',error_message='original-error',paid_at=789,retry_count=2,last_retry_at=234");
    }
    ~Fixture(){manager.reset();std::error_code ec;std::filesystem::remove_all(root,ec);}
    void open(){manager=std::make_unique<PoolManager>((root/"pool.sqlite").string());if(!manager->initialize())throw std::runtime_error("fixture initialize");}
    sqlite3* raw(){return PoolOrphanAccountingTestAccess::Database(manager->getDatabase());}
    Rows state(){Rows result;for(const auto& table:{"blocks","workers","payouts"}){auto rows=Read(raw(),std::string("SELECT * FROM ")+table+" ORDER BY 1");result.insert(result.end(),rows.begin(),rows.end());}return result;}
};
TEST(PoolOrphanAccounting, AtomicPolicyAndReopen) {
    Fixture f;auto paid=Read(f.raw(),"SELECT * FROM payouts WHERE status IN (2,3) ORDER BY payout_id");
    ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));
    EXPECT_EQ(Read(f.raw(),"SELECT pending_payout FROM workers ORDER BY worker_id"),(Rows{{"1:60"},{"1:0"},{"1:9"},{"1:8"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT orphaned FROM blocks"),(Rows{{"1:1"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT status,txid,error_message,paid_at,amount,retry_count,last_retry_at FROM payouts WHERE payout_id=1"),(Rows{{"1:3","3:","3:orphaned block","1:0","1:40","1:2","1:234"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT * FROM payouts WHERE payout_id IN (3,4) ORDER BY payout_id"),paid);
    auto after=f.state();ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));EXPECT_EQ(f.state(),after);
    f.manager.reset();f.open();EXPECT_EQ(f.state(),after);ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));EXPECT_EQ(f.state(),after);
    PoolDB::OrphanResult report{true,99,99};ASSERT_TRUE(f.manager->getDatabase().reconcileOrphanedBlock(std::string(64,'b'),report));EXPECT_FALSE(report.found);EXPECT_EQ(report.pending_reversed,0u);EXPECT_EQ(f.state(),after);
}
TEST(PoolOrphanAccounting, RequiredWritesAndCommitRollback) {
    for(int failure=0;failure<3;++failure) {
        Fixture f;
        if(failure==0)Sql(f.raw(),"CREATE TRIGGER refuse_second BEFORE UPDATE ON payouts WHEN OLD.payout_id=2 BEGIN SELECT RAISE(ABORT,'fixture write refusal'); END");
        if(failure==1)Sql(f.raw(),"CREATE TRIGGER refuse_block BEFORE UPDATE ON blocks BEGIN SELECT RAISE(ABORT,'fixture block refusal'); END");
        auto before=f.state();PoolDB::OrphanResult result{true,99,98};
        struct Commit {PoolDB::OrphanResult* result;bool called=false,unchanged=false;};Commit commit{&result};
        if(failure==2)sqlite3_commit_hook(f.raw(),[](void* p)->int{auto& c=*static_cast<Commit*>(p);c.called=true;c.unchanged=c.result->found&&c.result->pending_reversed==99&&c.result->already_paid==98;return 1;},&commit);
        EXPECT_FALSE(f.manager->getDatabase().reconcileOrphanedBlock(f.hash,result));
        sqlite3_commit_hook(f.raw(),nullptr,nullptr);
        EXPECT_EQ(result.pending_reversed,99u);EXPECT_EQ(result.already_paid,98u);EXPECT_EQ(f.state(),before);EXPECT_EQ(sqlite3_get_autocommit(f.raw()),1);
        if(failure==2){EXPECT_TRUE(commit.called);EXPECT_TRUE(commit.unchanged);}
        if(failure==0)Sql(f.raw(),"DROP TRIGGER refuse_second");if(failure==1)Sql(f.raw(),"DROP TRIGGER refuse_block");
        ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));
    }
}
TEST(PoolOrphanAccounting, CompleteTypedInventoryAndReadRefusal) {
    const char* changes[]={"UPDATE payouts SET amount=-1 WHERE payout_id=4","UPDATE payouts SET status=4 WHERE payout_id=4","UPDATE payouts SET amount='malformed' WHERE payout_id=4","UPDATE payouts SET worker_id='missing' WHERE payout_id=2","UPDATE workers SET pending_payout='malformed' WHERE worker_id='b'","UPDATE blocks SET orphaned=2","UPDATE payouts SET worker_id=CAST(X'620063' AS TEXT) WHERE payout_id=2"};
    for(const auto* sql:changes){Fixture f;Sql(f.raw(),sql);auto before=f.state();EXPECT_FALSE(f.manager->markBlockOrphaned(f.hash));EXPECT_EQ(f.state(),before);}
    Fixture f;auto before=f.state();
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*)->int{return action==SQLITE_READ&&table&&std::strcmp(table,"payouts")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_FALSE(f.manager->markBlockOrphaned(f.hash));sqlite3_set_authorizer(f.raw(),nullptr,nullptr);EXPECT_EQ(f.state(),before);
    struct Interrupt {sqlite3* db;int rows=0;};Interrupt interrupt{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* context,void* statement,void*)->int{
        auto& i=*static_cast<Interrupt*>(context);const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(sql&&std::strstr(sql,"FROM payouts p LEFT JOIN")&&++i.rows==2)sqlite3_interrupt(i.db);
        return 0;
    },&interrupt);
    EXPECT_FALSE(f.manager->markBlockOrphaned(f.hash));sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_GE(interrupt.rows,2);EXPECT_EQ(f.state(),before);
    ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));
}
TEST(PoolOrphanAccounting, BorrowedTransactionAndDurabilityOwner) {
    Fixture f;auto before=f.state();auto sync=Read(f.raw(),"PRAGMA synchronous");
    Sql(f.raw(),"BEGIN IMMEDIATE");Sql(f.raw(),"UPDATE workers SET last_seen=42 WHERE worker_id='d'");auto borrowed=f.state();
    EXPECT_FALSE(f.manager->markBlockOrphaned(f.hash));EXPECT_EQ(sqlite3_get_autocommit(f.raw()),0);EXPECT_EQ(f.state(),borrowed);Sql(f.raw(),"ROLLBACK");EXPECT_EQ(f.state(),before);EXPECT_EQ(Read(f.raw(),"PRAGMA synchronous"),sync);
    // Existing recursive DB-only transaction bodies remain usable. The new
    // transition refuses to commit or roll back that caller's transaction.
    ASSERT_TRUE(f.manager->getDatabase().runInTransaction([&]{PoolDB::OrphanResult result;return !f.manager->getDatabase().reconcileOrphanedBlock(f.hash,result)&&f.manager->getDatabase().addWorkerPending("a",1);}));
    EXPECT_EQ(Read(f.raw(),"SELECT pending_payout FROM workers WHERE worker_id='a'"),(Rows{{"1:101"}}));
    struct Sync {bool full=false,began=false,committed=false;};Sync observed;
    sqlite3_set_authorizer(f.raw(),[](void* context,int action,const char* name,const char* value,const char*,const char*)->int{
        auto& s=*static_cast<Sync*>(context);
        if(action==SQLITE_PRAGMA&&name&&std::strcmp(name,"synchronous")==0&&value&&std::strcmp(value,"FULL")==0)s.full=true;
        if(action==SQLITE_TRANSACTION&&name&&std::strcmp(name,"BEGIN")==0)s.began=s.full;
        if(action==SQLITE_TRANSACTION&&name&&std::strcmp(name,"COMMIT")==0)s.committed=s.began;
        return SQLITE_OK;
    },&observed);
    ASSERT_TRUE(f.manager->markBlockOrphaned(f.hash));sqlite3_set_authorizer(f.raw(),nullptr,nullptr);
    EXPECT_TRUE(observed.full);EXPECT_TRUE(observed.began);EXPECT_TRUE(observed.committed);EXPECT_EQ(Read(f.raw(),"PRAGMA synchronous"),sync);
}
} // namespace pool_orphan_accounting_checks
