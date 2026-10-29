#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
// Isolated real permanent-index graph. Every service is destroyed before the
// saved graph is restored; no production index-lifetime API is added.
struct RawIngressGraph {
    decltype(g_block_index) previous_index;
    decltype(g_candidates) previous_candidates;
    decltype(g_orphan_pool) previous_orphans;
    RawIngressGraph() {
        std::lock_guard<std::recursive_mutex> guard(g_block_index_mutex);
        previous_index=std::move(g_block_index); previous_candidates=std::move(g_candidates);
        previous_orphans=std::move(g_orphan_pool);
        g_block_index.clear(); g_candidates.clear(); g_orphan_pool.clear(); InvalidateAncestryCache();
    }
    ~RawIngressGraph() {
        std::lock_guard<std::recursive_mutex> guard(g_block_index_mutex);
        g_candidates.clear(); g_orphan_pool.clear(); g_block_index.clear();
        g_block_index=std::move(previous_index); g_candidates=std::move(previous_candidates);
        g_orphan_pool=std::move(previous_orphans); InvalidateAncestryCache();
    }
};
class RawIngressFixture {
public:
    RawIngressGraph graph;
    OrchardAdmissionFixture f;
    struct Notices final:RuntimeBlockNotifications {
        unsigned published=0;
        struct Prepared final:PreparedRuntimeBlockNotifications {
            Notices& owner; explicit Prepared(Notices& v):owner(v){}
            void PublishAfterCommit()noexcept override{++owner.published;}
        };
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection)override {
            OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height==102);
            return std::make_unique<Prepared>(*this);
        }
    };
    std::shared_ptr<Notices> notices=std::make_shared<Notices>();
    std::shared_ptr<BlockStorage> files=std::make_shared<BlockStorage>();
    CBlockIndex* parent=nullptr;
    DaemonContext context;
    DaemonContext* previous=DaemonContext::instance();
    explicit RawIngressFixture(bool real_pow=false,
        OrchardAdmissionFixture::HistoricalSpend spend=OrchardAdmissionFixture::HistoricalSpend::None):f(real_pow,spend) {
        // Header-only siblings exercise the real missing-body logging path.
        // Match the logger dependency normally installed by DaemonApp.
        ShieldedStateStartupTestAccess::InitializeRawIngressLogger(*f.service);
        for(uint32_t height=0;height<f.blocks.size();++height) {
            auto* index=dinero::AddBlockIndex(f.blocks[height].header,height);
            OrchardAdmissionFixture::Require(index!=nullptr);
            index->status|=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
            index->chainwork=f.db.getBlockWork(index->hash)->GetHex();parent=index;
        }
        ShieldedStateStartupTestAccess::BoundaryState(*f.service,*parent,*f.replay);
        OrchardAdmissionFixture::Require(files->init(f.path/"flatfiles")==Status::Ok);
        f.service->setBlockStorage(files);f.service->setRuntimeBlockNotifications(notices);
        context.chainstate=f.service;context.mempool=f.ingress;DaemonContext::setInstance(&context);
    }
    ~RawIngressFixture() {
        DaemonContext::setInstance(previous);
        f.service->setBlockStorage(nullptr);
        files->close();
    }
    auto Build() {
        const auto shield=f.Shield();
        OrchardAdmissionFixture::Require(f.ingress->SubmitBody(shield,TxOrigin::INTERNAL).accepted());
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        return assembler.CreateOrchardBlock(OrchardMiningPayout);
    }
    auto Submit(const std::vector<uint8_t>& bytes) {
        return BlockAcceptor::AcceptBlockFromRPC(util::hex(bytes),"isolated-orchard-fixture");
    }
};
}
TEST(OrchardRawIngress, ActualRpcRouterConnectsExactShieldBody) {
    RawIngressFixture fixture;const auto built=fixture.Build();ASSERT_TRUE(built);
    const auto prior_marker=fixture.f.db.getShieldedTipMarker();ASSERT_TRUE(prior_marker.ok());
    const auto result=fixture.Submit(built->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
    EXPECT_TRUE(result.connected);EXPECT_FALSE(result.relayed);EXPECT_EQ(result.height,102u);
    EXPECT_EQ(result.block_hash,built->Header().GetHash());EXPECT_EQ(fixture.notices->published,1u);
    ASSERT_NE(fixture.f.service->GetActiveTip(),fixture.parent);
    EXPECT_EQ(fixture.f.service->GetActiveTip(),dinero::FindBlockIndex(result.block_hash));
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*fixture.f.service));
    std::string alignment;EXPECT_TRUE(fixture.f.service->IsCanonicalStateAligned(&alignment))<<alignment;
    ASSERT_TRUE(fixture.f.db.getShieldedTipMarker().ok());
    const auto marker=fixture.f.db.getShieldedTipMarker();
    EXPECT_EQ(marker->height,102);
    EXPECT_EQ(marker->block_hash,result.block_hash);
    EXPECT_EQ(marker->shielded_root,prior_marker->shielded_root);
    EXPECT_EQ(marker->tree_size,prior_marker->tree_size);
    EXPECT_EQ(marker->nullifier_count,prior_marker->nullifier_count);
    const auto retired=fixture.f.db.getLegacyRetirementState();ASSERT_TRUE(retired.ok());
    EXPECT_EQ(retired->height,102u);EXPECT_EQ(retired->block_hash,result.block_hash);
    EXPECT_EQ(retired->record.boundary_parent,fixture.parent->hash);
    EXPECT_EQ(retired->record.tree_root,prior_marker->shielded_root);
    EXPECT_EQ(retired->record.tree_size,prior_marker->tree_size);
    EXPECT_EQ(retired->record.nullifier_count,prior_marker->nullifier_count);
    ASSERT_TRUE(fixture.f.db.getOrchardState().ok());EXPECT_EQ(fixture.f.db.getOrchardState()->pool_balance,5000u);
    const auto retained=ReadRuntimeBlockUnderLock(fixture.f.db,fixture.files.get(),result.block_hash,102);
    ASSERT_TRUE(retained.ok());ASSERT_TRUE(retained->IsOrchardProfile());EXPECT_EQ(retained->Orchard().WireBytes(),built->WireBytes());
    const auto metadata=fixture.f.db.getHeaderMetadata(result.block_hash);ASSERT_TRUE(metadata.ok());
    EXPECT_NE(metadata->status_flags&BLOCK_HAVE_UNDO,0u);EXPECT_GT(metadata->undo_size,0u);
    const auto again=fixture.Submit(built->WireBytes());ASSERT_TRUE(again.accepted())<<again.reason;
    EXPECT_TRUE(again.connected);EXPECT_FALSE(again.relayed);EXPECT_EQ(fixture.notices->published,1u);
    const auto after=fixture.f.db.getHeaderMetadata(result.block_hash);ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->status_flags,metadata->status_flags);EXPECT_EQ(after->undo_file,metadata->undo_file);
    EXPECT_EQ(after->undo_pos,metadata->undo_pos);EXPECT_EQ(after->undo_size,metadata->undo_size);
}
TEST(OrchardRawIngress, MissingOwnerRefusesBeforePersistenceThenRetry) {
    RawIngressFixture fixture;const auto built=fixture.Build();ASSERT_TRUE(built);
    fixture.f.service->setRuntimeBlockNotifications(nullptr);
    const auto refused=fixture.Submit(built->WireBytes());EXPECT_EQ(refused.code,BlockRejectCode::CONNECT_FAILED);
    EXPECT_FALSE(refused.connected);EXPECT_EQ(fixture.f.db.getHeader(built->Header().GetHash()).status(),Status::NotFound);
    EXPECT_EQ(fixture.f.service->GetActiveTip(),fixture.parent);EXPECT_EQ(fixture.notices->published,0u);
    fixture.f.service->setRuntimeBlockNotifications(fixture.notices);
    const auto retry=fixture.Submit(built->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_TRUE(retry.connected);EXPECT_EQ(fixture.notices->published,1u);
}
TEST(OrchardRawIngress, StrictIncomingHeaderAndExactFramingRefuse) {
    RawIngressFixture fixture;const auto built=fixture.Build();ASSERT_TRUE(built);
    auto header=built->Header();header.reserved[0]=1;auto bytes=built->WireBytes();
    const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto bad=fixture.Submit(bytes);EXPECT_FALSE(bad.accepted());EXPECT_FALSE(bad.connected);
    EXPECT_EQ(fixture.f.db.getHeader(header.GetHash()).status(),Status::NotFound);
    bytes=built->WireBytes();bytes.push_back(0);
    EXPECT_FALSE(fixture.Submit(bytes).accepted());
    EXPECT_EQ(fixture.f.db.getHeader(built->Header().GetHash()).status(),Status::NotFound);
    EXPECT_EQ(fixture.f.service->GetActiveTip(),fixture.parent);EXPECT_EQ(fixture.notices->published,0u);
    fixture.f.CheckUnpublished();
    const auto retry=fixture.Submit(built->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
}
TEST(OrchardRawIngress, DisconnectDatabaseReopenAndRetainedBodyReconnect) {
    RawIngressFixture fixture;const auto built=fixture.Build();ASSERT_TRUE(built);
    const auto first=fixture.Submit(built->WireBytes());ASSERT_TRUE(first.accepted())<<first.reason;
    auto* child=fixture.f.service->GetActiveTip();ASSERT_NE(child,fixture.parent);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*fixture.f.service,child));
    EXPECT_EQ(fixture.f.service->GetActiveTip(),fixture.parent);fixture.f.CheckUnpublished();
    fixture.f.db.close();ASSERT_EQ(fixture.f.db.init(fixture.f.path),Status::Ok);
    const auto again=fixture.Submit(built->WireBytes());ASSERT_TRUE(again.accepted())<<again.reason;
    EXPECT_TRUE(again.connected);EXPECT_EQ(fixture.f.service->GetActiveTip(),child);
    EXPECT_EQ(fixture.notices->published,3u);EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*fixture.f.service));
}
TEST(OrchardRawIngress, PreferredHeaderSiblingDoesNotChangeCanonicalIndexPublication) {
    RawIngressFixture fixture;const auto built=fixture.Build();ASSERT_TRUE(built);
    // Two ordinary sibling headers with equal work. Keep the header-only sibling
    // preferred, so ABC cannot incidentally rehydrate the submitted block's
    // metadata while importing its preferred header branch.
    auto low=built->Header(),high=low;
    for(uint32_t nonce=0;nonce<64;++nonce) {
        auto header=built->Header();header.nonce=nonce;
        if(header.GetHash()<low.GetHash())low=header;
        if(high.GetHash()<header.GetHash())high=header;
    }
    ASSERT_NE(low.GetHash(),high.GetHash());
    auto headers=std::make_shared<consensus::HeaderChainSelector>();
    for(const auto& block:fixture.f.blocks)ASSERT_TRUE(headers->AddHeader(block.header));
    ASSERT_TRUE(headers->AddHeader(low));
    const auto preferred=headers->GetBestHeaderValue();ASSERT_TRUE(preferred);
    ASSERT_EQ(preferred->hash,low.GetHash());
    fixture.f.service->setHeaderChainSelector(headers);
    auto wire=built->WireBytes();const auto prefix=high.SerializeForHash();
    std::copy(prefix.begin(),prefix.end(),wire.begin());
    const auto result=fixture.Submit(wire);ASSERT_TRUE(result.accepted())<<result.reason;
    ASSERT_TRUE(result.connected);EXPECT_EQ(result.block_hash,high.GetHash());
    const auto still_preferred=headers->GetBestHeaderValue();ASSERT_TRUE(still_preferred);
    EXPECT_EQ(still_preferred->hash,low.GetHash());
    auto* selected=fixture.f.service->GetActiveTip();ASSERT_NE(selected,fixture.parent);
    EXPECT_EQ(selected->hash,high.GetHash());EXPECT_EQ(fixture.notices->published,1u);
    const auto metadata=fixture.f.db.getHeaderMetadata(high.GetHash());ASSERT_TRUE(metadata.ok());
    EXPECT_EQ(selected->status,metadata->status_flags);
    EXPECT_EQ(selected->file_number,metadata->file_number);EXPECT_EQ(selected->data_pos,metadata->data_pos);
    EXPECT_EQ(selected->data_size,metadata->data_size);EXPECT_EQ(selected->undo_file,metadata->undo_file);
    EXPECT_EQ(selected->undo_pos,metadata->undo_pos);EXPECT_EQ(selected->undo_size,metadata->undo_size);
    EXPECT_EQ(metadata->status_flags&BLOCK_VALID_MASK,uint32_t(BLOCK_VALID_MASK));
    EXPECT_NE(metadata->status_flags&BLOCK_HAVE_UNDO,0u);
    const auto body=ReadRuntimeBlockUnderLock(fixture.f.db,fixture.files.get(),high.GetHash(),102);
    ASSERT_TRUE(body.ok());ASSERT_TRUE(body->IsOrchardProfile());EXPECT_EQ(body->Orchard().WireBytes(),wire);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*fixture.f.service));
}

#else
TEST(OrchardRawIngress, InactiveProfileKeepsHistoricalRoute) {
    ChainstateService service;
    EXPECT_FALSE(service.TryAcceptOrchardBlockFromRPC(""));
}
#endif
