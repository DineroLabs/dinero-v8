#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
void ConfigureCompactServiceContext(DaemonContext& context,const CompactStartupFixture& fixture) {
    auto config=std::make_shared<ConfigService>();
    config->Set("datadir",(fixture.f.path/"flatfiles").string());
    context.config=std::move(config);
    context.logger=std::make_shared<LoggerService>("");
    context.block_storage=fixture.files;
}
}

TEST(OrchardCompactService, InitializeReconstructsWithoutFullStorageOrAdmission) {
    CompactStartupFixture fixture;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true; // restored by the existing fixture owner
    DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();
    service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));
    EXPECT_EQ(service->GetConsensusUTXOSet(),nullptr);
    EXPECT_EQ(service->utxoIndex(),nullptr);
    EXPECT_EQ(service->GetActiveTip(),nullptr);
    EXPECT_FALSE(service->IsStarted());
    EXPECT_THROW((void)ChainstateService::AcquireWalletIndexUse(service),std::exception);
    EXPECT_THROW(service->setChainDB(nullptr),std::exception);
    EXPECT_THROW(service->setOwnedChainDB(std::make_shared<ChainDB>()),std::exception);
    EXPECT_NO_THROW(service->setChainDB(&fixture.reopened));
    EXPECT_FALSE(service->Init(context)); // never replace the enrolled owner
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    EXPECT_FALSE(std::filesystem::exists(fixture.f.path/"flatfiles"/"orchard-startup-replay"));
}

TEST(OrchardCompactService, ExistingScratchRefusesWithoutFallbackOrDeletion) {
    CompactStartupFixture fixture;
    GetConfig().utreexo_stateless=true;
    const auto scratch=fixture.f.path/"flatfiles"/"orchard-startup-replay";
    ASSERT_TRUE(std::filesystem::create_directory(scratch));
    {std::ofstream sentinel(scratch/"owned-by-earlier-operation");sentinel<<"retain";ASSERT_TRUE(sentinel.good());}
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    ChainstateService service;service.setChainDB(&fixture.reopened);
    EXPECT_FALSE(service.Init(context));
    EXPECT_EQ(service.GetConsensusUTXOSet(),nullptr);
    EXPECT_EQ(service.utxoIndex(),nullptr);
    EXPECT_EQ(service.GetActiveTip(),nullptr);
    EXPECT_FALSE(service.IsStarted());
    std::ifstream sentinel(scratch/"owned-by-earlier-operation");std::string retained;
    sentinel>>retained;EXPECT_EQ(retained,"retain");
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}

namespace {
std::unique_ptr<OrchardBranchReplay> CompleteCompactInverseBranch(CompactStartupFixture& fixture) {
    // Complete the genuine independent history/Orchard branch before acquiring
    // the selected writer. This does not manufacture a test validation seal.
    std::vector<BlockHeader> history;std::set<TxId> ids;
    for(const auto& block:fixture.f.blocks) {
        history.push_back(block.header);
        for(const auto& tx:block.vtx)OrchardAdmissionFixture::Require(ids.insert(tx.GetTxid()).second);
    }
    const auto& last=*fixture.second;
    auto branch=std::make_unique<OrchardBranchReplay>(*fixture.independent,history,ids,
        OrchardParentReplay::Target{last.Height(),last.Header().GetHash(),
            *fixture.reopened.getBlockWork(last.Header().GetHash())});
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for(const auto& source:{fixture.first,fixture.second})
        branch->Append(OrchardBlockCandidate::DecodeExact(source->WireBytes()),source->Height(),
            *fixture.reopened.getBlockWork(source->Header().GetHash()),uint64_t(now));
    branch->Finish();return branch;
}
auto PrepareCompactSealedInverse(CompactStartupFixture& fixture,OrchardCompactChainstate& owner,
    const std::shared_ptr<const CatalogSolvedTemplate>& source,const consensus::ValidatedOrchardBlock& seal,
    std::optional<bool> witness_override=std::nullopt) {
    const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
    const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
    return PreparedOrchardChainstateWrite::DisconnectCompactIndexed(fixture.startup_mutex,fixture.reopened,
        fixture.f.token,*fixture.files,*dinero::FindBlockIndex(context.block_hash),owner,context,block,
        *fixture.reopened.getHeader(context.parent_hash),[&fixture](uint32_t h){return fixture.Mtp(h);},witness_override.value_or(seal.WitnessRequired()),&seal);
}
}
TEST(OrchardCompactInverse, CompactDetachedInverseAbandonCommitAndBoundaryReopen) {
    CompactStartupFixture fixture;auto branch=CompleteCompactInverseBranch(fixture);
    auto owner=fixture.Restore();ASSERT_TRUE(owner);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    const auto& second=branch->ProvenBlock(fixture.second->Height(),fixture.second->Header().GetHash());
    ASSERT_EQ(second.WitnessRequired(),Params().enforce_witness_commitment &&
        fixture.second->Height()>=Params().witness_commitment_enforcement_height);
    {auto abandoned=PrepareCompactSealedInverse(fixture,*owner,fixture.second,second);}
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    {auto write=PrepareCompactSealedInverse(fixture,*owner,fixture.second,second);write->Commit();}
    const auto& first=branch->ProvenBlock(fixture.first->Height(),fixture.first->Header().GetHash());
    {auto write=PrepareCompactSealedInverse(fixture,*owner,fixture.first,first);write->Commit();}
    EXPECT_EQ(fixture.reopened.getTip()->hash,fixture.parent->hash);
    size_t coins=0;ASSERT_EQ(fixture.reopened.forEachUTXO(
        [&](const uint256&,uint32_t,const Coin&){++coins;return true;}),Status::Ok);EXPECT_EQ(coins,0u);
    owner.reset();fixture.reopened.close();ASSERT_EQ(fixture.reopened.init(fixture.f.path),Status::Ok);
    auto boundary=fixture.Restore();ASSERT_TRUE(boundary);
    fixture.Connect(*boundary,fixture.first);fixture.Connect(*boundary,fixture.second);
    EXPECT_EQ(*fixture.reopened.getOrchardCatalogState(fixture.second->Header().GetHash()),
        fixture.expected_catalog.at(fixture.second->Header().GetHash()));
    EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactInverse, CompactDetachedInverseRejectsForeignSealAndWitnessPolicy) {
    CompactStartupFixture fixture;auto branch=CompleteCompactInverseBranch(fixture);
    auto owner=fixture.Restore();ASSERT_TRUE(owner);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    const auto& wrong=branch->ProvenBlock(fixture.first->Height(),fixture.first->Header().GetHash());
    const auto& correct=branch->ProvenBlock(fixture.second->Height(),fixture.second->Header().GetHash());
    EXPECT_THROW((void)PrepareCompactSealedInverse(fixture,*owner,fixture.second,wrong),std::exception);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    EXPECT_THROW((void)PrepareCompactSealedInverse(fixture,*owner,fixture.second,correct,!correct.WitnessRequired()),std::exception);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    {auto retry=PrepareCompactSealedInverse(fixture,*owner,fixture.second,correct);}
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
#endif
