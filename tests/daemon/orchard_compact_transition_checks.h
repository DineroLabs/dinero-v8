#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/services/historical_compact_replay.h"
#include "daemon/services/historical_catalog.h"
#include "daemon/services/historical_catalog_range.h"
namespace dinero {
struct OrchardCompactTransitionTestAccess {
    static void SelectAuditedComponent(ChainstateService& service) {
        // Component setup, not Start qualification: real reconstruction, owned
        // DB and completed detached startup audit precede selected publication.
        // Preserve the real selected lifetime counters and both mutexes.
        auto audit=service.PrepareCompactStartupAudit();
        if(!audit)throw std::runtime_error("compact transition fixture startup proof");
        {
            std::lock_guard<AnnotatedRecursiveMutex> selected(service.activation_mutex_);
            if(!service.BindCompactStartupAuditUnderLock(*audit))
                throw std::runtime_error("compact transition fixture startup binding");
            service.PublishActiveTipLocked(audit->tip,ChainstateService::TipPublishReason::kStartupLoad);
            std::lock_guard<std::mutex> lifetime(service.wallet_index_use_mutex_);
            if(service.selected_read_accepting_ || service.wallet_index_accepting_ ||
                service.wallet_index_stopping_ || service.selected_read_uses_ || service.wallet_index_uses_ ||
                service.compact_startup_uses_!=1)
                throw std::runtime_error("compact transition fixture admission before-image");
            service.selected_read_accepting_=true;
        }
        // No started flag, wallet-index admission, networking, workers or IPC.
    }
};
namespace {
// These component fixtures cross the historical/Orchard boundary. Keep the
// original Orchard-only observer unchanged for its separate typed-fork tests.
std::unique_ptr<PreparedRuntimeBlockNotifications> PrepareCompactFixtureNotice(
    TypedForkNotices& recorded,const RuntimeBlockBody& body,uint32_t height,
    RuntimeBlockDirection direction) {
    OrchardAdmissionFixture::Require(
        body.IsOrchardProfile()==(height>=Params().orchard_activation_height));
    if(body.IsOrchardProfile())return recorded.Prepare(body,height,direction);
    OrchardAdmissionFixture::Require(height>0 && recorded.event_count<recorded.events.size());
    return std::make_unique<TypedForkNotices::Block>(recorded,
        TypedForkNotices::Event{body.Historical().GetHash(),height,direction});
}
struct CompactTransitionNotices final:RuntimeBlockNotifications {
    TypedForkNotices recorded;
    std::optional<uint256> refuse_disconnect;
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection direction)override {
        if(direction==RuntimeBlockDirection::Disconnect && refuse_disconnect &&
            body.IsOrchardProfile() && body.Orchard().Header().GetHash()==*refuse_disconnect)return {};
        return PrepareCompactFixtureNotice(recorded,body,height,direction);
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(std::shared_ptr<const RuntimeReorgPlan> plan)override {
        return recorded.PrepareReorg(std::move(plan));
    }
};
struct CompactTransitionFixture {
    std::shared_ptr<CompactStartupFixture> storage;
    DaemonContext* previous=DaemonContext::instance();
    DaemonContext context;
    std::shared_ptr<ChainstateService> service;
    std::shared_ptr<CompactTransitionNotices> notices=std::make_shared<CompactTransitionNotices>();
    std::vector<BlockHeader> extra_headers;
    explicit CompactTransitionFixture(
        const std::function<void(CompactStartupFixture&)>& capture={},
        const std::vector<BlockHeader>& retained={},
        OrchardAdmissionFixture::HistoricalSpend spend=OrchardAdmissionFixture::HistoricalSpend::None)
        :storage(std::make_shared<CompactStartupFixture>(capture,spend)),extra_headers(retained){Open();}
    void Open() {
        auto& f=*storage;GetConfig().utreexo_stateless=true;
        ConfigureCompactServiceContext(context,f);
        service=std::make_shared<ChainstateService>();context.chainstate=service;
        DaemonContext::setInstance(&context);
        service->setOwnedChainDB(std::shared_ptr<ChainDB>(storage,&f.reopened));
        OrchardAdmissionFixture::Require(service->Init(context));
        auto headers=CompactBindingSelector(f);
        for(const auto& header:extra_headers)OrchardAdmissionFixture::Require(headers->AddHeader(header));
        service->setHeaderChainSelector(std::move(headers));
        service->setRuntimeBlockNotifications(notices);
        OrchardCompactTransitionTestAccess::SelectAuditedComponent(*service);
        OrchardAdmissionFixture::Require(!service->IsStarted() && !service->GetConsensusUTXOSet() && !service->utxoIndex());
    }
    void Close() {
        if(service)service->Stop();
        context.chainstate.reset();service.reset();
    }
    void Reopen() {
        Close();storage->reopened.close();
        OrchardAdmissionFixture::Require(storage->reopened.init(storage->f.path)==Status::Ok);Open();
    }
    ~CompactTransitionFixture(){Close();DaemonContext::setInstance(previous);}
    auto Head() {
        auto selected=service->AcquireBlockIngressActivationLock();
        const auto profile=consensus::SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);
        OrchardAdmissionFixture::Require(bool(profile));
        return ReadRuntimeOutboxUnderLock(storage->reopened,*profile,{},1).head;
    }
    void CheckSelected(const uint256& hash,uint32_t height) {
        ASSERT_NE(service->GetActiveTip(),nullptr);EXPECT_EQ(service->GetActiveTip()->hash,hash);
        EXPECT_EQ(service->GetActiveTip()->height,height);
        const auto tip=storage->reopened.getTip(),validated=storage->reopened.getValidatedTip();
        ASSERT_TRUE(tip.ok());ASSERT_TRUE(validated.ok());
        EXPECT_EQ(tip->hash,hash);EXPECT_EQ(tip->height,int32_t(height));
        EXPECT_EQ(validated->hash,hash);EXPECT_EQ(validated->height,int32_t(height));
        EXPECT_EQ(tip->work,ChainworkFromHex(service->GetActiveTip()->chainwork));
        const auto catalog=storage->reopened.getOrchardCatalogState(hash);ASSERT_TRUE(catalog.ok());
        EXPECT_EQ(*catalog,storage->expected_catalog.at(hash));
        size_t full_coins=0;
        ASSERT_EQ(storage->reopened.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++full_coins;return true;}),Status::Ok);
        EXPECT_EQ(full_coins,0u);EXPECT_EQ(service->GetConsensusUTXOSet(),nullptr);
        EXPECT_FALSE(service->IsStarted());
    }
};
}
TEST(OrchardCompactTransition, TwoBlockInvalidationAndRootReconnection) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto& observed=f.notices->recorded;
    const auto first=disk.first->Header().GetHash(),second=disk.second->Header().GetHash();
    const auto original=disk.reopened.getOrchardState();ASSERT_TRUE(original.ok());
    const auto head=f.Head();ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));
    std::string error;ASSERT_TRUE(f.service->InvalidateBlock(first,error))<<error;
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.parent->hash,101));
    ASSERT_EQ(observed.plan_count,1u);EXPECT_TRUE(observed.progress[0].complete);
    EXPECT_EQ(observed.progress[0].disconnected,2u);EXPECT_EQ(observed.progress[0].connected,0u);
    ASSERT_EQ(observed.event_count,2u);EXPECT_EQ(observed.events[0].hash,second);EXPECT_EQ(observed.events[1].hash,first);
    EXPECT_EQ(f.Head().sequence,head.sequence+2);
    ASSERT_TRUE(f.service->ReconsiderBlock(first,error))<<error;
    f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));EXPECT_EQ(*disk.reopened.getOrchardState(),*original);
    ASSERT_EQ(observed.event_count,4u);EXPECT_EQ(observed.events[2].hash,first);EXPECT_EQ(observed.events[3].hash,second);
    EXPECT_EQ(observed.events[2].direction,RuntimeBlockDirection::Connect);EXPECT_EQ(f.Head().sequence,head.sequence+4);
    f.service->ActivateBestChain();EXPECT_EQ(observed.event_count,4u);EXPECT_EQ(f.Head().sequence,head.sequence+4);
}
TEST(OrchardCompactTransition, PartialRollbackReopenAndRetry) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto& observed=f.notices->recorded;
    const auto first=disk.first->Header().GetHash(),second=disk.second->Header().GetHash();
    const auto head=f.Head();f.notices->refuse_disconnect=first;
    std::string error;EXPECT_FALSE(f.service->InvalidateBlock(first,error));EXPECT_FALSE(error.empty());
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(first,102));
    ASSERT_EQ(observed.plan_count,1u);EXPECT_FALSE(observed.progress[0].complete);
    EXPECT_EQ(observed.progress[0].disconnected,1u);EXPECT_EQ(observed.progress[0].connected,0u);
    ASSERT_EQ(observed.event_count,1u);EXPECT_EQ(observed.events[0].hash,second);
    EXPECT_EQ(FindBlockIndex(first)->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(f.Head().sequence,head.sequence+1);
    std::shared_ptr<const RuntimeReorgPlan> retained;
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        auto intent=ReadRuntimeReorgIntentUnderLock(disk.reopened);ASSERT_TRUE(intent);
        retained=intent->plan;ASSERT_EQ(retained->disconnect.size(),2u);
        EXPECT_EQ(retained->disconnect[0].hash,second);EXPECT_EQ(retained->disconnect[1].hash,first);
    }
    f.notices->refuse_disconnect.reset();ASSERT_NO_THROW(f.Reopen());
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(first,102));
    ASSERT_TRUE(f.service->InvalidateBlock(first,error))<<error;
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.parent->hash,101));
    ASSERT_EQ(observed.plan_count,2u);EXPECT_TRUE(observed.progress[1].complete);
    EXPECT_EQ(observed.progress[1].disconnected,1u);ASSERT_EQ(observed.event_count,2u);
    EXPECT_EQ(observed.events[1].hash,first);EXPECT_EQ(f.Head().sequence,head.sequence+2);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        auto intent=ReadRuntimeReorgIntentUnderLock(disk.reopened);ASSERT_TRUE(intent);
        EXPECT_EQ(intent->plan->disconnect[0].hash,retained->disconnect[0].hash);
        EXPECT_EQ(intent->plan->disconnect.size(),retained->disconnect.size());
    }
    ASSERT_TRUE(f.service->ReconsiderBlock(first,error))<<error;f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));EXPECT_EQ(observed.event_count,4u);
    EXPECT_EQ(f.Head().sequence,head.sequence+4);
}
TEST(OrchardCompactTransition, NestedUnpreparedInvalidationPreservesState) {
    CompactTransitionFixture f;auto& disk=*f.storage;const auto rows=disk.Rows();const auto archives=disk.ArchiveBytes();
    const auto head=f.Head();std::string error;
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(f.service->InvalidateBlock(disk.first->Header().GetHash(),error));
    }
    EXPECT_FALSE(error.empty());EXPECT_EQ(disk.Rows(),rows);EXPECT_EQ(disk.ArchiveBytes(),archives);
    EXPECT_EQ(f.Head(),head);EXPECT_EQ(f.notices->recorded.event_count,0u);EXPECT_EQ(f.notices->recorded.plan_count,0u);
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.second->Header().GetHash(),103));
}

