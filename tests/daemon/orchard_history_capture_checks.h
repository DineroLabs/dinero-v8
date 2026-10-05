#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/services/orchard_history_capture.h"
namespace dinero::wallet::detail {
struct RuntimeReplaySpoolTestAccess {
    static sqlite3* Handle(RuntimeReplaySpool& spool){return spool.db_.get();}
};
}
namespace dinero {
struct OrchardHistoryCaptureTestAccess {
    static sqlite3* Handle(OrchardHistoryCapture& capture) {
        return wallet::detail::RuntimeReplaySpoolTestAccess::Handle(capture.spool_);
    }
    static uint64_t Root(const OrchardHistoryCapture& capture){return capture.transactions_root_;}
    static uint64_t Remaining(const OrchardHistoryCapture& capture){return capture.reverse_remaining_;}
    static uint64_t Transactions(const OrchardHistoryCapture& capture){return capture.transaction_count_;}
    static void SetTransactions(OrchardHistoryCapture& capture,uint64_t n){capture.transaction_count_=n;}
};
struct OrchardBranchCaptureTestAccess {
    static void Record(OrchardBranchReplay& replay,const TxId& id) {
        replay.RecordTransactions(std::span<const TxId>(&id,1));
    }
};
namespace {
void CaptureHeaders(OrchardHistoryCapture& capture,const std::vector<Block>& blocks) {
    std::vector<OrchardHistoryCapture::Header> headers;arith_uint256 work{0};
    for(const auto& block:blocks) {
        work+=GetBlockProof(block.header.difficulty);headers.push_back({block.GetHash(),block.header,work});
    }
    std::reverse(headers.begin(),headers.end());
    for(size_t start=0;start<headers.size();start+=OrchardHistoryCapture::HeaderBatchLimit) {
        const auto count=std::min(OrchardHistoryCapture::HeaderBatchLimit,headers.size()-start);
        capture.CaptureReverse(std::span<const OrchardHistoryCapture::Header>(headers.data()+start,count));
    }
}
void CaptureBodies(OrchardHistoryCapture& capture,const std::vector<Block>& blocks) {
    for(uint32_t h=0;h<blocks.size();++h)capture.RecordBody(h,blocks[h]);capture.Finish();
}
// Capture-only material. No PoW, signatures or full-history validation claim.
Block CaptureOnlyBlock(uint32_t height,const uint256& parent,const Transaction* duplicate=nullptr) {
    Block block{};block.header.version=1;block.header.prev_block_hash=parent;
    block.header.timestamp=height+1;block.header.nonce=height;block.header.difficulty=0x207fffff;
    Transaction coinbase;coinbase.vin.resize(1);
    coinbase.vin[0].prevout=TxOutPoint(TxId(uint256{}),UINT32_MAX);
    coinbase.vin[0].scriptSig={4,uint8_t(height),uint8_t(height>>8),uint8_t(height>>16),uint8_t(height>>24)};
    coinbase.vout.emplace_back(AmountUna::Zero(),std::vector<uint8_t>{0x51});
    block.vtx.push_back(duplicate?*duplicate:coinbase);block.header.merkle_root=consensus::ComputeMerkleRoot(block.vtx);return block;
}
}
TEST(OrchardHistoryCapture, ExactHistoryAndBodyDigestsPreserveFullReplay) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    EXPECT_THROW(capture.HeaderAt(0),std::runtime_error);CaptureHeaders(capture,f.blocks);
    OrchardParentReplay replay({f.tip.height,f.tip.hash,ChainworkFromHex(f.tip.chainwork)},parent_replay_limits);
    for(uint32_t h=0;h<f.blocks.size();++h) {
        const auto header=capture.HeaderAt(h);EXPECT_EQ(header.hash,f.blocks[h].GetHash());
        EXPECT_EQ(header.header.SerializeForHash(),f.blocks[h].header.SerializeForHash());
        replay.Append(f.blocks[h],h,header.work);capture.RecordBody(h,f.blocks[h]);
    }
    replay.Finish();capture.Finish();EXPECT_EQ(replay.Record(),*f.Read());
    EXPECT_EQ(capture.Count(),f.blocks.size());
    for(uint32_t h=0;h<f.blocks.size();++h) {
        uint256 expected;crypto::CSHA256().Write(f.blocks[h].Serialize()).Finalize(expected.data);
        EXPECT_EQ(capture.WireHashAt(h),expected);
        for(const auto& tx:f.blocks[h].vtx)EXPECT_TRUE(capture.ContainsTransaction(tx.GetTxid()));
    }
    EXPECT_FALSE(capture.ContainsTransaction(TxId(uint256{})));f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, MissingAlteredRowsAndMembershipNodesRefuse) {
    OwnedSelectedParentFixture f;
    for(char kind:{'h','w','t'}) {
        OrchardHistoryCapture capture(f.tip.height,f.tip.hash);CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);
        auto* db=OrchardHistoryCaptureTestAccess::Handle(capture);
        ASSERT_EQ(sqlite3_exec(db,"PRAGMA query_only=OFF",nullptr,nullptr,nullptr),SQLITE_OK);
        const std::string sql=kind=='h'?"DELETE FROM records WHERE substr(k,1,1)=x'68'":
            kind=='w'?"DELETE FROM records WHERE substr(k,1,1)=x'77'":
            "DELETE FROM records WHERE substr(k,1,1)=x'74'";
        ASSERT_EQ(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr),SQLITE_OK);
        if(kind=='h')EXPECT_THROW(capture.HeaderAt(0),std::runtime_error);
        if(kind=='w')EXPECT_THROW(capture.WireHashAt(0),std::runtime_error);
        if(kind=='t') {
            EXPECT_THROW(capture.ContainsTransaction(f.blocks.front().vtx.front().GetTxid()),std::runtime_error);
            EXPECT_THROW(capture.ContainsTransaction(TxId(uint256{})),std::runtime_error);
        }
    }
    OrchardHistoryCapture altered(f.tip.height,f.tip.hash);CaptureHeaders(altered,f.blocks);CaptureBodies(altered,f.blocks);
    auto* db=OrchardHistoryCaptureTestAccess::Handle(altered);
    ASSERT_EQ(sqlite3_exec(db,"PRAGMA query_only=OFF; UPDATE records SET v=zeroblob(length(v)) WHERE substr(k,1,1)=x'68'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(altered.HeaderAt(0),std::runtime_error);f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, ReadFailureAndInterruptedEofReturnNoPrefix) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);auto* db=OrchardHistoryCaptureTestAccess::Handle(capture);
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(capture.HeaderAt(0),std::runtime_error);
    EXPECT_THROW(capture.ContainsTransaction(TxId(uint256{})),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned kind,void* context,void* statement,void*) {
        const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(kind==SQLITE_TRACE_ROW&&sql&&std::string(sql)=="SELECT v,tag FROM records WHERE k=?1")
            sqlite3_interrupt(static_cast<sqlite3*>(context));return 0;
    },db);
    EXPECT_THROW(capture.HeaderAt(0),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_EQ(capture.HeaderAt(0).hash,f.blocks.front().GetHash());
    EXPECT_TRUE(capture.ContainsTransaction(f.blocks.back().vtx.front().GetTxid()));f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, FailedCommitAndDuplicateBodiesNeverPublishRoot) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);CaptureHeaders(capture,f.blocks);
    capture.RecordBody(0,f.blocks[0]);const auto root=OrchardHistoryCaptureTestAccess::Root(capture);
    auto* db=OrchardHistoryCaptureTestAccess::Handle(capture);sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    EXPECT_THROW(capture.RecordBody(1,f.blocks[1]),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(OrchardHistoryCaptureTestAccess::Root(capture),root);
    EXPECT_THROW(capture.Finish(),std::runtime_error);
    EXPECT_THROW(capture.RecordBody(1,f.blocks[1]),std::runtime_error);
    const auto first=CaptureOnlyBlock(0,{});const auto second=CaptureOnlyBlock(1,first.GetHash(),&first.vtx.front());
    const std::vector<Block> duplicate{first,second};OrchardHistoryCapture repeated(1,second.GetHash());
    CaptureHeaders(repeated,duplicate);repeated.RecordBody(0,first);
    EXPECT_THROW(repeated.RecordBody(1,second),std::runtime_error);
    EXPECT_THROW(repeated.Finish(),std::runtime_error);
    f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, HeaderCommitRefusalPreservesCapturePosition) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    auto* db=OrchardHistoryCaptureTestAccess::Handle(capture);
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    EXPECT_THROW(CaptureHeaders(capture,f.blocks),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(OrchardHistoryCaptureTestAccess::Remaining(capture),f.blocks.size());
    EXPECT_THROW(capture.HeaderAt(0),std::runtime_error);
    EXPECT_THROW(capture.Finish(),std::runtime_error);
    EXPECT_THROW(CaptureHeaders(capture,f.blocks),std::runtime_error);f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, IncompleteDiscontinuousAndWrongBodyCapturesRefuse) {
    OwnedSelectedParentFixture f;
    OrchardHistoryCapture incomplete(f.tip.height,f.tip.hash);CaptureHeaders(incomplete,f.blocks);
    incomplete.RecordBody(0,f.blocks[0]);
    EXPECT_THROW(incomplete.Finish(),std::runtime_error);
    EXPECT_THROW(incomplete.RecordBody(1,f.blocks[1]),std::runtime_error);
    OrchardHistoryCapture unordered(f.tip.height,f.tip.hash);CaptureHeaders(unordered,f.blocks);
    EXPECT_THROW(unordered.RecordBody(1,f.blocks[1]),std::runtime_error);
    EXPECT_THROW(unordered.Finish(),std::runtime_error);
    OrchardHistoryCapture wrong(f.tip.height,f.tip.hash);CaptureHeaders(wrong,f.blocks);
    auto body=f.blocks[0];++body.header.nonce;
    EXPECT_THROW(wrong.RecordBody(0,body),std::runtime_error);
    // Matching header hash alone cannot bind substituted transaction IDs.
    OrchardHistoryCapture unbound(f.tip.height,f.tip.hash);CaptureHeaders(unbound,f.blocks);
    auto changed=f.blocks[0];changed.vtx.front().vout.front().value=AmountUna::Una(1);
    EXPECT_THROW(unbound.RecordBody(0,changed),std::runtime_error);
    OrchardHistoryCapture headers(f.tip.height,f.tip.hash);
    const OrchardHistoryCapture::Header wrong_header{f.blocks.front().GetHash(),f.blocks.front().header,arith_uint256(1)};
    EXPECT_THROW(headers.CaptureReverse(std::span<const OrchardHistoryCapture::Header>(&wrong_header,1)),std::runtime_error);
    EXPECT_THROW(headers.Finish(),std::runtime_error);f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, FullBranchUsesCapturedHistoryAndRejectsHistoricalDuplicates) {
    ProofHandoffFixture f;OrchardHistoryCapture capture(f.parent->height,f.parent->hash);
    CaptureHeaders(capture,f.f.blocks);CaptureBodies(capture,f.f.blocks);
    const OrchardParentReplay::Target target{103,f.second->Header().GetHash(),*f.f.db.getBlockWork(f.second->Header().GetHash())};
    OrchardBranchReplay branch(*f.replay,capture,target);
    f.Append(branch,OrchardBlockCandidate::DecodeExact(f.first->WireBytes()),102);f.Append(branch,*f.body,103);branch.Finish();
    const auto reference=f.Complete();EXPECT_EQ(branch.ProvenState(),reference->ProvenState());
    EXPECT_EQ(branch.ProvenBlock(103,target.hash).Context().block_hash,target.hash);
    OrchardBranchReplay duplicate(*f.replay,capture,target);
    EXPECT_THROW(OrchardBranchCaptureTestAccess::Record(duplicate,f.f.blocks.front().vtx.front().GetTxid()),std::runtime_error);
    // The branch-local path also preserves uniqueness independently of history.
    OrchardBranchCaptureTestAccess::Record(duplicate,f.first->Transactions().front().GetTxid());
    EXPECT_THROW(OrchardBranchCaptureTestAccess::Record(duplicate,f.first->Transactions().front().GetTxid()),std::runtime_error);
}
TEST(OrchardHistoryCapture, CompleteTransactionTraversalMatchesValidatedBodies) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);
    EXPECT_THROW(capture.ForEachTransaction([](const TxId&){return true;}),std::runtime_error);
    OrchardParentReplay replay({f.tip.height,f.tip.hash,ChainworkFromHex(f.tip.chainwork)},parent_replay_limits);
    std::set<TxId> expected;
    for(uint32_t h=0;h<f.blocks.size();++h) {
        replay.Append(f.blocks[h],h,capture.HeaderAt(h).work);capture.RecordBody(h,f.blocks[h]);
        for(const auto& tx:f.blocks[h].vtx)ASSERT_TRUE(expected.insert(tx.GetTxid()).second);
    }
    replay.Finish();capture.Finish();std::set<TxId> actual;
    EXPECT_EQ(capture.ForEachTransaction([&](const TxId& id){return actual.insert(id).second;}),expected.size());
    EXPECT_EQ(capture.TransactionCount(),expected.size());EXPECT_EQ(actual,expected);
    EXPECT_EQ(capture.UsageNow().statement_bytes,0);f.CheckUnpublished();
}
TEST(OrchardHistoryCapture, TransactionTraversalRefusesLateReadAndVisitorFailure) {
    std::vector<Block> blocks;uint256 parent;
    for(uint32_t h=0;h<32;++h){auto b=CaptureOnlyBlock(h,parent);parent=b.GetHash();blocks.push_back(std::move(b));}
    OrchardHistoryCapture capture(31,parent);CaptureHeaders(capture,blocks);CaptureBodies(capture,blocks);
    auto* db=OrchardHistoryCaptureTestAccess::Handle(capture);
    struct Reads {sqlite3* db;unsigned rows=0;} reads{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned kind,void* context,void* statement,void*) {
        auto& reads=*static_cast<Reads*>(context);const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(kind==SQLITE_TRACE_ROW&&sql&&std::string(sql)=="SELECT v,tag FROM records WHERE k=?1"&&++reads.rows==20)
            sqlite3_interrupt(reads.db);return 0;
    },&reads);
    size_t visited=0;
    EXPECT_THROW(capture.ForEachTransaction([&](const TxId&){++visited;return true;}),std::runtime_error);
    sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_GT(visited,0u);EXPECT_LT(visited,blocks.size());EXPECT_EQ(capture.UsageNow().statement_bytes,0);
    visited=0;EXPECT_THROW(capture.ForEachTransaction([&](const TxId&){return ++visited<7;}),std::runtime_error);
    EXPECT_EQ(visited,7u);
    EXPECT_THROW(capture.ForEachTransaction([](const TxId&)->bool{throw std::logic_error("visitor refused");}),std::logic_error);
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(capture.ForEachTransaction([](const TxId&){return true;}),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);visited=0;
    EXPECT_EQ(capture.ForEachTransaction([&](const TxId&){++visited;return true;}),blocks.size());
    EXPECT_EQ(visited,blocks.size());EXPECT_EQ(capture.UsageNow().statement_bytes,0);
}
TEST(OrchardHistoryCapture, MissingMembershipAndWrongCountsRefuseCompletion) {
    std::vector<Block> blocks;uint256 parent;
    for(uint32_t h=0;h<8;++h){auto b=CaptureOnlyBlock(h,parent);parent=b.GetHash();blocks.push_back(std::move(b));}
    for(int delta:{-1,1}) {
        OrchardHistoryCapture capture(7,parent);CaptureHeaders(capture,blocks);
        for(uint32_t h=0;h<blocks.size();++h)capture.RecordBody(h,blocks[h]);
        OrchardHistoryCaptureTestAccess::SetTransactions(capture,blocks.size()+delta);
        EXPECT_THROW(capture.Finish(),std::runtime_error);
        EXPECT_THROW(capture.ForEachTransaction([](const TxId&){return true;}),std::runtime_error);
    }
    OrchardHistoryCapture missing(7,parent);CaptureHeaders(missing,blocks);
    for(uint32_t h=0;h<blocks.size();++h)missing.RecordBody(h,blocks[h]);
    auto* db=OrchardHistoryCaptureTestAccess::Handle(missing);
    // Delete all leaves (both current and historical persistent-tree copies).
    ASSERT_EQ(sqlite3_exec(db,"DELETE FROM records WHERE substr(k,1,1)=x'74' AND substr(v,1,2)=x'0001'",nullptr,nullptr,nullptr),SQLITE_OK);
    ASSERT_GT(sqlite3_changes(db),0);EXPECT_THROW(missing.Finish(),std::runtime_error);
    EXPECT_THROW(missing.Finished(),std::runtime_error);
    OrchardHistoryCapture failed(7,parent);CaptureHeaders(failed,blocks);failed.RecordBody(0,blocks[0]);
    db=OrchardHistoryCaptureTestAccess::Handle(failed);const auto before=OrchardHistoryCaptureTestAccess::Transactions(failed);
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    EXPECT_THROW(failed.RecordBody(1,blocks[1]),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(OrchardHistoryCaptureTestAccess::Transactions(failed),before);
}
TEST(OrchardHistoryCapture, MembershipTraversalCoversMaximumDepthAndOldRoots) {
    using namespace wallet::detail;RuntimeReplaySpool spool;RuntimeReplayDiskMembership membership(spool);
    using Key=RuntimeReplayDiskMembership::Key;
    EXPECT_EQ(membership.ForEach(0,0,[](const Key&){return false;}),0u);
    EXPECT_THROW(membership.ForEach(0,1,[](const Key&){return true;}),std::runtime_error);
    RuntimeReplayDiskMembership::Root root=0,first=0;std::set<Key> expected;
    {
        RuntimeReplaySpool::Batch batch(spool);
        Key key{};root=membership.With(root,key);first=root;expected.insert(key);
        // Zero plus every one-hot key creates a 256-branch Patricia path.
        for(unsigned bit=0;bit<256;++bit) {
            key={};key[bit/8]=uint8_t(1u<<(7-bit%8));root=membership.With(root,key);expected.insert(key);
        }
        EXPECT_EQ(membership.With(root,Key{}),root);batch.Commit();
    }
    spool.Freeze();std::set<Key> actual;std::optional<Key> previous;
    EXPECT_EQ(membership.ForEach(root,257,[&](const Key& key){
        EXPECT_TRUE(!previous||*previous<key);previous=key;return actual.insert(key).second;
    }),257u);EXPECT_EQ(actual,expected);
    EXPECT_EQ(membership.ForEach(first,1,[](const Key& key){return key==Key{};}),1u);
    EXPECT_THROW(membership.ForEach(root,256,[](const Key&){return true;}),std::runtime_error);
    EXPECT_THROW(membership.ForEach(root,258,[](const Key&){return true;}),std::runtime_error);
    EXPECT_THROW(membership.ForEach(root,0,[](const Key&){return true;}),std::runtime_error);
    EXPECT_EQ(spool.UsageNow().statement_bytes,0);
}
TEST(OrchardHistoryCapture, HeaderPagerSpillsWithoutRetainingHistoryVector) {
    // Generated capture metadata only. This measures SQLite pager allocation,
    // NOT full replay RSS, 125000-block validation, PoW or release capacity.
    constexpr uint32_t count=16384;std::vector<Block> blocks;blocks.reserve(count);uint256 parent;
    for(uint32_t h=0;h<count;++h){auto block=CaptureOnlyBlock(h,parent);parent=block.GetHash();blocks.push_back(std::move(block));}
    OrchardHistoryCapture capture(count-1,parent);CaptureHeaders(capture,blocks);
    const auto first=blocks.front().header.SerializeForHash(),last=blocks.back().header.SerializeForHash();
    blocks.clear();blocks.shrink_to_fit();
    EXPECT_EQ(capture.HeaderAt(0).header.SerializeForHash(),first);
    EXPECT_EQ(capture.HeaderAt(count-1).header.SerializeForHash(),last);
    EXPECT_LT(capture.UsageNow().pager_bytes,2*1024*1024);EXPECT_EQ(capture.UsageNow().statement_bytes,0);
    EXPECT_THROW(capture.Finish(),std::runtime_error);
}
} // namespace dinero
#endif
