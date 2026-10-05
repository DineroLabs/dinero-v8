#include "consensus/active_chain_ancestry.h"
#pragma once
namespace dinero {
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct TypedForkNotices final:RuntimeBlockNotifications {
    struct Event {uint256 hash;uint32_t height;RuntimeBlockDirection direction;};
    std::array<Event,32> events{};size_t event_count=0;
    std::array<std::shared_ptr<const RuntimeReorgPlan>,8> plans{};
    std::array<RuntimeReorgProgress,8> progress{};size_t plan_count=0;
    struct Block final:PreparedRuntimeBlockNotifications {
        TypedForkNotices& owner;Event event;
        Block(TypedForkNotices& value,Event next):owner(value),event(next){}
        void PublishAfterCommit()noexcept override{owner.events[owner.event_count++]=event;}
    };
    struct Plan final:PreparedRuntimeReorgNotifications {
        TypedForkNotices& owner;size_t index;
        Plan(TypedForkNotices& value,size_t slot):owner(value),index(slot){}
        void Finish(RuntimeReorgProgress value)noexcept override{owner.progress[index]=value;}
    };
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection direction)override {
        OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height>=102 && event_count<events.size());
        return std::make_unique<Block>(*this,Event{body.Orchard().Header().GetHash(),height,direction});
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(std::shared_ptr<const RuntimeReorgPlan> plan)override {
        OrchardAdmissionFixture::Require(bool(plan) && plan_count<plans.size());
        const auto slot=plan_count;auto prepared=std::make_unique<Plan>(*this,slot);
        plans[slot]=std::move(plan);++plan_count;return prepared;
    }
};
}
TEST(OrchardTypedFork, HeavierRetainedBranchUsesActualUndoAndCanonicalReconnection) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);
    // Historical fixture bodies were independently replayed into its coin
    // state, but the runtime archival reader also requires actual flatfiles.
    // Record only real writeBlock positions; preserve all existing flags and
    // never create historical undo that this fixture did not execute.
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(g_block_index_mutex);
        for(uint32_t height=0;height<f.f.blocks.size();++height) {
            const auto& body=f.f.blocks[height];const auto hash=body.GetHash();
            auto* index=dinero::FindBlockIndex(hash);ASSERT_NE(index,nullptr);
            const auto position=f.files->writeBlock(hash,body);ASSERT_TRUE(position.ok());
            CBlockIndex recorded=*index;recorded.file_number=position->file_number;
            recorded.data_pos=position->offset;recorded.data_size=position->size;
            rocksdb::WriteBatch batch;
            ASSERT_EQ(f.f.db.updateBlockIndex(f.f.token,&recorded,&batch),Status::Ok);
            ASSERT_EQ(f.f.db.writeBatch(f.f.token,std::move(batch),true),Status::Ok);
            index->file_number=recorded.file_number;index->data_pos=recorded.data_pos;
            index->data_size=recorded.data_size;
            const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),hash,height);
            ASSERT_TRUE(stored.ok());ASSERT_FALSE(stored->IsOrchardProfile());
            EXPECT_EQ(stored->Historical().Serialize(),body.Serialize());
        }
    }
    // End actual historical flatfile fixture setup.
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* a102=f.f.service->GetActiveTip();ASSERT_EQ(a102->height,102u);
    const auto parent_body=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_body.GetTxid(),0),
                                                  parent_body.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    ASSERT_EQ(second->Height(),103u);ASSERT_EQ(second->Transactions().size(),2u);
    ASSERT_TRUE(f.Submit(second->WireBytes()).connected);auto* a103=f.f.service->GetActiveTip();
    ASSERT_EQ(a103->pprev,a102);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    const auto original=f.f.db.getOrchardState();ASSERT_TRUE(original.ok());
    for(auto* node:{a102,a103}) {
        const auto meta=f.f.db.getHeaderMetadata(node->hash);ASSERT_TRUE(meta.ok());
        EXPECT_NE(meta->status_flags&BLOCK_HAVE_UNDO,0u);EXPECT_GT(meta->undo_size,0u);
    }
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(a102->hash,error))<<error;
    ASSERT_EQ(f.f.service->GetActiveTip(),f.parent);ASSERT_EQ(observer->plan_count,1u);
    EXPECT_TRUE(observer->progress[0].complete);EXPECT_EQ(observer->progress[0].disconnected,2u);
    EXPECT_EQ(observer->progress[0].connected,0u);f.f.CheckUnpublished();
    const auto [bheader,bbytes]=ForkSibling(*first);
    ASSERT_TRUE(f.Submit(bbytes).connected);auto* b102=f.f.service->GetActiveTip();
    ASSERT_EQ(b102->hash,bheader.GetHash());ASSERT_NE(b102,a102);ASSERT_EQ(b102->pprev,f.parent);
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a102->hash,error))<<error;
    // Reconsider runs ABC under its real selected owner; expensive parent replay
    // is deferred until the actual root maintenance call below.
    EXPECT_EQ(f.f.service->GetActiveTip(),b102);
    const auto retained=f.Submit(second->WireBytes());ASSERT_TRUE(retained.retained())<<retained.reason;
    EXPECT_FALSE(retained.connected);EXPECT_EQ(f.f.service->GetActiveTip(),b102);
    const auto before=observer->event_count;
    f.f.service->PumpReplayMetadataRecovery();
    ASSERT_EQ(f.f.service->GetActiveTip(),a103);EXPECT_EQ(observer->event_count,before+3);
    ASSERT_EQ(observer->plan_count,2u);EXPECT_TRUE(observer->progress[1].complete);
    EXPECT_EQ(observer->progress[1].disconnected,1u);EXPECT_EQ(observer->progress[1].connected,2u);
    const auto& plan=*observer->plans[1];ASSERT_EQ(plan.disconnect.size(),1u);ASSERT_EQ(plan.connect.size(),2u);
    EXPECT_EQ(plan.disconnect[0].hash,b102->hash);EXPECT_EQ(plan.connect[0].hash,a102->hash);
    EXPECT_EQ(plan.connect[1].hash,a103->hash);
    EXPECT_EQ(plan.disconnect[0].body.Orchard().WireBytes(),bbytes);
    EXPECT_EQ(plan.connect[0].body.Orchard().WireBytes(),first->WireBytes());
    EXPECT_EQ(plan.connect[1].body.Orchard().WireBytes(),second->WireBytes());
    EXPECT_EQ(observer->events[before].hash,b102->hash);EXPECT_EQ(observer->events[before].direction,RuntimeBlockDirection::Disconnect);
    EXPECT_EQ(observer->events[before+1].hash,a102->hash);EXPECT_EQ(observer->events[before+1].direction,RuntimeBlockDirection::Connect);
    EXPECT_EQ(observer->events[before+2].hash,a103->hash);EXPECT_EQ(observer->events[before+2].direction,RuntimeBlockDirection::Connect);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        const auto first_intent=ReadRuntimeReorgIntentUnderLock(f.f.db);ASSERT_TRUE(first_intent);
        const auto fork_intent=ReadRuntimeReorgIntentUnderLock(f.f.db,first_intent->cursor);ASSERT_TRUE(fork_intent);
        EXPECT_EQ(fork_intent->plan->disconnect.front().hash,b102->hash);
        EXPECT_EQ(fork_intent->plan->connect.back().hash,a103->hash);
        EXPECT_FALSE(ReadRuntimeReorgIntentUnderLock(f.f.db,fork_intent->cursor));
    }
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    std::string alignment;EXPECT_TRUE(f.f.service->IsCanonicalStateAligned(&alignment))<<alignment;
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,original->pool_balance);
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);EXPECT_EQ(f.f.ingress->mempool().getStats().last_connected_height,103u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto retry=f.Submit(second->WireBytes());EXPECT_TRUE(retry.connected);EXPECT_EQ(observer->event_count,before+3);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardTypedFork, InvalidatedParentRefusesUnseenChildBeforeStorage) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);
    // Historical fixture bodies were independently replayed into its coin
    // state, but the runtime archival reader also requires actual flatfiles.
    // Record only real writeBlock positions; preserve all existing flags and
    // never create historical undo that this fixture did not execute.
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(g_block_index_mutex);
        for(uint32_t height=0;height<f.f.blocks.size();++height) {
            const auto& body=f.f.blocks[height];const auto hash=body.GetHash();
            auto* index=dinero::FindBlockIndex(hash);ASSERT_NE(index,nullptr);
            const auto position=f.files->writeBlock(hash,body);ASSERT_TRUE(position.ok());
            CBlockIndex recorded=*index;recorded.file_number=position->file_number;
            recorded.data_pos=position->offset;recorded.data_size=position->size;
            rocksdb::WriteBatch batch;
            ASSERT_EQ(f.f.db.updateBlockIndex(f.f.token,&recorded,&batch),Status::Ok);
            ASSERT_EQ(f.f.db.writeBatch(f.f.token,std::move(batch),true),Status::Ok);
            index->file_number=recorded.file_number;index->data_pos=recorded.data_pos;
            index->data_size=recorded.data_size;
            const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),hash,height);
            ASSERT_TRUE(stored.ok());ASSERT_FALSE(stored->IsOrchardProfile());
            EXPECT_EQ(stored->Historical().Serialize(),body.Serialize());
        }
    }
    // End actual historical flatfile fixture setup.
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* parent=f.f.service->GetActiveTip();ASSERT_EQ(parent->height,102u);
    const auto parent_body=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_body.GetTxid(),0),
                                                  parent_body.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto child=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(child);
    ASSERT_EQ(child->Height(),103u);ASSERT_EQ(child->Transactions().size(),2u);
    const auto hash=child->Header().GetHash();
    ASSERT_EQ(dinero::FindBlockIndex(hash),nullptr);
    ASSERT_EQ(f.f.db.getHeader(hash).status(),Status::NotFound);
    // This child has never entered the graph. The operator invalidates its
    // actual canonical parent using the real disconnect/undo owner.
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(parent->hash,error))<<error;
    ASSERT_EQ(f.f.service->GetActiveTip(),f.parent);
    ASSERT_NE(parent->status&BLOCK_FAILED_VALID,0u);
    const auto before=f.f.db.getHeaderMetadata(parent->hash);ASSERT_TRUE(before.ok());
    ASSERT_NE(before->status_flags&BLOCK_FAILED_VALID,0u);
    const auto snapshot=[&] {
        std::map<std::string,uintmax_t> sizes;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(f.f.path/"flatfiles"))
            if(entry.is_regular_file())sizes.emplace(entry.path().string(),entry.file_size());
        return sizes;
    };
    const auto sizes=snapshot();const auto events=observer->event_count;const auto plans=observer->plan_count;
    for(unsigned attempt=0;attempt<2;++attempt) {
        const auto result=f.Submit(child->WireBytes());EXPECT_TRUE(result.rejected());
        EXPECT_FALSE(result.retained());EXPECT_FALSE(result.connected);EXPECT_FALSE(result.relayed);
        EXPECT_EQ(result.reason,"Orchard parent header owner unavailable");
        EXPECT_EQ(dinero::FindBlockIndex(hash),nullptr);
        EXPECT_EQ(f.f.db.getHeader(hash).status(),Status::NotFound);
        EXPECT_EQ(f.f.db.getHeaderMetadata(hash).status(),Status::NotFound);
        EXPECT_EQ(f.f.db.getBlockHeight(hash).status(),Status::NotFound);
        EXPECT_EQ(f.f.db.getBlockWork(hash).status(),Status::NotFound);
        EXPECT_EQ(snapshot(),sizes);
        EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
        EXPECT_EQ(observer->event_count,events);EXPECT_EQ(observer->plan_count,plans);
        const auto after=f.f.db.getHeaderMetadata(parent->hash);ASSERT_TRUE(after.ok());
        EXPECT_EQ(after->status_flags,before->status_flags);EXPECT_EQ(after->data_pos,before->data_pos);
        EXPECT_EQ(after->undo_pos,before->undo_pos);
    }
}
TEST(OrchardTypedFork, HeaderSelectedSchedulerRetainsThenAcknowledgesActualFork) {
    NetworkRoutingFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);
    // Historical fixture bodies were independently replayed into its coin
    // state, but the runtime archival reader also requires actual flatfiles.
    // Record only real writeBlock positions; preserve all existing flags and
    // never create historical undo that this fixture did not execute.
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(g_block_index_mutex);
        for(uint32_t height=0;height<f.f.blocks.size();++height) {
            const auto& body=f.f.blocks[height];const auto hash=body.GetHash();
            auto* index=dinero::FindBlockIndex(hash);ASSERT_NE(index,nullptr);
            const auto position=f.files->writeBlock(hash,body);ASSERT_TRUE(position.ok());
            CBlockIndex recorded=*index;recorded.file_number=position->file_number;
            recorded.data_pos=position->offset;recorded.data_size=position->size;
            rocksdb::WriteBatch batch;
            ASSERT_EQ(f.f.db.updateBlockIndex(f.f.token,&recorded,&batch),Status::Ok);
            ASSERT_EQ(f.f.db.writeBatch(f.f.token,std::move(batch),true),Status::Ok);
            index->file_number=recorded.file_number;index->data_pos=recorded.data_pos;
            index->data_size=recorded.data_size;
            const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),hash,height);
            ASSERT_TRUE(stored.ok());ASSERT_FALSE(stored->IsOrchardProfile());
            EXPECT_EQ(stored->Historical().Serialize(),body.Serialize());
        }
    }
    // End actual historical flatfile fixture setup.
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* a102=f.f.service->GetActiveTip();ASSERT_EQ(a102->height,102u);
    const auto parent_body=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_body.GetTxid(),0),
                                                  parent_body.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    ASSERT_EQ(second->Height(),103u);ASSERT_EQ(second->Transactions().size(),2u);
    ASSERT_TRUE(f.Submit(second->WireBytes()).connected);auto* a103=f.f.service->GetActiveTip();
    ASSERT_EQ(a103->pprev,a102);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    const auto original=f.f.db.getOrchardState();ASSERT_TRUE(original.ok());
    for(auto* node:{a102,a103}) {
        const auto meta=f.f.db.getHeaderMetadata(node->hash);ASSERT_TRUE(meta.ok());
        EXPECT_NE(meta->status_flags&BLOCK_HAVE_UNDO,0u);EXPECT_GT(meta->undo_size,0u);
    }
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(a102->hash,error))<<error;
    ASSERT_EQ(f.f.service->GetActiveTip(),f.parent);ASSERT_EQ(observer->plan_count,1u);
    EXPECT_TRUE(observer->progress[0].complete);EXPECT_EQ(observer->progress[0].disconnected,2u);
    EXPECT_EQ(observer->progress[0].connected,0u);f.f.CheckUnpublished();
    const auto [bheader,bbytes]=ForkSibling(*first);
    ASSERT_TRUE(f.Submit(bbytes).connected);auto* b102=f.f.service->GetActiveTip();
    ASSERT_EQ(b102->hash,bheader.GetHash());ASSERT_NE(b102,a102);ASSERT_EQ(b102->pprev,f.parent);
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a102->hash,error))<<error;
    // Reconsider runs ABC under its real selected owner; expensive parent replay
    // is deferred until the actual root maintenance call below.
    EXPECT_EQ(f.f.service->GetActiveTip(),b102);
    for(const auto& block:f.f.blocks)ASSERT_TRUE(f.headers->AddHeader(block.header));
    ASSERT_TRUE(f.headers->AddHeader(first->Header()));ASSERT_TRUE(f.headers->AddHeader(second->Header()));
    consensus::BlockDownloadScheduler scheduler(f.headers.get(),f.files.get());
    scheduler.SetLocalTipHeight(102);
    scheduler.SetGetTipHeightCallback([&] {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        return f.f.service->GetActiveTip()->height;
    });
    scheduler.SetGetBlockHashAtHeightCallback([&](uint32_t height,uint256& hash) {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        return consensus::GetActiveChainHashAtHeight(f.f.service->GetActiveTip(),height,hash);
    });
    scheduler.SetSendGetDataCallback([](const uint256&,uint32_t){});
    std::map<uint256,unsigned> offers;
    scheduler.SetConnectBlockBytesCallback([&](const auto& bytes,const auto& hash,uint32_t height,const auto&) {
        ++offers[hash];
        const auto result=SubmitDownloadedOrchardBlock(f.f.service,f.owner,f.parallel,bytes,hash,height);
        if(result==OrchardNetworkDisposition::Connected)return consensus::ConnectBlockResult::CONNECTED;
        if(result==OrchardNetworkDisposition::Stored)return consensus::ConnectBlockResult::ACCEPTED_NOT_ACTIVE;
        return consensus::ConnectBlockResult::TEMPORARY_FAIL;
    });
    scheduler.OnHeadersProcessed();scheduler.Tick();
    ASSERT_TRUE(scheduler.OnOrchardBlockReceived(first->WireBytes()));
    ASSERT_TRUE(scheduler.OnOrchardBlockReceived(second->WireBytes()));
    const auto connected=f.queue->getMetrics().blocks_connected.load();
    const auto retained=f.queue->getMetrics().blocks_retained.load();
    const auto published=observer->event_count;
    scheduler.Tick();scheduler.Tick();
    ASSERT_EQ(f.f.service->GetActiveTip(),b102);
    EXPECT_EQ(scheduler.GetLocalTipHeight(),102u);
    EXPECT_FALSE(scheduler.IsBlockConnected(a102->hash));EXPECT_FALSE(scheduler.IsBlockConnected(a103->hash));
    EXPECT_TRUE(scheduler.HasReceivedBlock(a102->hash));EXPECT_TRUE(scheduler.HasReceivedBlock(a103->hash));
    EXPECT_EQ(offers[a102->hash],1u);EXPECT_EQ(offers[a103->hash],1u);
    EXPECT_EQ(f.queue->getMetrics().blocks_connected.load(),connected);
    EXPECT_EQ(f.queue->getMetrics().blocks_retained.load(),retained+2u);
    EXPECT_EQ(observer->event_count,published);
    // Further normal scheduler work must not re-offer bodies already retained.
    scheduler.Tick();EXPECT_EQ(offers[a102->hash],1u);EXPECT_EQ(offers[a103->hash],1u);
    // Only the real root maintenance transition supplies canonical completion.
    f.f.service->PumpReplayMetadataRecovery();ASSERT_EQ(f.f.service->GetActiveTip(),a103);
    ASSERT_EQ(observer->plan_count,2u);EXPECT_TRUE(observer->progress[1].complete);
    EXPECT_EQ(observer->progress[1].disconnected,1u);EXPECT_EQ(observer->progress[1].connected,2u);
    EXPECT_EQ(observer->event_count,published+3u);
    scheduler.Tick();
    EXPECT_TRUE(scheduler.IsBlockConnected(a102->hash));EXPECT_TRUE(scheduler.IsBlockConnected(a103->hash));
    EXPECT_EQ(offers[a102->hash],1u);EXPECT_EQ(offers[a103->hash],1u);
    EXPECT_TRUE(scheduler.IsFullySynchronized());EXPECT_EQ(scheduler.GetLocalTipHeight(),103u);
    EXPECT_EQ(f.queue->getMetrics().blocks_connected.load(),connected);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    const auto tip=f.f.db.getTip();ASSERT_TRUE(tip.ok());EXPECT_EQ(tip->hash,a103->hash);
}
namespace {
void BranchReplayHistory(CanonicalPoolFixture& f) {
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(g_block_index_mutex);
        for(uint32_t height=0;height<f.f.blocks.size();++height) {
            const auto& body=f.f.blocks[height];const auto hash=body.GetHash();
            auto* index=dinero::FindBlockIndex(hash);ASSERT_NE(index,nullptr);
            const auto position=f.files->writeBlock(hash,body);ASSERT_TRUE(position.ok());
            CBlockIndex recorded=*index;recorded.file_number=position->file_number;
            recorded.data_pos=position->offset;recorded.data_size=position->size;
            rocksdb::WriteBatch batch;
            ASSERT_EQ(f.f.db.updateBlockIndex(f.f.token,&recorded,&batch),Status::Ok);
            ASSERT_EQ(f.f.db.writeBatch(f.f.token,std::move(batch),true),Status::Ok);
            index->file_number=recorded.file_number;index->data_pos=recorded.data_pos;
            index->data_size=recorded.data_size;
            const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),hash,height);
            ASSERT_TRUE(stored.ok());ASSERT_FALSE(stored->IsOrchardProfile());
            EXPECT_EQ(stored->Historical().Serialize(),body.Serialize());
        }
    }

}
auto BranchReplayWrongProof(const OrchardMiningTemplate& built) {
    const auto block=OrchardBlockCandidate::DecodeExact(built.WireBytes());
    OrchardAdmissionFixture::Require(bool(block.Utreexo()) && !block.Utreexo()->spent_outputs.empty());
    auto proof=*block.Utreexo();++proof.spent_outputs.front().value;
    auto wire=block.WireBytes();wire.resize(wire.size()-block.Utreexo()->serialize().size());
    const auto suffix=proof.serialize();wire.insert(wire.end(),suffix.begin(),suffix.end());
    const auto decoded=OrchardBlockCandidate::DecodeExact(wire);
    OrchardAdmissionFixture::Require(decoded.Header().GetHash()==block.Header().GetHash() &&
        decoded.MatchesTransactionRoot() && decoded.WireBytes()!=block.WireBytes());
    return wire;
}
}
TEST(OrchardBranchReplay, InvalidLateProofRefusesBeforeAnyReorgEffect) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);ASSERT_NO_FATAL_FAILURE(BranchReplayHistory(f));
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* a102=f.f.service->GetActiveTip();const auto parent_tx=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_tx.GetTxid(),0),parent_tx.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    const auto bad=BranchReplayWrongProof(*second);
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(a102->hash,error))<<error;
    const auto [header,bytes]=ForkSibling(*first);ASSERT_TRUE(f.Submit(bytes).connected);
    auto* selected=f.f.service->GetActiveTip();ASSERT_EQ(selected->hash,header.GetHash());
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a102->hash,error))<<error;
    ASSERT_TRUE(f.Submit(bad).retained());ASSERT_EQ(f.f.service->GetActiveTip(),selected);
    const auto events=observer->event_count,plans=observer->plan_count;
    const auto state=f.f.db.getOrchardState();ASSERT_TRUE(state.ok());
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),selected);EXPECT_EQ(observer->event_count,events);
    EXPECT_EQ(observer->plan_count,plans);EXPECT_EQ(*f.f.db.getOrchardState(),*state);
    EXPECT_EQ(f.f.db.getTip()->hash,selected->hash);EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    const auto meta=f.f.db.getHeaderMetadata(second->Header().GetHash());ASSERT_TRUE(meta.ok());
    EXPECT_EQ(meta->status_flags&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD|BLOCK_HAVE_UNDO),0u);
}
TEST(OrchardBranchReplay, PostActivationForkReplaysTheCompletePrefix) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);ASSERT_NO_FATAL_FAILURE(BranchReplayHistory(f));
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* common=f.f.service->GetActiveTip();const auto parent_tx=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_tx.GetTxid(),0),parent_tx.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    ASSERT_TRUE(f.Submit(second->WireBytes()).connected);auto* a103=f.f.service->GetActiveTip();
    const auto third=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(third);
    ASSERT_TRUE(f.Submit(third->WireBytes()).connected);auto* a104=f.f.service->GetActiveTip();
    ASSERT_EQ(a104->height,104u);const auto state=f.f.db.getOrchardState();ASSERT_TRUE(state.ok());
    const auto sets=f.f.db.getOrchardCommitmentSets(*state);ASSERT_TRUE(sets.ok());
    EXPECT_EQ(sets->anchor_references,3u);
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(a103->hash,error))<<error;
    ASSERT_EQ(f.f.service->GetActiveTip(),common);
    const auto [header,bytes]=ForkSibling(*second);ASSERT_TRUE(f.Submit(bytes).connected);
    auto* b103=f.f.service->GetActiveTip();ASSERT_EQ(b103->hash,header.GetHash());
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a103->hash,error))<<error;
    ASSERT_TRUE(f.Submit(third->WireBytes()).retained());ASSERT_EQ(f.f.service->GetActiveTip(),b103);
    const auto events=observer->event_count,plans=observer->plan_count;
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    EXPECT_EQ(f.f.service->GetActiveTip(),b103);EXPECT_EQ(observer->plan_count,plans);
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    ASSERT_EQ(f.f.service->GetActiveTip(),a104);EXPECT_EQ(observer->event_count,events+3);
    ASSERT_EQ(observer->plan_count,plans+1);EXPECT_TRUE(observer->progress[plans].complete);
    EXPECT_EQ(observer->progress[plans].disconnected,1u);EXPECT_EQ(observer->progress[plans].connected,2u);
    EXPECT_EQ(*f.f.db.getOrchardState(),*state);
    EXPECT_EQ(*f.f.db.getOrchardCommitmentSets(*state),*sets);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardBranchReplay, ChangedProofWireAfterPreparationRefusesBeforeRollback) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);ASSERT_NO_FATAL_FAILURE(BranchReplayHistory(f));
    const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    auto* a102=f.f.service->GetActiveTip();const auto parent_tx=first->Transactions()[1];
    const MempoolTransaction spend(SelectionSpend(f.f,OutPoint(parent_tx.GetTxid(),0),parent_tx.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);
    const auto bad=BranchReplayWrongProof(*second);std::string error;
    ASSERT_TRUE(f.f.service->InvalidateBlock(a102->hash,error))<<error;
    const auto [header,bytes]=ForkSibling(*first);ASSERT_TRUE(f.Submit(bytes).connected);
    auto* selected=f.f.service->GetActiveTip();ASSERT_EQ(selected->hash,header.GetHash());
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a102->hash,error))<<error;
    ASSERT_TRUE(f.Submit(second->WireBytes()).retained());
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    const auto events=observer->event_count,plans=observer->plan_count;
    const auto hash=second->Header().GetHash();
    {
        auto selected_owner=f.f.service->AcquireBlockIngressActivationLock();
        // A serialized local archival replacement through actual append and
        // metadata APIs. Header/hash/work and the captured graph are unchanged.
        // This is not a race or a synchronization-removal control.
        const auto position=f.files->writeBlockBytes(hash,{bad.begin(),bad.end()});ASSERT_TRUE(position.ok());
        auto meta=f.f.db.getHeaderMetadata(hash);ASSERT_TRUE(meta.ok());
        meta->file_number=position->file_number;meta->data_pos=position->offset;meta->data_size=position->size;
        ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,*meta),Status::Ok);
        const auto body=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),hash,103);
        ASSERT_TRUE(body.ok());EXPECT_EQ(body->Orchard().WireBytes(),bad);
    }
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),selected);EXPECT_EQ(observer->event_count,events);
    EXPECT_EQ(observer->plan_count,plans);EXPECT_EQ(f.f.db.getTip()->hash,selected->hash);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}

#endif
} // namespace dinero
