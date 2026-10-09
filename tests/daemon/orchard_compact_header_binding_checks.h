#pragma once
#include "consensus/header_store.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
std::vector<BlockHeader> CompactBindingHeaders(const CompactStartupFixture& f) {
    std::vector<BlockHeader> headers;
    for(const auto& block:f.f.blocks)headers.push_back(block.header);
    headers.push_back(f.first->Header());headers.push_back(f.second->Header());return headers;
}
std::shared_ptr<consensus::HeaderChainSelector> CompactBindingSelector(
        const CompactStartupFixture& f,bool omit_last=false) {
    auto out=std::make_shared<consensus::HeaderChainSelector>();auto headers=CompactBindingHeaders(f);
    if(omit_last)headers.pop_back();
    for(const auto& h:headers)OrchardAdmissionFixture::Require(out->AddHeader(h));
    return out;
}
}
TEST(OrchardCompactHeaderBinding, CompleteAncestryPreservesBranchesAndBinding) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture);
    const auto sibling=CompactGraphSibling(fixture);ASSERT_TRUE(headers->AddHeader(sibling));
    const auto before=headers->GetHeaderValue(sibling.GetHash());ASSERT_TRUE(before);
    const auto count=headers->GetHeaderCount();const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);ASSERT_TRUE(service.Init(context));
    ASSERT_NO_THROW(service.setHeaderChainSelector(headers));
    EXPECT_NO_THROW(service.setHeaderChainSelector(headers));
    EXPECT_THROW(service.setHeaderChainSelector(nullptr),std::exception);
    EXPECT_THROW(service.setHeaderChainSelector(CompactBindingSelector(fixture)),std::exception);
    const auto after=headers->GetHeaderValue(sibling.GetHash());ASSERT_TRUE(after);
    EXPECT_EQ(after->hash,before->hash);EXPECT_EQ(after->chainwork,before->chainwork);
    EXPECT_EQ(headers->GetHeaderCount(),count);
    EXPECT_TRUE(service.GetSyncSnapshot().has_best_header);
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactHeaderBinding, MissingAncestryRefusesBeforeBindingAndRetries) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture,true);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);ASSERT_TRUE(service.Init(context));
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    const auto graph=CompactGlobalGraphSnapshot();const auto count=headers->GetHeaderCount();
    EXPECT_THROW(service.setHeaderChainSelector(headers),std::exception);
    EXPECT_FALSE(service.GetSyncSnapshot().has_best_header);EXPECT_EQ(headers->GetHeaderCount(),count);
    EXPECT_EQ(CompactGlobalGraphSnapshot(),graph);EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    ASSERT_TRUE(headers->AddHeader(fixture.second->Header()));
    EXPECT_NO_THROW(service.setHeaderChainSelector(headers));EXPECT_TRUE(service.GetSyncSnapshot().has_best_header);
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
}
TEST(OrchardCompactHeaderBinding, PreboundIncompleteSelectorRefusesBeforeGraphAndRetries) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture,true);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);service.setHeaderChainSelector(headers);
    const auto graph=CompactGlobalGraphSnapshot();const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    EXPECT_FALSE(service.Init(context));EXPECT_EQ(CompactGlobalGraphSnapshot(),graph);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    ASSERT_TRUE(headers->AddHeader(fixture.second->Header()));ASSERT_TRUE(service.Init(context));
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactHeaderBinding, PersistedInteriorWorkMismatchRefusesDespiteMatchingTip) {
    CompactStartupFixture fixture;auto correct=CompactBindingSelector(fixture);
    consensus::HeaderStore store((fixture.f.path/"binding-header-store").string());ASSERT_TRUE(store.Open());
    const auto history=CompactBindingHeaders(fixture);
    for(size_t height=0;height<history.size();++height) {
        auto entry=correct->GetHeaderValue(history[height].GetHash());ASSERT_TRUE(entry);
        if(height==1)entry->chainwork+=arith_uint256(1);
        ASSERT_TRUE(store.StoreHeader(*entry));
    }
    ASSERT_TRUE(store.StoreBestHeader(history.back().GetHash()));
    auto corrupt=std::make_shared<consensus::HeaderChainSelector>(&store);
    const auto tip=corrupt->GetHeaderValue(history.back().GetHash());ASSERT_TRUE(tip);
    EXPECT_EQ(tip->chainwork,correct->GetHeaderValue(history.back().GetHash())->chainwork);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);ASSERT_TRUE(service.Init(context));
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    EXPECT_THROW(service.setHeaderChainSelector(corrupt),std::exception);
    EXPECT_FALSE(service.GetSyncSnapshot().has_best_header);
    const auto still_wrong=corrupt->GetHeaderValue(history[1].GetHash());ASSERT_TRUE(still_wrong);
    EXPECT_NE(still_wrong->chainwork,correct->GetHeaderValue(history[1].GetHash())->chainwork);
    EXPECT_NO_THROW(service.setHeaderChainSelector(correct));
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactHeaderBinding, ChangedDurableSelectionRefusesBindingAndRetries) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);ASSERT_TRUE(service.Init(context));
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();const auto graph=CompactGlobalGraphSnapshot();
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,fixture.first->Header().GetHash(),fixture.first->Height()),Status::Ok);
    const auto changed=fixture.Rows();
    EXPECT_THROW(service.setHeaderChainSelector(headers),std::exception);
    EXPECT_FALSE(service.GetSyncSnapshot().has_best_header);
    EXPECT_EQ(fixture.Rows(),changed);EXPECT_EQ(fixture.ArchiveBytes(),archives);EXPECT_EQ(CompactGlobalGraphSnapshot(),graph);
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,fixture.second->Header().GetHash(),fixture.second->Height()),Status::Ok);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_NO_THROW(service.setHeaderChainSelector(headers));
    EXPECT_EQ(service.GetActiveTip(),nullptr);EXPECT_FALSE(service.IsStarted());
}
#endif