namespace {
auto CompactSolvedSibling(const CatalogSolvedTemplate& source) {
    auto header=source.Header();const auto nonce=header.nonce;const auto original=header.GetHash();
    // SolveHeader restarts at nonce zero and would rediscover the same block.
    // Search OTHER nonces, preserving timestamp, target and all commitments.
    for(uint32_t step=1;step<=4'000'000;++step) {
        header.nonce=nonce+step;
        if(!consensus::CheckProofOfWork(header,false) || header.GetHash()==original)continue;
        auto bytes=source.WireBytes();const auto prefix=header.SerializeForHash();
        OrchardAdmissionFixture::Require(bytes.size()>=prefix.size());
        std::copy(prefix.begin(),prefix.end(),bytes.begin());
        OrchardAdmissionFixture::Require(OrchardBlockCandidate::DecodeExact(bytes).Header().GetHash()==header.GetHash());
        return std::pair{header,bytes};
    }
    throw std::runtime_error("compact sibling nonce search exhausted");
}
void CheckCompactReplacementPlan(const TypedForkNotices& observed,size_t event_before,
    const uint256& outgoing,const uint256& first,const uint256& second,
    const std::vector<uint8_t>& outgoing_wire,const std::vector<uint8_t>& first_wire,
    const std::vector<uint8_t>& second_wire) {
    ASSERT_EQ(observed.plan_count,2u);
    const auto& plan=*observed.plans[1];const auto& progress=observed.progress[1];
    EXPECT_TRUE(progress.complete);EXPECT_EQ(progress.disconnected,1u);EXPECT_EQ(progress.connected,2u);
    ASSERT_EQ(plan.disconnect.size(),1u);ASSERT_EQ(plan.connect.size(),2u);
    EXPECT_EQ(plan.disconnect[0].hash,outgoing);EXPECT_EQ(plan.connect[0].hash,first);EXPECT_EQ(plan.connect[1].hash,second);
    EXPECT_EQ(plan.disconnect[0].body.Orchard().WireBytes(),outgoing_wire);
    EXPECT_EQ(plan.connect[0].body.Orchard().WireBytes(),first_wire);
    EXPECT_EQ(plan.connect[1].body.Orchard().WireBytes(),second_wire);
    ASSERT_EQ(observed.event_count,event_before+3);
    EXPECT_EQ(observed.events[event_before].hash,outgoing);
    EXPECT_EQ(observed.events[event_before].direction,RuntimeBlockDirection::Disconnect);
    EXPECT_EQ(observed.events[event_before+1].hash,first);
    EXPECT_EQ(observed.events[event_before+1].direction,RuntimeBlockDirection::Connect);
    EXPECT_EQ(observed.events[event_before+2].hash,second);
    EXPECT_EQ(observed.events[event_before+2].direction,RuntimeBlockDirection::Connect);
}
}
TEST(OrchardCompactTransition, DistinctBoundaryBranchReturnsToHeavierOriginal) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto& observed=f.notices->recorded;
    const auto original=disk.reopened.getOrchardState();ASSERT_TRUE(original.ok());
    const auto first=disk.first->Header().GetHash(),second=disk.second->Header().GetHash();
    const auto head=f.Head();std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(first,error))<<error;
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.parent->hash,101));
    const auto [sibling,wire]=CompactSolvedSibling(*disk.first);ASSERT_NE(sibling.GetHash(),first);
    const auto accepted=BlockAcceptor::AcceptBlockFromRPC(util::hex(wire),"compact-boundary-fork-fixture");
    ASSERT_TRUE(accepted.connected)<<accepted.reason;
    ASSERT_EQ(f.service->GetActiveTip()->hash,sibling.GetHash());
    const auto selected=disk.reopened.getTip();ASSERT_TRUE(selected.ok());EXPECT_EQ(selected->hash,sibling.GetHash());
    const auto before=observed.event_count;ASSERT_EQ(before,3u);
    ASSERT_TRUE(f.service->ReconsiderBlock(first,error))<<error;f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));
    const auto restored=disk.reopened.getOrchardState();ASSERT_TRUE(restored.ok());EXPECT_EQ(*restored,*original);
    ASSERT_NO_FATAL_FAILURE(CheckCompactReplacementPlan(observed,before,sibling.GetHash(),first,second,
        wire,disk.first->WireBytes(),disk.second->WireBytes()));
    EXPECT_EQ(f.Head().sequence,head.sequence+6);
    const auto final_head=f.Head();const auto events=observed.event_count;
    f.service->ActivateBestChain();EXPECT_EQ(f.Head(),final_head);EXPECT_EQ(observed.event_count,events);
    ASSERT_NO_THROW(f.Reopen());ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));
    EXPECT_EQ(f.Head(),final_head);f.service->ActivateBestChain();EXPECT_EQ(observed.event_count,events);
}
TEST(OrchardCompactTransition, PostActivationForkMatchesFullNodeReferenceAfterReopen) {
    std::shared_ptr<const CatalogSolvedTemplate> third;
    std::optional<storage::OrchardStoredState> expected_state;
    std::vector<BlockHeader> retained;
    CompactTransitionFixture f([&](CompactStartupFixture& disk) {
        // Execute the reference on the actual full owner BEFORE compact conversion.
        // Then undo through the public service so every base setup invariant is restored.
        BlockAssembler assembler(&disk.f.db);WireOrchardAssembler(assembler,disk.f);
        third=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));
        OrchardAdmissionFixture::Require(third && third->Height()==104);
        OrchardAdmissionFixture::Require(disk.Submit(third->WireBytes()).connected);
        const auto catalog=disk.f.db.getOrchardCatalogState(third->Header().GetHash());
        const auto state=disk.f.db.getOrchardState();
        OrchardAdmissionFixture::Require(catalog.ok() && state.ok());
        disk.expected_catalog.emplace(third->Header().GetHash(),*catalog);expected_state=*state;
        retained.push_back(third->Header());std::string error;
        OrchardAdmissionFixture::Require(disk.f.service->InvalidateBlock(third->Header().GetHash(),error));
        OrchardAdmissionFixture::Require(disk.f.service->GetActiveTip()->hash==disk.second->Header().GetHash());
        const auto tip=disk.f.db.getTip(),validated=disk.f.db.getValidatedTip();
        OrchardAdmissionFixture::Require(tip.ok() && validated.ok() && tip->hash==disk.second->Header().GetHash() && validated->hash==tip->hash);
    },retained);
    auto& disk=*f.storage;auto& observed=f.notices->recorded;
    ASSERT_TRUE(third);ASSERT_TRUE(expected_state);ASSERT_EQ(f.extra_headers.size(),1u);
    const auto second=disk.second->Header().GetHash(),last=third->Header().GetHash();
    const auto head=f.Head();std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(second,error))<<error;
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.first->Header().GetHash(),102));
    const auto [sibling,wire]=CompactSolvedSibling(*disk.second);ASSERT_NE(sibling.GetHash(),second);
    const auto accepted=BlockAcceptor::AcceptBlockFromRPC(util::hex(wire),"compact-postactivation-fork-fixture");
    ASSERT_TRUE(accepted.connected)<<accepted.reason;ASSERT_EQ(f.service->GetActiveTip()->hash,sibling.GetHash());
    const auto before=observed.event_count;ASSERT_EQ(before,2u);
    // Reconsider clears the actual failed subtree; root activation completes detached proofs.
    ASSERT_TRUE(f.service->ReconsiderBlock(second,error))<<error;f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(last,104));
    const auto state=disk.reopened.getOrchardState();ASSERT_TRUE(state.ok());EXPECT_EQ(*state,*expected_state);
    ASSERT_NO_FATAL_FAILURE(CheckCompactReplacementPlan(observed,before,sibling.GetHash(),second,last,
        wire,disk.second->WireBytes(),third->WireBytes()));
    EXPECT_EQ(observed.plans[1]->disconnect.back().body.Orchard().Header().prev_block_hash,disk.first->Header().GetHash());
    EXPECT_EQ(f.Head().sequence,head.sequence+5);
    const auto final_head=f.Head();const auto events=observed.event_count;
    ASSERT_NO_THROW(f.Reopen());ASSERT_NO_FATAL_FAILURE(f.CheckSelected(last,104));
    const auto reopened=disk.reopened.getOrchardState();ASSERT_TRUE(reopened.ok());EXPECT_EQ(*reopened,*expected_state);
    EXPECT_EQ(f.Head(),final_head);f.service->ActivateBestChain();EXPECT_EQ(observed.event_count,events);EXPECT_EQ(f.Head(),final_head);
}

TEST(OrchardCompactTransition, InvalidSelectedTipRejectsSideBodyBeforeStorage) {
    CompactTransitionFixture f;auto& disk=*f.storage;
    const auto [sibling,wire]=CompactSolvedSibling(*disk.second);const auto hash=sibling.GetHash();
    ASSERT_NE(hash,disk.second->Header().GetHash());
    ASSERT_EQ(disk.reopened.getHeader(hash).status(),Status::NotFound);ASSERT_EQ(FindBlockIndex(hash),nullptr);
    const auto validated=disk.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,disk.first->Header().GetHash(),102),Status::Ok);
    }
    const auto rows=disk.Rows();const auto archives=disk.ArchiveBytes();
    const auto refused=BlockAcceptor::AcceptBlockFromRPC(util::hex(wire),"compact-invalid-selected-fixture");
    EXPECT_FALSE(refused.connected);EXPECT_FALSE(refused.retained());
    EXPECT_EQ(refused.code,BlockRejectCode::CONNECT_FAILED);
    EXPECT_EQ(disk.Rows(),rows);EXPECT_EQ(disk.ArchiveBytes(),archives);
    EXPECT_EQ(disk.reopened.getHeader(hash).status(),Status::NotFound);EXPECT_EQ(FindBlockIndex(hash),nullptr);
    EXPECT_EQ(f.notices->recorded.event_count,0u);EXPECT_EQ(f.notices->recorded.plan_count,0u);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,validated->hash,validated->height),Status::Ok);
    }
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(disk.second->Header().GetHash(),103));
    const auto retry=BlockAcceptor::AcceptBlockFromRPC(util::hex(wire),"compact-restored-selected-fixture");
    EXPECT_TRUE(retry.retained())<<retry.reason;
    const auto stored=disk.reopened.getHeader(hash);ASSERT_TRUE(stored.ok());EXPECT_EQ(stored->GetHash(),hash);
}
TEST(OrchardCompactTransition, HistoricalParentRollbackReopenAndReactivation) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto& observed=f.notices->recorded;
    ASSERT_NE(disk.parent,nullptr);ASSERT_NE(disk.parent->pprev,nullptr);
    const auto historical_hash=disk.parent->pprev->hash;
    const auto historical_height=disk.parent->pprev->height;
    ASSERT_EQ(historical_height,100u);
    const auto parent_hash=disk.parent->hash;
    const auto first=disk.first->Header().GetHash(),second=disk.second->Header().GetHash();
    const auto original=disk.reopened.getOrchardState();ASSERT_TRUE(original.ok());
    const auto head=f.Head();
    const auto check_historical=[&] {
        ASSERT_NE(f.service->GetActiveTip(),nullptr);
        EXPECT_EQ(f.service->GetActiveTip()->hash,historical_hash);
        EXPECT_EQ(f.service->GetActiveTip()->height,historical_height);
        const auto tip=disk.reopened.getTip(),validated=disk.reopened.getValidatedTip();
        ASSERT_TRUE(tip.ok());ASSERT_TRUE(validated.ok());
        EXPECT_EQ(tip->hash,historical_hash);EXPECT_EQ(validated->hash,historical_hash);
        EXPECT_EQ(tip->height,int32_t(historical_height));EXPECT_EQ(validated->height,int32_t(historical_height));
        EXPECT_EQ(tip->work,ChainworkFromHex(f.service->GetActiveTip()->chainwork));
        EXPECT_EQ(disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
        EXPECT_EQ(disk.reopened.getOrchardState().status(),Status::NotFound);
        size_t full_coins=0;
        ASSERT_EQ(disk.reopened.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++full_coins;return true;}),Status::Ok);
        EXPECT_EQ(full_coins,0u);EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);
        EXPECT_FALSE(f.service->IsStarted());
    };
    std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(parent_hash,error))<<error;
    ASSERT_NO_FATAL_FAILURE(check_historical());
    ASSERT_EQ(observed.plan_count,1u);EXPECT_TRUE(observed.progress[0].complete);
    EXPECT_EQ(observed.progress[0].disconnected,3u);EXPECT_EQ(observed.progress[0].connected,0u);
    ASSERT_EQ(observed.event_count,3u);
    EXPECT_EQ(observed.events[0].hash,second);EXPECT_EQ(observed.events[1].hash,first);
    EXPECT_EQ(observed.events[2].hash,parent_hash);EXPECT_EQ(f.Head().sequence,head.sequence+3);
    ASSERT_NO_THROW(f.Reopen());ASSERT_NO_FATAL_FAILURE(check_historical());
    ASSERT_TRUE(f.service->ReconsiderBlock(parent_hash,error))<<error;
    f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));
    ASSERT_TRUE(disk.reopened.getOrchardState().ok());
    EXPECT_EQ(*disk.reopened.getOrchardState(),*original);
    ASSERT_EQ(observed.event_count,6u);EXPECT_EQ(observed.events[3].hash,parent_hash);
    EXPECT_EQ(observed.events[4].hash,first);EXPECT_EQ(observed.events[5].hash,second);
    EXPECT_EQ(f.Head().sequence,head.sequence+6);
    ASSERT_NO_THROW(f.Reopen());ASSERT_NO_FATAL_FAILURE(f.CheckSelected(second,103));
    f.service->ActivateBestChain();EXPECT_EQ(observed.event_count,6u);
    EXPECT_EQ(f.Head().sequence,head.sequence+6);
}

