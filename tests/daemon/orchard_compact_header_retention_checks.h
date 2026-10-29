#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "orchard_retention_header_recipe.h"
#include "orchard_retention_header_nonces.h"
namespace {
std::shared_ptr<consensus::HeaderChainSelector> CompactRetentionSelector(
        const CompactStartupFixture& f,consensus::HeaderStore& store) {
    auto out=std::make_shared<consensus::HeaderChainSelector>(&store);
    for(const auto& h:CompactBindingHeaders(f))OrchardAdmissionFixture::Require(out->AddHeader(h));
    return out;
}
std::vector<consensus::HeaderIndexEntry> CompactRetentionExpected(
        const CompactStartupFixture& f,const consensus::HeaderChainSelector& selector) {
    std::vector<consensus::HeaderIndexEntry> out;
    for(const auto& h:CompactBindingHeaders(f)) {
        auto e=selector.GetHeaderValue(h.GetHash());OrchardAdmissionFixture::Require(bool(e));
        e->parent=nullptr;out.push_back(*e);
    }
    return out;
}
std::map<uint256,std::string> CompactRetentionStored(consensus::HeaderStore& store) {
    std::vector<consensus::HeaderIndexEntry> entries;
    OrchardAdmissionFixture::Require(store.LoadAllHeaders(entries));
    std::map<uint256,std::string> out;
    for(const auto& e:entries) {
        std::ostringstream s;s<<e.prev_hash.GetHex()<<':'<<e.height<<':'<<e.chainwork.GetHex()<<':';
        for(auto b:e.header.SerializeForHash())s<<int(b)<<',';
        OrchardAdmissionFixture::Require(out.emplace(e.hash,s.str()).second);
    }
    uint256 best;OrchardAdmissionFixture::Require(store.LoadBestHeader(best));
    out.emplace(uint256{},best.GetHex());return out;
}
}
TEST(OrchardCompactHeaderRetention, ExactMatchMultipleOwnersAndLastRelease) {
    CompactStartupFixture fixture;
    consensus::HeaderStore store((fixture.f.path/"retention-multiple").string());ASSERT_TRUE(store.Open());
    auto headers=CompactRetentionSelector(fixture,store);
    auto expected=CompactRetentionExpected(fixture,*headers);
    const auto count=headers->GetHeaderCount();const auto stored=CompactRetentionStored(store);
    auto wrong=expected;wrong[1].chainwork+=arith_uint256(1);
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAncestry(headers,wrong));
    EXPECT_TRUE(headers->LoadFromStorage()); // failed acquisition left no owner
    EXPECT_EQ(CompactRetentionStored(store),stored);
    auto first=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(first);
    auto second=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(second);
    EXPECT_THROW(headers->Clear(),std::exception);EXPECT_FALSE(headers->LoadFromStorage());
    EXPECT_EQ(headers->GetHeaderCount(),count);EXPECT_TRUE(headers->MatchesAncestry(expected));
    EXPECT_EQ(CompactRetentionStored(store),stored);
    first.reset();EXPECT_THROW(headers->Clear(),std::exception);EXPECT_FALSE(headers->LoadFromStorage());
    EXPECT_EQ(CompactRetentionStored(store),stored);
    second.reset();EXPECT_TRUE(headers->LoadFromStorage());EXPECT_TRUE(headers->MatchesAncestry(expected));
    EXPECT_EQ(CompactRetentionStored(store),stored);
    EXPECT_NO_THROW(headers->Clear());EXPECT_EQ(headers->GetHeaderCount(),0u);
}
TEST(OrchardCompactHeaderRetention, ServiceBindingRetainsUntilDestruction) {
    CompactStartupFixture fixture;
    consensus::HeaderStore store((fixture.f.path/"retention-service").string());ASSERT_TRUE(store.Open());
    auto headers=CompactRetentionSelector(fixture,store);
    const auto expected=CompactRetentionExpected(fixture,*headers);const auto stored=CompactRetentionStored(store);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    EXPECT_NO_THROW(service->setHeaderChainSelector(headers));
    EXPECT_THROW(headers->Clear(),std::exception);EXPECT_FALSE(headers->LoadFromStorage());
    EXPECT_THROW(service->setHeaderChainSelector(nullptr),std::exception);
    EXPECT_TRUE(headers->MatchesAncestry(expected));EXPECT_EQ(CompactRetentionStored(store),stored);
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    service.reset();EXPECT_TRUE(headers->LoadFromStorage());EXPECT_TRUE(headers->MatchesAncestry(expected));
    EXPECT_EQ(CompactRetentionStored(store),stored);EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactHeaderRetention, FailedInitializationReleasesOnlyNewRetention) {
    CompactStartupFixture fixture;
    consensus::HeaderStore store((fixture.f.path/"retention-init").string());ASSERT_TRUE(store.Open());
    auto headers=CompactRetentionSelector(fixture,store);
    const auto expected=CompactRetentionExpected(fixture,*headers);const auto stored=CompactRetentionStored(store);
    auto other=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(other);
    auto* tip=FindBlockIndex(fixture.second->Header().GetHash());ASSERT_NE(tip,nullptr);
    struct RestoreNonce {CBlockIndex& node;uint32_t value;~RestoreNonce(){node.nonce=value;}} restore{*tip,tip->nonce};
    tip->nonce^=1; // serialized data conflict; all pointers and synchronization retained
    const auto graph=CompactGlobalGraphSnapshot();const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    EXPECT_FALSE(service->Init(context));EXPECT_EQ(CompactGlobalGraphSnapshot(),graph);
    EXPECT_THROW(headers->Clear(),std::exception); // separate existing owner survives failure
    other.reset();EXPECT_TRUE(headers->LoadFromStorage()); // failed Init released its own guard
    EXPECT_EQ(CompactRetentionStored(store),stored);EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    tip->nonce=restore.value;ASSERT_TRUE(service->Init(context));
    EXPECT_THROW(headers->Clear(),std::exception);EXPECT_FALSE(headers->LoadFromStorage());
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    service.reset();EXPECT_TRUE(headers->LoadFromStorage());EXPECT_TRUE(headers->MatchesAncestry(expected));
    EXPECT_EQ(CompactRetentionStored(store),stored);EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}

