#pragma once
#include "daemon/orchard_snapshot_store.h"
#include "daemon/services/block_ingress_service.h"
#include "consensus/validation_queue.h"
#include <sstream>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct OrchardSnapshotStoreTestAccess {
    static void CreateFresh(const std::filesystem::path& path){OrchardSnapshotStore::CreateFreshStore(path);}
    static auto Capture(const ChainDB& db,consensus::OrchardSnapshotProfile profile,const std::vector<uint8_t>& accumulator) {
        return OrchardSnapshotStore::Capture(db,profile,accumulator);
    }
    static auto Install(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,BlockStorage& files,
        const consensus::HeaderChainSelector& headers,const consensus::OrchardSnapshotFile& file) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::Install(mutex,db,token,files,headers,file);
    }
    static auto Restore(AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::Restore(mutex,db,headers);
    }
    static bool Recover(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
        const consensus::HeaderChainSelector& headers) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::RecoverBaseMarkers(mutex,db,token,headers);
    }
};
TEST(OrchardSnapshotRuntime, FullNodeRejectsBadDnorst02WithCompactBootstrapOff) {
    using namespace consensus;
    CanonicalPoolFixture fixture(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    const auto config=ShieldedStateStartupTestAccess::SnapshotBootstrapConfig(*fixture.f.service);
    ASSERT_TRUE(config->CompactSnapshotBootstrapEnabled());config->Set("assumeutxo_compact_bootstrap","0");
    ASSERT_FALSE(GetConfig().utreexo_stateless);ASSERT_NE(fixture.f.service->GetConsensusUTXOSet(),nullptr);
    const auto honest=fixture.Build();ASSERT_TRUE(honest);ASSERT_LT(honest->Transactions().size(),253u);
    auto cb=honest->Transactions().front().Historical();
    const auto original=FindStateCommitment(cb,StateCommitmentEncoding::Orchard);
    ASSERT_EQ(original.status,StateCommitmentStatus::Ok);
    bool changed=false;
    for(auto& output:cb.vout) {
        auto& script=output.scriptPubKey;
        if(script.size()==39&&script[0]==0x6a&&script[2]=='D'&&script[3]=='N'&&script[4]=='R'&&script[5]=='S') {
            script.back()^=1;changed=true;
        }
    }
    ASSERT_TRUE(changed);ASSERT_NE(FindStateCommitment(cb,StateCommitmentEncoding::Orchard).root,original.root);
    auto bytes=honest->WireBytes();const auto wire=cb.Serialize(TxSerializationMode::WithWitness);
    ASSERT_EQ(wire.size(),honest->Transactions().front().Historical().Serialize(TxSerializationMode::WithWitness).size());
    ASSERT_EQ(bytes.at(128),honest->Transactions().size());std::copy(wire.begin(),wire.end(),bytes.begin()+129);
    auto header=honest->Header();std::vector<TxId> ids{cb.GetTxid()};
    for(size_t i=1;i<honest->Transactions().size();++i)ids.push_back(honest->Transactions()[i].GetTxid());
    header.merkle_root=ComputeTransactionMerkleRoot(ids);
    const auto encode=[&] {const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());};
    encode();auto draft=OrchardBlockCandidate::DecodeExact(bytes);
    FirstBoundaryView view(*fixture.f.replay);
    const auto context=SelectedOrchardBlockContext(draft.Header(),honest->Height());ASSERT_TRUE(context);
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,*context,view,{},true);
    const auto& parent=fixture.f.blocks.back().header;const auto& forest=*fixture.f.replay->Forest();
    header.utreexo_root=PrepareOrchardForestTransition(coins,parent,forest).Root();
    header=OrchardAdmissionFixture::SolveHeader(header);encode();
    const auto forged=OrchardBlockCandidate::DecodeExact(bytes);
    const auto final_context=SelectedOrchardBlockContext(header,honest->Height());ASSERT_TRUE(final_context);
    const auto final_coins=PrepareOrchardBlockCoinsUnderChainstateLock(forged,*final_context,view,{},true);
    std::string error;ASSERT_TRUE(forged.CheckIdentityCommitments(true,error))<<error;
    ASSERT_NO_THROW(CheckOrchardBlockUtreexoProof(forged,final_coins,parent,forest));
    ASSERT_TRUE(PrepareOrchardForestTransition(final_coins,parent,forest).MatchesHeader(header));
    const auto refused=fixture.Submit(bytes);EXPECT_FALSE(refused.connected);
    EXPECT_EQ(refused.code,BlockRejectCode::INVALID_TRANSACTION);
    EXPECT_EQ(refused.reason,"Orchard state commitment rejected");
    EXPECT_EQ(fixture.f.service->GetActiveTip(),fixture.parent);fixture.f.CheckUnpublished();
    const auto valid=SolveCatalogTemplate(honest);ASSERT_TRUE(valid);
    const auto accepted=fixture.Submit(valid->WireBytes());EXPECT_TRUE(accepted.connected)<<accepted.reason;
    EXPECT_FALSE(config->CompactSnapshotBootstrapEnabled());
}
TEST(OrchardSnapshotRuntime, RealRegtestActivationFullAndCompactImportReopenAndFloor) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);
    const auto accepted=source.Submit(first->WireBytes());ASSERT_TRUE(accepted.connected)<<accepted.reason;
    HeaderChainSelector headers;for(const auto& block:source.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    const auto full=Access::Capture(source.f.db,OrchardSnapshotProfile::Full,live.GetForest().serialize());
    ASSERT_GT(full.state.tree_size,0u);ASSERT_GT(full.state.pool_balance,0u);ASSERT_FALSE(full.nullifiers.empty());
    for(const auto profile:{OrchardSnapshotProfile::Full,OrchardSnapshotProfile::Compact}) {
        auto section=full;section.profile=profile;OrchardSnapshotCoins coins;
        if(profile==OrchardSnapshotProfile::Full)coins=live.GetUTXOs();else section.accumulator=section.base_catalog.stump;
        SnapshotMetadata header;header.version=SNAPSHOT_VERSION_V6;header.block_hash=section.state.block_hash;
        header.block_height=section.state.height;header.timestamp=first->Header().timestamp;header.utxo_count=coins.size();
        std::ostringstream output(std::ios::binary);ASSERT_NO_THROW(WriteOrchardSnapshotFile(output,header,section,coins));
        const auto bytes=output.str();std::istringstream input(bytes,std::ios::binary);
        const auto decoded=ReadOrchardSnapshotFile(input,bytes.size(),profile);
        shielded_store_fixture::TempDir directory;
        const auto database_path=directory.path/"chainstate";
        ASSERT_NO_THROW(Access::CreateFresh(database_path));EXPECT_ANY_THROW(Access::CreateFresh(database_path));
        ChainDB db;ASSERT_EQ(db.init(database_path),Status::Ok);BlockStorage files;
        ASSERT_EQ(files.init(directory.path/"files"),Status::Ok);AnnotatedRecursiveMutex mutex;
        const auto stale=source.f.blocks[1].GetHash();ASSERT_EQ(db.putHeightIndex(source.f.token,1,stale),Status::Ok);
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getBlockHashByHeight(1).value(),stale);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
        ASSERT_EQ(db.deleteHeightIndex(source.f.token,1),Status::Ok);
        MutableParams().state_commitment_burial_depth=1;
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getTip().status(),Status::NotFound);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
        MutableParams().state_commitment_burial_depth=0;
        auto installed=Access::Install(mutex,db,source.f.token,files,headers,decoded);
        std::string factory_marker;EXPECT_EQ(db.getRaw(storage::OrchardSnapshotFreshKey,factory_marker),Status::NotFound);
        EXPECT_EQ(bool(installed.full),profile==OrchardSnapshotProfile::Full);
        EXPECT_EQ(bool(installed.compact),profile==OrchardSnapshotProfile::Compact);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        ASSERT_TRUE(db.getHeaderMetadata(section.state.block_hash).ok());
        EXPECT_EQ(db.getHeaderMetadata(section.state.block_hash)->status_flags&(BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS|BLOCK_HAVE_UNDO),0u);
        EXPECT_EQ(db.getOrchardUndoParent(section.state).status(),Status::Corruption);
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getOrchardState()->block_hash,section.state.block_hash);
        installed.full.reset();installed.compact.reset();db.close();ASSERT_EQ(db.init(database_path),Status::Ok);
        auto restored=Access::Restore(mutex,db,headers);
        if(profile==OrchardSnapshotProfile::Full) {
            ASSERT_TRUE(restored.full);EXPECT_EQ(restored.full->GetSetSize(),coins.size());
            EXPECT_EQ(restored.full->GetForest().serialize(),section.accumulator);
            const auto block=OrchardBlockCandidate::DecodeExact(section.base_body);
            EXPECT_ANY_THROW(PreparedOrchardChainstateWrite::Disconnect(mutex,db,source.f.token,*restored.full,
                *SelectedOrchardBlockContext(block.Header(),section.state.height),block,source.f.blocks.back().header,
                restored.full->GetForest(),true,true));
        } else {
            ASSERT_TRUE(restored.compact);std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
            ASSERT_TRUE(restored.compact->SelectedUnderLock(mutex));
            EXPECT_EQ(restored.compact->SelectedUnderLock(mutex)->Encode(),section.base_catalog.Encode());
        }
        // A forged canonical marker cannot enroll an owner after restart.
        const auto original=db.getForestTipMarker();ASSERT_TRUE(original.ok());auto forged=*original;forged.forest_root.data[0]^=1;
        ASSERT_EQ(db.putForestTipMarker(source.f.token,forged),Status::Ok);
        EXPECT_ANY_THROW(Access::Restore(mutex,db,headers));
        ASSERT_EQ(db.putForestTipMarker(source.f.token,*original),Status::Ok);
        ASSERT_NO_THROW(Access::Restore(mutex,db,headers));
        restored.full.reset();restored.compact.reset();
        // Reproduce the marker-above-history-tip gap. Repair may publish only
        // this exact authenticated base, never a plausible marker alone.
        const auto genesis=source.f.blocks.front().GetHash();const auto genesis_work=db.getBlockWork(genesis);ASSERT_TRUE(genesis_work.ok());
        ASSERT_EQ(db.setTip(source.f.token,genesis,0,*genesis_work),Status::Ok);
        ASSERT_EQ(db.setValidatedTip(source.f.token,genesis,0),Status::Ok);
        ASSERT_EQ(db.putForestTipMarker(source.f.token,forged),Status::Ok);
        EXPECT_ANY_THROW(Access::Recover(mutex,db,source.f.token,headers));EXPECT_EQ(db.getTip()->hash,genesis);
        ASSERT_EQ(db.putForestTipMarker(source.f.token,*original),Status::Ok);
        ASSERT_TRUE(Access::Recover(mutex,db,source.f.token,headers));EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        EXPECT_FALSE(Access::Recover(mutex,db,source.f.token,headers));ASSERT_NO_THROW(Access::Restore(mutex,db,headers));
        const auto base=storage::ReadOrchardSnapshotBase(db);ASSERT_TRUE(base);
        const auto key=storage::OrchardSnapshotProofKey(base->proof_digest,0);std::string proof;
        ASSERT_EQ(db.getRaw(key,proof),Status::Ok);auto corrupted=proof;corrupted.back()^=1;
        {rocksdb::WriteBatch batch;batch.Put(key,corrupted);ASSERT_EQ(db.writeBatch(source.f.token,std::move(batch),true),Status::Ok);}
        EXPECT_ANY_THROW(Access::Restore(mutex,db,headers));EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        {rocksdb::WriteBatch batch;batch.Put(key,proof);ASSERT_EQ(db.writeBatch(source.f.token,std::move(batch),true),Status::Ok);}
        ASSERT_NO_THROW(Access::Restore(mutex,db,headers));db.close();
    }
}
TEST(OrchardSnapshotRuntime, ActualServiceBootstrapExportAndRestartBothProfiles) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    OrchardCycleFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{53},0);
    const auto recipient=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{55},0);
    const auto [shield_body,shield_bundle]=source.Shield(receiver);
    ASSERT_TRUE(source.f.ingress->SubmitBody(shield_body,TxOrigin::INTERNAL).accepted());
    BlockAssembler first_assembler(&source.f.db);WireOrchardAssembler(first_assembler,source.f);
    const auto first=SolveCatalogTemplate(first_assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(first);
    ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();
    for(const auto& block:source.f.blocks)ASSERT_TRUE(headers->AddHeader(block.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));source.f.service->setHeaderChainSelector(headers);
    shielded_store_fixture::TempDir exports;
    const auto full_path=exports.path/"full.v6";
    const auto exported=source.f.service->ExportSnapshot(full_path);ASSERT_TRUE(exported.success)<<exported.error_message;
    // A checksum-valid full container whose logical state computes a different
    // DNORST02 root must remain rejected when compact bootstrap is switched off.
    const auto bad_root_path=exports.path/"bad-root-full.v6";
    {
        std::ifstream input(full_path,std::ios::binary);
        auto forged=ReadOrchardSnapshotFile(input,std::filesystem::file_size(full_path),OrchardSnapshotProfile::Full);
        ++forged.orchard.state.pool_balance;
        ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(bad_root_path,forged.metadata,forged.orchard,forged.coins).success);
    }
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    auto compact=Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize());
    SnapshotMetadata metadata;metadata.version=SNAPSHOT_VERSION_V6;metadata.block_hash=first->Header().GetHash();
    metadata.block_height=first->Height();metadata.timestamp=first->Header().timestamp;
    const auto compact_path=exports.path/"compact.v6";
    ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(compact_path,metadata,compact,{}).success);
    source.f.service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
    const auto empty=orchard::OrchardFrontier::Empty();const auto leaves=CycleCommitments(shield_bundle.Authorization());
    const auto frontier=empty.Append(leaves);auto [note,note_index]=CycleReceive(receiver,shield_bundle.Authorization());
    const auto witness=orchard::WalletWitness::ForAppendedLeaf(empty,leaves,note_index);
    const std::vector<orchard::WalletSpendInput> spend{{note,witness}};
    const std::vector<orchard::WalletPayment> payment{{400000,recipient.Receiver(orchard::WalletScope::External,{})}};
    const auto transfer_bundle=orchard::WalletBundlePlan::PrepareSpend(receiver,spend,frontier.Root(),payment)
        .Prove(orchard::SigningContext::Create(source.Domain(),0,{}, {},100000));
    const auto transfer=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{}, {},100000,transfer_bundle.Bytes()));
    const auto spent_nullifier=CycleHash(note.Facts().nullifier);
    ASSERT_TRUE(source.f.ingress->SubmitBody(transfer,TxOrigin::INTERNAL).accepted());
    const OutPoint legacy_coin(source.f.blocks[2].vtx.front().GetTxid(),0);
    ASSERT_TRUE(source.f.ingress->SubmitBody(MempoolTransaction(SelectionSpend(source.f,legacy_coin,
        source.f.replay->ProvenUtxos().at(legacy_coin),100000)),TxOrigin::INTERNAL).accepted());
    const auto& shield=first->Transactions().at(1);const OutPoint base_coin(shield.GetTxid(),0);
    ASSERT_TRUE(source.f.ingress->SubmitBody(MempoolTransaction(SelectionSpend(source.f,base_coin,
        shield.OutputCoin(0,102),200000)),TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);ASSERT_TRUE(headers->AddHeader(second->Header()));
    const auto second_catalog=source.f.db.getOrchardCatalogState(second->Header().GetHash());ASSERT_TRUE(second_catalog.ok());
    const auto full_second=exports.path/"full-second.v6";
    const auto second_export=source.f.service->ExportSnapshot(full_second);ASSERT_TRUE(second_export.success)<<second_export.error_message;
    auto compact_second=Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,UtreexoStump::fromForest(live.GetForest()).serialize());
    auto second_metadata=metadata;second_metadata.block_hash=second->Header().GetHash();second_metadata.block_height=second->Height();
    second_metadata.timestamp=second->Header().timestamp;const auto compact_second_path=exports.path/"compact-second.v6";
    ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(compact_second_path,second_metadata,compact_second,{}).success);
    const auto third=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(third);
    ASSERT_TRUE(source.Submit(third->WireBytes()).connected);ASSERT_TRUE(headers->AddHeader(third->Header()));
    auto sibling_header=second->Header();++sibling_header.timestamp;
    sibling_header=OrchardAdmissionFixture::SolveHeader(sibling_header);auto sibling_bytes=second->WireBytes();
    const auto sibling_prefix=sibling_header.SerializeForHash();std::copy(sibling_prefix.begin(),sibling_prefix.end(),sibling_bytes.begin());
    for(const bool stateless:{false,true}) {
        shielded_store_fixture::TempDir directory;const auto path=directory.path/"chainstate";
        Access::CreateFresh(path);const auto snapshot=stateless?compact_path:full_path;
        RawIngressGraph isolated;
        struct ContextGuard {DaemonContext* prior=DaemonContext::instance();
            ~ContextGuard(){BlockAcceptor::SetContext(prior);DaemonContext::setInstance(prior);}} guard;
        GetConfig().utreexo_stateless=stateless;
        for(unsigned run=0;run<2;++run) {
            // A real executable has no permanent graph from its previous run.
            // Keep the earlier graph alive but detached until this service is
            // destroyed, rather than accidentally borrowing its undo locators.
            std::unique_ptr<RawIngressGraph> cold_graph;
            if(stateless&&run==1)cold_graph=std::make_unique<RawIngressGraph>();
            auto db=std::make_shared<ChainDB>();ASSERT_EQ(db->init(path),Status::Ok);
            DaemonContext context;context.config=std::make_shared<ConfigService>();
            context.config->Set("datadir",directory.path.string());
            EXPECT_TRUE(context.config->CompactSnapshotBootstrapEnabled()); // Default is on.
            if(!stateless)context.config->Set("assumeutxo_compact_bootstrap","0");
            if(stateless&&run==0)context.config->Set("assumeutxo_snapshot",snapshot.string());
            context.logger=std::make_shared<LoggerService>("");
            context.block_storage=std::make_shared<BlockStorage>();ASSERT_EQ(context.block_storage->init(directory.path/"files"),Status::Ok);
            auto service=std::make_shared<ChainstateService>();context.chainstate=service;
            struct StopGuard {std::shared_ptr<ChainstateService> service;DaemonContext& context;
                ~StopGuard(){if(context.validation_queue)context.validation_queue->stop();context.block_ingress=nullptr;
                    if(auto pool=std::dynamic_pointer_cast<MempoolService>(context.mempool))pool->Stop();
                    context.mempool.reset();service->Stop();context.chainstate.reset();}} stop{service,context};
            DaemonContext::setInstance(&context);service->setOwnedChainDB(db);
            ASSERT_TRUE(service->Init(context));auto* original=service->GetConsensusUTXOSet();
            if(stateless)EXPECT_EQ(original,nullptr);
            context.logger_interface=source.f.logger.get();auto pool=std::make_shared<MempoolService>();
            ASSERT_TRUE(pool->Init(context));context.mempool=pool;
            service->setHeaderChainSelector(headers);service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
            if(stateless) {
                service->setValidationMode(ValidationMode::STATEFUL);EXPECT_FALSE(service->Start());
                EXPECT_EQ(service->GetActiveTip(),nullptr);service->setValidationMode(ValidationMode::STATELESS);
            }
            ASSERT_TRUE(service->Start());
            if(!stateless&&run==0) {
                EXPECT_FALSE(context.config->CompactSnapshotBootstrapEnabled());
                const auto refused_root=service->LoadSnapshot(bad_root_path);
                EXPECT_FALSE(refused_root.success);EXPECT_TRUE(refused_root.checksum_valid);
                EXPECT_EQ(refused_root.utxos_imported,0u);
                EXPECT_EQ(db->getTip()->height,0);EXPECT_EQ(service->GetConsensusUTXOSet(),original);
                const auto wrong_profile=service->LoadSnapshot(compact_path);EXPECT_FALSE(wrong_profile.success);
                EXPECT_EQ(db->getTip()->height,0);EXPECT_EQ(service->GetConsensusUTXOSet(),original);
                const auto imported=service->LoadSnapshot(snapshot);ASSERT_TRUE(imported.success)<<imported.error_message;
                EXPECT_EQ(service->GetConsensusUTXOSet(),original);
            }
            const auto& selected=run==0?metadata:second_metadata;
            ASSERT_NE(service->GetActiveTip(),nullptr);EXPECT_EQ(service->GetActiveTip()->hash,selected.block_hash);
            EXPECT_EQ(bool(service->GetConsensusUTXOSet()),!stateless);EXPECT_TRUE(service->IsHealthy());
            // This is the selected-owner read used by NodeCore's accumulator
            // RPC. A compact v6 owner must work without a fallback full forest,
            // including the cold restart with no configured snapshot path.
            const auto accumulator=service->getUtreexoRpcSnapshot();ASSERT_TRUE(accumulator.ok());
            EXPECT_EQ(accumulator->block_hash,selected.block_hash);EXPECT_EQ(accumulator->height,selected.block_height);
            EXPECT_EQ(accumulator->compact,stateless);
            if(!stateless) {
                GetConfig().utreexo_stateless=true;
                EXPECT_FALSE(service->getUtreexoRpcSnapshot().ok()); // no legacy fallback in Orchard
                GetConfig().utreexo_stateless=false;
                EXPECT_TRUE(service->getUtreexoRpcSnapshot().ok());
            }
            {auto lease=ChainstateService::AcquireBlockIngressUse(service);ASSERT_TRUE(lease);}
            const auto copy=directory.path/("export-"+std::to_string(run)+".v6");
            if(stateless) {
                context.config->Set("assumeutxo_compact_bootstrap","0");
                {std::ofstream sentinel(copy);sentinel<<"retain on disabled compact export";}
                const auto refused_export=service->ExportSnapshot(copy);
                EXPECT_FALSE(refused_export.success);
                EXPECT_NE(refused_export.error_message.find("disabled by local policy"),std::string::npos);
                {std::ifstream sentinel(copy);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(sentinel),{}),
                    "retain on disabled compact export");}
                const auto previous=GetConfig().utreexo_stateless;GetConfig().utreexo_stateless=true;
                const auto refused_load=service->LoadSnapshot(compact_path);GetConfig().utreexo_stateless=previous;
                EXPECT_FALSE(refused_load.success);
                EXPECT_NE(refused_load.error_message.find("disabled by local policy"),std::string::npos);
                EXPECT_EQ(service->GetActiveTip()->hash,selected.block_hash);EXPECT_TRUE(service->IsHealthy());
                context.config->Set("assumeutxo_compact_bootstrap","1");
            }
            const auto result=service->ExportSnapshot(copy);ASSERT_TRUE(result.success)<<result.error_message;
            const auto expected_path=run==0?snapshot:(stateless?compact_second_path:full_second);
            std::ifstream expected(expected_path,std::ios::binary),actual(copy,std::ios::binary);
            EXPECT_EQ(std::string(std::istreambuf_iterator<char>(expected),{}),std::string(std::istreambuf_iterator<char>(actual),{}));
            if(run==1) {
                const auto before=db->getHeaderMetadata(metadata.block_hash);ASSERT_TRUE(before.ok());std::string error;
                EXPECT_FALSE(service->InvalidateBlock(metadata.block_hash,error));
                EXPECT_EQ(db->getHeaderMetadata(metadata.block_hash)->status_flags,before->status_flags);
                ASSERT_TRUE(service->InvalidateBlock(second_metadata.block_hash,error))<<error;
                ASSERT_EQ(service->GetActiveTip()->hash,metadata.block_hash);
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).status(),Status::NotFound);
                auto queue=ValidationQueue::CreateCanonicalOnly();context.validation_queue=queue;
                auto ingress=std::make_shared<BlockIngressService>();ASSERT_TRUE(ingress->Init(context));ASSERT_TRUE(ingress->Start());
                context.block_ingress=ingress.get();queue->start();
                const auto forked=ingress->SubmitHex(util::hex(sibling_bytes),BlockOrigin::P2P);
                ASSERT_TRUE(forked.connected)<<forked.reason;ASSERT_EQ(service->GetActiveTip()->hash,sibling_header.GetHash());
                ASSERT_TRUE(service->ReconsiderBlock(second_metadata.block_hash,error))<<error;
                const auto retained=ingress->SubmitHex(util::hex(third->WireBytes()),BlockOrigin::P2P);
                ASSERT_TRUE(retained.retained()||retained.connected)<<retained.reason;
                service->PumpReplayMetadataRecovery();
                ASSERT_EQ(service->GetActiveTip()->hash,third->Header().GetHash());
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).value(),second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardCatalogState(third->Header().GetHash()).value(),
                    source.f.db.getOrchardCatalogState(third->Header().GetHash()).value());
                queue->stop();context.validation_queue.reset();context.block_ingress=nullptr;ingress->Stop();
            }
            if(run==0) {
                auto queue=ValidationQueue::CreateCanonicalOnly();context.validation_queue=queue;
                auto ingress=std::make_shared<BlockIngressService>();ASSERT_TRUE(ingress->Init(context));ASSERT_TRUE(ingress->Start());
                context.block_ingress=ingress.get();queue->start();
                const auto connected=ingress->SubmitHex(util::hex(second->WireBytes()),BlockOrigin::P2P);
                ASSERT_TRUE(connected.connected)<<connected.reason;
                EXPECT_EQ(service->GetActiveTip()->hash,second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).value(),second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardCatalogState(second_metadata.block_hash).value(),*second_catalog);
                if(!stateless){EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(legacy_coin));
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(base_coin));}
                queue->stop();context.validation_queue.reset();context.block_ingress=nullptr;ingress->Stop();
                if(!stateless) {
                    // Supply genuinely mined historical bodies only after the
                    // assumed tip has advanced. Completion must not rewind it
                    // or overwrite the new coins with the snapshot's base set.
                    for(uint32_t height=1;height<source.f.blocks.size();++height) {
                        const auto& block=source.f.blocks[height];const auto hash=block.GetHash();
                        const auto position=context.block_storage->writeBlock(hash,block);ASSERT_TRUE(position.ok());
                        auto row=db->getHeaderMetadata(hash);ASSERT_TRUE(row.ok());
                        row->file_number=position->file_number;row->data_pos=position->offset;row->data_size=position->size;
                        row->status_flags|=BLOCK_HAVE_DATA;
                        ASSERT_EQ(db->putHeaderMetadata(source.f.token,hash,*row),Status::Ok);
                    }
                    for(unsigned attempt=0;attempt<2000&&
                        service->GetBackgroundValidationProgress().status==ChainstateService::BackgroundValidationStatus::InProgress;++attempt)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    const auto progress=service->GetBackgroundValidationProgress();
                    ASSERT_TRUE(service->IsBackgroundValidationComplete())<<progress.error_message;
                    EXPECT_FALSE(service->IsAssumeUTXOActive());EXPECT_EQ(service->GetActiveTip()->hash,second_metadata.block_hash);
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(legacy_coin));
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(base_coin));
                } else EXPECT_FALSE(service->IsBackgroundValidationComplete());
            }
            pool->Stop();context.mempool.reset();pool.reset();
            service->Stop();context.chainstate.reset();service.reset();
        }
    }
}
TEST(OrchardSnapshotRuntime, IndependentLaterRegtestGatePreservesRoot1ThenExportsRoot2) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=103;
    MutableParams().state_commitment_burial_depth=0;
    source.f.service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
    auto headers=std::make_shared<HeaderChainSelector>();
    for(const auto& block:source.f.blocks)ASSERT_TRUE(headers->AddHeader(block.header));
    source.f.service->setHeaderChainSelector(headers);
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    shielded_store_fixture::TempDir directory;const auto path=directory.path/"snapshot.v6";
    {std::ofstream previous(path);previous<<"retain before activation";}
    EXPECT_FALSE(source.f.service->ExportSnapshot(path).success);
    {std::ifstream previous(path);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(previous),{}),"retain before activation");}
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);
    const auto exported=source.f.service->ExportSnapshot(path);ASSERT_TRUE(exported.success)<<exported.error_message;
    std::ifstream input(path,std::ios::binary);const auto file=ReadOrchardSnapshotFile(input,std::filesystem::file_size(path),OrchardSnapshotProfile::Full);
    EXPECT_EQ(file.metadata.block_height,103u);EXPECT_EQ(file.orchard.parent_catalog.block,first->Header().GetHash());
    const auto database=directory.path/"chainstate";Access::CreateFresh(database);ChainDB db;ASSERT_EQ(db.init(database),Status::Ok);
    BlockStorage files;ASSERT_EQ(files.init(directory.path/"files"),Status::Ok);AnnotatedRecursiveMutex mutex;
    const auto installed=Access::Install(mutex,db,source.f.token,files,*headers,file);ASSERT_TRUE(installed.full);
    EXPECT_EQ(installed.full->GetBestBlock(),second->Header().GetHash());
    ASSERT_NO_THROW(Access::Restore(mutex,db,*headers));
}
TEST(OrchardSnapshotRuntime, UnmarkedEmptyReadyStoreCannotSelfInitializeGenesis) {
    shielded_store_fixture::TempDir directory;const auto path=directory.path/"chainstate";
    OrchardSnapshotStoreTestAccess::CreateFresh(path);auto db=std::make_shared<ChainDB>();ASSERT_EQ(db->init(path),Status::Ok);
    const auto token=ChainWriteToken::CreateForTesting();
    {rocksdb::WriteBatch batch;batch.Delete(storage::OrchardSnapshotFreshKey);ASSERT_EQ(db->writeBatch(token,std::move(batch),true),Status::Ok);}
    DaemonContext context;context.config=std::make_shared<ConfigService>();context.config->Set("datadir",directory.path.string());
    context.logger=std::make_shared<LoggerService>("");context.block_storage=std::make_shared<BlockStorage>();
    ASSERT_EQ(context.block_storage->init(directory.path/"files"),Status::Ok);
    auto service=std::make_shared<ChainstateService>();context.chainstate=service;service->setOwnedChainDB(db);
    EXPECT_FALSE(service->Init(context));EXPECT_FALSE(service->IsStarted());EXPECT_EQ(service->GetActiveTip(),nullptr);
    EXPECT_EQ(db->getTip().status(),Status::NotFound);EXPECT_EQ(db->getBlockHashByHeight(0).status(),Status::NotFound);
    size_t coins=0;ASSERT_EQ(db->forEachUTXO([&](const auto&,uint32_t,const auto&){++coins;return true;}),Status::Ok);EXPECT_EQ(coins,0u);
    service->Stop();context.chainstate.reset();
}
} // namespace dinero
#endif