TEST(OrchardOutgoingHandoff, HistoricalForkProofRetainsSelectedBeforeImage) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto* tip=f.service->GetActiveTip();
    auto* fork=disk.parent->pprev;ASSERT_NE(fork,nullptr);ASSERT_EQ(fork->height,100u);
    const std::vector<CBlockIndex*> path{tip,tip->pprev,disk.parent};
    const auto state=disk.reopened.getOrchardState();ASSERT_TRUE(state.ok());const auto head=f.Head();
    auto owner=OrchardOutgoingHandoffTestAccess::Capture(*f.service,path,fork);ASSERT_TRUE(owner);
    EXPECT_THROW((void)owner->parent->HistoricalPrefix(),std::exception);
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    }
    ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    const auto& proof=owner->parent->HistoricalPrefix();const auto& target=proof.ValidatedTarget();
    EXPECT_EQ(target.height,fork->height);EXPECT_EQ(target.hash,fork->hash);
    EXPECT_EQ(target.chainwork,ChainworkFromHex(fork->chainwork));
    const auto& snapshot=proof.State();EXPECT_EQ(snapshot.activation_height,102u);
    const auto header=disk.reopened.getHeader(fork->hash);ASSERT_TRUE(header.ok());
    const auto stump=consensus::UtreexoStump::deserialize(snapshot.stump);
    EXPECT_EQ(stump.getCommitment(),std::vector<uint8_t>(header->utreexo_root.begin(),header->utreexo_root.end()));
    ASSERT_EQ(proof.ProvenUndo().size(),1u);EXPECT_EQ(proof.ProvenUndo().front().height,100u);
    EXPECT_EQ(proof.ProvenUndo().front().block_hash,fork->hash);
    EXPECT_EQ(proof.ProvenState().Height(),100u);
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_EQ(f.service->GetActiveTip(),tip);EXPECT_EQ(*disk.reopened.getOrchardState(),*state);
    EXPECT_EQ(f.Head().sequence,head.sequence);EXPECT_EQ(f.notices->recorded.event_count,0u);
}
TEST(OrchardOutgoingHandoff, HistoricalForkBindingRejectsChangedHistoricalInputs) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto* tip=f.service->GetActiveTip();
    auto* fork=disk.parent->pprev;ASSERT_NE(fork,nullptr);
    const std::vector<CBlockIndex*> path{tip,tip->pprev,disk.parent};
    auto owner=OrchardOutgoingHandoffTestAccess::Capture(*f.service,path,fork);ASSERT_TRUE(owner);
    ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    const auto fork_nonce=fork->nonce;fork->nonce^=1;
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));fork->nonce=fork_nonce;
    const auto parent_nonce=disk.parent->nonce;disk.parent->nonce^=1;
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));disk.parent->nonce=parent_nonce;
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    const auto validated=disk.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,fork->hash,fork->height),Status::Ok);
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,validated->hash,validated->height),Status::Ok);
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,{tip,tip->pprev},fork));
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_EQ(f.service->GetActiveTip(),tip);EXPECT_EQ(f.notices->recorded.event_count,0u);
}
TEST(OrchardOutgoingHandoff, HistoricalPrefixFailurePoisonsResults) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto* target=disk.parent->pprev;
    ASSERT_NE(target,nullptr);
    HistoricalCompactReplay::Target wanted{target->height,target->hash,ChainworkFromHex(target->chainwork)};
    EXPECT_THROW((HistoricalCompactReplay(wanted,{100,64*1024*1024})),std::exception);
    EXPECT_THROW((HistoricalCompactReplay({disk.parent->height,disk.parent->hash,
        ChainworkFromHex(disk.parent->chainwork)},{102,64*1024*1024})),std::exception);
    HistoricalCompactReplay incomplete(wanted,{101,64*1024*1024});
    EXPECT_THROW((void)incomplete.State(),std::exception);
    EXPECT_THROW(incomplete.Finish(),std::exception);
    EXPECT_THROW((void)incomplete.ProvenState(),std::exception);
    auto* genesis=target;while(genesis->pprev)genesis=genesis->pprev;
    const auto body=storage::ReadArchivalBlock(disk.reopened,disk.files.get(),genesis->hash);
    ASSERT_TRUE(body.ok());
    HistoricalCompactReplay bad_work(wanted,{101,64*1024*1024});
    EXPECT_THROW(bad_work.Append(*body,0,arith_uint256(0)),std::exception);
    EXPECT_THROW(bad_work.Append(*body,0,ChainworkFromHex(genesis->chainwork)),std::exception);
    EXPECT_THROW(bad_work.Finish(),std::exception);
    EXPECT_THROW((void)bad_work.ProvenUndo(),std::exception);
    EXPECT_EQ(f.notices->recorded.event_count,0u);
}

TEST(OrchardOutgoingHandoff, HistoricalCatalogUsesExactPrefixAndDistinctEncoding) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto* tip=f.service->GetActiveTip();
    auto* fork=disk.parent->pprev;ASSERT_NE(fork,nullptr);
    const auto selected=disk.reopened.getOrchardState();ASSERT_TRUE(selected.ok());
    const auto head=f.Head();const std::vector<CBlockIndex*> path{tip,tip->pprev,disk.parent};
    auto owner=OrchardOutgoingHandoffTestAccess::Capture(*f.service,path,fork);ASSERT_TRUE(owner);
    ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    const auto& catalog=owner->parent->HistoricalCatalog();const auto state=catalog.State();
    EXPECT_EQ(state.height,100u);EXPECT_EQ(state.block,fork->hash);
    EXPECT_EQ(state.parent,fork->prev_hash);EXPECT_EQ(state.work,ChainworkFromHex(fork->chainwork));
    EXPECT_EQ(state.stump,owner->parent->HistoricalPrefix().State().stump);
    uint64_t count=0;
    for(uint32_t height=0;height<=disk.parent->height;++height) {
        const auto hash=disk.reopened.getBlockHashByHeight(int(height));ASSERT_TRUE(hash.ok());
        const auto body=storage::ReadArchivalBlock(disk.reopened,disk.files.get(),*hash);ASSERT_TRUE(body.ok());
        for(const auto& tx:body->vtx) {
            EXPECT_EQ(catalog.ContainsTransaction(tx.GetTxid()),height<=fork->height);
            if(height<=fork->height)++count;
        }
    }
    EXPECT_EQ(state.transaction_count,count);
    for(const auto& [point,coin]:owner->parent->HistoricalPrefix().ProvenState().ProvenUtxos()) {
        EXPECT_EQ(catalog.NonTransparentCoin(point),coin.is_confidential||!coin.commitment.empty());
        const auto metadata=catalog.LegacyCoin(point);
        if(coin.height<state.leaf_activation) {
            ASSERT_TRUE(metadata);EXPECT_EQ(metadata->height,coin.height);EXPECT_EQ(metadata->coinbase,coin.isCoinbase);
        } else EXPECT_FALSE(metadata);
    }
    const auto wire=state.Encode();EXPECT_EQ(storage::catalog::HistoricalState::Decode(wire).Encode(),wire);
    EXPECT_THROW((void)storage::catalog::State::Decode(wire),std::exception);
    auto corrupt=wire;corrupt.back()^=1;
    EXPECT_THROW((void)storage::catalog::HistoricalState::Decode(corrupt),std::exception);
    auto boundary=state;boundary.height=boundary.activation-1;
    EXPECT_THROW((void)boundary.Encode(),std::exception);
    auto missing=state;missing.transactions={};
    EXPECT_THROW((void)missing.Encode(),std::exception);
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_EQ(f.service->GetActiveTip(),tip);EXPECT_EQ(*disk.reopened.getOrchardState(),*selected);
    EXPECT_EQ(f.Head().sequence,head.sequence);EXPECT_EQ(f.notices->recorded.event_count,0u);
}

} // namespace dinero
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct HistoricalCatalogTestAccess {
    static auto Create(ChainDB& db,const ChainWriteToken& token,const HistoricalCompactReplay& replay,
        const OrchardHistoryCapture& history) {return PreparedHistoricalCatalog::Create(db,token,replay,history);}
};
namespace {
struct HistoricalWriterFixture {
    CompactStartupFixture disk;
    std::unique_ptr<OrchardCompactChainstate> live;
    std::unique_ptr<HistoricalCompactReplay> lower;
    std::unique_ptr<OrchardHistoryCapture> history;
    std::unique_ptr<PreparedHistoricalCatalog> low_catalog;
    std::unique_ptr<PreparedOrchardCatalog> high_catalog;
    HistoricalWriterFixture() {
        const auto require=[](bool ok){OrchardAdmissionFixture::Require(ok);};
        live=disk.Restore();require(bool(live));disk.Undo(*live,disk.second);disk.Undo(*live,disk.first);
        const auto* fork=disk.parent->pprev;require(fork&&fork->height==100);
        lower=std::make_unique<HistoricalCompactReplay>(HistoricalCompactReplay::Target{
            fork->height,fork->hash,ChainworkFromHex(fork->chainwork)},HistoricalCompactReplay::Limits{
                SelectedParentReplayWorkLimits().blocks,SelectedParentReplayWorkLimits().serialized_bytes});
        history=std::make_unique<OrchardHistoryCapture>(disk.parent->height,disk.parent->hash);
        CaptureHeaders(*history,disk.f.blocks);arith_uint256 work{0};
        for(uint32_t h=0;h<disk.f.blocks.size();++h) {
            const auto& block=disk.f.blocks[h];work+=GetBlockProof(block.header.difficulty);
            if(h<=fork->height)lower->Append(block,h,work);
            history->RecordBody(h,block);
        }
        lower->Finish();history->Finish();
        low_catalog=HistoricalCatalogTestAccess::Create(disk.reopened,disk.f.token,*lower,*history);
        high_catalog=OrchardParentCatalogTestAccess::Create(disk.reopened,disk.f.token,*disk.independent,*history);
    }
    auto Prepare(bool connecting) {
        return PreparedOrchardChainstateWrite::HistoricalCompactIndexed(disk.startup_mutex,disk.reopened,disk.f.token,
            *disk.files,*disk.parent,*live,disk.f.blocks.at(101),*low_catalog,std::cref(*high_catalog),connecting);
    }
    auto Head() {
        const auto profile=consensus::SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);
        OrchardAdmissionFixture::Require(bool(profile));
        std::lock_guard<AnnotatedRecursiveMutex> lock(disk.startup_mutex);
        return ReadRuntimeOutboxUnderLock(disk.reopened,*profile,{},1).head;
    }
};
}
TEST(OrchardOutgoingHandoff, HistoricalWriterRoundTripAndAbandonment) {
    HistoricalWriterFixture f;auto& db=f.disk.reopened;const auto parent=f.disk.parent->hash;
    const auto parent_catalog=db.getOrchardCatalogState(parent);ASSERT_TRUE(parent_catalog.ok());
    const auto rows=f.disk.Rows();const auto head=f.Head();
    {auto abandoned=f.Prepare(false);ASSERT_TRUE(abandoned);EXPECT_EQ(f.disk.Rows(),rows);}
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.Head(),head);
    {auto writer=f.Prepare(false);writer->Commit();}
    const auto low=f.low_catalog->State();ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,low.block);
    ASSERT_TRUE(db.getValidatedTip().ok());EXPECT_EQ(db.getValidatedTip()->hash,low.block);
    const auto encoded=db.getHistoricalCompactCatalogState(low.block);ASSERT_TRUE(encoded.ok());EXPECT_EQ(*encoded,low.Encode());
    EXPECT_EQ(db.getOrchardCatalogState(low.block).status(),Status::NotFound);
    EXPECT_EQ(db.getLegacyRetirementState().status(),Status::NotFound);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(f.disk.startup_mutex);
        EXPECT_EQ(f.live->SelectedUnderLock(f.disk.startup_mutex),nullptr);
        ASSERT_NE(f.live->HistoricalUnderLock(f.disk.startup_mutex),nullptr);
        EXPECT_EQ(f.live->HistoricalUnderLock(f.disk.startup_mutex)->Encode(),low.Encode());
        EXPECT_EQ(f.live->RetirementUnderLock(f.disk.startup_mutex),nullptr);
    }
    size_t coins=0;ASSERT_EQ(db.forEachUTXO([&](const auto&,uint32_t,const auto&){++coins;return true;}),Status::Ok);EXPECT_EQ(coins,0u);
    EXPECT_EQ(f.Head().sequence,head.sequence+1);
    {auto writer=f.Prepare(true);writer->Commit();}
    EXPECT_EQ(db.getTip()->hash,parent);EXPECT_EQ(db.getValidatedTip()->hash,parent);
    EXPECT_EQ(*db.getOrchardCatalogState(parent),*parent_catalog);EXPECT_EQ(f.Head().sequence,head.sequence+2);
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(f.disk.startup_mutex);
        EXPECT_EQ(f.live->HistoricalUnderLock(f.disk.startup_mutex),nullptr);
        ASSERT_NE(f.live->SelectedUnderLock(f.disk.startup_mutex),nullptr);
        EXPECT_EQ(f.live->SelectedUnderLock(f.disk.startup_mutex)->Encode(),*parent_catalog);
    }
    // Reopen at the genuine activation parent, where the existing startup
    // reconstruction is qualified. Historical-tip reopen remains a separate gate.
    f.live.reset();db.close();ASSERT_EQ(db.init(f.disk.f.path),Status::Ok);
    f.live=f.disk.Restore();ASSERT_TRUE(f.live);
}
TEST(OrchardOutgoingHandoff, HistoricalWriterRechecksPreparedFilter) {
    HistoricalWriterFixture f;auto& db=f.disk.reopened;const auto parent=f.disk.parent->hash;
    // Establish the previously absent historical filter through the real
    // writer, then return to the same parent before the refusal experiment.
    {auto establish=f.Prepare(false);establish->Commit();}
    {auto restore=f.Prepare(true);restore->Commit();}
    const auto filter=db.getBlockFilter(parent);ASSERT_TRUE(filter.ok());const auto head=f.Head();
    const auto previous_catalog=db.getHistoricalCompactCatalogState(f.low_catalog->State().block);
    ASSERT_TRUE(previous_catalog.ok());
    auto writer=f.Prepare(false);auto corrupt=filter->data;corrupt.push_back(0xff);
    ASSERT_EQ(db.putBlockFilter(f.disk.f.token,parent,corrupt,filter->element_count),Status::Ok);
    EXPECT_THROW(writer->Commit(),consensus::OrchardStateLookupError);
    EXPECT_THROW(writer->Commit(),std::logic_error);writer.reset();
    EXPECT_EQ(db.getTip()->hash,parent);EXPECT_EQ(db.getValidatedTip()->hash,parent);EXPECT_EQ(f.Head(),head);
    const auto retained_catalog=db.getHistoricalCompactCatalogState(f.low_catalog->State().block);
    ASSERT_TRUE(retained_catalog.ok());EXPECT_EQ(*retained_catalog,*previous_catalog);
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(f.disk.startup_mutex);
        ASSERT_NE(f.live->SelectedUnderLock(f.disk.startup_mutex),nullptr);
        EXPECT_EQ(f.live->SelectedUnderLock(f.disk.startup_mutex)->block,parent);
    }
    ASSERT_EQ(db.putBlockFilter(f.disk.f.token,parent,filter->data,filter->element_count),Status::Ok);
    {auto retry=f.Prepare(false);retry->Commit();}
    EXPECT_EQ(db.getTip()->hash,f.low_catalog->State().block);EXPECT_EQ(f.Head().sequence,head.sequence+1);
}
}

