#include "wallet/runtime_replay_spool.h"
#include "wallet/runtime_replay_disk_membership.h"
#include <gtest/gtest.h>
#include <set>

namespace dinero::wallet::detail {
struct RuntimeReplaySpoolTestAccess {
    static sqlite3* Handle(RuntimeReplaySpool& spool){return spool.db_.get();}
};
}
namespace {
using namespace dinero::wallet::detail;
using Bytes=RuntimeReplaySpool::Bytes;
using Membership=RuntimeReplayDiskMembership;
using Key=Membership::Key;
Key SingleBit(unsigned bit){Key k{};k[bit/8]=uint8_t(1u<<(7-bit%8));return k;}
Bytes Sequence(uint64_t sequence){Bytes k{'e'};for(unsigned i=0;i<8;++i){k.push_back(uint8_t(sequence));sequence>>=8;}return k;}
TEST(RuntimeReplaySpool,OwnedCopiesCheckedWritesRollbackAndFreeze) {
    RuntimeReplaySpool spool;const Bytes key{1,0,2},value{0,9,0,8};
    EXPECT_FALSE(spool.Get(key));EXPECT_THROW(spool.Insert(key,value),std::runtime_error);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(key,value);batch.Commit();}
    auto copy=spool.Get(key);ASSERT_TRUE(copy);EXPECT_EQ(*copy,value);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{3},Bytes{});EXPECT_THROW(spool.Insert(key,Bytes{4}),std::runtime_error);}
    EXPECT_FALSE(spool.Get(Bytes{3}));EXPECT_EQ(*spool.Get(key),value);
    copy->at(0)=42;EXPECT_EQ(*spool.Get(key),value);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{3},Bytes{});batch.Commit();}
    ASSERT_TRUE(spool.Get(Bytes{3}));EXPECT_TRUE(spool.Get(Bytes{3})->empty());
    spool.Freeze();EXPECT_THROW((RuntimeReplaySpool::Batch(spool)),std::runtime_error);
    EXPECT_THROW(spool.Insert(Bytes{4},value),std::runtime_error);EXPECT_EQ(*spool.Get(key),value);
}
TEST(RuntimeReplaySpool,ReadAndCommitFailuresReturnNoInventedData) {
    RuntimeReplaySpool spool;sqlite3* db=RuntimeReplaySpoolTestAccess::Handle(spool);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{1},Bytes{2});batch.Commit();}
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW((void)spool.Get(Bytes{1}),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(*spool.Get(Bytes{1}),Bytes{2});
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{3},Bytes{4});EXPECT_THROW(batch.Commit(),std::runtime_error);}
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_FALSE(spool.Get(Bytes{3}));EXPECT_EQ(*spool.Get(Bytes{1}),Bytes{2});
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{3},Bytes{4});batch.Commit();}
    EXPECT_EQ(*spool.Get(Bytes{3}),Bytes{4});
}
TEST(RuntimeReplaySpool,MalformedStorageAndInterruptedReadRefuse) {
    RuntimeReplaySpool spool;sqlite3* db=RuntimeReplaySpoolTestAccess::Handle(spool);
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{1},Bytes{0,2,0,3});batch.Commit();}
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned kind,void* context,void* statement,void*) {
        if(kind==SQLITE_TRACE_ROW) {
            const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if(sql&&std::string(sql)=="SELECT v,tag FROM records WHERE k=?1")sqlite3_interrupt(static_cast<sqlite3*>(context));
        }
        return 0;
    },db);
    EXPECT_THROW((void)spool.Get(Bytes{1}),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_EQ(*spool.Get(Bytes{1}),(Bytes{0,2,0,3}));
    ASSERT_EQ(sqlite3_exec(db,"UPDATE records SET v='text'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)spool.Get(Bytes{1}),std::runtime_error);
    EXPECT_THROW((void)spool.Get(Bytes{}),std::runtime_error);
    EXPECT_THROW((void)spool.Get(Bytes(RuntimeReplaySpool::MaximumKeyBytes+1)),std::runtime_error);
}
TEST(RuntimeReplaySpool,PagerSpillsWhileRecordsRemainExact) {
    RuntimeReplaySpool spool;Bytes value(8192);for(size_t i=0;i<value.size();++i)value[i]=uint8_t(i);
    const int initial=spool.UsageNow().pager_bytes;
    for(uint64_t page=0;page<32;++page) {
        RuntimeReplaySpool::Batch batch(spool);
        for(uint64_t i=0;i<64;++i)spool.Insert(Sequence(page*64+i),value);
        batch.Commit();
    }
    spool.Freeze();const auto usage=spool.UsageNow();
    // Actual pager allocation, not an RSS/allocator-total claim. Sixteen MiB
    // of values must not become a sixteen MiB private pager cache.
    EXPECT_LT(usage.pager_bytes,2*1024*1024);EXPECT_LT(usage.pager_bytes-initial,2*1024*1024);
    EXPECT_EQ(usage.statement_bytes,0);
    for(uint64_t i:{0,63,64,1023,2047})EXPECT_EQ(*spool.Get(Sequence(i)),value);
    EXPECT_FALSE(spool.Get(Sequence(2048)));
}
TEST(RuntimeReplaySpool,DiskMembershipRetainsIndependentBranchesAndEverySplit) {
    RuntimeReplaySpool spool;Membership membership(spool);Membership::Root root=0;
    const Key zero{};std::vector<Membership::Root> snapshots;
    {RuntimeReplaySpool::Batch batch(spool);root=membership.With(root,zero);batch.Commit();}
    snapshots.push_back(root);
    for(unsigned bit=0;bit<256;++bit) {
        RuntimeReplaySpool::Batch batch(spool);root=membership.With(root,SingleBit(bit));batch.Commit();snapshots.push_back(root);
    }
    Membership::Root left=0,right=0;
    {RuntimeReplaySpool::Batch batch(spool);left=membership.With(snapshots[1],SingleBit(255));right=membership.With(snapshots[1],SingleBit(254));EXPECT_EQ(membership.With(left,SingleBit(255)),left);batch.Commit();}
    spool.Freeze();
    for(size_t i=0;i<snapshots.size();++i) {
        EXPECT_TRUE(membership.Contains(snapshots[i],zero));
        for(unsigned bit=0;bit<256;++bit)EXPECT_EQ(membership.Contains(snapshots[i],SingleBit(bit)),bit<i);
    }
    EXPECT_TRUE(membership.Contains(left,SingleBit(255)));EXPECT_FALSE(membership.Contains(left,SingleBit(254)));
    EXPECT_TRUE(membership.Contains(right,SingleBit(254)));EXPECT_FALSE(membership.Contains(right,SingleBit(255)));
    EXPECT_FALSE(membership.Contains(0,zero));
}
TEST(RuntimeReplaySpool,AlteredValuesKeysAndForeignStoreTagsRefuse) {
    RuntimeReplaySpool spool,foreign;
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{1},Bytes{7,0,8});batch.Commit();}
    auto* original=RuntimeReplaySpoolTestAccess::Handle(spool);auto* other=RuntimeReplaySpoolTestAccess::Handle(foreign);
    auto* copy=sqlite3_backup_init(other,"main",original,"main");ASSERT_NE(copy,nullptr);
    const int copied=sqlite3_backup_step(copy,-1),finished=sqlite3_backup_finish(copy);
    ASSERT_EQ(copied,SQLITE_DONE);ASSERT_EQ(finished,SQLITE_OK);
    EXPECT_THROW((void)foreign.Get(Bytes{1}),std::runtime_error);EXPECT_EQ(*spool.Get(Bytes{1}),(Bytes{7,0,8}));
    ASSERT_EQ(sqlite3_exec(original,"UPDATE records SET k=x'02' WHERE k=x'01'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)spool.Get(Bytes{2}),std::runtime_error);
    ASSERT_EQ(sqlite3_exec(original,"UPDATE records SET k=x'01' WHERE k=x'02'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_EQ(*spool.Get(Bytes{1}),(Bytes{7,0,8}));
    ASSERT_EQ(sqlite3_exec(original,"UPDATE records SET v=x'070009'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)spool.Get(Bytes{1}),std::runtime_error);
}
TEST(RuntimeReplaySpool,OrderedNamespaceWalkAuthenticatesEveryReturnedRow) {
    RuntimeReplaySpool spool;
    {RuntimeReplaySpool::Batch batch(spool);spool.Insert(Bytes{'g',9},Bytes{1});spool.Insert(Bytes{'h',0},Bytes{2});spool.Insert(Bytes{'h',2},Bytes{3});spool.Insert(Bytes{'i',0},Bytes{4});batch.Commit();}
    const auto first=spool.Next('h',Bytes{'h'});ASSERT_TRUE(first);EXPECT_EQ(first->first,(Bytes{'h',0}));EXPECT_EQ(first->second,Bytes{2});
    const auto second=spool.Next('h',first->first);ASSERT_TRUE(second);EXPECT_EQ(second->first,(Bytes{'h',2}));EXPECT_EQ(second->second,Bytes{3});
    EXPECT_FALSE(spool.Next('h',second->first));
    ASSERT_EQ(sqlite3_exec(RuntimeReplaySpoolTestAccess::Handle(spool),"UPDATE records SET v=x'09' WHERE k=x'6802'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)spool.Next('h',first->first),std::runtime_error);
    EXPECT_THROW((void)spool.Next('h',Bytes{'g'}),std::runtime_error);
}
} // namespace