namespace {
// Value-owned ancestry for the actual ASERT routine. These local parent pointers
// never refer to selector-owned entries and never escape the fixture.
std::deque<consensus::HeaderIndexEntry> CompactRetentionLocalPath(
        const std::vector<BlockHeader>& history,size_t count) {
    std::deque<consensus::HeaderIndexEntry> out;
    for(size_t i=0;i<count;++i)out.emplace_back(history.at(i),out.empty()?nullptr:&out.back());
    return out;
}
// Mining is performed once by the retained offline generator. Runtime still
// checks the exact deterministic header identity, required difficulty and PoW.
BlockHeader CompactRetentionChild(const consensus::HeaderIndexEntry& parent,uint64_t salt) {
    const auto first=std::begin(retention_vectors::records),last=std::end(retention_vectors::records);
    const auto it=std::lower_bound(first,last,salt,[](const auto& row,uint64_t value){return row.salt<value;});
    OrchardAdmissionFixture::Require(it!=last && it->salt==salt);
    auto h=retention_vectors::Child(parent,salt,it->nonce);
    OrchardAdmissionFixture::Require(h.difficulty==it->bits && h.GetHash().GetHex()==it->hash &&
        consensus::CheckProofOfWork(h,false));
    return h;
}
std::vector<BlockHeader> CompactRetentionVectorHistory() {
    auto local=retention_vectors::History(CompactRetentionChild);
    std::vector<BlockHeader> history;history.reserve(local.size());
    for(const auto& e:local)history.push_back(e.header);
    return history;
}
std::shared_ptr<consensus::HeaderChainSelector> CompactRetentionVectorSelector(
        const std::vector<BlockHeader>& history) {
    auto out=std::make_shared<consensus::HeaderChainSelector>();
    for(const auto& h:history)OrchardAdmissionFixture::Require(out->AddHeader(h));
    return out;
}
std::vector<consensus::HeaderIndexEntry> CompactRetentionPathValues(
        const consensus::HeaderChainSelector& selector,const std::vector<BlockHeader>& path) {
    std::vector<consensus::HeaderIndexEntry> out;
    for(const auto& h:path) {
        auto value=selector.GetHeaderValue(h.GetHash());OrchardAdmissionFixture::Require(bool(value));
        value->parent=nullptr;out.push_back(*value);
    }
    return out;
}
// This is the actual production side-header budget, not a lowered fixture knob.
// Count assertions below require the real full-budget eviction route to execute.
constexpr size_t CompactRetentionRealSideBudget=10000;
}
TEST(OrchardCompactHeaderRetention, CapacityEvictsUnretainedTipAndGuardOwnsSelector) {
    retention_vectors::Profile fixture;
    const auto history=CompactRetentionVectorHistory();auto headers=CompactRetentionVectorSelector(history);
    ASSERT_EQ(history.size(),104u);
    const size_t prefix_count=102;
    auto prefix=CompactRetentionLocalPath(history,prefix_count);
    std::vector<BlockHeader> leaves;leaves.reserve(CompactRetentionRealSideBudget);
    for(size_t i=0;i<CompactRetentionRealSideBudget;++i) {
        auto h=CompactRetentionChild(prefix.back(),i+1);
        ASSERT_TRUE(headers->AddHeader(h));leaves.push_back(std::move(h));
    }
    const auto best=headers->GetHeaderValue(history.back().GetHash());ASSERT_TRUE(best);
    ASSERT_EQ(headers->GetHeaderCount(),history.size()+CompactRetentionRealSideBudget);
    std::sort(leaves.begin(),leaves.end(),[](const auto& a,const auto& b){return a.GetHash()<b.GetHash();});
    std::vector<BlockHeader> retained_path(history.begin(),history.begin()+prefix_count);
    retained_path.push_back(leaves.front());
    const auto expected=CompactRetentionPathValues(*headers,retained_path);
    auto guard=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(guard);
    consensus::HeaderIndexEntry parent(leaves.back(),&prefix.back());
    std::optional<BlockHeader> incoming;
    // Equal work but a greater hash keeps this a losing branch, so admission
    // must use the actual capacity/eviction route rather than new-best bypass.
    for(uint64_t salt=100001;salt<200001;++salt) {
        auto h=CompactRetentionChild(parent,salt);
        if(best->hash<h.GetHash()){incoming=std::move(h);break;}
    }
    ASSERT_TRUE(incoming);
    ASSERT_EQ(consensus::HeaderIndexEntry(*incoming,&parent).chainwork,best->chainwork);
    const auto count=headers->GetHeaderCount();
    ASSERT_TRUE(headers->AddHeader(*incoming));
    EXPECT_EQ(headers->GetHeaderCount(),count);
    EXPECT_TRUE(headers->GetHeaderValue(leaves.front().GetHash()));
    EXPECT_FALSE(headers->GetHeaderValue(leaves[1].GetHash()));
    EXPECT_TRUE(headers->MatchesAncestry(expected));
    std::weak_ptr<consensus::HeaderChainSelector> weak=headers;
    headers.reset();EXPECT_FALSE(weak.expired());
    guard.reset();EXPECT_TRUE(weak.expired());
}
TEST(OrchardCompactHeaderRetention, EvictingDescendantStopsAtRetainedAncestor) {
    retention_vectors::Profile fixture;
    const auto history=CompactRetentionVectorHistory();auto headers=CompactRetentionVectorSelector(history);
    auto canonical=CompactRetentionLocalPath(history,history.size());
    const auto branch103=CompactRetentionChild(canonical.back(),300001);
    ASSERT_TRUE(headers->AddHeader(branch103));canonical.emplace_back(branch103,&canonical.back());
    const auto branch104=CompactRetentionChild(canonical.back(),300002);
    ASSERT_TRUE(headers->AddHeader(branch104));
    const size_t prefix_count=102;
    auto prefix=CompactRetentionLocalPath(history,prefix_count);
    const auto retained=CompactRetentionChild(prefix.back(),400001);
    ASSERT_TRUE(headers->AddHeader(retained));prefix.emplace_back(retained,&prefix.back());
    const auto descendant=CompactRetentionChild(prefix.back(),400002);
    ASSERT_TRUE(headers->AddHeader(descendant));
    std::vector<BlockHeader> retained_path(history.begin(),history.begin()+prefix_count);
    retained_path.push_back(retained);
    const auto expected=CompactRetentionPathValues(*headers,retained_path);
    auto guard=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(guard);
    // All other losing leaves have more work than the retained branch's child.
    // Evicting that child must prune exactly one node and stop at its owner.
    const auto& parent102=canonical[history.size()-1];
    for(size_t i=0;i<CompactRetentionRealSideBudget-2;++i) {
        auto h=CompactRetentionChild(parent102,500001+i);
        ASSERT_GT(consensus::HeaderIndexEntry(h,&parent102).chainwork,
            consensus::HeaderIndexEntry(descendant,&prefix.back()).chainwork);
        ASSERT_TRUE(headers->AddHeader(h));
    }
    ASSERT_EQ(headers->GetHeaderCount(),history.size()+2+CompactRetentionRealSideBudget);
    const auto count=headers->GetHeaderCount();
    const auto incoming=CompactRetentionChild(parent102,600001);
    ASSERT_TRUE(headers->AddHeader(incoming));
    EXPECT_FALSE(headers->GetHeaderValue(descendant.GetHash()));
    EXPECT_TRUE(headers->GetHeaderValue(retained.GetHash()));
    EXPECT_TRUE(headers->MatchesAncestry(expected));EXPECT_EQ(headers->GetHeaderCount(),count);
    // The retained tip now is the lowest-work leaf, but is not evictable.
    // An equal-work newcomer cannot displace the remaining unretained leaves.
    const auto equal=CompactRetentionChild(parent102,600002);
    EXPECT_FALSE(headers->AddHeader(equal));EXPECT_EQ(headers->GetHeaderCount(),count);
    EXPECT_TRUE(headers->MatchesAncestry(expected));
}
#endif