#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardCompactStartupAudit, HistoricalPrefixReopenPreservesSource) {
    HistoricalWriterFixture f;const auto expected=f.low_catalog->State();
    {auto write=f.Prepare(false);write->Commit();}
    f.live.reset();f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    const auto rows=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();
    f.live=f.disk.Restore();ASSERT_TRUE(f.live);
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.disk.ArchiveBytes(),archives);
    std::lock_guard<AnnotatedRecursiveMutex> lock(f.disk.startup_mutex);
    EXPECT_EQ(f.live->SelectedUnderLock(f.disk.startup_mutex),nullptr);
    EXPECT_EQ(f.live->RetirementUnderLock(f.disk.startup_mutex),nullptr);
    const auto* selected=f.live->HistoricalUnderLock(f.disk.startup_mutex);ASSERT_NE(selected,nullptr);
    EXPECT_EQ(selected->Encode(),expected.Encode());
}
TEST(OrchardCompactStartupAudit, HistoricalPrefixLegacyMismatchRefusesWithoutRepair) {
    HistoricalWriterFixture f;
    {auto write=f.Prepare(false);write->Commit();}
    f.live.reset();auto& db=f.disk.reopened;
    const auto frontier=db.getShieldedState(ChainDB::ShieldedStateRecord::Frontier);ASSERT_TRUE(frontier.ok());
    ASSERT_EQ(db.putShieldedState(f.disk.f.token,ChainDB::ShieldedStateRecord::Frontier,"malformed historical frontier"),Status::Ok);
    const auto rows=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();
    EXPECT_THROW(f.disk.Restore(),std::exception);
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.disk.ArchiveBytes(),archives);
    ASSERT_EQ(db.putShieldedState(f.disk.f.token,ChainDB::ShieldedStateRecord::Frontier,*frontier),Status::Ok);
    f.live=f.disk.Restore();ASSERT_TRUE(f.live);
}
TEST(OrchardCompactStartupAudit, HistoricalPrefixServiceInitAndUnpublishedAudit) {
    auto fixture=std::make_shared<HistoricalWriterFixture>();auto& f=*fixture;const auto expected=f.low_catalog->State();
    {auto write=f.Prepare(false);write->Commit();}
    f.live.reset();f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    const auto rows=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();
    DaemonContext context;ConfigureCompactServiceContext(context,f.disk);GetConfig().utreexo_stateless=true;
    auto service=std::make_shared<ChainstateService>();context.chainstate=service;
    struct Cleanup {
        DaemonContext* previous;DaemonContext& context;std::shared_ptr<ChainstateService>& service;
        ~Cleanup(){service->Stop();context.chainstate.reset();service.reset();DaemonContext::setInstance(previous);}
    } cleanup{DaemonContext::instance(),context,service};
    DaemonContext::setInstance(&context);
    service->setOwnedChainDB(std::shared_ptr<ChainDB>(fixture,&f.disk.reopened));
    ASSERT_TRUE(service->Init(context));EXPECT_EQ(service->GetActiveTip(),nullptr);
    service->setHeaderChainSelector(CompactBindingSelector(f.disk));
    OrchardCompactTransitionTestAccess::SelectAuditedComponent(*service);
    ASSERT_NE(service->GetActiveTip(),nullptr);EXPECT_EQ(service->GetActiveTip()->hash,expected.block);
    EXPECT_EQ(service->GetActiveTip()->height,expected.height);EXPECT_FALSE(service->IsStarted());
    EXPECT_EQ(service->GetConsensusUTXOSet(),nullptr);EXPECT_EQ(service->utxoIndex(),nullptr);
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.disk.ArchiveBytes(),archives);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
HistoricalCompactReplay::Target RangeTarget(const OrchardAdmissionFixture& f,uint32_t height) {
    arith_uint256 work{0};for(uint32_t h=0;h<=height;++h)work+=GetBlockProof(f.blocks.at(h).header.difficulty);
    return {height,f.blocks.at(height).GetHash(),work};
}
void AppendRange(HistoricalCompactReplay& replay,const OrchardAdmissionFixture& f,uint32_t height) {
    arith_uint256 work{0};for(uint32_t h=0;h<=height;++h) {
        work+=GetBlockProof(f.blocks.at(h).header.difficulty);replay.Append(f.blocks.at(h),h,work);
    }
}
}
TEST(OrchardOutgoingHandoff, HistoricalCheckpointsMatchIndependentPrefixes) {
    OrchardAdmissionFixture f(true);HistoricalCompactReplay range(RangeTarget(f,100),{101,64*1024*1024});
    range.CaptureCheckpointsFrom(98,16*1024*1024);
    EXPECT_THROW((void)range.CheckpointAt(98),std::exception);
    AppendRange(range,f,100);
    EXPECT_THROW((void)range.CheckpointAt(98),std::exception);
    range.Finish();ASSERT_EQ(range.CheckpointCount(),3u);
    for(uint32_t height=98;height<=100;++height) {
        HistoricalCompactReplay reference(RangeTarget(f,height),{101,64*1024*1024});
        AppendRange(reference,f,height);reference.Finish();const auto step=range.CheckpointAt(height);
        EXPECT_EQ(step.target.height,height);EXPECT_EQ(step.target.hash,f.blocks[height].GetHash());
        EXPECT_EQ(step.parent,f.blocks[height-1].GetHash());EXPECT_EQ(step.target.chainwork,reference.ValidatedTarget().chainwork);
        EXPECT_EQ(step.snapshot.stump,reference.State().stump);
        EXPECT_EQ(step.snapshot.legacy_state_root,reference.State().legacy_state_root);
        EXPECT_EQ(step.snapshot.tree_size,reference.State().tree_size);
        EXPECT_EQ(step.snapshot.legacy_value,reference.State().legacy_value);
        EXPECT_EQ(step.frontier,reference.ProvenState().ShieldedTree()->SerializeFrontier());
        EXPECT_EQ(step.anchors,reference.ProvenState().ShieldedAnchors()->SerializePersistenceBytes());
        std::string delta,error;ASSERT_TRUE(SerializeUtreexoDelta(*reference.ProvenUndo().front().undo.utreexo_delta,delta,error));
        EXPECT_EQ(step.delta,delta);const auto undo=UndoRecord::Deserialize(step.undo);
        ASSERT_EQ(undo.created.size(),f.blocks[height].vtx.front().vout.size());
        EXPECT_EQ(undo.created.front().txid,f.blocks[height].vtx.front().GetTxid().AsUint256());
        uint256 digest;crypto::CSHA256().Write(f.blocks[height].Serialize()).Finalize(digest.data);
        EXPECT_EQ(step.wire_hash,digest);
    }
    EXPECT_THROW((void)range.CheckpointAt(97),std::exception);
    EXPECT_THROW((void)range.CheckpointAt(101),std::exception);
    EXPECT_EQ(range.ProvenState().Height(),100u);EXPECT_EQ(range.ProvenUndo().front().height,100u);
}
TEST(OrchardOutgoingHandoff, HistoricalCheckpointBudgetAndFailureRefuseAllResults) {
    OrchardAdmissionFixture f(true);HistoricalCompactReplay range(RangeTarget(f,100),{101,64*1024*1024});
    range.CaptureCheckpointsFrom(0,1);
    EXPECT_THROW(AppendRange(range,f,0),std::exception);
    EXPECT_THROW((void)range.CheckpointAt(0),std::exception);
    EXPECT_THROW((void)range.State(),std::exception);
    EXPECT_THROW(range.Finish(),std::exception);
    HistoricalCompactReplay late(RangeTarget(f,100),{101,64*1024*1024});
    AppendRange(late,f,0);EXPECT_THROW(late.CaptureCheckpointsFrom(0,1024),std::exception);
    HistoricalCompactReplay interrupted(RangeTarget(f,100),{101,64*1024*1024});
    interrupted.CaptureCheckpointsFrom(0,16*1024*1024);AppendRange(interrupted,f,0);
    EXPECT_THROW(interrupted.Finish(),std::exception);
    EXPECT_THROW((void)interrupted.CheckpointAt(0),std::exception);
    EXPECT_THROW((void)interrupted.CheckpointCount(),std::exception);
}
TEST(OrchardOutgoingHandoff, HistoricalCheckpointEpochResetKeepsEarlierPrefix) {
    OrchardAdmissionFixture f(true);
    // Empty historical pools, but genuine validator epoch resets and retained
    // undo snapshots; this does not qualify nonempty shielded epoch balances.
    MutableParams().shielded_epoch_reset_height=98;
    MutableParams().shielded_spend_auth_epoch_reset_height=100;
    HistoricalCompactReplay range(RangeTarget(f,100),{101,64*1024*1024});
    range.CaptureCheckpointsFrom(97,16*1024*1024);AppendRange(range,f,100);range.Finish();
    EXPECT_EQ(range.CheckpointAt(97).snapshot.legacy_epoch_height,1u);
    EXPECT_EQ(range.CheckpointAt(98).snapshot.legacy_epoch_height,98u);
    EXPECT_EQ(range.CheckpointAt(99).snapshot.legacy_epoch_height,98u);
    EXPECT_EQ(range.CheckpointAt(100).snapshot.legacy_epoch_height,100u);
    for(uint32_t height:{98u,100u}) {
        const auto step=range.CheckpointAt(height);const auto undo=UndoRecord::Deserialize(step.undo);
        ASSERT_TRUE(undo.pre_reset_shielded_epoch.has_value());
        HistoricalCompactReplay reference(RangeTarget(f,height),{101,64*1024*1024});
        AppendRange(reference,f,height);reference.Finish();EXPECT_EQ(step.snapshot.legacy_state_root,reference.State().legacy_state_root);
        EXPECT_EQ(step.snapshot.stump,reference.State().stump);EXPECT_EQ(step.snapshot.legacy_value,0u);
    }
    EXPECT_EQ(range.Accounting().epoch_height,100u);EXPECT_EQ(range.Accounting().blocks_read,1u);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct HistoricalRangeCatalogTestAccess {
    static auto Create(ChainDB& db,const ChainWriteToken& token,const PreparedHistoricalCatalog& initial,
        const HistoricalCompactReplay& replay,const OrchardHistoryCapture& history,
        const std::function<Block(uint32_t,const uint256&)>& body_at) {
        return PreparedHistoricalCatalogRange::Create(db,token,initial,replay,history,body_at);
    }
};
TEST(OrchardOutgoingHandoff, HistoricalIncrementalCatalogMatchesIndependentPrefix) {
    OrchardAdmissionFixture f(true);const auto tip=f.db.getTip();ASSERT_TRUE(tip.ok());
    OrchardHistoryCapture history(101,f.blocks.back().GetHash());CaptureHeaders(history,f.blocks);
    for(uint32_t h=0;h<f.blocks.size();++h)history.RecordBody(h,f.blocks[h]);history.Finish();
    HistoricalCompactReplay initial(RangeTarget(f,98),{101,64*1024*1024});AppendRange(initial,f,98);initial.Finish();
    const auto first=HistoricalCatalogTestAccess::Create(f.db,f.token,initial,history);
    HistoricalCompactReplay range(RangeTarget(f,100),{101,64*1024*1024});range.CaptureCheckpointsFrom(98,16*1024*1024);
    AppendRange(range,f,100);range.Finish();
    unsigned reads=0;
    const auto catalog=HistoricalRangeCatalogTestAccess::Create(f.db,f.token,*first,range,history,
        [&](uint32_t height,const uint256& hash){++reads;EXPECT_EQ(hash,f.blocks.at(height).GetHash());return f.blocks.at(height);});
    EXPECT_EQ(reads,2u);EXPECT_EQ(catalog->First(),98u);EXPECT_EQ(catalog->Last(),100u);
    for(uint32_t height=98;height<=100;++height) {
        HistoricalCompactReplay reference(RangeTarget(f,height),{101,64*1024*1024});AppendRange(reference,f,height);reference.Finish();
        const auto full=HistoricalCatalogTestAccess::Create(f.db,f.token,reference,history);
        EXPECT_EQ(catalog->At(height).Encode(),full->State().Encode());
        EXPECT_EQ(f.db.getHistoricalCompactCatalogState(f.blocks[height].GetHash()).status(),Status::NotFound);
    }
    EXPECT_THROW((void)catalog->At(97),std::exception);
    EXPECT_THROW((void)catalog->At(101),std::exception);
    EXPECT_THROW((void)HistoricalRangeCatalogTestAccess::Create(f.db,f.token,*first,range,history,
        [&](uint32_t height,const uint256&){return f.blocks.at(height-1);}),std::exception);
    const auto after=f.db.getTip();ASSERT_TRUE(after.ok());EXPECT_EQ(after->hash,tip->hash);EXPECT_EQ(after->height,tip->height);
}
}
#endif


