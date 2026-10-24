#include "daemon/orchard_snapshot_store.h"
#include "daemon/services/orchard_branch_replay.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_header.h"
#include "consensus/block_lifecycle.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "common/annotated_mutex.h"
#include "storage/block_storage.h"
#include "storage/forest_restore.h"
#include "storage/orchard_storage_mode.h"
#include "storage/shielded_cf_comparator.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/anchor_history.h"
#include "crypto/secure_random.h"
#include "util/hex.h"
#include <rocksdb/iterator.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero {
namespace {
using namespace consensus;
void Require(bool ok,int line=__builtin_LINE()) {
    if(!ok)throw std::runtime_error("Orchard snapshot selected storage refused at check "+std::to_string(line));
}
template<class T>T Need(StatusOr<T> value){Require(value.ok());return std::move(*value);}
void Need(Status value){Require(value==Status::Ok);}
Transaction CompiledGenesis() {
    Transaction genesis;std::vector<uint8_t> wire;size_t consumed=0;
    Require(util::unhex(Params().genesis.genesisCoinbaseHex,wire)&&
        TransactionSerializer::Deserialize(genesis,wire,consumed)&&consumed==wire.size());return genesis;
}
OrchardSnapshotCoins ReadCoins(const ChainDB& db) {
    OrchardSnapshotCoins coins;
    Need(db.forEachUTXO([&](const uint256& id,uint32_t index,const Coin& stored){
        Require(stored.height>=0);UTXOEntry coin;coin.value=AmountUna::Una(stored.amount);
        Require(util::unhex(stored.script_pubkey,coin.scriptPubKey));coin.height=uint32_t(stored.height);
        coin.isCoinbase=stored.coinbase;coin.is_confidential=stored.is_confidential;coin.commitment=stored.commitment;
        Require(coins.emplace(OutPoint(TxId(id),index),std::move(coin)).second);return true;
    }));return coins;
}
std::unique_ptr<ConsensusUTXOSet> FullPublication(const OrchardSnapshotSection& s,const OrchardSnapshotCoins& coins) {
    VerifyOrchardSnapshotCoins(s,coins);auto live=std::make_unique<ConsensusUTXOSet>();
    Require(live->BulkLoad(coins,s.state.height,s.state.block_hash));
    live->ReplaceForestGuarded(UtreexoForest::deserialize(s.accumulator));return live;
}
}
void OrchardSnapshotStore::CreateFreshStore(const std::filesystem::path& destination) {
    namespace fs=std::filesystem;
    Require(!fs::exists(destination)&&!fs::is_symlink(fs::symlink_status(destination))&&
        fs::is_directory(destination.parent_path()));
    auto temporary=destination;temporary+=".v6-init-"+util::hex(secure_random_bytes(16));bool owned=false;
    try {
        Require(fs::create_directory(temporary));owned=true;
        fs::permissions(temporary,fs::perms::owner_all,fs::perm_options::replace);
        {
            struct Store {
                std::unique_ptr<rocksdb::DB> db;std::vector<rocksdb::ColumnFamilyHandle*> handles;
                ~Store(){if(db)for(auto* handle:handles)db->DestroyColumnFamilyHandle(handle);}
            } store;
            rocksdb::Options options;options.create_if_missing=true;options.create_missing_column_families=true;
            std::vector<rocksdb::ColumnFamilyDescriptor> families;
            for(const auto* name:{"default","meta","blocks","headers","height","txindex","utxo","utreexo","prebase_coins","shielded_state_v1"}) {
                rocksdb::ColumnFamilyOptions family(options);
                if(std::string_view(name)=="shielded_state_v1")family=storage::ShieldedStateColumnFamilyOptions(std::move(family));
                families.emplace_back(name,std::move(family));
            }
            rocksdb::DB* opened=nullptr;const auto status=rocksdb::DB::Open(options,temporary.string(),families,&store.handles,&opened);
            store.db.reset(opened);Require(status.ok()&&store.db&&store.handles.size()==10);
            consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
            const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
            std::string marker;storage::catalog::Number(marker,0,4);
            const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
            marker.append(reinterpret_cast<const char*>(genesis.data),32);
            const auto root=tree.Root();marker.append(reinterpret_cast<const char*>(root.data()),root.size());
            storage::catalog::Number(marker,0,8);storage::catalog::Number(marker,0,8);
            std::string schema;storage::catalog::Number(schema,4,4);rocksdb::WriteBatch batch;
            batch.Put(storage::OrchardSnapshotFreshKey,"DNOV6-FRESH-1");
            batch.Put(store.handles[1],"schema_version",schema);
            batch.Put(store.handles[1],"storage_layout_v1","shielded-state-v1:READY");
            batch.Put(store.handles[1],"shielded_tip",marker);
            batch.Put(store.handles[9],"Mshielded_frontier",rocksdb::Slice(reinterpret_cast<const char*>(frontier.data()),frontier.size()));
            batch.Put(store.handles[9],"Mshielded_anchor_history",rocksdb::Slice(reinterpret_cast<const char*>(history.data()),history.size()));
            rocksdb::WriteOptions writes;writes.sync=true;Require(store.db->Write(writes,&batch).ok());
        }
        // Exercise the ordinary READY opening rules before publishing a name.
        {ChainDB check;Need(check.init(temporary));Require(check.hasSeparatedShieldedState()&&check.getTip().status()==Status::NotFound);check.close();}
        Require(!fs::exists(destination)&&!fs::is_symlink(fs::symlink_status(destination)));
        fs::rename(temporary,destination);owned=false;
#ifndef _WIN32
        const int descriptor=open(destination.parent_path().c_str(),O_RDONLY);Require(descriptor>=0);
        const int synced=fsync(descriptor);const int closed=close(descriptor);Require(synced==0&&closed==0);
#endif
    } catch(...) {
        // Only this exclusively created, still-unpublished directory belongs
        // to this operation. Interrupted processes leave it unselected.
        if(owned){std::error_code ignored;fs::remove_all(temporary,ignored);}throw;
    }
}
void OrchardSnapshotStore::SeedRetainedHeaders(const ChainDB& db,consensus::HeaderChainSelector& headers) {
    std::vector<std::pair<int32_t,uint256>> ordered;
    Need(db.forEachHeaderMetadata([&](const auto& hash,const ChainDB::PersistedHeaderMetadata& metadata){
        ordered.emplace_back(metadata.height,hash);return true;
    }));std::sort(ordered.begin(),ordered.end());
    for(const auto& [height,hash]:ordered) {
        const auto header=Need(db.getHeader(hash));
        // Only genuinely validated headers enter the selector. Invalid or
        // orphaned side records cannot rescue a missing selected ancestry.
        (void)headers.AddHeader(header);
    }
}
consensus::OrchardSnapshotSection OrchardSnapshotStore::Capture(const ChainDB& db,
    consensus::OrchardSnapshotProfile profile,const std::vector<uint8_t>& accumulator) {
    using namespace consensus;OrchardSnapshotSection s;s.profile=profile;s.accumulator=accumulator;
    s.state=Need(db.getOrchardState());s.retirement=Need(db.getLegacyRetirementState());
    s.base_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(s.state.block_hash)));
    if(profile==OrchardSnapshotProfile::Full) {
        // Live forests retain dispensable internal cache cells. Deserialization
        // rebuilds them, so normalize the trusted export view once; import
        // continues to require the resulting canonical fixed-point encoding.
        s.accumulator=CanonicalOrchardSnapshotForest(accumulator);
    }
    s.parent_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(s.base_catalog.parent)));
    s.base_body=ReadStoredOrchardBlock(db,s.state.block_hash,Params().enforce_witness_commitment&&
        s.state.height>=Params().witness_commitment_enforcement_height).WireBytes();
    Need(db.forEachOrchardNullifier([&](const auto& nf,const auto& owner){s.nullifiers.push_back(nf);return true;}));
    Need(db.forEachOrchardAnchor([&](const auto& anchor,uint64_t refs){s.anchors.push_back({anchor,refs});return true;}));
    const auto read=[&](const auto& id){return Need(db.getOrchardCatalogNode(id));};
    auto inventory=std::make_shared<OrchardSnapshotCatalog>(std::filesystem::path(db.db_->GetName()).parent_path());
    for(const auto* state:{&s.parent_catalog,&s.base_catalog}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count){
            storage::catalog::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto& bytes){
                inventory->Insert(id,bytes);return true;
            });
        };
        walk(storage::catalog::Kind::Transactions,state->transactions,state->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,state->legacy,state->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,state->nontransparent,state->nontransparent_count);
    }
    inventory->Seal();s.disk_nodes=std::move(inventory);ValidateOrchardSnapshotSection(s);return s;
}
consensus::OrchardBlockContext OrchardSnapshotStore::Authenticate(const consensus::OrchardSnapshotSection& s,
    const consensus::HeaderChainSelector& headers,uint32_t burial,bool require_best_ancestry) {
    using namespace consensus;
    Require(OrchardCatalogCommitmentActive(Params(),s.state.height)&&
        s.base_catalog.leaf_activation==GetUtreexoMaturityLeafActivationHeight());
    const auto genesis=CompiledGenesis();
    storage::catalog::Tree transactions(storage::catalog::Kind::Transactions,[&](const uint256& id){
        return ReadOrchardSnapshotNode(s,id);
    });
    Require(transactions.Find(s.parent_catalog.transactions,storage::catalog::TransactionKey(genesis.GetTxid())).has_value());
    const auto captured=headers.GetHeaderValue(s.state.block_hash);Require(captured&&captured->height==s.state.height);
    const auto base=*captured;
    if(require_best_ancestry) {
        HeaderIndexEntry selected;Require(headers.GetHeaderAtHeightCopy(s.state.height,selected)&&selected.hash==base.hash);
    } else Require(burial==0);
    const auto parent=headers.GetHeaderValue(base.prev_hash);const auto best=headers.GetBestHeaderValue();
    Require(parent&&best&&uint64_t(parent->height)+1==base.height);
    const auto context=SelectedOrchardBlockContext(base.header,base.height);Require(bool(context));
    const OrchardBranchMtpLookup mtp=[&](uint32_t height)->std::optional<uint64_t> {
        uint256 ancestor;uint32_t selected=0,found=0,time=0;
        if(height>parent->height||!headers.GetAncestorHashByHash(parent->hash,height,ancestor,selected)||
            selected!=parent->height||!headers.GetMedianTimePastByHash(ancestor,time,found)||found!=height)return {};
        return time;
    };
    VerifyOrchardSnapshotSection(s,s.profile,*context,parent->header,base.header,parent->chainwork,base.chainwork,
        base.hash,std::max(best->height,base.height),burial,mtp,Params().enforce_witness_commitment&&
        base.height>=Params().witness_commitment_enforcement_height);
    return *context;
}
OrchardSnapshotStore::Publication OrchardSnapshotStore::Install(AnnotatedRecursiveMutex& mutex,ChainDB& db,
    const ChainWriteToken& token,BlockStorage& files,const consensus::HeaderChainSelector& headers,
    const consensus::OrchardSnapshotFile& file) {
    using namespace consensus;mutex.AssertHeld("install authenticated Orchard snapshot");
    const auto& s=file.orchard;const auto context=Authenticate(s,headers,Params().state_commitment_burial_depth);
    VerifyOrchardSnapshotCoins(s,file.coins);
    Require(file.metadata.version==SNAPSHOT_VERSION_V6&&file.metadata.block_hash==s.state.block_hash&&
        file.metadata.block_height==s.state.height&&file.metadata.utxo_count==file.coins.size());
    Require(db.hasSeparatedShieldedState()&&!storage::ReadOrchardSnapshotBase(db)&&
        db.getOrchardState().status()==Status::NotFound&&db.getLegacyRetirementState().status()==Status::NotFound);
    const auto genesis_hash=uint256::FromHexUnsafe(Params().genesis_hash);
    const auto tip=db.getTip();Require(!tip.ok()?tip.status()==Status::NotFound:tip->height==0&&tip->hash==genesis_hash);
    const auto consistent=db.getValidatedTip();
    Require(consistent.status()==Status::NotFound||(consistent.ok()&&tip.ok()&&consistent->height==0&&consistent->hash==genesis_hash));
    // Header-only preparation may be retried, but an old canonical height map
    // is not a fresh store merely because its tip or coin table was lost.
    std::unique_ptr<rocksdb::Iterator> heights(db.db_->NewIterator(rocksdb::ReadOptions(),db.cf_[db.idx_height_].get()));
    Require(bool(heights));bool saw_genesis=false;
    for(heights->SeekToFirst();heights->Valid();heights->Next()) {
        Require(!saw_genesis&&heights->key().ToString()==db.makeHeightKey(0)&&
            Need(db.getBlockHashByHeight(0))==genesis_hash);saw_genesis=true;
    }
    Require(heights->status().ok());
    // No replacing an active full store, compact owner, sidecar migration or
    // implicit profile conversion. Fresh genesis rows are the only exception.
    Require(!storage::ReadOrchardCompactStorageBinding(db));
    const auto genesis=CompiledGenesis();
    const auto old_coins=ReadCoins(db);
    for(const auto& [point,coin]:old_coins) {
        Require(point.txid==genesis.GetTxid()&&point.vout<genesis.vout.size()&&coin.height==0&&coin.isCoinbase&&
            coin.value==genesis.vout[point.vout].value&&coin.scriptPubKey==genesis.vout[point.vout].scriptPubKey&&
            !coin.is_confidential&&coin.commitment.empty());
    }
    Publication publication;
    if(s.profile==OrchardSnapshotProfile::Full)publication.full=FullPublication(s,file.coins);
    else {
        publication.compact=std::make_unique<OrchardCompactChainstate>();
        publication.compact->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
            &db,&mutex,s.base_catalog,s.retirement.record);
    }
    crypto::CSHA256 proof_hash;
    const auto proof_size=StreamOrchardSnapshotSection(s,[&](auto bytes){proof_hash.Write(bytes.data(),bytes.size());});
    uint256 proof_digest;proof_hash.Finalize(proof_digest.data);
    storage::OrchardSnapshotBase base{uint8_t(s.profile),s.state.height,s.state.block_hash,
        proof_digest,storage::OrchardSnapshotRetirementFingerprint(s.retirement.record),proof_size};
    // Bounded immutable preparation. Until the final synced batch selects the
    // base, interrupted chunks/nodes are unreachable and confer no authority.
    rocksdb::WriteBatch pending;size_t count=0;
    const auto flush=[&]{if(pending.Count()){Need(db.writeBatch(token,std::move(pending),true));pending=rocksdb::WriteBatch();count=0;}};
    const auto best=headers.GetBestHeaderValue();Require(best&&best->height<=uint32_t(INT32_MAX));
    uint256 base_ancestor;uint32_t captured_height=0;
    Require(uint64_t(best->height)>=uint64_t(s.state.height)+Params().state_commitment_burial_depth&&
        headers.GetAncestorHashByHash(best->hash,s.state.height,base_ancestor,captured_height)&&
        captured_height==best->height&&base_ancestor==s.state.block_hash);
    // Follow one captured hash-linked branch, not successive best-at-height
    // queries which could mix branches during concurrent header selection.
    auto ancestor=best->hash;
    for(uint64_t remaining=uint64_t(best->height)+1;remaining;--remaining) {
        const auto captured=headers.GetHeaderValue(ancestor);Require(captured&&captured->height==remaining-1);
        const auto& header=*captured;const auto height=header.height;ancestor=header.prev_hash;
        const auto old=db.getHeader(header.hash);
        if(old.ok())Require(old->SerializeForHash()==header.header.SerializeForHash()&&
            Need(db.getBlockHeight(header.hash))==int(height)&&Need(db.getBlockWork(header.hash))==header.chainwork);
        else {
            Require(old.status()==Status::NotFound);
            Need(db.putHeader(token,header.hash,header.header,int(height),header.chainwork,&pending));
            ChainDB::PersistedHeaderMetadata m;m.parent_hash=header.prev_hash;m.height=int32_t(height);
            m.chainwork=header.chainwork;m.status_flags=BLOCK_VALID_HEADER;
            Need(db.putHeaderMetadata(token,header.hash,m,&pending));
        }
        if(++count==1024)flush();
    }
    Require(ancestor.IsNull());
    flush();
    ForEachOrchardSnapshotNode(s,[&](const auto& id,const auto& bytes){
        Need(db.stageOrchardCatalogNode(token,id,bytes,pending));if(++count==1024)flush();
    });flush();
    std::string proof_chunk;proof_chunk.reserve(storage::OrchardSnapshotProofChunkBytes);uint64_t part=0;
    const auto write_chunk=[&] {
        const auto key=storage::OrchardSnapshotProofKey(base.proof_digest,part++);
        std::string previous;const auto status=db.getRaw(key,previous);
        Require(status==Status::NotFound||(status==Status::Ok&&previous==proof_chunk));
        if(status==Status::NotFound){pending.Put(key,proof_chunk);flush();}
        proof_chunk.clear();
    };
    Require(StreamOrchardSnapshotSection(s,[&](std::span<const uint8_t> bytes) {
        while(!bytes.empty()) {
            const auto n=std::min<size_t>(bytes.size(),storage::OrchardSnapshotProofChunkBytes-proof_chunk.size());
            proof_chunk.append(reinterpret_cast<const char*>(bytes.data()),n);bytes=bytes.subspan(n);
            if(proof_chunk.size()==storage::OrchardSnapshotProofChunkBytes)write_chunk();
        }
    })==proof_size);
    if(!proof_chunk.empty())write_chunk();
    const auto body=OrchardBlockCandidate::DecodeExact(s.base_body);
    const auto position=Need(files.writeBlockBytes(s.state.block_hash,std::string(s.base_body.begin(),s.base_body.end())));
    Require(position.offset<=UINT32_MAX);
    HeaderIndexEntry selected;Require(headers.GetHeaderAtHeightCopy(s.state.height,selected)&&selected.hash==s.state.block_hash);
    rocksdb::WriteBatch batch;
    std::vector<std::pair<uint256,uint64_t>> anchors;anchors.reserve(s.anchors.size());
    for(const auto& anchor:s.anchors)anchors.emplace_back(anchor.anchor,anchor.references);
    Need(db.stageOrchardSnapshotState(token,s.state,s.nullifiers,anchors,batch));
    Need(db.stageLegacyRetirementSnapshotState(token,s.retirement,batch));
    Need(db.stageOrchardCatalogState(token,s.parent_catalog.block,s.parent_catalog.Encode(),batch));
    Need(db.stageOrchardCatalogState(token,s.base_catalog.block,s.base_catalog.Encode(),batch));
    Need(db.stageOrchardBlock(token,body,Params().enforce_witness_commitment&&
        s.state.height>=Params().witness_commitment_enforcement_height,batch));
    Need(db.putHeader(token,selected.hash,selected.header,int(selected.height),selected.chainwork,&batch));
    ChainDB::PersistedHeaderMetadata metadata;metadata.parent_hash=context.parent_hash;metadata.height=int32_t(context.height);
    metadata.chainwork=selected.chainwork;metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_VALID_TREE|BLOCK_HAVE_DATA;
    metadata.file_number=position.file_number;metadata.data_pos=uint32_t(position.offset);metadata.data_size=position.size;
    Need(db.putHeaderMetadata(token,selected.hash,metadata,&batch));
    for(const auto& [point,coin]:old_coins)Need(db.deleteCoin(token,point.txid.AsUint256(),point.vout,&batch));
    for(const auto& [point,coin]:file.coins) {
        Coin value;value.amount=coin.value.GetUna();value.script_pubkey=util::hex(coin.scriptPubKey);
        value.height=int(coin.height);value.coinbase=coin.isCoinbase;
        Need(db.putCoin(token,point.txid.AsUint256(),point.vout,value,&batch));
    }
    if(s.profile==OrchardSnapshotProfile::Compact)
        batch.Put(storage::OrchardCompactStorageKey,storage::EncodeOrchardCompactStorageBinding(s.base_catalog));
    else Need(db.putUtreexoCheckpointWithChecksum(token,int(s.state.height),s.accumulator,&batch));
    batch.Put(storage::OrchardSnapshotBaseKey,base.Encode());
    batch.Delete(storage::OrchardSnapshotFreshKey);
    StageOrchardSnapshotJournal(context,selected.header,selected.chainwork,s.state,
        UtreexoStump::deserialize(s.base_catalog.stump),batch);
    Need(db.putForestTipMarker(token,{int32_t(s.state.height),s.state.block_hash,selected.header.utreexo_root},&batch));
    Need(db.putHeightIndex(token,int(s.state.height),s.state.block_hash,&batch));
    Need(db.setTip(token,s.state.block_hash,int(s.state.height),selected.chainwork,&batch));
    // This marker means state/forest consistency, not pre-base script history.
    // Base metadata deliberately lacks CHAIN/SCRIPTS and has no fabricated undo.
    Need(db.setValidatedTip(token,s.state.block_hash,int(s.state.height),&batch));
    // Recheck selected authority immediately before the sole state publication.
    (void)Authenticate(s,headers,Params().state_commitment_burial_depth);
    {
        // AddHeader uses a separate mutex from the selected writer. Bind the
        // exact independently validated base AND burial across publication,
        // not merely immediately before it. No header API/callback under lock.
        std::unique_lock<std::mutex> selection(headers.mutex_);
        const auto* best=headers.best_header_;
        const auto* selected_base=best?best->GetAncestor(s.state.height):nullptr;
        Require(selected_base&&selected_base->hash==s.state.block_hash&&selected_base->chainwork==s.base_catalog.work&&
            selected_base->header.SerializeForHash()==body.Header().SerializeForHash()&&
            uint64_t(best->height)>=uint64_t(s.state.height)+Params().state_commitment_burial_depth);
        try {if(db.writeBatch(token,std::move(batch),true)!=Status::Ok)throw CommitUncertain();}
        catch(...){throw CommitUncertain();}
    }
    return publication;
}
std::unique_ptr<OrchardBranchReplay> OrchardSnapshotStore::ReplayFromBaseline(const ChainDB& db,
    const consensus::HeaderChainSelector& headers,uint32_t height,const uint256& hash,const arith_uint256& work) {
    const auto baseline=ReadBaseline(db);(void)Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    std::vector<BlockHeader> history(size_t(baseline.state.height)+1);auto previous=baseline.state.block_hash;
    for(size_t remaining=history.size();remaining;--remaining) {
        const auto row=headers.GetHeaderValue(previous);Require(row&&row->height==remaining-1);
        history[remaining-1]=row->header;previous=row->prev_hash;
    }
    Require(previous.IsNull());
    return std::unique_ptr<OrchardBranchReplay>(new OrchardBranchReplay(baseline,history,{height,hash,work}));
}
consensus::OrchardSnapshotSection OrchardSnapshotStore::ReadBaseline(const ChainDB& db) {
    const auto base=storage::ReadOrchardSnapshotBase(db);Require(bool(base));
    std::string chunk;size_t position=0;uint64_t loaded=0,part=0;crypto::CSHA256 proof_hash;
    auto s=consensus::ReadOrchardSnapshotSection(base->proof_bytes,[&](std::span<uint8_t> bytes) {
        while(!bytes.empty()) {
            if(position==chunk.size()) {
                Require(loaded<base->proof_bytes);
                Need(db.getRaw(storage::OrchardSnapshotProofKey(base->proof_digest,part++),chunk));
                Require(chunk.size()==std::min<uint64_t>(storage::OrchardSnapshotProofChunkBytes,base->proof_bytes-loaded));
                proof_hash.Write(chunk);loaded+=chunk.size();position=0;
            }
            const auto n=std::min(bytes.size(),chunk.size()-position);
            std::copy_n(reinterpret_cast<const uint8_t*>(chunk.data()+position),n,bytes.data());
            position+=n;bytes=bytes.subspan(n);
        }
    },std::filesystem::path(db.db_->GetName()).parent_path());
    uint256 digest;proof_hash.Finalize(digest.data);
    Require(loaded==base->proof_bytes&&position==chunk.size()&&digest==base->proof_digest);
    Require(uint8_t(s.profile)==base->profile&&s.state.height==base->height&&s.state.block_hash==base->block&&
        storage::OrchardSnapshotRetirementFingerprint(s.retirement.record)==base->retirement);return s;
}
bool OrchardSnapshotStore::RecoverBaseMarkers(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
    const consensus::HeaderChainSelector& headers) {
    using namespace consensus;mutex.AssertHeld("recover authenticated v6 base markers");
    const auto floor=storage::ReadOrchardSnapshotBase(db);Require(bool(floor));const auto tip=Need(db.getTip());
    if(tip.height>=int(floor->height))return false;
    const auto baseline=ReadBaseline(db);const auto context=Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    const auto consistent=Need(db.getValidatedTip());
    Require(tip.height>=0&&consistent.hash==tip.hash&&consistent.height==tip.height);
    uint256 old_ancestor;uint32_t base_height=0;
    Require(headers.GetAncestorHashByHash(floor->block,uint32_t(tip.height),old_ancestor,base_height)&&
        base_height==floor->height&&old_ancestor==tip.hash);
    Require(Need(db.getOrchardState())==baseline.state&&Need(db.getLegacyRetirementState())==baseline.retirement&&
        db.getBlockHashByHeight(int(floor->height)+1).status()==Status::NotFound);
    const auto forest=Need(db.getForestTipMarker());const auto legacy=Need(db.getShieldedTipMarker());
    const auto header=Need(db.getHeader(floor->block));
    Require(forest.height==int(floor->height)&&forest.block_hash==floor->block&&forest.forest_root==header.utreexo_root&&
        legacy.height==int(floor->height)&&legacy.block_hash==floor->block&&legacy.shielded_root==baseline.retirement.record.tree_root&&
        legacy.tree_size==baseline.retirement.record.tree_size&&legacy.nullifier_count==baseline.retirement.record.nullifier_count);
    const bool compact=baseline.profile==OrchardSnapshotProfile::Compact;
    const auto accumulator=compact?baseline.base_catalog.stump:Need(db.getUtreexoCheckpoint(int(floor->height)));
    const auto actual=Capture(db,baseline.profile,accumulator);
    Require(HashOrchardSnapshotSection(actual)==HashOrchardSnapshotSection(baseline));
    Need(db.forEachOrchardNullifier([&](const auto&,const auto& owner){return owner==floor->block;}));
    if(compact)Require(ReadCoins(db).empty()&&storage::ReadOrchardCompactStorageBinding(db)==
        storage::EncodeOrchardCompactStorageBinding(baseline.base_catalog));
    else {storage::RequireFullOrchardStorage(db);VerifyOrchardSnapshotCoins(actual,ReadCoins(db));}
    // Only an exact, fully authenticated imported base may bridge a marker-
    // above-history-tip gap. Never rewind a post-base state or repair a subset.
    rocksdb::WriteBatch batch;
    Need(db.putHeightIndex(token,int(floor->height),floor->block,&batch));
    Need(db.setTip(token,floor->block,int(floor->height),baseline.base_catalog.work,&batch));
    Need(db.setValidatedTip(token,floor->block,int(floor->height),&batch));
    StageOrchardSnapshotJournal(context,header,baseline.base_catalog.work,baseline.state,
        UtreexoStump::deserialize(baseline.base_catalog.stump),batch);
    (void)Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    std::unique_lock<std::mutex> selection(headers.mutex_);const auto* best=headers.best_header_;
    const auto* selected_base=best?best->GetAncestor(floor->height):nullptr;
    Require(selected_base&&selected_base->hash==floor->block&&
        uint64_t(best->height)>=uint64_t(floor->height)+Params().state_commitment_burial_depth);
    Need(db.writeBatch(token,std::move(batch),true));return true;
}
OrchardSnapshotStore::Publication OrchardSnapshotStore::Restore(AnnotatedRecursiveMutex& mutex,const ChainDB& db,
    const consensus::HeaderChainSelector& headers) {
    using namespace consensus;mutex.AssertHeld("restore authenticated Orchard snapshot");
    const auto base=ReadBaseline(db);(void)Authenticate(base,headers,Params().state_commitment_burial_depth);
    const auto tip=Need(db.getTip()),consistent=Need(db.getValidatedTip());
    Require(tip.height>=int(base.state.height)&&tip.hash==consistent.hash&&tip.height==consistent.height);
    const auto selected=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(tip.hash)));
    const bool compact=base.profile==OrchardSnapshotProfile::Compact;std::vector<uint8_t> accumulator;
    if(compact)accumulator=selected.stump;
    else {
        UtreexoForest restored;std::string error;
        Need(storage::RestoreHistoricalForest(db,uint32_t(tip.height),restored,error,[&](uint32_t height,uint256& hash){
            uint32_t selected_height=0;
            return headers.GetAncestorHashByHash(tip.hash,height,hash,selected_height)&&selected_height==uint32_t(tip.height);
        }));accumulator=restored.serializeCanonicalLeaves(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
    }
    const auto current=Capture(db,base.profile,accumulator);(void)Authenticate(current,headers,0,false);
    uint256 base_ancestor;uint32_t selected_height=0;
    Require(headers.GetAncestorHashByHash(tip.hash,base.state.height,base_ancestor,selected_height)&&
        selected_height==uint32_t(tip.height)&&base_ancestor==base.state.block_hash);
    Require(current.state.height==uint32_t(tip.height)&&current.state.block_hash==tip.hash&&current.base_catalog.work==tip.work&&
        current.retirement.record==base.retirement.record);
    const auto forest=Need(db.getForestTipMarker());const auto legacy=Need(db.getShieldedTipMarker());
    const auto header=Need(db.getHeader(tip.hash));
    Require(forest.height==tip.height&&forest.block_hash==tip.hash&&forest.forest_root==header.utreexo_root&&
        legacy.height==tip.height&&legacy.block_hash==tip.hash&&legacy.shielded_root==current.retirement.record.tree_root&&
        legacy.tree_size==current.retirement.record.tree_size&&legacy.nullifier_count==current.retirement.record.nullifier_count);
    Publication publication;
    if(compact) {
        Require(ReadCoins(db).empty()&&storage::ReadOrchardCompactStorageBinding(db)==
            storage::EncodeOrchardCompactStorageBinding(selected));
        publication.compact=std::make_unique<OrchardCompactChainstate>();
        publication.compact->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
            &db,&mutex,selected,current.retirement.record);
    } else {storage::RequireFullOrchardStorage(db);publication.full=FullPublication(current,ReadCoins(db));}
    return publication;
}
} // namespace dinero
