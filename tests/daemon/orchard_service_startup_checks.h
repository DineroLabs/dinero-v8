#pragma once
// Included only by the dedicated service test target, after the shared honest
// chain fixture. This access shim never enters a library or daemon build.
#include "daemon/services/chainstate_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/config.h"
#include "consensus/orchard_header.h"
#include "daemon/runtime_block_reader.h"
#include <new>

// Test executable only. Refuse ordinary allocations on the publishing thread;
// other threads (including RocksDB workers) retain their normal allocator.
static thread_local bool refuse_tip_allocation = false;
static thread_local unsigned refused_tip_allocations = 0;
void* operator new(std::size_t n) {
    if (refuse_tip_allocation) { ++refused_tip_allocations; throw std::bad_alloc(); }
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace dinero {
struct ShieldedStateStartupTestAccess {
    static void Set(ChainstateService& s, CBlockIndex& tip, const consensus::UtreexoForest& forest) {
        s.logger_=std::make_shared<LoggerService>("");
        s.consensus_utxo_set_=std::make_unique<consensus::ConsensusUTXOSet>();
        s.consensus_utxo_set_->ReplaceForestGuarded(forest);
        s.consensus_utxo_set_->SetBestBlock(tip.hash,tip.height);
        s.PublishActiveTip(&tip,ChainstateService::TipPublishReason::kStartupLoad);
    }
    static bool Verify(ChainstateService& s) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        return s.VerifyConsensusJournalAtActiveTip();
    }
    static bool UndoCoverage(ChainstateService& s, uint32_t height, uint32_t count) {
        return s.VerifyActiveChainUndoCoverage(height,count);
    }
    static void Notifications(ChainstateService& s,std::shared_ptr<RuntimeBlockNotifications> n) { s.runtime_block_notifications_=std::move(n); }
    static bool Disconnect(ChainstateService& s,CBlockIndex* tip) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        return s.DisconnectTip(tip);
    }
    static bool Connect(ChainstateService& s,CBlockIndex* tip,std::string& error,bool& invalid) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        return s.ConnectTip(tip,&error,&invalid);
    }
    static bool Reorg(ChainstateService& s,const std::vector<CBlockIndex*>& old_path,
        const std::vector<CBlockIndex*>& new_path,std::unique_ptr<RuntimeReorgTransition>& prepared) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        return s.PrepareRuntimeReorgUnderLock(old_path,new_path,prepared);
    }
    static bool ForkPoint(ChainstateService& s,CBlockIndex* fork) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        return s.VerifyForkPointForestUnderLock(fork);
    }
    static void RestoreForest(ChainstateService& s,const consensus::UtreexoForest& f) {
        s.consensus_utxo_set_->ReplaceForestGuarded(f);
    }
    static void SeedPositions(ChainstateService& s) {
        s.utxo_position_index_=std::make_unique<indexing::UTXOPositionIndex>();
        s.utxo_position_index_->AddPosition(TxId(uint256{}),0,42);
    }
    static void LoadCoins(ChainstateService& s,ChainDB& db) {
        CHECK(db.forEachUTXO([&](const uint256& hash,uint32_t n,const Coin& coin) {
            CHECK(s.consensus_utxo_set_->AddCoin(OutPoint(TxId(hash),n),MemoryCoin(coin)));return true;
        })==Status::Ok);
    }
    static bool TipIs(const ChainstateService& s,const CBlockIndex* tip) { return s.active_tip_==tip; }
    static const ConsensusUTXOSet& Coins(const ChainstateService& s) { return *s.consensus_utxo_set_; }
    static void StaleMemory(ChainstateService& s) { s.consensus_utxo_set_->SetBestBlock(uint256{},0); }
    static void EmptyForest(ChainstateService& s) { s.consensus_utxo_set_->ReplaceForestGuarded(consensus::UtreexoForest{}); }
    static bool Verified(const ChainstateService& s) { return s.journal_verified_at_startup_; }
    static void PublishWithoutAllocations(ChainstateService& s, CBlockIndex* next, bool rollback) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(s.activation_mutex_);
        bool threw=false;refused_tip_allocations=0;refuse_tip_allocation=true;
        try {
            s.PublishActiveTipLocked(next,rollback ? ChainstateService::TipPublishReason::kRollback
                                                   : ChainstateService::TipPublishReason::kAdvancement);
        } catch (...) { threw=true; }
        refuse_tip_allocation=false;
        CHECK(!threw); // Diagnostics must not interrupt post-durable publication.
        CHECK(!next || refused_tip_allocations>0); // The logging failure was actually exercised.
        CHECK(s.active_tip_==next);
        std::lock_guard<std::mutex> published(s.published_tip_mutex_);
        CHECK(s.published_tip_valid_==bool(next));
        CHECK(s.published_tip_hash_==(next?next->hash:uint256{}));
        CHECK(s.published_tip_height_==(next?uint32_t(next->height):0));
    }
};
}
static void ServiceDisconnectChecksImpl(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path,bool reconnect=false,bool fork_audit=false,bool reorg_check=false) {
    using Access=dinero::ShieldedStateStartupTestAccess;
    const auto old_params=Params();const auto old_config=GetConfig();
    struct Restore { ChainParams p;NodeConfig c;~Restore(){MutableParams()=p;GetConfig()=c;} } restore{old_params,old_config};
    MutableParams().orchard_activation_height=c.activation_height;MutableParams().orchard_branch_id=c.domain.branch_id;
    MutableParams().enforce_witness_commitment=true;MutableParams().witness_commitment_enforcement_height=c.activation_height;
    GetConfig().utreexo_stateless=false;
    TempDir flatfiles;auto files=std::make_shared<BlockStorage>();CHECK(files->init(flatfiles.path)==Status::Ok);
    auto install=[&](const OrchardBlockContext& context,const OrchardBlockCandidate& candidate) {
        const auto body=RequiredValue(files->writeBlockBytes(context.block_hash,
            std::string(candidate.WireBytes().begin(),candidate.WireBytes().end())));
        const auto undo=RequiredValue(files->writeUndo(context.block_hash,RequiredValue(db.getUndo(context.block_hash)).Serialize()));
        ChainDB::PersistedHeaderMetadata m;m.height=context.height;m.parent_hash=context.parent_hash;
        m.chainwork=RequiredValue(db.getBlockWork(context.block_hash));m.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO;
        m.file_number=body.file_number;m.data_pos=body.offset;m.data_size=body.size;
        m.undo_file=undo.file_number;m.undo_pos=undo.offset;m.undo_size=undo.size;
        CHECK(db.putHeaderMetadata(token,context.block_hash,m)==Status::Ok);
    };
    install(c,block);
    std::optional<ChainDB::PersistedHeaderMetadata> outer_parent_metadata;
    if(reconnect && c.height>c.activation_height) {
        outer_parent_metadata=RequiredValue(db.getHeaderMetadata(c.parent_hash));
        const auto parent_body=ReadStoredOrchardBlock(db,c.parent_hash,true);
        const auto parent_context=SelectedOrchardBlockContext(parent_body.Header(),c.height-1);
        CHECK(parent_context);install(*parent_context,parent_body);
    }
    if(c.height==c.activation_height) {
        JournalContinuation(db,c,block,forest,true,[&](const OrchardBlockContext& next,
            const OrchardBlockCandidate& child,const UtreexoForest& child_forest) {
            ServiceDisconnectChecksImpl(db,next,child,child_forest,path,reconnect,fork_audit,reorg_check);
        },true);
    }

    auto index=DiskIndex(db,block.Header(),c.height);
    const auto parent_header=RequiredValue(db.getHeader(c.parent_hash));
    CBlockIndex parent=reconnect && c.height>c.activation_height ? DiskIndex(db,parent_header,c.height-1) : CBlockIndex(parent_header,c.height-1);parent.chainwork=RequiredValue(db.getBlockWork(c.parent_hash)).GetHex();
    index.pprev=&parent;
    std::unique_ptr<CBlockIndex> grandparent_index;
    if(reorg_check && c.height>c.activation_height) {
        grandparent_index=std::make_unique<CBlockIndex>(RequiredValue(db.getHeader(parent_header.prev_block_hash)),c.height-2);
        parent.pprev=grandparent_index.get();
        std::vector<CBlockIndex*> old_path{&index,&parent}, new_path{&parent,&index};
        const auto plan=ReadRuntimeReorgPlanUnderLock(db,files.get(),old_path,new_path);
        CHECK(plan && plan->disconnect.size()==2 && plan->connect.size()==2);
        CHECK(plan->disconnect[0].hash==index.hash && plan->disconnect[1].hash==parent.hash);
        CHECK(plan->connect[0].hash==parent.hash && plan->connect[1].hash==index.hash);
        CHECK(plan->disconnect[1].body.Orchard().Transactions().size()>1);
        const auto saved=RequiredValue(db.getHeaderMetadata(parent.hash));auto missing=saved;missing.data_size=0;
        CHECK(db.putHeaderMetadata(token,parent.hash,missing)==Status::Ok);
        CHECK(!ReadRuntimeReorgPlanUnderLock(db,files.get(),old_path,new_path)); // No prefix on missing ancestor.
        CHECK(!ReadRuntimeReorgPlanUnderLock(db,files.get(),old_path,{})); // Invalidation has no replacement path to catch it again.
        CHECK(db.putHeaderMetadata(token,parent.hash,saved)==Status::Ok);
    }
    struct Notifications final:RuntimeBlockNotifications {
        ChainDB& db;ChainstateService& service;const CBlockIndex& before;const CBlockIndex& after;
        const std::vector<uint8_t>& wire;RuntimeBlockDirection direction;bool refuse=false,published=false,coherent=false;unsigned prepared=0;
        bool allow_reorg=false;unsigned finishes=0;RuntimeReorgProgress progress;
        std::shared_ptr<const RuntimeReorgPlan> retained;
        struct ReorgPrepared final:PreparedRuntimeReorgNotifications {
            Notifications& owner;explicit ReorgPrepared(Notifications& n):owner(n){}
            void Finish(RuntimeReorgProgress p) noexcept override { owner.progress=p;++owner.finishes; }
        };
        std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(std::shared_ptr<const RuntimeReorgPlan> plan) override {
            if (!allow_reorg) return {};
            retained=std::move(plan);return std::make_unique<ReorgPrepared>(*this);
        }
        Notifications(ChainDB& d,ChainstateService& s,const CBlockIndex& b,const CBlockIndex& a,const std::vector<uint8_t>& w,RuntimeBlockDirection dir=RuntimeBlockDirection::Disconnect)
          :db(d),service(s),before(b),after(a),wire(w),direction(dir){}
        struct Prepared final:PreparedRuntimeBlockNotifications {
            Notifications& n;explicit Prepared(Notifications& value):n(value){}
            void PublishAfterCommit() noexcept override {
                n.published=true;
                const auto tip=n.db.getTip();
                n.coherent=tip.ok() && tip->hash==n.after.hash && Access::TipIs(n.service,&n.after) &&
                    Access::Coins(n.service).GetBestBlock()==n.after.hash &&
                    n.service.GetUTXOPositionIndex()->GetPositionCount()==0;
            }
        };
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,
            RuntimeBlockDirection direction) override {
            CHECK(direction==this->direction && height==(direction==RuntimeBlockDirection::Connect?after.height:before.height) && body.IsOrchardProfile());
            CHECK(body.Orchard().WireBytes()==wire && Access::TipIs(service,&before));
            CHECK(RequiredValue(db.getTip()).hash==before.hash);++prepared;
            if(refuse)return {};
            return std::make_unique<Prepared>(*this);
        }
    };
    ChainstateService service;service.setChainDB(&db);service.setBlockStorage(files);Access::Set(service,index,forest);Access::LoadCoins(service,db);Access::SeedPositions(service);
    db.close();const auto original=Inspect(path);CHECK(db.init(path)==Status::Ok);
    CHECK(!Access::Disconnect(service,&index)); // No silent omission of typed consumers.
    auto notifications=std::make_shared<Notifications>(db,service,index,parent,block.WireBytes());
    Access::Notifications(service,notifications);notifications->refuse=true;
    CHECK(!Access::Disconnect(service,&index) && !notifications->published);
    db.close();CHECK(Inspect(path)==original);CHECK(db.init(path)==Status::Ok);
    notifications->refuse=false;
    ++parent.timestamp;CHECK(!Access::Disconnect(service,&index));--parent.timestamp;
    GetConfig().utreexo_stateless=true;CHECK(!Access::Disconnect(service,&index));GetConfig().utreexo_stateless=false;
    db.close();CHECK(Inspect(path)==original);CHECK(db.init(path)==Status::Ok);
    std::unique_ptr<RuntimeReorgTransition> reorg;
    if(reorg_check) {
        CHECK(!Access::Reorg(service,{&index},{&index},reorg) && !reorg); // Per-block readiness is insufficient.
        notifications->allow_reorg=true;
        CHECK(Access::Reorg(service,{&index},{&index},reorg) && reorg);
        CHECK(notifications->retained->disconnect.size()==1 && notifications->retained->connect.size()==1);
        CHECK(notifications->retained->disconnect[0].body.Serialize()==block.WireBytes());
        const auto& typed=notifications->retained->disconnect[0].body.Orchard().Transactions();
        CHECK(typed.size()==block.Transactions().size());
        bool found_orchard=false;
        for(size_t i=0;i<typed.size();++i) {
            CHECK(typed[i].GetTxid()==block.Transactions()[i].GetTxid());
            CHECK(typed[i].Serialize(TxSerializationMode::WithWitness)==block.Transactions()[i].Serialize(TxSerializationMode::WithWitness));
            found_orchard |= typed[i].IsOrchard();
        }
        if(c.height==c.activation_height) CHECK(found_orchard); // Mixed boundary fixture includes a real authorized bundle.
        reorg.reset();CHECK(notifications->finishes==1 && notifications->progress.disconnected==0 && notifications->progress.connected==0);
        auto absent=RequiredValue(db.getHeaderMetadata(index.hash));const auto saved=absent;absent.data_size=0;
        CHECK(db.putHeaderMetadata(token,index.hash,absent)==Status::Ok);
        CHECK(!Access::Reorg(service,{&index},{&index},reorg) && !reorg);
        CHECK(db.putHeaderMetadata(token,index.hash,saved)==Status::Ok);
        CBlockIndex wrong_parent=parent;wrong_parent.hash=H(81);auto* old=index.pprev;index.pprev=&wrong_parent;
        CHECK(!Access::Reorg(service,{&index},{&index},reorg) && !reorg);index.pprev=old;
        std::vector<CBlockIndex*> one{&index};
        CHECK(!ReadRuntimeReorgPlanUnderLock(db,files.get(),one,one,1)); // Byte budget refuses the complete attempt.
        CHECK(!ReadRuntimeReorgPlanUnderLock(db,files.get(),one,one,64*1024*1024,1));
        CHECK(!Access::Reorg(service,{&index,&index},{},reorg) && !reorg); // Broken ancestry.
        db.close();CHECK(Inspect(path)==original);CHECK(db.init(path)==Status::Ok);
        CHECK(Access::Reorg(service,{&index},{&index},reorg));
    }
    CHECK(Access::Disconnect(service,&index));
    if(reorg) reorg->Disconnected();
    CHECK(notifications->published && notifications->coherent && notifications->prepared==2);
    CHECK(Access::TipIs(service,&parent) && RequiredValue(db.getTip()).hash==c.parent_hash);
    if(c.height==c.activation_height) {
        CHECK(db.getOrchardState().status()==Status::NotFound && db.getLegacyRetirementState().status()==Status::NotFound);
    } else {
        CHECK(RequiredValue(db.getOrchardState()).block_hash==c.parent_hash);
        CHECK(RequiredValue(db.getLegacyRetirementState()).block_hash==c.parent_hash);
    }
    CheckMemoryCoins(db,Access::Coins(service));
    CHECK(!Access::Disconnect(service,&index)); // Never replay undo against its parent.
    if(fork_audit && c.height>c.activation_height) {
        db.close();const auto unchanged=Inspect(path);CHECK(db.init(path)==Status::Ok);
        CHECK(Access::ForkPoint(service,&parent)); // Mixed body must never enter the historical decoder.
        CHECK(!Access::ForkPoint(service,&index)); // Header for another state is not a fork-point receipt.
        const auto good_forest=Access::Coins(service).GetForest();
        Access::EmptyForest(service);CHECK(!Access::ForkPoint(service,&parent));
        Access::RestoreForest(service,good_forest);
        CHECK(Access::ForkPoint(service,&parent));
        const auto metadata=RequiredValue(db.getHeaderMetadata(parent.hash));auto absent=metadata;
        absent.status_flags &= ~BLOCK_HAVE_DATA;
        CHECK(db.putHeaderMetadata(token,parent.hash,absent)==Status::Ok);
        CHECK(!Access::ForkPoint(service,&parent));
        CHECK(db.putHeaderMetadata(token,parent.hash,metadata)==Status::Ok);
        CHECK(Access::ForkPoint(service,&parent));
        db.close();CHECK(Inspect(path)==unchanged);CHECK(db.init(path)==Status::Ok);
    }
    if(reconnect) {
        std::string error;bool invalid=false;
        auto connected=std::make_shared<Notifications>(db,service,parent,index,block.WireBytes(),RuntimeBlockDirection::Connect);
        Access::Notifications(service,connected);
        // Boundary history certification is deliberately not fabricated here.
        if(c.height==c.activation_height) {
            CHECK(!Access::Connect(service,&index,error,invalid) && !invalid);
        } else {
            auto headers=std::make_shared<consensus::HeaderChainSelector>();
            const auto grandparent=RequiredValue(db.getHeader(parent_header.prev_block_hash));
            CHECK(ServiceFixtureParent(grandparent.utreexo_root,headers.get()).GetHash()==grandparent.GetHash());
            CHECK(headers->AddHeader(parent_header) && headers->AddHeader(block.Header()));
            CHECK(headers->GetHeaderValue(index.hash)->chainwork==RequiredValue(db.getBlockWork(index.hash)));
            // No header selector is a local refusal, never consensus poisoning.
            CHECK(!Access::Connect(service,&index,error,invalid) && !invalid);
            service.setHeaderChainSelector(headers);
            connected->refuse=true;
            CHECK(!Access::Connect(service,&index,error,invalid) && !invalid && !connected->published);
            connected->refuse=false;
            const auto checkpoints=Params().vCheckpoints;
            MutableParams().vCheckpoints[c.height]=H(99).GetHex();
            CHECK(!Access::Connect(service,&index,error,invalid) && invalid && !connected->published);
            MutableParams().vCheckpoints=checkpoints;
            const auto saved=index.chainwork;index.chainwork=arith_uint256(1).GetHex();
            CHECK(!Access::Connect(service,&index,error,invalid) && !invalid);index.chainwork=saved;
            Access::SeedPositions(service);
            CHECK(Access::Connect(service,&index,error,invalid));
            if(reorg) {
                reorg->Connected();reorg.reset();
                CHECK(notifications->finishes==2 && notifications->progress.disconnected==1 && notifications->progress.connected==1);
            }
            CHECK(!invalid && connected->published && connected->coherent && connected->prepared==2);
            CheckMemoryCoins(db,Access::Coins(service));
            CHECK((index.status&BLOCK_VALID_MASK)==BLOCK_VALID_MASK);
            CHECK((RequiredValue(db.getHeaderMetadata(index.hash)).status_flags&BLOCK_VALID_MASK)==BLOCK_VALID_MASK);
            CHECK(RequiredValue(db.getOrchardState()).block_hash==index.hash);
            CHECK(!Access::Connect(service,&index,error,invalid));
            // Repeat the real rollback to leave the enclosing fixture at parent.
            Access::Notifications(service,notifications);
            CHECK(Access::Disconnect(service,&index));
        }
    }
    if(reorg) {
        reorg.reset(); // Refused activation-boundary reconnect reports only the committed rollback.
        CHECK(notifications->finishes==2 && notifications->progress.disconnected==1 && notifications->progress.connected==0);
    }
    if(reorg_check) CHECK(notifications->retained->disconnect[0].body.Serialize()==block.WireBytes());
    if(outer_parent_metadata)CHECK(db.putHeaderMetadata(token,c.parent_hash,*outer_parent_metadata)==Status::Ok);
    std::cout<<"Actual service typed disconnect: notification readiness, atomic rollback, memory and observer ordering checked\n";
}
static void ServiceDisconnectChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    ServiceDisconnectChecksImpl(db,c,block,forest,path,false);
}
static void ServiceConnectChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    ServiceDisconnectChecksImpl(db,c,block,forest,path,true);
}
static void ServiceForkPointChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    ServiceDisconnectChecksImpl(db,c,block,forest,path,true,true);
}
static void ServiceReorgPlanChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    ServiceDisconnectChecksImpl(db,c,block,forest,path,true,false,true);
}
static void ServiceUndoCoverageChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    using Access=dinero::ShieldedStateStartupTestAccess;
    const auto old_params=Params();const auto old_config=GetConfig();
    struct Restore { ChainParams p;NodeConfig c;~Restore(){MutableParams()=p;GetConfig()=c;} } restore{old_params,old_config};
    MutableParams().orchard_activation_height=c.activation_height;
    MutableParams().orchard_branch_id=c.domain.branch_id;
    MutableParams().enforce_witness_commitment=true;
    MutableParams().witness_commitment_enforcement_height=c.activation_height;
    GetConfig().utreexo_stateless=false;
    TempDir flatfiles;auto files=std::make_shared<BlockStorage>();CHECK(files->init(flatfiles.path)==Status::Ok);
    const auto body=RequiredValue(files->writeBlockBytes(c.block_hash,std::string(block.WireBytes().begin(),block.WireBytes().end())));
    const auto undo=RequiredValue(db.getUndo(c.block_hash));
    const auto undo_pos=RequiredValue(files->writeUndo(c.block_hash,undo.Serialize()));
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.height=c.height;metadata.parent_hash=c.parent_hash;
    metadata.chainwork=RequiredValue(db.getBlockWork(c.block_hash));
    metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO;
    metadata.file_number=body.file_number;metadata.data_pos=body.offset;metadata.data_size=body.size;
    metadata.undo_file=undo_pos.file_number;metadata.undo_pos=undo_pos.offset;metadata.undo_size=undo_pos.size;
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    const auto check=[&](bool expected,uint32_t height,uint32_t count) {
        db.close();const auto before=Inspect(path);CHECK(db.init(path)==Status::Ok);
        {
            ChainstateService service;service.setChainDB(&db);service.setBlockStorage(files);
            CHECK(Access::UndoCoverage(service,height,count)==expected);
            CHECK(service.IsInSafeMode()==!expected);
        }
        db.close();CHECK(Inspect(path)==before);CHECK(db.init(path)==Status::Ok);
    };
    check(true,c.height,1);
    auto missing=metadata;missing.undo_size=0;
    CHECK(db.putHeaderMetadata(token,c.block_hash,missing)==Status::Ok);
    check(false,c.height,1); // A mixed body must not end the historical walk as success.
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    check(true,c.height,1);
    auto failed_metadata=metadata;failed_metadata.status_flags|=BLOCK_FAILED_VALID;
    CHECK(db.putHeaderMetadata(token,c.block_hash,failed_metadata)==Status::Ok);check(false,c.height,1);
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    check(false,c.height-1,1); // Persisted Orchard state forbids a legacy fallback.
    GetConfig().utreexo_stateless=true;check(false,c.height,1);GetConfig().utreexo_stateless=false;
    ++MutableParams().orchard_branch_id;check(false,c.height,1);--MutableParams().orchard_branch_id;
    MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
    check(false,c.height,1);
    MutableParams().orchard_activation_height=c.activation_height;MutableParams().orchard_branch_id=c.domain.branch_id;
    JournalContinuation(db,c,block,forest,true,[&](const OrchardBlockContext& next,
        const OrchardBlockCandidate& child,const UtreexoForest&) {
        auto child_metadata=metadata;child_metadata.height=next.height;child_metadata.parent_hash=next.parent_hash;
        child_metadata.chainwork=RequiredValue(db.getBlockWork(next.block_hash));
        const auto child_body=RequiredValue(files->writeBlockBytes(next.block_hash,
            std::string(child.WireBytes().begin(),child.WireBytes().end())));
        const auto child_undo=RequiredValue(files->writeUndo(next.block_hash,RequiredValue(db.getUndo(next.block_hash)).Serialize()));
        child_metadata.file_number=child_body.file_number;child_metadata.data_pos=child_body.offset;child_metadata.data_size=child_body.size;
        child_metadata.undo_file=child_undo.file_number;child_metadata.undo_pos=child_undo.offset;child_metadata.undo_size=child_undo.size;
        CHECK(db.putHeaderMetadata(token,next.block_hash,child_metadata)==Status::Ok);
        check(true,next.height,1);check(true,next.height,2);
        CHECK(db.putHeaderMetadata(token,c.block_hash,missing)==Status::Ok);
        check(true,next.height,1);check(false,next.height,2);check(false,next.height,0);
        CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
        // Both representations agree on a changed coin, but the committed
        // forest delta still identifies the actual spent coin.
        auto changed=undo;CHECK(!changed.spent.empty());++changed.spent.front().value;
        const auto altered=RequiredValue(files->writeUndo(c.block_hash,changed.Serialize()));
        auto changed_metadata=metadata;changed_metadata.undo_file=altered.file_number;
        changed_metadata.undo_pos=altered.offset;changed_metadata.undo_size=altered.size;
        CHECK(db.putHeaderMetadata(token,c.block_hash,changed_metadata)==Status::Ok);
        CHECK(db.putUndo(token,c.block_hash,changed)==Status::Ok);
        check(true,next.height,1);check(false,next.height,2);
        CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
        CHECK(db.putUndo(token,c.block_hash,undo)==Status::Ok);
        // An indexed body with a changed proof suffix must not be substituted
        // for the exact bytes retained by the atomic commit.
        auto changed_wire=block.WireBytes();changed_wire.back()^=1;
        const auto other_body=RequiredValue(files->writeBlockBytes(c.block_hash,
            std::string(changed_wire.begin(),changed_wire.end())));
        changed_metadata=metadata;changed_metadata.file_number=other_body.file_number;
        changed_metadata.data_pos=other_body.offset;changed_metadata.data_size=other_body.size;
        CHECK(db.putHeaderMetadata(token,c.block_hash,changed_metadata)==Status::Ok);
        check(true,next.height,1);check(false,next.height,2);
        CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
        check(true,next.height,2);
    });
    check(true,c.height,1);
    std::cout<<"Service undo coverage: bounded two-block walk, ancestor undo, coin/delta binding, exact body, safe mode and unchanged logical storage checked\n";

}
static void ServiceStartupChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    using Access=dinero::ShieldedStateStartupTestAccess;
    const auto old_params=Params(); const auto old_config=GetConfig();
    struct Restore { ChainParams p; NodeConfig c; ~Restore(){MutableParams()=p;GetConfig()=c;} } restore{old_params,old_config};
    MutableParams().orchard_activation_height=c.activation_height;
    MutableParams().orchard_branch_id=c.domain.branch_id; // Public synthetic fixture domain only.
    MutableParams().enforce_witness_commitment=true;
    MutableParams().witness_commitment_enforcement_height=c.activation_height;
    const auto selected=SelectedOrchardBlockContext(block.Header(),c.height);CHECK(selected);
    CHECK(selected->domain.genesis_wire==c.domain.genesis_wire);
    GetConfig().consensus_atomic_persist=false; GetConfig().utreexo_stateless=false;
    TempDir flatfiles;auto files=std::make_shared<BlockStorage>();CHECK(files->init(flatfiles.path)==Status::Ok);
    const auto body=RequiredValue(files->writeBlockBytes(c.block_hash,std::string(block.WireBytes().begin(),block.WireBytes().end())));
    const auto undo=RequiredValue(db.getUndo(c.block_hash));
    const auto undo_pos=RequiredValue(files->writeUndo(c.block_hash,undo.Serialize()));
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.height=c.height;metadata.parent_hash=c.parent_hash;
    metadata.chainwork=RequiredValue(db.getBlockWork(c.block_hash));metadata.status_flags=BLOCK_VALID_HEADER;
    metadata.status_flags|=BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO;
    metadata.file_number=body.file_number;metadata.data_pos=body.offset;metadata.data_size=body.size;
    metadata.undo_file=undo_pos.file_number;metadata.undo_pos=undo_pos.offset;metadata.undo_size=undo_pos.size;
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    auto index=DiskIndex(db,block.Header(),c.height);
    {
        ChainstateService service;Access::Set(service,index,forest);
        CBlockIndex next=index;next.hash=H(98);++next.height;
        Access::PublishWithoutAllocations(service,&next,false);
        Access::PublishWithoutAllocations(service,&index,true);
        Access::PublishWithoutAllocations(service,nullptr,true);
        Access::PublishWithoutAllocations(service,&index,false);
    }
    db.close();const auto original=Inspect(path);CHECK(db.init(path)==Status::Ok);
    const auto check=[&](bool expected,bool stale_memory=false,bool activation=false,bool empty_forest=false) {
        ChainstateService service;service.setChainDB(&db);service.setBlockStorage(files);
        Access::Set(service,index,forest);if(stale_memory)Access::StaleMemory(service);
        if(empty_forest)Access::EmptyForest(service);
        CHECK(Access::Verify(service)==expected);
        CHECK(service.IsInSafeMode()==!expected);
        if(!expected)CHECK(service.GetSafeModeReason().starts_with("orchard-startup-state:"));
        if(activation) {
            service.ActivateBestChain();
            CHECK(!Access::Verified(service)); // Failed audit may not become a one-shot success.
        }
        CHECK(RequiredValue(db.getTip()).hash==c.block_hash);
        CHECK(RequiredValue(db.getValidatedTip()).hash==c.block_hash);
        CHECK(service.GetActiveTip()==&index);
    };
    { ChainstateService early;early.setChainDB(&db);
      CHECK(!Access::Verify(early) && early.IsInSafeMode()); } // No logger, active tip or restored memory yet.
    const std::string key="orchard_consensus_journal:v1:00004e21:"+c.block_hash.GetHex();
    std::string record;CHECK(db.getRaw(key,record)==Status::Ok);
    rocksdb::WriteBatch remove;remove.Delete(key);Commit(db,remove);
    check(false,false,true); // Must fail even with the legacy optional journal disabled.
    rocksdb::WriteBatch replace;replace.Put(key,record);Commit(db,replace);
    check(true);
    GetConfig().consensus_atomic_persist=true;check(true);GetConfig().consensus_atomic_persist=false;
    check(false,true);
    check(false,false,false,true);
    auto changed=metadata;changed.undo_size=0;
    CHECK(db.putHeaderMetadata(token,c.block_hash,changed)==Status::Ok);check(false);
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    auto other=undo;++other.spent.front().value;
    const auto other_pos=RequiredValue(files->writeUndo(c.block_hash,other.Serialize()));
    changed=metadata;changed.undo_file=other_pos.file_number;changed.undo_pos=other_pos.offset;changed.undo_size=other_pos.size;
    CHECK(db.putHeaderMetadata(token,c.block_hash,changed)==Status::Ok);
    index=DiskIndex(db,block.Header(),c.height);check(false);
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);index=DiskIndex(db,block.Header(),c.height);
    // A strict service read must not fall back to the valid embedded body.
    changed=metadata;changed.data_size=0;
    CHECK(db.putHeaderMetadata(token,c.block_hash,changed)==Status::Ok);check(false);
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    auto incomplete=block.WireBytes();
    CHECK(block.Utreexo());incomplete.resize(incomplete.size()-block.Utreexo()->serialize().size());incomplete.back()=0;
    const auto incomplete_pos=RequiredValue(files->writeBlockBytes(c.block_hash,std::string(incomplete.begin(),incomplete.end())));
    changed=metadata;changed.file_number=incomplete_pos.file_number;changed.data_pos=incomplete_pos.offset;changed.data_size=incomplete_pos.size;
    CHECK(db.putHeaderMetadata(token,c.block_hash,changed)==Status::Ok);index=DiskIndex(db,block.Header(),c.height);
    CHECK(ReadRuntimeBlockUnderLock(db,files.get(),c.block_hash,c.height).ok());
    check(false); // Header and identity checks alone do not bind the proof suffix.
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);index=DiskIndex(db,block.Header(),c.height);
    ++MutableParams().orchard_branch_id;check(false);--MutableParams().orchard_branch_id;
    MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
    check(false); // Stored Orchard state must not be treated as an old optional journal.
    MutableParams().orchard_activation_height=c.activation_height;MutableParams().orchard_branch_id=c.domain.branch_id;
    GetConfig().utreexo_stateless=true;check(false);GetConfig().utreexo_stateless=false;
    db.close();
    {auto names=legacy;names.push_back(shielded_store_fixture::shielded);Raw raw(path,names);
     CHECK(raw.db->Delete(rocksdb::WriteOptions{},raw.cf(shielded_store_fixture::shielded),"O1S").ok());}
    CHECK(db.init(path)==Status::Ok);
    MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
    check(false); // The remaining retirement receipt also forbids legacy fallback.
    db.close();
    {auto names=legacy;names.push_back(shielded_store_fixture::shielded);Raw raw(path,names);
     CHECK(raw.db->Delete(rocksdb::WriteOptions{},raw.cf(shielded_store_fixture::shielded),"R1S").ok());}
    CHECK(db.init(path)==Status::Ok);
    MutableParams().orchard_activation_height=c.activation_height;MutableParams().orchard_branch_id=c.domain.branch_id;
    --index.height;check(false);++index.height; // Persisted height requires restore even before active-tip alignment.
    db.close();
    {auto names=legacy;names.push_back(shielded_store_fixture::shielded);Raw raw(path,names);
     raw.put(shielded_store_fixture::shielded,"O1S",original.at(shielded_store_fixture::shielded).at("O1S"));
     raw.put(shielded_store_fixture::shielded,"R1S",original.at(shielded_store_fixture::shielded).at("R1S"));}
    CHECK(db.init(path)==Status::Ok);
    check(true);
    db.close();CHECK(Inspect(path)==original);CHECK(db.init(path)==Status::Ok);
    // Historical stores with no Orchard state retain the optional-journal
    // behavior, including the pre-separation schema.
    for(bool separated:{false,true}) {
        TempDir historical;Seed(historical.path,separated,separated?std::optional<std::string>(ready):std::nullopt);ChainDB old_db;CHECK(old_db.init(historical.path)==Status::Ok);
        ChainstateService old_service;old_service.setChainDB(&old_db);
        CBlockIndex old_tip;old_tip.hash=H(45);old_tip.height=c.activation_height-1;
        Access::Set(old_service,old_tip,UtreexoForest{});
        CHECK(Access::Verify(old_service) && !old_service.IsInSafeMode());
    }
    std::cout<<"Service startup: optional legacy flag, missing journal, stale memory, locators, undo, domain, inactive profile and unsupported CSN checked\n";
}