#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct HistoricalRangeWriterFixture : HistoricalWriterFixture {
    std::unique_ptr<HistoricalCompactReplay> initial,range;
    std::unique_ptr<PreparedHistoricalCatalog> first;
    std::unique_ptr<PreparedHistoricalCatalogRange> catalogs;
    HistoricalRangeWriterFixture() {
        initial=std::make_unique<HistoricalCompactReplay>(RangeTarget(disk.f,99),HistoricalCompactReplay::Limits{101,64*1024*1024});
        AppendRange(*initial,disk.f,99);initial->Finish();
        first=HistoricalCatalogTestAccess::Create(disk.reopened,disk.f.token,*initial,*history);
        range=std::make_unique<HistoricalCompactReplay>(RangeTarget(disk.f,100),HistoricalCompactReplay::Limits{101,64*1024*1024});
        range->CaptureCheckpointsFrom(99,16*1024*1024);AppendRange(*range,disk.f,100);range->Finish();
        catalogs=HistoricalRangeCatalogTestAccess::Create(disk.reopened,disk.f.token,*first,*range,*history,
            [&](uint32_t height,const uint256& hash){OrchardAdmissionFixture::Require(disk.f.blocks.at(height).GetHash()==hash);return disk.f.blocks.at(height);});
    }
    CBlockIndex* Index(uint32_t height) {
        auto* index=disk.parent;while(index&&index->height>height)index=index->pprev;
        OrchardAdmissionFixture::Require(index&&index->height==height);return index;
    }
    auto PrepareAt(uint32_t height,bool connecting,const Block* body=nullptr,bool with_boundary=true) {
        return PreparedOrchardChainstateWrite::HistoricalCompactRangeIndexed(disk.startup_mutex,disk.reopened,disk.f.token,
            *disk.files,*Index(height),*live,body?*body:disk.f.blocks.at(height),*catalogs,with_boundary?high_catalog.get():nullptr,connecting);
    }
};
}
TEST(OrchardOutgoingHandoff, HistoricalRangeWriterTwoEdgesRoundTripAndAbandonment) {
    HistoricalRangeWriterFixture f;auto& db=f.disk.reopened;const auto parent=f.disk.parent->hash;
    const auto original=db.getOrchardCatalogState(parent);ASSERT_TRUE(original.ok());const auto head=f.Head();
    {auto writer=f.PrepareAt(101,false);writer->Commit();}
    const auto at100=f.disk.Rows();const auto head100=f.Head();
    ASSERT_TRUE(db.getHistoricalCompactCatalogState(f.disk.f.blocks[100].GetHash()).ok());
    EXPECT_EQ(*db.getHistoricalCompactCatalogState(f.disk.f.blocks[100].GetHash()),f.low_catalog->State().Encode());
    {auto abandoned=f.PrepareAt(100,false,nullptr,false);ASSERT_TRUE(abandoned);EXPECT_EQ(f.disk.Rows(),at100);}
    EXPECT_EQ(f.disk.Rows(),at100);EXPECT_EQ(f.Head(),head100);
    {auto writer=f.PrepareAt(100,false,nullptr,false);writer->Commit();}
    ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->height,99);EXPECT_EQ(db.getTip()->hash,f.disk.f.blocks[99].GetHash());
    EXPECT_EQ(db.getBlockHashByHeight(100).status(),Status::NotFound);
    EXPECT_EQ(db.getTxLocation(f.disk.f.blocks[100].vtx[0].GetTxid().AsUint256()).status(),Status::NotFound);
    ASSERT_TRUE(db.getHistoricalCompactCatalogState(f.disk.f.blocks[99].GetHash()).ok());
    EXPECT_EQ(*db.getHistoricalCompactCatalogState(f.disk.f.blocks[99].GetHash()),f.first->State().Encode());
    EXPECT_EQ(db.getLegacyRetirementState().status(),Status::NotFound);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
    {auto writer=f.PrepareAt(100,true,nullptr,false);writer->Commit();}
    {auto writer=f.PrepareAt(101,true);writer->Commit();}
    EXPECT_EQ(db.getTip()->hash,parent);EXPECT_EQ(*db.getOrchardCatalogState(parent),*original);EXPECT_EQ(f.Head().sequence,head.sequence+4);
    size_t coins=0;ASSERT_EQ(db.forEachUTXO([&](const auto&,uint32_t,const auto&){++coins;return true;}),Status::Ok);EXPECT_EQ(coins,0u);
    f.live.reset();db.close();ASSERT_EQ(db.init(f.disk.f.path),Status::Ok);f.live=f.disk.Restore();ASSERT_TRUE(f.live);
}
TEST(OrchardOutgoingHandoff, HistoricalRangeWriterWrongBodyBoundsAndSelectedPrefixRefuse) {
    HistoricalRangeWriterFixture f;const auto before=f.disk.Rows();const auto head=f.Head();
    EXPECT_THROW((void)f.PrepareAt(100,false),std::exception); // selected parent is 101
    EXPECT_THROW((void)f.PrepareAt(99,false),std::exception); // lower 98 not in range
    EXPECT_THROW((void)f.PrepareAt(101,false,&f.disk.f.blocks[100]),std::exception);
    EXPECT_THROW((void)f.PrepareAt(101,false,nullptr,false),std::exception); // boundary cannot be inferred
    EXPECT_EQ(f.disk.Rows(),before);EXPECT_EQ(f.Head(),head);
    {auto writer=f.PrepareAt(101,false);writer->Commit();}
    const auto at100=f.disk.Rows();const auto head100=f.Head();
    EXPECT_THROW((void)f.PrepareAt(100,false,&f.disk.f.blocks[99]),std::exception);
    EXPECT_EQ(f.disk.Rows(),at100);EXPECT_EQ(f.Head(),head100);
    {auto writer=f.PrepareAt(100,false,nullptr,false);writer->Commit();}
    EXPECT_EQ(f.disk.reopened.getTip()->height,99);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct HistoricalBranchPreparationTestAccess {
    static bool Apply(ChainstateService& service,CBlockIndex* node,
        const ChainstateService::PreparedHistoricalBranch& owner,bool connecting,std::string& error) {
        return service.ApplyHistoricalCompactTip(node,owner.Range(),nullptr,connecting,&error);
    }

    static auto Capture(ChainstateService& service,CBlockIndex* last,CBlockIndex* first) {
        auto lock=service.AcquireBlockIngressActivationLock();
        return service.CaptureHistoricalBranchUnderLock(last,first);
    }
    static bool Complete(ChainstateService& service,ChainstateService::PreparedHistoricalBranch& owner) {
        return service.CompleteHistoricalBranch(owner);
    }
    static bool Bind(ChainstateService& service,const ChainstateService::PreparedHistoricalBranch& owner,
                     CBlockIndex* last,CBlockIndex* first) {
        auto lock=service.AcquireBlockIngressActivationLock();
        return service.BindHistoricalBranchUnderLock(owner,last,first);
    }
};
namespace {
struct HistoricalPreparationFixture {
    std::shared_ptr<HistoricalWriterFixture> storage=std::make_shared<HistoricalWriterFixture>();
    DaemonContext* previous=DaemonContext::instance();
    DaemonContext context;
    std::shared_ptr<ChainstateService> service;
    HistoricalPreparationFixture() {
        auto& f=*storage;
        {auto write=f.Prepare(false);write->Commit();}
        f.live.reset();f.disk.reopened.close();
        OrchardAdmissionFixture::Require(f.disk.reopened.init(f.disk.f.path)==Status::Ok);
        ConfigureCompactServiceContext(context,f.disk);GetConfig().utreexo_stateless=true;
        service=std::make_shared<ChainstateService>();context.chainstate=service;DaemonContext::setInstance(&context);
        service->setOwnedChainDB(std::shared_ptr<ChainDB>(storage,&f.disk.reopened));
        OrchardAdmissionFixture::Require(service->Init(context));
        service->setHeaderChainSelector(CompactBindingSelector(f.disk));
        OrchardCompactTransitionTestAccess::SelectAuditedComponent(*service);
        OrchardAdmissionFixture::Require(service->GetActiveTip() && service->GetActiveTip()->height==100);
    }
    ~HistoricalPreparationFixture() {
        service->Stop();context.chainstate.reset();service.reset();DaemonContext::setInstance(previous);
    }
};
}
TEST(OrchardHistoricalController, HistoricalServiceRangeMatchesIndependentPrefixes) {
    CompactTransitionFixture f;auto& disk=*f.storage;auto* tip=f.service->GetActiveTip();
    auto* fork=disk.parent->pprev->pprev->pprev;ASSERT_EQ(fork->height,98u);
    const std::vector<CBlockIndex*> path{tip,tip->pprev,disk.parent,disk.parent->pprev,disk.parent->pprev->pprev};
    const auto head=f.Head();const auto selected=disk.reopened.getOrchardState();ASSERT_TRUE(selected.ok());
    auto owner=OrchardOutgoingHandoffTestAccess::Capture(*f.service,path,fork);ASSERT_TRUE(owner);
    EXPECT_THROW((void)owner->parent->HistoricalRange(),std::exception);
    ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    const auto& range=owner->parent->HistoricalRange();EXPECT_EQ(range.First(),98u);EXPECT_EQ(range.Last(),100u);
    for(uint32_t height=98;height<=100;++height) {
        auto* node=disk.parent;while(node->height>height)node=node->pprev;
        HistoricalCompactReplay proof({height,node->hash,ChainworkFromHex(node->chainwork)},
            {SelectedParentReplayWorkLimits().blocks,SelectedParentReplayWorkLimits().serialized_bytes});
        OrchardHistoryCapture history(height,node->hash);
        std::vector<Block> blocks(disk.f.blocks.begin(),disk.f.blocks.begin()+height+1);
        CaptureHeaders(history,blocks);arith_uint256 work{0};
        for(uint32_t h=0;h<=height;++h) {
            work+=GetBlockProof(blocks[h].header.difficulty);proof.Append(blocks[h],h,work);history.RecordBody(h,blocks[h]);
        }
        proof.Finish();history.Finish();
        const auto independent=HistoricalCatalogTestAccess::Create(disk.reopened,disk.f.token,proof,history);
        EXPECT_EQ(range.At(height).Encode(),independent->State().Encode());
    }
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_EQ(owner->parent->HistoricalCatalog().State().height,98u);
    EXPECT_EQ(owner->parent->HistoricalPrefix().ValidatedTarget().height,98u);
    EXPECT_EQ(f.service->GetActiveTip(),tip);EXPECT_EQ(*disk.reopened.getOrchardState(),*selected);
    EXPECT_EQ(f.Head(),head);EXPECT_EQ(f.notices->recorded.event_count,0u);
}
TEST(OrchardHistoricalController, HistoricalOnlyPreparationNeedsNoFutureBoundary) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* last=f.service->GetActiveTip();
    auto* first=last->pprev->pprev;ASSERT_EQ(first->height,98u);
    const auto before=*disk.reopened.getHistoricalCompactCatalogState(last->hash);
    const auto head=f.storage->Head();
    auto owner=HistoricalBranchPreparationTestAccess::Capture(*f.service,last,first);ASSERT_TRUE(owner);
    EXPECT_THROW((void)owner->Range(),std::exception);
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Complete(*f.service,*owner));
    }
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Complete(*f.service,*owner));
    EXPECT_EQ(owner->Range().First(),98u);EXPECT_EQ(owner->Range().Last(),100u);
    EXPECT_EQ(owner->Range().At(100).Encode(),before);
    EXPECT_TRUE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Complete(*f.service,*owner));
    EXPECT_TRUE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));
    // Neither the independently replayed target nor its checkpoints include
    // the future activation parent at101. Its live index is irrelevant here.
    const auto future_nonce=disk.parent->nonce;disk.parent->nonce^=1;
    EXPECT_TRUE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));disk.parent->nonce=future_nonce;
    EXPECT_EQ(f.service->GetActiveTip(),last);EXPECT_FALSE(f.service->IsStarted());
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(last->hash),before);
    EXPECT_EQ(disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_EQ(disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_EQ(f.storage->Head(),head);
}
TEST(OrchardHistoricalController, HistoricalOnlyPreparationRebindsBeforeAnyEffects) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* last=f.service->GetActiveTip();
    auto* first=last->pprev->pprev;
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Capture(*f.service,first,last));
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Capture(*f.service,disk.parent,first));
    auto owner=HistoricalBranchPreparationTestAccess::Capture(*f.service,last,first);ASSERT_TRUE(owner);
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Complete(*f.service,*owner));
    const auto head=f.storage->Head();const auto before=*disk.reopened.getHistoricalCompactCatalogState(last->hash);
    const auto old_nonce=first->nonce;first->nonce^=1;
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));first->nonce=old_nonce;
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first->pprev));
    const auto validated=disk.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,first->hash,first->height),Status::Ok);
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,validated->hash,validated->height),Status::Ok);
    EXPECT_TRUE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*owner,last,first));
    EXPECT_EQ(f.service->GetActiveTip(),last);EXPECT_EQ(f.storage->Head(),head);
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(last->hash),before);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardHistoricalController, HistoricalServiceEdgesPublishOnlyAfterPreparedConsumers) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* high=f.service->GetActiveTip();
    auto* middle=high->pprev;auto* low=middle->pprev;const auto original=f.storage->Head();
    auto proof=HistoricalBranchPreparationTestAccess::Capture(*f.service,high,low);ASSERT_TRUE(proof);
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Complete(*f.service,*proof));
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Bind(*f.service,*proof,high,low));
    struct Notices final:RuntimeBlockNotifications {
        TypedForkNotices recorded;bool refuse=true;
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t h,RuntimeBlockDirection d)override {
            if(refuse)return {};return PrepareCompactFixtureNotice(recorded,body,h,d);
        }
    };
    auto notices=std::make_shared<Notices>();f.service->setRuntimeBlockNotifications(notices);std::string error;
    const auto rows=disk.Rows();
    EXPECT_FALSE(HistoricalBranchPreparationTestAccess::Apply(*f.service,high,*proof,false,error));
    EXPECT_FALSE(error.empty());EXPECT_EQ(disk.Rows(),rows);EXPECT_EQ(f.storage->Head(),original);
    EXPECT_EQ(f.service->GetActiveTip(),high);EXPECT_EQ(notices->recorded.event_count,0u);
    notices->refuse=false;
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Apply(*f.service,high,*proof,false,error))<<error;
    EXPECT_EQ(f.service->GetActiveTip(),middle);EXPECT_EQ(disk.reopened.getTip()->hash,middle->hash);
    EXPECT_EQ(f.storage->Head().sequence,original.sequence+1);EXPECT_EQ(notices->recorded.event_count,1u);
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Apply(*f.service,middle,*proof,false,error))<<error;
    EXPECT_EQ(f.service->GetActiveTip(),low);EXPECT_EQ(disk.reopened.getTip()->hash,low->hash);
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(low->hash),proof->Range().At(low->height).Encode());
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Apply(*f.service,middle,*proof,true,error))<<error;
    ASSERT_TRUE(HistoricalBranchPreparationTestAccess::Apply(*f.service,high,*proof,true,error))<<error;
    EXPECT_EQ(f.service->GetActiveTip(),high);EXPECT_EQ(disk.reopened.getTip()->hash,high->hash);
    EXPECT_EQ(f.storage->Head().sequence,original.sequence+4);ASSERT_EQ(notices->recorded.event_count,4u);
    EXPECT_EQ(notices->recorded.events[0].direction,RuntimeBlockDirection::Disconnect);
    EXPECT_EQ(notices->recorded.events[3].direction,RuntimeBlockDirection::Connect);
    EXPECT_EQ(disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_EQ(disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);EXPECT_FALSE(f.service->IsStarted());
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardHistoricalController, HistoricalOnlyOutgoingPreparationPreservesSelectedOwner) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* tip=f.service->GetActiveTip();
    auto* fork=tip->pprev->pprev;const std::vector<CBlockIndex*> path{tip,tip->pprev};
    const auto head=f.storage->Head();const auto selected=*disk.reopened.getHistoricalCompactCatalogState(tip->hash);
    auto owner=OrchardOutgoingHandoffTestAccess::Capture(*f.service,path,fork);ASSERT_TRUE(owner);
    EXPECT_EQ(owner->parent,nullptr);ASSERT_TRUE(owner->historical);
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(*f.service,*owner));
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,{tip},fork));
    const auto nonce=tip->pprev->nonce;tip->pprev->nonce^=1;
    EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));tip->pprev->nonce=nonce;
    EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(*f.service,*owner,path,fork));
    EXPECT_EQ(owner->historical->Range().At(tip->height).Encode(),selected);
    EXPECT_EQ(f.service->GetActiveTip(),tip);EXPECT_EQ(f.storage->Head(),head);
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(tip->hash),selected);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardCompactTransition, HistoricalOnlyInvalidationRetainsTypedIntentAndReactivates) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* tip=f.service->GetActiveTip();
    auto* target=tip->pprev;auto* fork=target->pprev;const auto first=tip->hash,second=target->hash;
    const auto before=f.storage->Head();auto notices=std::make_shared<CompactTransitionNotices>();
    f.service->setRuntimeBlockNotifications(notices);std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(target->hash,error))<<error;
    ASSERT_EQ(f.service->GetActiveTip(),fork);EXPECT_EQ(disk.reopened.getTip()->hash,fork->hash);
    EXPECT_EQ(f.storage->Head().sequence,before.sequence+2);
    ASSERT_EQ(notices->recorded.plan_count,1u);ASSERT_EQ(notices->recorded.event_count,2u);
    EXPECT_EQ(notices->recorded.events[0].hash,first);EXPECT_EQ(notices->recorded.events[1].hash,second);
    EXPECT_TRUE(notices->recorded.progress[0].complete);EXPECT_EQ(notices->recorded.progress[0].disconnected,2u);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto intent=ReadRuntimeReorgIntentUnderLock(disk.reopened);ASSERT_TRUE(intent);
        ASSERT_EQ(intent->plan->disconnect.size(),2u);EXPECT_TRUE(intent->plan->connect.empty());
        EXPECT_FALSE(intent->plan->disconnect[0].body.IsOrchardProfile());
        EXPECT_FALSE(intent->plan->disconnect[1].body.IsOrchardProfile());
        EXPECT_EQ(intent->outbox_origin,before);
        std::string raw;ASSERT_EQ(disk.reopened.getRaw("runtime_reorg_intent:v1:record:0000000000000001",raw),Status::Ok);
        EXPECT_EQ(raw.substr(0,6),"DNRI02");
    }
    ASSERT_TRUE(f.service->ReconsiderBlock(second,error))<<error;f.service->ActivateBestChain();
    ASSERT_NE(f.service->GetActiveTip(),nullptr);EXPECT_EQ(f.service->GetActiveTip()->hash,disk.second->Header().GetHash());
    EXPECT_EQ(f.service->GetActiveTip()->height,103u);
    EXPECT_TRUE(disk.reopened.getOrchardState().ok());EXPECT_TRUE(disk.reopened.getLegacyRetirementState().ok());
    EXPECT_EQ(f.storage->Head().sequence,before.sequence+7);EXPECT_EQ(notices->recorded.event_count,7u);
    EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);EXPECT_FALSE(f.service->IsStarted());
}
TEST(OrchardHistoricalController, HistoricalOnlyIntentRequiresCompactSelection) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;auto* tip=f.service->GetActiveTip();
    const std::vector<CBlockIndex*> path{tip};const auto head=f.storage->Head();
    auto selected=f.service->AcquireBlockIngressActivationLock();
    // This case tests the durable intent store through the generic strict
    // reader. Supply its exact durable index representation; reconstructed live
    // levels are handled only by the service with a completed replay owner.
    const auto metadata=disk.reopened.getHeaderMetadata(tip->hash);ASSERT_TRUE(metadata.ok());
    EXPECT_EQ(tip->status,metadata->status_flags|BLOCK_VALID_MASK);
    CBlockIndex durable_view=*tip;durable_view.status=metadata->status_flags;
    const std::vector<CBlockIndex*> durable_path{&durable_view};
    const auto plan=ReadRuntimeReorgPlanUnderLock(disk.reopened,disk.files.get(),durable_path,{});ASSERT_TRUE(plan);
    durable_view.status^=BLOCK_VALID_HEADER;
    EXPECT_FALSE(ReadRuntimeReorgPlanUnderLock(disk.reopened,disk.files.get(),durable_path,{}));
    durable_view.status=metadata->status_flags;
    const auto binding=storage::ReadOrchardCompactStorageBinding(disk.reopened);ASSERT_TRUE(binding);
    rocksdb::WriteBatch remove;remove.Delete(storage::OrchardCompactStorageKey);
    ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(remove),true),Status::Ok);
    EXPECT_THROW((void)PersistRuntimeReorgIntentUnderLock(disk.reopened,disk.f.token,*plan),std::exception);
    EXPECT_FALSE(ReadRuntimeReorgIntentUnderLock(disk.reopened));
    rocksdb::WriteBatch restore;restore.Put(storage::OrchardCompactStorageKey,*binding);
    ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(restore),true),Status::Ok);
    const auto cursor=PersistRuntimeReorgIntentUnderLock(disk.reopened,disk.f.token,*plan);
    const auto intent=ReadRuntimeReorgIntentUnderLock(disk.reopened);ASSERT_TRUE(intent);EXPECT_EQ(intent->cursor,cursor);
    EXPECT_EQ(intent->outbox_origin,head);EXPECT_EQ(disk.reopened.getTip()->hash,tip->hash);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardCompactTransition, HistoricalCommittedPrefixReopensAndRetriesRemainingEdge) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;
    const auto high=f.service->GetActiveTip()->hash;
    const auto middle=f.service->GetActiveTip()->pprev->hash;
    const auto low=f.service->GetActiveTip()->pprev->pprev->hash;
    const auto before=f.storage->Head();const auto archives=disk.ArchiveBytes();
    struct Notices final:RuntimeBlockNotifications {
        TypedForkNotices recorded;std::optional<uint256> refuse;
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
            const RuntimeBlockBody& body,uint32_t h,RuntimeBlockDirection direction)override {
            if(direction==RuntimeBlockDirection::Disconnect && !body.IsOrchardProfile() &&
                refuse && body.Historical().GetHash()==*refuse)return {};
            return PrepareCompactFixtureNotice(recorded,body,h,direction);
        }
        std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
            std::shared_ptr<const RuntimeReorgPlan> plan)override {
            return recorded.PrepareReorg(std::move(plan));
        }
    };
    auto notices=std::make_shared<Notices>();notices->refuse=middle;
    f.service->setRuntimeBlockNotifications(notices);std::string error;
    ASSERT_FALSE(f.service->InvalidateBlock(middle,error));EXPECT_FALSE(error.empty());
    ASSERT_EQ(f.service->GetActiveTip()->hash,middle);
    EXPECT_EQ(disk.reopened.getTip()->hash,middle);
    EXPECT_EQ(disk.reopened.getValidatedTip()->hash,middle);
    EXPECT_EQ(f.storage->Head().sequence,before.sequence+1);
    ASSERT_EQ(notices->recorded.plan_count,1u);ASSERT_EQ(notices->recorded.event_count,1u);
    EXPECT_EQ(notices->recorded.events[0].hash,high);
    EXPECT_FALSE(notices->recorded.progress[0].complete);
    EXPECT_EQ(notices->recorded.progress[0].disconnected,1u);
    EXPECT_EQ(FindBlockIndex(middle)->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    std::string retained;
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto intent=ReadRuntimeReorgIntentUnderLock(disk.reopened);ASSERT_TRUE(intent);
        ASSERT_EQ(intent->plan->disconnect.size(),2u);
        EXPECT_EQ(intent->plan->disconnect[0].hash,high);
        EXPECT_EQ(intent->plan->disconnect[1].hash,middle);
        EXPECT_EQ(intent->outbox_origin,before);
        ASSERT_EQ(disk.reopened.getRaw("runtime_reorg_intent:v1:record:0000000000000001",retained),Status::Ok);
        EXPECT_EQ(retained.substr(0,6),"DNRI02");
    }
    const auto prefix=*disk.reopened.getHistoricalCompactCatalogState(middle);
    const auto archives_before_reopen=disk.ArchiveBytes();
    // Destroy the original service and DB connection. The actual startup audit
    // must recover the committed historical prefix before retrying its suffix.
    f.service->Stop();f.context.chainstate.reset();f.service.reset();disk.reopened.close();
    ASSERT_EQ(disk.reopened.init(disk.f.path),Status::Ok);
    ConfigureCompactServiceContext(f.context,disk);
    f.service=std::make_shared<ChainstateService>();f.context.chainstate=f.service;
    f.service->setOwnedChainDB(std::shared_ptr<ChainDB>(f.storage,&disk.reopened));
    ASSERT_TRUE(f.service->Init(f.context));
    f.service->setHeaderChainSelector(CompactBindingSelector(disk));
    notices->refuse.reset();f.service->setRuntimeBlockNotifications(notices);
    ASSERT_NO_THROW(OrchardCompactTransitionTestAccess::SelectAuditedComponent(*f.service));
    ASSERT_EQ(f.service->GetActiveTip()->hash,middle);
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(middle),prefix);
    EXPECT_EQ(disk.ArchiveBytes(),archives_before_reopen);
    EXPECT_EQ(f.storage->Head().sequence,before.sequence+1);
    ASSERT_TRUE(f.service->InvalidateBlock(middle,error))<<error;
    ASSERT_EQ(f.service->GetActiveTip()->hash,low);
    EXPECT_EQ(disk.reopened.getTip()->hash,low);EXPECT_EQ(disk.reopened.getValidatedTip()->hash,low);
    EXPECT_EQ(f.storage->Head().sequence,before.sequence+2);
    ASSERT_EQ(notices->recorded.plan_count,2u);ASSERT_EQ(notices->recorded.event_count,2u);
    EXPECT_EQ(notices->recorded.events[1].hash,middle);
    EXPECT_TRUE(notices->recorded.progress[1].complete);
    EXPECT_EQ(notices->recorded.progress[1].disconnected,1u);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();std::string after;
        ASSERT_EQ(disk.reopened.getRaw("runtime_reorg_intent:v1:record:0000000000000001",after),Status::Ok);
        EXPECT_EQ(after,retained);
    }
    // Preparation appends immutable undo before notification can refuse.
    // Preserve every original archive byte, and accept exactly the committed
    // high record, the abandoned middle record, and the committed middle retry.
    // Reopening above must append nothing. No extra or altered record is allowed.
    const auto high_meta=disk.reopened.getHeaderMetadata(high);ASSERT_TRUE(high_meta.ok());
    const auto middle_meta=disk.reopened.getHeaderMetadata(middle);ASSERT_TRUE(middle_meta.ok());
    ASSERT_EQ(high_meta->undo_file,middle_meta->undo_file);
    auto undo_name=disk.files->getBlockFilePath(high_meta->undo_file).filename().string();
    undo_name.replace(0,3,"rev");
    ASSERT_TRUE(archives.contains(undo_name));
    const auto final_archives=disk.ArchiveBytes();ASSERT_EQ(final_archives.size(),archives.size());
    for(const auto& [name,bytes]:archives) {
        ASSERT_TRUE(final_archives.contains(name));
        if(name!=undo_name)EXPECT_EQ(final_archives.at(name),bytes);
        else {ASSERT_GE(final_archives.at(name).size(),bytes.size());
            EXPECT_EQ(final_archives.at(name).substr(0,bytes.size()),bytes);}
    }
    const auto high_undo=disk.reopened.getUndo(high);ASSERT_TRUE(high_undo.ok());
    const auto middle_undo=disk.reopened.getUndo(middle);ASSERT_TRUE(middle_undo.ok());
    const std::vector<std::vector<uint8_t>> records{
        high_undo->Serialize(),middle_undo->Serialize(),middle_undo->Serialize()};
    uint64_t offset=archives.at(undo_name).size();
    EXPECT_EQ(high_meta->undo_pos,offset);EXPECT_EQ(high_meta->undo_size,records.front().size());
    for(size_t i=0;i<records.size();++i) {
        if(i==2){EXPECT_EQ(middle_meta->undo_pos,offset);EXPECT_EQ(middle_meta->undo_size,records[i].size());}
        const auto record=disk.files->readUndo({high_meta->undo_file,offset,uint32_t(records[i].size())});
        ASSERT_TRUE(record.ok());EXPECT_EQ(*record,records[i]);offset+=records[i].size()+8;
    }
    EXPECT_EQ(final_archives.at(undo_name).size(),offset);
    EXPECT_EQ(disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_EQ(disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);EXPECT_FALSE(f.service->IsStarted());
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardHistoricalIndexOwner, ReconstructedValidationRetainsExactDurableAndLiveOwners) {
    HistoricalRangeWriterFixture f;auto& db=f.disk.reopened;
    auto* high=f.Index(101);auto* low=high->pprev;
    const auto durable=db.getHeaderMetadata(high->hash);ASSERT_TRUE(durable.ok());
    const auto low_durable=db.getHeaderMetadata(low->hash);ASSERT_TRUE(low_durable.ok());
    // These are the same live validation levels produced by the independently
    // replayed startup audit. The real range and boundary proofs remain required.
    high->status=durable->status_flags|BLOCK_VALID_MASK;
    low->status=low_durable->status_flags|BLOCK_VALID_MASK;
    const auto live_status=high->status,low_status=low->status;
    const auto rows=f.disk.Rows();const auto head=f.Head();
    high->status|=BLOCK_FAILED_VALID;
    EXPECT_THROW((void)f.PrepareAt(101,false),std::exception);
    high->status=live_status;
    const auto position=high->data_pos;high->data_pos^=1;
    EXPECT_THROW((void)f.PrepareAt(101,false),std::exception);
    high->data_pos=position;
    {
        auto writer=f.PrepareAt(101,false);
        high->status^=BLOCK_VALID_HEADER;
        EXPECT_THROW(writer->Commit(),std::exception);
        high->status=live_status;
    }
    {
        auto writer=f.PrepareAt(101,false);
        low->status^=BLOCK_VALID_HEADER;
        EXPECT_THROW(writer->Commit(),std::exception);
        low->status=low_status;
    }
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.Head(),head);
    {
        auto writer=f.PrepareAt(101,false);writer->Commit();
    }
    const auto after=db.getHeaderMetadata(high->hash);ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->status_flags&BLOCK_VALID_MASK,durable->status_flags&BLOCK_VALID_MASK);
    EXPECT_EQ(high->status,after->status_flags|BLOCK_VALID_MASK);
    EXPECT_EQ(low->status,low_status);
    EXPECT_EQ(db.getTip()->hash,low->hash);EXPECT_EQ(f.Head().sequence,head.sequence+1);
    {
        auto writer=f.PrepareAt(101,true);writer->Commit();
    }
    EXPECT_EQ(db.getTip()->hash,high->hash);EXPECT_EQ(f.Head().sequence,head.sequence+2);
    EXPECT_EQ(high->status,db.getHeaderMetadata(high->hash)->status_flags|BLOCK_VALID_MASK);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
