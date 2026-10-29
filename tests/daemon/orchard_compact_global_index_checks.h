#pragma once
#include <sstream>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
std::map<std::string,std::string> CompactGlobalGraphSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);
    std::map<std::string,std::string> out;
    for(const auto& [hash,p]:g_block_index) {
        std::ostringstream s;
        s<<p.get()<<' '<<p->hash.GetHex()<<' '<<p->prev_hash.GetHex()<<' '<<p->height<<' '
         <<p->version<<' '<<p->merkle_root.GetHex()<<' '<<p->timestamp<<' '<<p->bits<<' '
         <<p->nonce<<' '<<p->chainwork<<' '<<p->status<<' '<<p->file_number<<' '<<p->data_pos<<' '
         <<p->data_size<<' '<<p->undo_file<<' '<<p->undo_pos<<' '<<p->undo_size<<' '<<p->pprev;
        for(auto* child:p->children)s<<' '<<child;
        out.emplace("node:"+hash.GetHex(),s.str());
    }
    std::ostringstream c;for(auto* p:g_candidates.Snapshot())c<<p<<' ';out.emplace("candidates",c.str());
    for(const auto& [hash,children]:g_orphan_pool) {
        std::ostringstream s;for(auto* child:children)s<<child<<' ';
        out.emplace("orphan:"+hash.GetHex(),s.str());
    }
    return out;
}
BlockHeader CompactGraphSibling(const CompactStartupFixture& f) {
    auto header=f.f.blocks.back().header;
    header.merkle_root=uint256::FromHexUnsafe(std::string(64,'a'));
    return OrchardAdmissionFixture::SolveHeader(header);
}
}
TEST(OrchardCompactGlobalIndex, ExistingPointersAndCompetingBranchSurvivePublication) {
    CompactStartupFixture fixture;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    std::vector<CBlockIndex*> original;
    for(const auto& block:fixture.f.blocks)original.push_back(FindBlockIndex(block.GetHash()));
    original.push_back(FindBlockIndex(fixture.first->Header().GetHash()));
    original.push_back(FindBlockIndex(fixture.second->Header().GetHash()));
    for(auto* p:original)ASSERT_NE(p,nullptr);
    const auto sibling=CompactGraphSibling(fixture);auto* branch=AddBlockIndex(sibling,101);
    ASSERT_NE(branch,nullptr);const auto branch_state=CompactGlobalGraphSnapshot().at("node:"+branch->hash.GetHex());
    GetConfig().utreexo_stateless=true;
    DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);
    ASSERT_TRUE(service.Init(context));
    for(size_t height=0;height<original.size();++height) {
        const auto* p=FindBlockIndex(original[height]->hash);ASSERT_EQ(p,original[height]);
        EXPECT_EQ(p->height,height);EXPECT_EQ(p->pprev,height?original[height-1]:nullptr);
        EXPECT_TRUE(IsEligibleForCandidacy(p->status));
        const auto m=fixture.reopened.getHeaderMetadata(p->hash);ASSERT_TRUE(m.ok());
        EXPECT_EQ(p->file_number,m->file_number);EXPECT_EQ(p->data_pos,m->data_pos);EXPECT_EQ(p->data_size,m->data_size);
    }
    EXPECT_EQ(FindBlockIndex(sibling.GetHash()),branch);
    EXPECT_EQ(CompactGlobalGraphSnapshot().at("node:"+branch->hash.GetHex()),branch_state);
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactGlobalIndex, MissingSelectedNodesLinkExistingHeaderOnlyOrphan) {
    CompactStartupFixture fixture;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    // Existing serialized fixture owner retains the original graph objects.
    // New service is destroyed before its isolated graph restores those objects.
    RawIngressGraph isolated;
    auto child=fixture.second->Header();child.prev_block_hash=fixture.second->Header().GetHash();
    child.timestamp+=120;child.merkle_root=uint256::FromHexUnsafe(std::string(64,'b'));
    child=OrchardAdmissionFixture::SolveHeader(child);
    auto* orphan=AddBlockIndex(child,fixture.second->Height()+1);ASSERT_NE(orphan,nullptr);
    ASSERT_EQ(orphan->pprev,nullptr);ASSERT_TRUE(g_orphan_pool.contains(child.prev_block_hash));
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    {
        ChainstateService service;service.setChainDB(&fixture.reopened);ASSERT_TRUE(service.Init(context));
        auto* tip=FindBlockIndex(fixture.second->Header().GetHash());ASSERT_NE(tip,nullptr);
        EXPECT_EQ(FindBlockIndex(child.GetHash()),orphan);EXPECT_EQ(orphan->pprev,tip);
        EXPECT_EQ(orphan->chainwork,chainwork::AddWork(tip->chainwork,chainwork::WorkForBits(orphan->bits)));
        EXPECT_EQ(orphan->status&(BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO|BLOCK_VALID_SCRIPTS),0u);
        EXPECT_FALSE(IsEligibleForCandidacy(orphan->status));
        EXPECT_FALSE(g_orphan_pool.contains(child.prev_block_hash));
        EXPECT_NE(std::find(tip->children.begin(),tip->children.end(),orphan),tip->children.end());
        EXPECT_EQ(GetBestCandidate(),tip);
        const auto m=fixture.reopened.getHeaderMetadata(tip->hash);ASSERT_TRUE(m.ok());
        EXPECT_NE(tip->status&BLOCK_HAVE_UNDO,0u);
        EXPECT_EQ(tip->undo_file,m->undo_file);EXPECT_EQ(tip->undo_pos,m->undo_pos);EXPECT_EQ(tip->undo_size,m->undo_size);
        EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
    }
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactGlobalIndex, LateExistingConflictPreservesEntireGraphAndRetries) {
    CompactStartupFixture fixture;
    auto* tip=FindBlockIndex(fixture.second->Header().GetHash());ASSERT_NE(tip,nullptr);
    struct RestoreNonce {CBlockIndex& node;uint32_t before;~RestoreNonce(){node.nonce=before;}} restore{*tip,tip->nonce};
    tip->nonce^=1; // Serialized conflicting value; no pointer/lifetime/synchronization removal.
    const auto graph=CompactGlobalGraphSnapshot();const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);
    EXPECT_FALSE(service.Init(context));EXPECT_EQ(CompactGlobalGraphSnapshot(),graph);
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    tip->nonce=restore.before;
    EXPECT_TRUE(service.Init(context));EXPECT_EQ(FindBlockIndex(tip->hash),tip);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
#endif
