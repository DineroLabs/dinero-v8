#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "consensus/pow_context.h"
#include "consensus/pow.h"
namespace dinero {
struct OrchardReplayHeaderViewTestAccess {
    static void Append(OrchardReplayHeaderView& view,const BlockHeader& header,uint32_t height) {
        view.AppendHeader(header,height);
    }
    static sqlite3* Handle(OrchardReplayHeaderView& view) {
        return wallet::detail::RuntimeReplaySpoolTestAccess::Handle(view.appended_);
    }
    static uint32_t Height(const OrchardReplayHeaderView& view){return view.height_;}
    static uint256 Hash(const OrchardReplayHeaderView& view){return view.hash_;}
};
struct OrchardBranchAncestryTestAccess {
    static bool UsesCapturedHeaders(const OrchardBranchReplay& branch) {
        return !branch.headers_ && bool(branch.captured_headers_);
    }
};
namespace {
void CompareAncestry(const OrchardReplayHeaderView& view,const consensus::HeaderChainSelector& selector,
    const uint256& tip,uint32_t height) {
    const auto expected=selector.GetHeaderValue(tip);const auto actual=view.GetHeaderValue(tip);
    ASSERT_TRUE(expected);ASSERT_TRUE(actual);EXPECT_EQ(actual->height,height);
    EXPECT_EQ(actual->header.SerializeForHash(),expected->header.SerializeForHash());
    for(uint32_t h=0;h<=height;++h) {
        uint256 left,right;uint32_t lh=0,rh=0,mtp=0,found=0;
        ASSERT_TRUE(selector.GetAncestorHashByHash(tip,h,left,lh));
        ASSERT_TRUE(view.GetAncestorHashByHash(tip,h,right,rh));EXPECT_EQ(left,right);EXPECT_EQ(lh,rh);
        ASSERT_TRUE(selector.GetMedianTimePastByHash(left,mtp,found));EXPECT_EQ(found,h);
        EXPECT_EQ(view.MedianTimePast(h),mtp);
    }
    for(auto anchor:{std::optional<uint32_t>{},std::optional<uint32_t>{0},
        std::optional<uint32_t>{height/2},std::optional<uint32_t>{height}}) {
        consensus::HeaderAsertContext left,right;
        ASSERT_TRUE(selector.GetAsertContextByHash(tip,left,anchor));
        ASSERT_TRUE(view.GetAsertContextByHash(tip,right,anchor));
        EXPECT_EQ(left.parent_height,right.parent_height);EXPECT_EQ(left.parent_mtp,right.parent_mtp);
        EXPECT_EQ(left.block1_time,right.block1_time);ASSERT_EQ(bool(left.timing_anchor),bool(right.timing_anchor));
        if(left.timing_anchor) {
            EXPECT_EQ(left.timing_anchor->height,right.timing_anchor->height);
            EXPECT_EQ(left.timing_anchor->time,right.timing_anchor->time);
            EXPECT_EQ(left.timing_anchor->bits,right.timing_anchor->bits);
        }
    }
}
template<class F> void ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode expected,F run) {
    try {run();ADD_FAILURE()<<"Expected exact contextual header refusal";}
    catch(const consensus::OrchardHeaderError& error){EXPECT_EQ(error.Code(),expected);}
}
BlockHeader AncestrySolved(BlockHeader header,bool valid=true) {
    for(uint32_t n=0;n<4'000'000;++n) {
        header.nonce=n;if(consensus::CheckProofOfWork(header,false)==valid)return header;
    }
    throw std::runtime_error("isolated ancestry fixture nonce search exhausted");
}
}
TEST(OrchardBranchAncestry, AncestorsWindowsAndAsertMatchExistingSelector) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);OrchardReplayHeaderView view(capture);
    consensus::HeaderChainSelector selector;
    for(const auto& block:f.blocks)ASSERT_TRUE(selector.AddHeader(block.header));
    auto header=f.blocks.back().header;auto height=f.tip.height;
    CompareAncestry(view,selector,header.GetHash(),height);
    for(unsigned i=0;i<16;++i) {
        header.prev_block_hash=header.GetHash();header.timestamp+=120;++header.nonce;++height;
        ASSERT_TRUE(selector.AddHeader(header));OrchardReplayHeaderViewTestAccess::Append(view,header,height);
        CompareAncestry(view,selector,header.GetHash(),height);
    }
    // Keep the existing selector's uint32 MTP conversion exactly. This isolated
    // metadata check is not an endorsement of a new timestamp consensus rule.
    header.prev_block_hash=header.GetHash();header.timestamp+=uint64_t(UINT32_MAX)+1;++height;
    ASSERT_TRUE(selector.AddHeader(header));OrchardReplayHeaderViewTestAccess::Append(view,header,height);
    CompareAncestry(view,selector,header.GetHash(),height);
    consensus::HeaderAsertContext missing;missing.parent_height=99;
    EXPECT_FALSE(view.GetAsertContextByHash(uint256{},missing,{}));EXPECT_EQ(missing.parent_height,0u);
    EXPECT_FALSE(view.GetHeaderValue(uint256{}));
    uint256 out=header.GetHash();uint32_t selected=99;
    EXPECT_FALSE(view.GetAncestorHashByHash(uint256{},0,out,selected));EXPECT_TRUE(out.IsNull());EXPECT_EQ(selected,0u);
    EXPECT_THROW(view.MedianTimePast(height+1),consensus::OrchardHeaderLookupError);
}
TEST(OrchardBranchAncestry, CommonGateKeepsRealWorkTimingAndCheckpointRules) {
    struct Restore {ChainParams p=Params();~Restore(){MutableParams()=p;}}restore;
    SelectParams(Chain::REGTEST);MutableParams().orchard_activation_height=1;
    MutableParams().orchard_branch_id=1;MutableParams().sixty_second_activation_height=4;
    MutableParams().regtest_enforce_pow=true;
    std::vector<Block> blocks{SelectedGenesis()};
    consensus::HeaderChainSelector selector;ASSERT_TRUE(selector.AddHeader(blocks.front().header));
    const auto consensus=GetConsensusForCurrentNetwork();
    for(uint32_t h=1;h<4;++h) {
        auto block=CaptureOnlyBlock(h,blocks.back().GetHash());
        block.header.timestamp=blocks.front().header.timestamp+h*120;
        const auto parent=selector.GetHeaderValue(block.header.prev_block_hash);ASSERT_TRUE(parent);
        consensus::HeaderAsertContext ancestry;ASSERT_TRUE(selector.GetAsertContextByHash(parent->hash,ancestry,{}));
        const auto input=BuildAsertInputForCandidateTimes(ancestry.parent_mtp,ancestry.block1_time,
            static_cast<NoChainDb*>(nullptr),h,block.header.timestamp,consensus,ancestry.timing_anchor);
        ASSERT_TRUE(input);block.header.difficulty=ComputeAsertBits(*input);block.header=AncestrySolved(block.header);
        ASSERT_TRUE(selector.AddHeader(block.header));blocks.push_back(std::move(block));
    }
    OrchardHistoryCapture capture(3,blocks.back().GetHash());CaptureHeaders(capture,blocks);CaptureBodies(capture,blocks);
    OrchardReplayHeaderView view(capture);auto parent=blocks.back().header;
    for(uint32_t h=4;h<=6;++h) {
        auto child=CaptureOnlyBlock(h,parent.GetHash()).header;child.timestamp=parent.timestamp+60;
        consensus::HeaderAsertContext ancestry;
        ASSERT_TRUE(selector.GetAsertContextByHash(parent.GetHash(),ancestry,TimingUpgradeAnchorHeight(h,consensus)));
        const auto input=BuildAsertInputForCandidateTimes(ancestry.parent_mtp,ancestry.block1_time,
            static_cast<NoChainDb*>(nullptr),h,child.timestamp,consensus,ancestry.timing_anchor);
        ASSERT_TRUE(input);child.difficulty=ComputeAsertBits(*input);child=AncestrySolved(child);
        const auto check=[&](const BlockHeader& candidate,bool captured,uint64_t now) {
            const auto context=consensus::SelectedOrchardBlockContext(candidate,h);ASSERT_TRUE(context);
            if(captured)consensus::CheckOrchardHeaderUnderChainstateLock(candidate,parent,*context,view,now);
            else consensus::CheckOrchardHeaderUnderChainstateLock(candidate,parent,*context,selector,now);
        };
        for(bool captured:{false,true}) {
            EXPECT_NO_THROW(check(child,captured,child.timestamp));
            auto wrong=AncestrySolved(child,false);
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::ProofOfWork,[&]{check(wrong,captured,child.timestamp);});
            wrong=child;wrong.difficulty^=1;
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::Difficulty,[&]{check(wrong,captured,child.timestamp);});
            wrong=child;wrong.timestamp=ancestry.parent_mtp;
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::TimeTooOld,[&]{check(wrong,captured,child.timestamp);});
            wrong=child;wrong.version=0;
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::Shape,[&]{check(wrong,captured,child.timestamp);});
            wrong=child;wrong.reserved[11]=1;
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::Shape,[&]{check(wrong,captured,child.timestamp);});
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::TimeTooNew,[&]{check(child,captured,child.timestamp-7201);});
            MutableParams().vCheckpoints={{2,blocks[1].GetHash().GetHex()}};
            ExpectAncestryHeaderError(consensus::OrchardHeaderErrorCode::Checkpoint,[&]{check(child,captured,child.timestamp);});
            MutableParams().vCheckpoints={{2,blocks[2].GetHash().GetHex()}};
            EXPECT_NO_THROW(check(child,captured,child.timestamp));MutableParams().vCheckpoints.clear();
        }
        ASSERT_TRUE(selector.AddHeader(child));OrchardReplayHeaderViewTestAccess::Append(view,child,h);
        CompareAncestry(view,selector,child.GetHash(),h);parent=child;
    }
}
TEST(OrchardBranchAncestry, MissingRowsReadDenialAndInterruptedEofRefuse) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);OrchardReplayHeaderView view(capture);
    auto header=f.blocks.back().header;header.prev_block_hash=header.GetHash();header.timestamp+=120;
    const auto height=f.tip.height+1;OrchardReplayHeaderViewTestAccess::Append(view,header,height);
    auto* db=OrchardReplayHeaderViewTestAccess::Handle(view);
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*) {
        return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(view.GetHeaderValue(header.GetHash()),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned kind,void* context,void* statement,void*) {
        const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(kind==SQLITE_TRACE_ROW&&sql&&std::string(sql)=="SELECT v,tag FROM records WHERE k=?1")
            sqlite3_interrupt(static_cast<sqlite3*>(context));return 0;
    },db);
    EXPECT_THROW(view.GetHeaderValue(header.GetHash()),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);
    ASSERT_TRUE(view.GetHeaderValue(header.GetHash()));
    ASSERT_EQ(sqlite3_exec(db,"UPDATE records SET v=zeroblob(length(v))",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(view.GetHeaderValue(header.GetHash()),std::runtime_error);
    ASSERT_EQ(sqlite3_exec(db,"DELETE FROM records",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(view.GetHeaderValue(header.GetHash()),std::runtime_error);
    EXPECT_THROW(view.MedianTimePast(height),std::runtime_error);
    auto* historical=OrchardHistoryCaptureTestAccess::Handle(capture);
    ASSERT_EQ(sqlite3_exec(historical,"PRAGMA query_only=OFF; DELETE FROM records WHERE substr(k,1,1)=x'68'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(view.MedianTimePast(0),std::runtime_error);
}
TEST(OrchardBranchAncestry, CommitRefusalPreservesPositionAndPoisonsAppend) {
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);OrchardReplayHeaderView view(capture);
    auto header=f.blocks.back().header;header.prev_block_hash=header.GetHash();header.timestamp+=120;
    auto* db=OrchardReplayHeaderViewTestAccess::Handle(view);sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    EXPECT_THROW(OrchardReplayHeaderViewTestAccess::Append(view,header,f.tip.height+1),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(OrchardReplayHeaderViewTestAccess::Height(view),f.tip.height);
    EXPECT_EQ(OrchardReplayHeaderViewTestAccess::Hash(view),f.tip.hash);
    EXPECT_THROW(view.GetHeaderValue(f.tip.hash),std::runtime_error);
    EXPECT_THROW(OrchardReplayHeaderViewTestAccess::Append(view,header,f.tip.height+1),std::runtime_error);
}
TEST(OrchardBranchAncestry, FullBranchUsesCaptureWithoutHistoricalSelector) {
    ProofHandoffFixture f;OrchardHistoryCapture capture(f.parent->height,f.parent->hash);
    CaptureHeaders(capture,f.f.blocks);CaptureBodies(capture,f.f.blocks);
    const OrchardParentReplay::Target target{103,f.second->Header().GetHash(),*f.f.db.getBlockWork(f.second->Header().GetHash())};
    OrchardBranchReplay branch(*f.replay,capture,target);
    ASSERT_TRUE(OrchardBranchAncestryTestAccess::UsesCapturedHeaders(branch));
    f.Append(branch,OrchardBlockCandidate::DecodeExact(f.first->WireBytes()),102);f.Append(branch,*f.body,103);branch.Finish();
    const auto reference=f.Complete();EXPECT_EQ(branch.ProvenState(),reference->ProvenState());
    EXPECT_EQ(branch.ProvenBlock(103,target.hash).Context().block_hash,target.hash);
    EXPECT_EQ(branch.ProvenCommitments(),reference->ProvenCommitments());
    EXPECT_TRUE(OrchardBranchAncestryTestAccess::UsesCapturedHeaders(branch));
}
TEST(OrchardBranchAncestry, AppendedHeadersSpillWithinPagerBudget) {
    // Header storage/pager check only: no full-body replay, PoW, total RSS,
    // 125000-block capacity or release qualification is implied by this test.
    OwnedSelectedParentFixture f;OrchardHistoryCapture capture(f.tip.height,f.tip.hash);
    CaptureHeaders(capture,f.blocks);CaptureBodies(capture,f.blocks);OrchardReplayHeaderView view(capture);
    auto header=f.blocks.back().header;
    for(uint32_t h=f.tip.height+1;h<16384;++h) {
        header.prev_block_hash=header.GetHash();header.timestamp+=120;header.nonce=h;
        OrchardReplayHeaderViewTestAccess::Append(view,header,h);
    }
    const auto tip=view.GetHeaderValue(header.GetHash());ASSERT_TRUE(tip);EXPECT_EQ(tip->height,16383u);
    EXPECT_LT(view.UsageNow().pager_bytes,2*1024*1024);EXPECT_EQ(view.UsageNow().statement_bytes,0);
}
} // namespace dinero
#endif