void CheckHistoricalSpendTransition(OrchardAdmissionFixture::HistoricalSpend mode) {
    CompactTransitionFixture f({}, {},mode);auto& disk=*f.storage;
    const auto& spend_block=disk.f.blocks.at(101);
    const bool child=mode==OrchardAdmissionFixture::HistoricalSpend::SameBlockChild;
    ASSERT_EQ(spend_block.vtx.size(),child?3u:2u);
    std::string witness_error;
    EXPECT_TRUE(consensus::ValidateWitnessCommitment(spend_block.vtx,witness_error))<<witness_error;
    const auto funding=OutPoint(disk.f.blocks.at(1).vtx.front().GetTxid(),0);
    const auto parent_spend=spend_block.vtx.at(1).GetTxid().AsUint256();
    EXPECT_EQ(spend_block.vtx.at(1).vin.front().prevout.txid.AsUint256(),funding.txid.AsUint256());
    EXPECT_EQ(spend_block.vtx.at(1).vin.front().witness.front().size(),64u);
    if(child) {
        EXPECT_EQ(spend_block.vtx.at(2).vin.front().prevout.txid.AsUint256(),parent_spend);
        EXPECT_EQ(spend_block.vtx.at(2).vin.front().witness.front().size(),64u);
    }
    EXPECT_EQ(disk.independent->ProvenState().ProvenUtxos().count(funding),0u);
    const auto final_point=OutPoint(spend_block.vtx.back().GetTxid(),0);
    EXPECT_EQ(disk.independent->ProvenState().ProvenUtxos().count(final_point),1u);
    if(child)EXPECT_EQ(disk.independent->ProvenState().ProvenUtxos().count(OutPoint(spend_block.vtx.at(1).GetTxid(),0)),0u);
    const auto original=*disk.reopened.getOrchardCatalogState(disk.second->Header().GetHash());
    const auto original_state=*disk.reopened.getOrchardState();
    const auto head=f.Head();std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(spend_block.GetHash(),error))<<error;
    ASSERT_EQ(f.service->GetActiveTip()->height,100u);
    // Historical undo retains every input, including the same-block parent
    // output. Its fields must match the actual signed body, not a fabricated
    // durable coin. The ephemeral output must not appear in the proven UTXOs.
    const auto undo=disk.reopened.getUndo(spend_block.GetHash());ASSERT_TRUE(undo.ok());
    ASSERT_EQ(undo->spent.size(),child?2u:1u);
    EXPECT_EQ(undo->spent.front().prev_txid,funding.txid.AsUint256());
    EXPECT_EQ(undo->spent.front().height,1u);EXPECT_TRUE(undo->spent.front().is_coinbase);
    if(child) {
        const auto& coin=undo->spent.at(1);const auto& output=spend_block.vtx.at(1).vout.front();
        EXPECT_EQ(coin.prev_txid,parent_spend);EXPECT_EQ(coin.prev_vout,0u);
        EXPECT_EQ(coin.value,output.value.GetUna());EXPECT_EQ(coin.scriptPubKey,output.scriptPubKey);
        EXPECT_EQ(coin.height,101u);EXPECT_FALSE(coin.is_coinbase);
    }
    EXPECT_EQ(f.Head().sequence,head.sequence+3);
    ASSERT_EQ(f.notices->recorded.plan_count,1u);
    EXPECT_EQ(f.notices->recorded.progress.at(0).disconnected,3u);
    EXPECT_TRUE(f.notices->recorded.progress.at(0).complete);
    ASSERT_EQ(f.notices->recorded.event_count,3u);
    EXPECT_EQ(f.notices->recorded.events.at(2).hash,spend_block.GetHash());
    EXPECT_EQ(disk.reopened.getTxLocation(parent_spend).status(),Status::NotFound);
    if(child)EXPECT_EQ(disk.reopened.getTxLocation(spend_block.vtx.at(2).GetTxid().AsUint256()).status(),Status::NotFound);
    // Independent full replay restores the actual mature funding coin. Compare
    // its entire forest commitment to the compact historical catalog and tip.
    assumeutxo::AssumeUtxoReplayEngine reference;
    ASSERT_TRUE(reference.SeedGenesis(disk.f.blocks.front(),error))<<error;
    for(uint32_t h=1;h<=100;++h)
        ASSERT_TRUE(reference.ConnectAndAdvance(disk.f.blocks[h],h,disk.f.blocks[h].GetHash(),error))<<error;
    ASSERT_EQ(reference.ProvenUtxos().count(funding),1u);
    EXPECT_EQ(reference.ProvenUtxos().at(funding).height,1u);
    const auto raw=disk.reopened.getHistoricalCompactCatalogState(disk.f.blocks[100].GetHash());ASSERT_TRUE(raw.ok());
    const auto historical=storage::catalog::HistoricalState::Decode(*raw);
    const auto stump=consensus::UtreexoStump::deserialize(historical.stump);
    const auto root=stump.getCommitment();
    ASSERT_EQ(root.size(),32u);uint256 compact_root;std::copy(root.begin(),root.end(),compact_root.begin());
    EXPECT_EQ(compact_root,uint256::FromHexUnsafe(reference.UtreexoRootHex()));
    EXPECT_EQ(disk.reopened.getTip()->hash,disk.f.blocks[100].GetHash());
    EXPECT_EQ(disk.reopened.getValidatedTip()->hash,disk.f.blocks[100].GetHash());
    size_t stored_coins=0;
    ASSERT_EQ(disk.reopened.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++stored_coins;return true;}),Status::Ok);
    EXPECT_EQ(stored_coins,0u);
    const auto before_reopen=disk.Rows();ASSERT_NO_THROW(f.Reopen());
    ASSERT_EQ(f.service->GetActiveTip()->height,100u);EXPECT_EQ(disk.Rows(),before_reopen);
    EXPECT_EQ(*disk.reopened.getHistoricalCompactCatalogState(disk.f.blocks[100].GetHash()),*raw);
    ASSERT_TRUE(f.service->ReconsiderBlock(spend_block.GetHash(),error))<<error;
    f.service->ActivateBestChain();
    ASSERT_EQ(f.service->GetActiveTip()->hash,disk.second->Header().GetHash());
    EXPECT_EQ(*disk.reopened.getOrchardCatalogState(disk.second->Header().GetHash()),original);
    EXPECT_EQ(*disk.reopened.getOrchardState(),original_state);
    const auto restored=disk.reopened.getTxLocation(parent_spend);ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored->first,spend_block.GetHash());EXPECT_EQ(restored->second,1u);
    if(child) {
        const auto restored_child=disk.reopened.getTxLocation(spend_block.vtx.at(2).GetTxid().AsUint256());
        ASSERT_TRUE(restored_child.ok());EXPECT_EQ(restored_child->first,spend_block.GetHash());EXPECT_EQ(restored_child->second,2u);
    }
    EXPECT_EQ(f.Head().sequence,head.sequence+6);EXPECT_EQ(f.notices->recorded.event_count,6u);
    EXPECT_FALSE(f.service->IsStarted());EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);
}
}
TEST(OrchardHistoricalSpendTransition, MatureFundingRollbackReopenAndReconnect) {
    CheckHistoricalSpendTransition(OrchardAdmissionFixture::HistoricalSpend::MatureCoin);
}
TEST(OrchardHistoricalSpendTransition, SameBlockChildRollbackReopenAndReconnect) {
    CheckHistoricalSpendTransition(OrchardAdmissionFixture::HistoricalSpend::SameBlockChild);
}
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardHistoricalController, HistoricalDeliveryPageBindsSelectedCatalog) {
    HistoricalPreparationFixture f;auto& disk=f.storage->disk;
    auto* tip=f.service->GetActiveTip();ASSERT_NE(tip,nullptr);ASSERT_EQ(tip->height,100u);
    const auto rows=disk.Rows();const auto head=f.storage->Head();
    const auto page=f.service->getRuntimeDeliveryPage({},1);ASSERT_TRUE(page.ok());
    EXPECT_EQ((*page)->head,head);
    EXPECT_TRUE(f.service->VerifyConsensusJournalAtActiveTip());
    const auto nonce=tip->nonce;tip->nonce^=1;
    EXPECT_FALSE(f.service->getRuntimeDeliveryPage({},1).ok());tip->nonce=nonce;
    const auto validated=disk.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,tip->pprev->hash,tip->pprev->height),Status::Ok);
    EXPECT_FALSE(f.service->getRuntimeDeliveryPage({},1).ok());
    ASSERT_EQ(disk.reopened.setValidatedTip(disk.f.token,validated->hash,validated->height),Status::Ok);
    const auto binding=storage::ReadOrchardCompactStorageBinding(disk.reopened);ASSERT_TRUE(binding);
    rocksdb::WriteBatch remove;remove.Delete(storage::OrchardCompactStorageKey);
    ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(remove),true),Status::Ok);
    EXPECT_FALSE(f.service->getRuntimeDeliveryPage({},1).ok());
    rocksdb::WriteBatch restore;restore.Put(storage::OrchardCompactStorageKey,*binding);
    ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(restore),true),Status::Ok);
    ASSERT_TRUE(f.service->getRuntimeDeliveryPage({},1).ok());
    EXPECT_TRUE(f.service->VerifyConsensusJournalAtActiveTip());
    EXPECT_EQ(disk.Rows(),rows);EXPECT_EQ(f.storage->Head(),head);EXPECT_EQ(f.service->GetActiveTip(),tip);
    EXPECT_EQ(disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_EQ(disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);EXPECT_FALSE(f.service->IsStarted());
}
}
#endif
