#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
// RawIngressFixture replaces the isolated live coin owner after its base
// construction. Initialize this fixture's real pool after that replacement so
// its historical coin adapter points to the current owner before any admission.
struct CanonicalPoolFixture : RawIngressFixture {
    explicit CanonicalPoolFixture(bool real_pow=false,
        OrchardAdmissionFixture::HistoricalSpend spend=OrchardAdmissionFixture::HistoricalSpend::None):RawIngressFixture(real_pow,spend) {
        f.ingress->Stop();
        context.mempool.reset();
        f.ingress=std::make_shared<MempoolService>();
        context.config=std::make_shared<ConfigService>();
        context.logger_interface=f.logger.get();
        OrchardAdmissionFixture::Require(f.ingress->Init(context));
        context.mempool=f.ingress;
    }
};
struct CanonicalPoolNotices final:RuntimeBlockNotifications {
    unsigned published=0;
    struct Event final:PreparedRuntimeBlockNotifications {
        CanonicalPoolNotices& owner;
        explicit Event(CanonicalPoolNotices& value):owner(value){}
        void PublishAfterCommit()noexcept override{++owner.published;}
    };
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection)override {
        OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height>=102);
        return std::make_unique<Event>(*this);
    }
};
struct CanonicalPoolLogger final:ILogger {
    bool refuse=false;
    void info(const std::string& text)override {
        if(refuse && text.find("Block 102 connected:")!=std::string::npos)
            throw std::runtime_error("fixture reversible pool preparation refusal");
    }
    void log(LogLevel,const std::string& text)override{info(text);}
    void debug(const std::string&)override{} void warning(const std::string&)override{}
    void error(const std::string&)override{} void setLogLevel(LogLevel)override{}
    void setLogFile(const std::string&)override{} void shutdown()override{}
};
// Capture untrusted wire for exercising the production metadata-aware path.
// The older proof-only verifier intentionally refuses these legacy coinbases
// during the regtest grace window; it is not this fixture's admission owner.
auto CanonicalPoolUnverifiedProof(RawIngressFixture& fixture,const MempoolTransaction& body) {
    auto selected=fixture.f.service->AcquireBlockIngressActivationLock();
    auto* coins=fixture.f.service->GetConsensusUTXOSet();
    std::shared_ptr<consensus::IUTXOProvider> provider(fixture.f.service,coins);
    network::BridgeNode bridge(provider,&coins->GetForest(),nullptr,nullptr,nullptr,coins);
    return CaptureUtreexoTransactionPayload(body,bridge);
}
auto CanonicalPoolProof(RawIngressFixture& fixture,const MempoolTransaction& body) {
    auto selected=fixture.f.service->AcquireBlockIngressActivationLock();
    auto* coins=fixture.f.service->GetConsensusUTXOSet();
    std::shared_ptr<consensus::IUTXOProvider> provider(fixture.f.service,coins);
    network::BridgeNode bridge(provider,&coins->GetForest(),nullptr,nullptr,nullptr,coins);
    const auto wire=CaptureUtreexoTransactionPayload(body,bridge);
    OrchardAdmissionFixture::Require(bool(wire));
    const auto stump=[&]{auto guard=coins->LockForestShared();return consensus::UtreexoStump::fromForest(coins->GetForest());}();
    auto verified=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies)
        .VerifyInputs(stump,fixture.f.service->GetActiveTip()->height);
    return verified;
}
MempoolTransaction CanonicalPoolSurvivor(RawIngressFixture& f) {
    const OutPoint point(f.f.blocks[2].vtx.front().GetTxid(),0);
    return MempoolTransaction(SelectionSpend(f.f,point,f.f.replay->ProvenUtxos().at(point),200000));
}
}
TEST(OrchardCanonicalPool, ConfirmedShieldLeavesPoolBeforeNextBlock) {
    CanonicalPoolFixture f;const auto first=f.Build();ASSERT_TRUE(first);
    auto notices=std::make_shared<CanonicalPoolNotices>();f.f.service->setRuntimeBlockNotifications(notices);
    auto& pool=f.f.ingress->mempool();const auto shield=first->Transactions()[1].GetTxid().AsUint256();
    const auto survivor=CanonicalPoolSurvivor(f);ASSERT_TRUE(f.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto parent_root=f.f.blocks.back().header.utreexo_root;
    ASSERT_TRUE(pool.refreshProof(survivor.GetTxid().AsUint256(),{parent_root.begin(),parent_root.end()},101));
    auto relay=std::make_shared<TxRelayManager>(nullptr);relay->SetCsnMode(true);f.context.tx_relay=relay;
    unsigned refresh_attempts=0;
    relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){
        ++refresh_attempts;EXPECT_EQ(pool.size(),1u);EXPECT_EQ(notices->published,1u);
        EXPECT_EQ(f.f.service->GetActiveTip()->height,102u);
        throw std::runtime_error("fixture ambiguous postcommit refresh delivery");
    });
    const auto result=f.Submit(first->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
    EXPECT_FALSE(pool.hasTransaction(shield));ASSERT_EQ(pool.size(),1u);
    ASSERT_TRUE(pool.getMempoolEntry(survivor.GetTxid().AsUint256()));
    EXPECT_EQ(pool.getMempoolEntry(survivor.GetTxid().AsUint256())->tx.Serialize(),survivor.Serialize());
    EXPECT_EQ(pool.getStats().last_connected_height,102u);EXPECT_EQ(refresh_attempts,1u);
    EXPECT_EQ(pool.getStaleCount(),1u);
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    EXPECT_EQ(second->Height(),103u);ASSERT_EQ(second->Transactions().size(),2u);
    EXPECT_EQ(second->Transactions()[1].Serialize(TxSerializationMode::WithWitness),survivor.Serialize());
    const auto next=f.Submit(second->WireBytes());ASSERT_TRUE(next.accepted())<<next.reason;
    EXPECT_EQ(pool.size(),0u);EXPECT_EQ(pool.getStats().last_connected_height,103u);
    EXPECT_EQ(notices->published,2u);EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardCanonicalPool, PreparationRefusalPreservesPoolAndCanonicalParent) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);auto& pool=f.f.ingress->mempool();
    const auto id=built->Transactions()[1].GetTxid().AsUint256();const auto before=pool.getMempoolEntry(id);ASSERT_TRUE(before);
    CanonicalPoolLogger logger;pool.setLogger(&logger);
    struct RestoreLogger {Mempool& pool;ILogger* previous;~RestoreLogger(){pool.setLogger(previous);}} restore{pool,f.f.logger.get()};
    const auto previous_height=pool.getStats().last_connected_height;logger.refuse=true;
    const auto refused=f.Submit(built->WireBytes());EXPECT_FALSE(refused.accepted());EXPECT_FALSE(refused.connected);
    ASSERT_TRUE(pool.getMempoolEntry(id));EXPECT_EQ(pool.getMempoolEntry(id)->tx.Serialize(),before->tx.Serialize());
    EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getStats().last_connected_height,previous_height);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);f.f.CheckUnpublished();
    logger.refuse=false;
    // Actual activation keeps the operationally failed candidate on cooldown.
    // Retry the same body after that existing 250ms policy interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto retry=f.Submit(built->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_EQ(pool.size(),0u);EXPECT_EQ(pool.getStats().last_connected_height,102u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardCanonicalPool, WrongContextAndMissingProviderPreservePool) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);auto& pool=f.f.ingress->mempool();
    DaemonContext other;DaemonContext::setInstance(&other);
    const auto refused=f.f.service->TryAcceptOrchardBlockFromRPC(util::hex(built->WireBytes()));
    DaemonContext::setInstance(&f.context);ASSERT_TRUE(refused);EXPECT_FALSE(refused->accepted());
    EXPECT_EQ(pool.size(),1u);EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);f.f.CheckUnpublished();
    f.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_FALSE(f.Submit(built->WireBytes()).accepted());EXPECT_EQ(pool.size(),1u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::WaitForActivationRetry(*f.f.service,built->Header().GetHash()));
    struct ChangeOwner final:RuntimeBlockNotifications {
        unsigned prepared=0;
        RawIngressFixture& fixture;
        explicit ChangeOwner(RawIngressFixture& value):fixture(value){}
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection direction)override {
            ++prepared;
            auto result=fixture.notices->Prepare(body,height,direction);
            fixture.context.mempool.reset();
            return result;
        }
    };
    auto changed_owner=std::make_shared<ChangeOwner>(f);
    f.f.service->setRuntimeBlockNotifications(changed_owner);
    const auto changed=f.Submit(built->WireBytes());f.context.mempool=f.f.ingress;
    EXPECT_EQ(changed_owner->prepared,1u);
    EXPECT_FALSE(changed.accepted());EXPECT_EQ(pool.size(),1u);EXPECT_EQ(f.notices->published,0u);
    f.f.CheckUnpublished();
    f.f.service->setRuntimeBlockNotifications(f.notices);
    // Read the real tracker: successive refusals increase the cooldown.
    // No retry is submitted while waiting and no tracker state is changed.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::WaitForActivationRetry(*f.f.service,built->Header().GetHash()));
    const auto retry=f.Submit(built->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_EQ(pool.size(),0u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardCanonicalPool, DisconnectReopenAndExplicitReadmissionKeepSelectedHeight) {
    CanonicalPoolFixture f;const auto first=f.Build();ASSERT_TRUE(first);auto& pool=f.f.ingress->mempool();
    // The old creation-height leaf cannot authenticate maturity at this parent.
    // Keep its actual proof-reader refusal; use a genuinely created v2 output below.
    EXPECT_FALSE(CanonicalPoolProof(f,CanonicalPoolSurvivor(f)));
    auto notices=std::make_shared<CanonicalPoolNotices>();f.f.service->setRuntimeBlockNotifications(notices);
    ASSERT_TRUE(f.Submit(first->WireBytes()).accepted());EXPECT_EQ(pool.size(),0u);
    auto* parent=f.f.service->GetActiveTip();ASSERT_EQ(parent->height,102u);
    const auto shield=f.f.Shield(2);ASSERT_TRUE(f.f.ingress->SubmitBody(shield,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto built=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(built);ASSERT_EQ(built->Height(),103u);
    const auto first_shield=MempoolTransaction::FromOrchard(first->Transactions()[1].Orchard());
    const auto point=OutPoint(first_shield.GetTxid(),0);
    const MempoolTransaction survivor(SelectionSpend(f.f,point,first_shield.OutputCoin(0,102),200000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto parent_receipt=CanonicalPoolProof(f,survivor);ASSERT_TRUE(parent_receipt);
    const auto result=f.Submit(built->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
    EXPECT_EQ(pool.size(),1u);auto* child=f.f.service->GetActiveTip();ASSERT_NE(child,parent);
    const auto child_receipt=CanonicalPoolProof(f,survivor);ASSERT_TRUE(child_receipt);
    ASSERT_TRUE(pool.publishProofPayload(*child_receipt));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,child));
    EXPECT_EQ(f.f.service->GetActiveTip(),parent);
    EXPECT_EQ(pool.getStats().last_connected_height,102u);EXPECT_EQ(pool.size(),1u);
    EXPECT_FALSE(pool.getCachedUtxoTxPayload(survivor.GetTxid().AsUint256()));
    EXPECT_FALSE(pool.publishProofPayload(*child_receipt));ASSERT_TRUE(pool.publishProofPayload(*parent_receipt));
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    ASSERT_TRUE(f.f.ingress->SubmitBody(shield,TxOrigin::INTERNAL).accepted());EXPECT_EQ(pool.size(),2u);
    const auto again=f.Submit(built->WireBytes());ASSERT_TRUE(again.accepted())<<again.reason;
    EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getStats().last_connected_height,103u);
    EXPECT_FALSE(pool.publishProofPayload(*parent_receipt));EXPECT_TRUE(pool.publishProofPayload(*child_receipt));
    EXPECT_EQ(notices->published,4u);EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}


TEST(OrchardPoolProofTransport, AdmissionRefreshAndSelectionUseExactProof) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto& pool=f.f.ingress->mempool();
    const auto body=MempoolTransaction::FromOrchard(block->Transactions()[1].Orchard());
    const auto id=body.GetTxid().AsUint256();
    const auto wire=CanonicalPoolUnverifiedProof(f,body);ASSERT_TRUE(wire);
    const auto payload=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        auto* coins=f.f.service->GetConsensusUTXOSet();auto forest=coins->LockForestShared();
        const auto stump=consensus::UtreexoStump::fromForest(coins->GetForest());
        ASSERT_EQ(f.f.service->GetActiveTip()->height,101u);
        EXPECT_FALSE(payload.VerifyInputs(stump,101));
    }
    ASSERT_TRUE(pool.removeTransaction(id));ASSERT_EQ(pool.size(),0u);
    const auto admitted=f.f.ingress->SubmitProofBody(payload,TxOrigin::INTERNAL);
    ASSERT_TRUE(admitted.accepted())<<admitted.message;
    ASSERT_EQ(pool.size(),1u);const auto entry=pool.getMempoolEntry(id);ASSERT_TRUE(entry);
    EXPECT_EQ(entry->cached_utxotx_payload,payload.Wire());EXPECT_EQ(entry->validated_at_root,payload.Root());
    EXPECT_EQ(entry->validated_at_height,101u);EXPECT_FALSE(entry->is_proof_stale);
    const auto selected=pool.CaptureTypedBlockSelection(1000000,4000000,102);
    ASSERT_TRUE(selected.available);ASSERT_EQ(selected.transactions.size(),1u);
    EXPECT_EQ(selected.transactions.front().Serialize(),body.Serialize());
    ASSERT_TRUE(f.f.ingress->SubmitProofBody(payload,TxOrigin::INTERNAL).accepted());
    EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getMempoolEntry(id)->fee,entry->fee);
    EXPECT_EQ(pool.getMempoolEntry(id)->cached_utxotx_payload,payload.Wire());
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
}
TEST(OrchardPoolProofTransport, WrongRootBodyAndCachedProofRefuseWithoutPublication) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto& pool=f.f.ingress->mempool();
    const auto body=MempoolTransaction::FromOrchard(block->Transactions()[1].Orchard());
    const auto id=body.GetTxid().AsUint256();
    const auto wire=CanonicalPoolUnverifiedProof(f,body);ASSERT_TRUE(wire);
    const auto good=UtreexoTransactionPayload::Decode(*wire,RelayTransactionReadMode::AvailableFamilies);
    auto wrong=good.Wire();wrong.back()^=1;
    const auto bad=UtreexoTransactionPayload::Decode(wrong,RelayTransactionReadMode::AvailableFamilies);
    ASSERT_TRUE(pool.removeTransaction(id));
    EXPECT_FALSE(f.f.ingress->SubmitProofBody(bad,TxOrigin::INTERNAL).accepted());EXPECT_EQ(pool.size(),0u);
    ASSERT_TRUE(f.f.ingress->SubmitProofBody(good,TxOrigin::INTERNAL).accepted());
    const auto before=pool.getMempoolEntry(id);ASSERT_TRUE(before);
    EXPECT_FALSE(f.f.ingress->SubmitProofBody(bad,TxOrigin::INTERNAL).accepted());
    EXPECT_EQ(pool.getMempoolEntry(id)->cached_utxotx_payload,before->cached_utxotx_payload);
    EXPECT_EQ(pool.getMempoolEntry(id)->validated_at_root,before->validated_at_root);
    // One v2 input: the final metadata byte before the root is its coinbase
    // flag. Legacy inclusion alone does not bind that flag. Exact selected
    // metadata must refuse this forged ordinary-coin claim without replacing
    // the previously accepted proof cache.
    ASSERT_EQ(good.Body().Inputs().size(),1u);ASSERT_EQ(good.Wire().front(),2u);
    auto forged=good.Wire();ASSERT_GT(forged.size(),33u);
    ASSERT_EQ(forged[forged.size()-33],1u);forged[forged.size()-33]=0;
    const auto false_metadata=UtreexoTransactionPayload::Decode(forged,RelayTransactionReadMode::AvailableFamilies);
    EXPECT_FALSE(f.f.ingress->SubmitProofBody(false_metadata,TxOrigin::INTERNAL).accepted());
    EXPECT_EQ(pool.getMempoolEntry(id)->cached_utxotx_payload,before->cached_utxotx_payload);
    EXPECT_EQ(pool.getMempoolEntry(id)->validated_at_root,before->validated_at_root);
    const auto other=CanonicalPoolSurvivor(f);
    {
        auto selected=ChainstateService::AcquireMempoolChainstateRead(f.f.service);ASSERT_TRUE(selected);
        const std::vector<MempoolProofView> mismatched{{other,good.Wire()}};
        EXPECT_FALSE(selected->ValidateBlockSelectionWithProofs(mismatched,102).result.accepted());
    }
    ASSERT_TRUE(pool.setCachedUtxoTxPayload(id,wrong));
    const auto selection=pool.CaptureTypedBlockSelection(1000000,4000000,102);
    ASSERT_TRUE(selection.available);EXPECT_TRUE(selection.transactions.empty());
    EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getMempoolEntry(id)->cached_utxotx_payload,wrong);
    ASSERT_TRUE(f.f.ingress->SubmitProofBody(good,TxOrigin::INTERNAL).accepted());
    EXPECT_EQ(pool.getMempoolEntry(id)->cached_utxotx_payload,good.Wire());
    EXPECT_EQ(pool.CaptureTypedBlockSelection(1000000,4000000,102).transactions.size(),1u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
}

#else
TEST(OrchardCanonicalPool, InactiveServiceDoesNotRouteTypedBody) {
    ChainstateService service;EXPECT_FALSE(service.TryAcceptOrchardBlockFromRPC(""));
}
#endif

TEST(OrchardPoolProofTransport, MissingProofValidatorRefusesSuppliedBytes) {
    MempoolChainstateReadGuard guard;const std::vector<uint8_t> bytes{1,2,3};
    const MempoolProofView body{MempoolTransaction{},bytes};
    EXPECT_FALSE(guard.ValidateOrchardWithProofs(body,{}).result.accepted());
    EXPECT_FALSE(guard.ValidateBlockSelectionWithProofs({&body,1},1).result.accepted());
}
