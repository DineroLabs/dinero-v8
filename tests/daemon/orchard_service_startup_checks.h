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
