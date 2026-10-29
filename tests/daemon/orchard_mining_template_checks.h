#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
constexpr const char* OrchardMiningPayout = "rdin1pl300xwevn94hhr56l629fzkzxgdkp6u5msynwq3045yqsm0c5y5qyan2ec";
void WireOrchardAssembler(BlockAssembler& assembler, OrchardAdmissionFixture& fixture) {
    const auto owner = fixture.service;
    assembler.SetChainstateReadGuardFactory([owner] {
        return ChainstateService::AcquireMiningReadGuard(owner);
    });
    const auto pool = fixture.ingress;
    assembler.SetMempoolAccessFactory([pool] { return MempoolService::AcquirePoolUse(pool); });
}
}
TEST(OrchardMiningTemplate, MixedBodiesFeesCommitmentsAndProof) {
    OrchardAdmissionFixture f;
    const auto shield = f.Shield(1);
    ASSERT_TRUE(f.ingress->SubmitBody(shield, TxOrigin::INTERNAL).accepted());
    const OutPoint point(f.blocks[2].vtx.front().GetTxid(), 0);
    const auto parent = SelectionSpend(f, point, f.replay->ProvenUtxos().at(point), 100000);
    const auto parent_body = MempoolTransaction(parent);
    const auto child = SelectionSpend(f, OutPoint(parent.GetTxid(), 0), parent_body.OutputCoin(0, 102), 200000);
    ASSERT_TRUE(f.ingress->SubmitBody(parent_body, TxOrigin::INTERNAL).accepted());
    ASSERT_TRUE(f.ingress->SubmitBody(MempoolTransaction(child), TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.db); WireOrchardAssembler(assembler, f);
    const auto built = assembler.CreateOrchardBlock(OrchardMiningPayout);
    ASSERT_TRUE(built); ASSERT_EQ(built->Transactions().size(), 4u);
    EXPECT_EQ(built->TotalFees(), 400000u); EXPECT_EQ(built->Height(), 102u);
    EXPECT_EQ(built->Header().prev_block_hash, f.tip.hash);
    const auto body = OrchardBlockCandidate::DecodeExact(built->WireBytes());
    std::string error;
    EXPECT_TRUE(body.CheckIdentityCommitments(true, error));
    EXPECT_TRUE(body.CheckCoinbaseHeight(102, error)); EXPECT_TRUE(body.CheckSizeLimits(error));
    EXPECT_EQ(built->Weight(), body.Weight());
    const auto& cb = body.Transactions().front().Historical();
    EXPECT_EQ(cb.vout.front().value.GetUna(), ConsensusSubsidy::GetBlockSubsidy(
        102, Params().sixty_second_activation_height).GetUna() + 400000);
    ASSERT_TRUE(consensus::FindFilterCommitmentIndex(cb));
    EXPECT_EQ(consensus::FindStateCommitment(cb, consensus::StateCommitmentEncoding::Orchard).status,
        consensus::StateCommitmentStatus::Ok);
    std::map<TxId,std::vector<uint8_t>> expected{{shield.GetTxid(),shield.Serialize()},
        {parent.GetTxid(),parent.Serialize()}, {child.GetTxid(),child.Serialize()}};
    std::set<TxId> seen;
    for (size_t i = 1; i < body.Transactions().size(); ++i) {
        const auto& tx = body.Transactions()[i];
        if (tx.GetTxid() == child.GetTxid()) EXPECT_TRUE(seen.contains(parent.GetTxid()));
        EXPECT_EQ(tx.Serialize(TxSerializationMode::WithWitness), expected.at(tx.GetTxid()));
        EXPECT_TRUE(seen.insert(tx.GetTxid()).second);
    }
    ASSERT_TRUE(body.Utreexo()); EXPECT_EQ(body.Utreexo()->spent_outputs.size(), 3u);
    EXPECT_EQ(body.Utreexo()->spend_proof.targets.size(), 2u);
    FirstBoundaryView view(*f.replay);
    const auto context = consensus::SelectedOrchardBlockContext(body.Header(), 102); ASSERT_TRUE(context);
    const auto coins = consensus::PrepareOrchardBlockCoinsUnderChainstateLock(body, *context, view, {}, true);
    EXPECT_NO_THROW(consensus::CheckOrchardBlockUtreexoProof(body, coins, f.blocks.back().header, *f.replay->Forest()));
    EXPECT_EQ(consensus::PrepareOrchardForestTransition(coins, f.blocks.back().header, *f.replay->Forest()).Root(), body.Header().utreexo_root);
    EXPECT_EQ(f.ingress->mempool().size(), 3u); f.CheckUnpublished();
}
TEST(OrchardMiningTemplate, CompleteFrameBudgetAndOwnerRefusal) {
    OrchardAdmissionFixture f; const auto shield = f.Shield();
    ASSERT_TRUE(f.ingress->SubmitBody(shield, TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.db); assembler.setMempool(&f.ingress->mempool());
    EXPECT_THROW(assembler.CreateOrchardBlock(OrchardMiningPayout), std::runtime_error);
    WireOrchardAssembler(assembler, f);
    f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_THROW(assembler.CreateOrchardBlock(OrchardMiningPayout), std::runtime_error);
    f.service->setRuntimeBlockNotifications(std::make_shared<OrchardAdmissionFixture::AdmissionNotifications>());
    // The body fits exactly before the real coinbase/header/proof are charged.
    const auto body_budget = uint32_t(shield.GetSize() * 4);
    ASSERT_EQ(f.ingress->mempool().CaptureTypedBlockSelection(body_budget / 4, body_budget, 102).transactions.size(), 1u);
    assembler.SetMaxBlockWeight(body_budget);
    const auto small = assembler.CreateOrchardBlock(OrchardMiningPayout); ASSERT_TRUE(small);
    EXPECT_EQ(small->Transactions().size(), 1u); EXPECT_EQ(small->TotalFees(), 0u);
    EXPECT_LE(small->Weight(), body_budget);
    assembler.SetMaxBlockWeight(1);
    EXPECT_THROW(assembler.CreateOrchardBlock(OrchardMiningPayout), MiningTemplateSizeError);
    assembler.SetMaxBlockWeight(4000000);
    const auto retry = assembler.CreateOrchardBlock(OrchardMiningPayout); ASSERT_TRUE(retry);
    EXPECT_EQ(retry->Transactions().size(), 2u); EXPECT_EQ(retry->Transactions()[1].Serialize(), shield.Serialize());
    EXPECT_EQ(f.ingress->mempool().size(), 1u); f.CheckUnpublished();
}
TEST(OrchardMiningTemplate, EmptyFilterIsExplicitlyCommitted) {
    OrchardAdmissionFixture f; BlockAssembler assembler(&f.db); WireOrchardAssembler(assembler, f);
    const auto ordinary = assembler.CreateOrchardBlock(OrchardMiningPayout); ASSERT_TRUE(ordinary);
    auto cb = ordinary->Transactions().front().Historical();
    // The owned constructor also accepts a zero-valued, empty-script coinbase
    // payout; its filter contains no spendable or spent scripts.
    cb.vout.clear(); cb.vout.emplace_back(AmountUna::Zero(), std::vector<uint8_t>{});
    auto owner = ChainstateService::AcquireMiningReadGuard(f.service); ASSERT_TRUE(owner);
    const auto built = owner->BuildOrchardTemplate(ordinary->Header(), cb, {}, 102); ASSERT_TRUE(built);
    const auto candidate = OrchardBlockCandidate::DecodeExact(built->WireBytes());
    const auto& coinbase = candidate.Transactions().front().Historical();
    const auto index = consensus::FindFilterCommitmentIndex(coinbase); ASSERT_TRUE(index);
    const auto commitment = consensus::ExtractFilterCommitment(coinbase, *index); ASSERT_TRUE(commitment);
    EXPECT_TRUE(commitment->IsNull());
    FirstBoundaryView view(*f.replay);
    const auto context = consensus::SelectedOrchardBlockContext(candidate.Header(), 102); ASSERT_TRUE(context);
    const auto coins = consensus::PrepareOrchardBlockCoinsUnderChainstateLock(candidate, *context, view, {}, true);
    EXPECT_TRUE(consensus::CheckOrchardBlockFilter(candidate, coins).IsEmpty());
    f.CheckUnpublished();
}
TEST(OrchardMiningTemplate, RealShieldConnectDisconnectAndReopen) {
    using Access = ShieldedStateStartupTestAccess;
    OrchardAdmissionFixture f; const auto shield = f.Shield();
    ASSERT_TRUE(f.ingress->SubmitBody(shield, TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.db); WireOrchardAssembler(assembler, f);
    const auto built = assembler.CreateOrchardBlock(OrchardMiningPayout); ASSERT_TRUE(built);
    const auto candidate = OrchardBlockCandidate::DecodeExact(built->WireBytes());
    auto files = std::make_shared<BlockStorage>(); ASSERT_EQ(files->init(f.path/"flatfiles"), Status::Ok);
    f.service->setBlockStorage(files);
    auto headers = std::make_shared<consensus::HeaderChainSelector>();
    for (const auto& b : f.blocks) ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(candidate.Header())); f.service->setHeaderChainSelector(headers);
    const auto work = ChainworkFromHex(f.tip.chainwork) + GetBlockProof(candidate.Header().difficulty);
    ASSERT_EQ(f.db.putHeader(f.token, candidate.Header().GetHash(), candidate.Header(), 102, work), Status::Ok);
    const auto location = files->writeBlockBytes(candidate.Header().GetHash(),
        {candidate.WireBytes().begin(),candidate.WireBytes().end()}); ASSERT_TRUE(location.ok());
    ChainDB::PersistedHeaderMetadata metadata; metadata.height=102; metadata.parent_hash=f.tip.hash;
    metadata.chainwork=work; metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA;
    metadata.file_number=location->file_number; metadata.data_pos=location->offset; metadata.data_size=location->size;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,candidate.Header().GetHash(),metadata),Status::Ok);
    CBlockIndex child(candidate.Header(),102); child.pprev=&f.tip; child.chainwork=work.GetHex(); child.status=metadata.status_flags;
    child.file_number=metadata.file_number; child.data_pos=metadata.data_pos; child.data_size=metadata.data_size;
    struct Notices final:RuntimeBlockNotifications {
        unsigned published=0;
        struct Prepared final:PreparedRuntimeBlockNotifications {
            Notices& owner; explicit Prepared(Notices& o):owner(o){}
            void PublishAfterCommit()noexcept override{++owner.published;}
        };
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection)override {
            OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height==102);
            return std::make_unique<Prepared>(*this);
        }
    };
    auto notices=std::make_shared<Notices>(); f.service->setRuntimeBlockNotifications(notices);
    struct RestoreTip {
        OrchardAdmissionFixture& fixture; CBlockIndex& child;
        ~RestoreTip(){
            if(Access::BoundaryTipIs(*fixture.service,&child)) Access::DisconnectBoundary(*fixture.service,&child);
            fixture.service->setRuntimeBlockNotifications(nullptr); fixture.service->setBlockStorage(nullptr);
        }
    } restore{f,child};
    std::string error;bool invalid=false;
    ASSERT_TRUE(Access::ConnectBoundary(*f.service,&child,error,invalid))<<error;
    EXPECT_FALSE(invalid); EXPECT_EQ(notices->published,1u);
    ASSERT_TRUE(f.db.getOrchardState().ok()); EXPECT_EQ(f.db.getOrchardState()->pool_balance,5000u);
    EXPECT_EQ(f.db.getOrchardState()->block_hash,candidate.Header().GetHash());
    EXPECT_EQ(f.db.getCoin(shield.Inputs().front().txid.AsUint256(),0).status(),Status::NotFound);
    EXPECT_TRUE(Access::AuditBoundary(*f.service));
    ASSERT_TRUE(Access::DisconnectBoundary(*f.service,&child));
    EXPECT_EQ(notices->published,2u); f.CheckUnpublished();
    ASSERT_TRUE(f.db.getCoin(shield.Inputs().front().txid.AsUint256(),0).ok());
    f.db.close(); ASSERT_EQ(f.db.init(f.path),Status::Ok);
    ASSERT_TRUE(Access::ConnectBoundary(*f.service,&child,error,invalid))<<error;
    EXPECT_EQ(notices->published,3u); EXPECT_TRUE(Access::AuditBoundary(*f.service));
    ASSERT_TRUE(Access::DisconnectBoundary(*f.service,&child));
}
#else
TEST(OrchardMiningTemplate, DefaultOwnerUnavailable) {
    MiningChainstateReadGuard owner;
    EXPECT_FALSE(owner.BuildOrchardTemplate({}, {}, {}, 1));
}
#endif
