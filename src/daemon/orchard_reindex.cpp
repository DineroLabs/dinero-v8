#include "daemon/orchard_reindex.h"
#include "daemon/orchard_chainstate_write.h"
#include "daemon/services/assumeutxo_replay.h"
#include "common/annotated_mutex.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "consensus/orchard_header.h"
#include "consensus/reindexer_detail.h"
#include "consensus/shielded/shielded_root.h"
#include "storage/chain_db.h"
#include "storage/shielded_migration.h"
#include "util/hex.h"
#include <rocksdb/write_batch.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <regex>
#include <stdexcept>

namespace dinero {
namespace {
using namespace consensus;
using Record = reindex_detail::DiskBlockRecord;
using Stats = BlockReindexer::Stats;
template<class T> T Need(StatusOr<T> value) {
    if (!value.ok()) throw std::runtime_error("Reindex required read: " + std::string(StatusToString(value.status())));
    return std::move(*value);
}
void Need(Status status) {
    if (status != Status::Ok) throw std::runtime_error("Reindex required write: " + std::string(StatusToString(status)));
}
void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Scratch {
    std::filesystem::path path;
    explicit Scratch(const std::filesystem::path& p):path(p) {
        Require(std::filesystem::create_directory(path), "Typed reindex scratch already exists");
    }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path,ec); }
};
struct ValidatedHeader {
    BlockHeader header;
    uint32_t height;
    arith_uint256 work;
    FilePosition pos;
};
using ValidatedHeaders = std::map<uint256,ValidatedHeader>;
std::vector<std::filesystem::path> BlockFiles(const std::filesystem::path& datadir) {
    std::vector<std::filesystem::path> files;
    const std::regex pattern("blk[0-9]{5}\\.dat");
    for (const auto& entry:std::filesystem::directory_iterator(datadir/"blocks")) {
        if (!std::regex_match(entry.path().filename().string(),pattern)) continue;
        Require(entry.is_regular_file() && !entry.is_symlink(), "Invalid reindex block file");
        files.push_back(entry.path());
    }
    std::sort(files.begin(),files.end());
    Require(!files.empty(), "Reindex block inventory is empty");
    return files;
}
std::vector<size_t> SelectedFrames(const std::vector<Record>& records,const uint256& tip) {
    return Need(reindex_detail::SelectCanonicalChain(records,tip.GetHex()));
}
const Record& ExactFrame(const std::vector<Record>& records,const uint256& hash,
                         const std::vector<uint8_t>& body) {
    const Record* found=nullptr;
    for (const auto& record:records) if(record.hash==hash) {
        Require(record.body==body,"Conflicting framed body for the same block hash");
        if(!found)found=&record;
    }
    Require(found!=nullptr,"Retained delivery body missing from block files");
    return *found;
}
Block Historical(const Record& record) {
    const auto block=Block::Deserialize(record.body.data(),record.body.size());
    Require(block.has_value() && block->GetHash()==record.hash &&
        block->header.SerializeForHash()==record.header.SerializeForHash(),"Invalid historical reindex body");
    return *block;
}
void Remember(ValidatedHeaders& known,const Record& record,uint32_t height,arith_uint256 work) {
    const auto [it,added]=known.emplace(record.hash,ValidatedHeader{record.header,height,work,record.pos});
    Require(added || (it->second.height==height && it->second.work==work &&
        it->second.header.SerializeForHash()==record.header.SerializeForHash()),"Validated reindex header conflict");
}
void StoreHeader(ChainDB& db,const ChainWriteToken& token,const ValidatedHeader& h) {
    const auto hash=h.header.GetHash();
    auto prior=db.getHeaderMetadata(hash);
    if(prior.ok()) {
        Require(prior->height==int32_t(h.height) && prior->parent_hash==h.header.prev_block_hash &&
            prior->chainwork==h.work && Need(db.getHeader(hash)).SerializeForHash()==h.header.SerializeForHash(),
            "Reindex retained header mismatch");
        return;
    }
    Require(prior.status()==Status::NotFound && h.pos.offset<=UINT32_MAX,"Reindex header unavailable or locator out of range");
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash=h.header.prev_block_hash;metadata.height=h.height;metadata.chainwork=h.work;
    metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA;
    metadata.file_number=h.pos.file_number;metadata.data_pos=uint32_t(h.pos.offset);metadata.data_size=h.pos.size;
    rocksdb::WriteBatch batch;
    Need(db.putHeader(token,hash,h.header,h.height,h.work,&batch));
    Need(db.putHeaderMetadata(token,hash,metadata,&batch));
    Need(db.writeBatch(token,std::move(batch),true));
}
// A rebuild after a below-activation transition is deliberately a full genesis
// replay. No historical inverse or valid-tip marker substitutes for validation.
std::unique_ptr<assumeutxo::AssumeUtxoReplayEngine> RebuildHistorical(
    ChainDB& candidate,const ChainWriteToken& token,BlockStorage& files,
    const std::filesystem::path& datadir,const std::filesystem::path& candidate_path,
    const BlockReindexer::Config& configuration,const std::vector<Record>& records,
    const uint256& target,uint32_t height,Scratch& scratch,size_t generation,
    ValidatedHeaders& known,Stats& stats) {
    Require(height<Params().orchard_activation_height,"Historical replay reached Orchard height");
    const auto path=SelectedFrames(records,target);
    Require(path.size()==height,"Historical target ancestry height mismatch");
    const auto genesis=std::find_if(records.begin(),records.end(),[](const auto& record) {
        return record.hash.GetHex()==Params().genesis_hash;
    });
    Require(genesis!=records.end(),"Canonical genesis frame missing");
    auto replay=std::make_unique<assumeutxo::AssumeUtxoReplayEngine>();
    std::string error;
    Require(replay->SeedGenesis(Historical(*genesis),error),"Canonical genesis validation failed");
    arith_uint256 work=GetBlockProof(genesis->header.difficulty);
    Remember(known,*genesis,0,work);
    uint32_t h=0;
    for(const auto index:path) {
        const auto& record=records.at(index);
        const auto block=Historical(record);
        if(!replay->ConnectAndAdvance(block,++h,record.hash,error))
            throw std::runtime_error("Historical consensus replay refused: "+error);
        work+=GetBlockProof(record.header.difficulty);
        Remember(known,record,h,work);
    }
    const auto prefix=scratch.path/("prefix-"+std::to_string(generation));
    const auto migrated=scratch.path/("migrated-"+std::to_string(generation));
    candidate.close();
    Need(candidate.init(prefix));
    auto config=configuration;
    config.mode=BlockReindexer::Mode::FULL;config.use_assumevalid=false;
    config.anchor_state.reset();config.undo_rebuild_window.reset();
    config.known_canonical_tip_hash=target;
    config.preserve_shielded_state_on_init=false;
    BlockReindexer writer(datadir,&candidate,&files,config);
    const auto written=Need(writer.execute());
    Require(written.success && written.canonical_truncated_at_height<0,"Historical candidate write did not complete exactly");
    const auto tip=Need(candidate.getTip());
    Require(tip.hash==target && tip.height==int32_t(height) && Need(candidate.getBlockWork(target))==work,
        "Historical candidate tip/work mismatch");
    const auto& proven=replay->ProvenUtxos();
    size_t count=0;
    bool equal=true;
    Need(candidate.forEachUTXO([&](const uint256& txid,uint32_t n,const Coin& coin) {
        const auto it=proven.find(OutPoint(TxId(txid),n));
        std::vector<uint8_t> script;
        if(it==proven.end() || coin.height<0 || !util::unhex(coin.script_pubkey,script)) { equal=false;return false; }
        const auto& p=it->second;
        equal=p.value.GetUna()==coin.amount && p.scriptPubKey==script && p.height==uint32_t(coin.height) &&
            p.isCoinbase==coin.coinbase && p.is_confidential==coin.is_confidential && p.commitment==coin.commitment;
        ++count;return equal;
    }));
    Require(equal && count==proven.size(),"Historical persisted coin set differs from independent replay");
    const auto marker=Need(candidate.getForestTipMarker());
    Require(marker.height==int32_t(height) && marker.block_hash==target &&
        marker.forest_root.GetHex()==replay->UtreexoRootHex(),"Historical persisted forest differs from replay");
    const auto frontier=replay->ShieldedTree()->SerializeFrontier();
    const auto anchors=replay->ShieldedAnchors()->SerializePersistenceBytes();
    Require(Need(candidate.getShieldedState(ChainDB::ShieldedStateRecord::Frontier))==std::string(frontier.begin(),frontier.end()) &&
        Need(candidate.getShieldedState(ChainDB::ShieldedStateRecord::AnchorHistory))==std::string(anchors.begin(),anchors.end()),
        "Historical persisted shielded state differs from replay");
    // Both iterators promise complete, ordered (height, nullifier) visits.
    // Compare actual records, including for a final tip below activation.
    using NullifierRow=std::pair<uint32_t,std::array<uint8_t,32>>;
    std::vector<NullifierRow> nullifiers;
    Require(replay->ShieldedNullifiers()->ForEach([&](uint32_t h,const uint8_t* bytes) {
        std::array<uint8_t,32> hash;std::copy_n(bytes,hash.size(),hash.begin());
        nullifiers.emplace_back(h,hash);return true;
    }),"Independent historical nullifier enumeration failed");
    size_t nullifier_index=0;bool nullifiers_equal=true;
    Need(candidate.forEachShieldedNullifier([&](uint32_t h,const uint8_t* bytes) {
        if(nullifier_index>=nullifiers.size() || nullifiers[nullifier_index].first!=h ||
            !std::equal(bytes,bytes+32,nullifiers[nullifier_index].second.begin())) {
            nullifiers_equal=false;return false;
        }
        ++nullifier_index;return true;
    }));
    Require(nullifiers_equal && nullifier_index==nullifiers.size(),"Historical persisted nullifiers differ from independent replay");
    Require(Need(candidate.getUtreexoCheckpoint(height))==replay->Forest()->serialize(),
        "Historical persisted forest checkpoint differs from independent replay");
    const auto check_header=[&](const uint256& hash) {
        const auto& v=known.at(hash);
        Require(Need(candidate.getHeader(hash)).SerializeForHash()==v.header.SerializeForHash() &&
            Need(candidate.getBlockHeight(hash))==int32_t(v.height) && Need(candidate.getBlockWork(hash))==v.work &&
            Need(candidate.getBlockHashByHeight(v.height))==hash,"Historical persisted ancestry differs from replay");
    };
    check_header(genesis->hash);
    for(const auto index:path)check_header(records[index].hash);
    rocksdb::WriteBatch checked;
    Need(candidate.setValidatedTip(token,target,height,&checked));
    Need(candidate.writeBatch(token,std::move(checked),true));
    candidate.close();
    Require(std::filesystem::create_directory(migrated),"Migration target already exists");
    for(const auto& entry:std::filesystem::recursive_directory_iterator(prefix))
        Require(!entry.is_symlink() && (entry.is_regular_file() || entry.is_directory()),"Non-regular prefix entry");
    std::filesystem::copy(prefix,migrated,std::filesystem::copy_options::recursive);
    // A single permitted record must fit in one bounded migration batch.
    // Keep the existing record/row/nullifier ceilings; budget the batch to
    // accommodate that record instead of supplying contradictory limits.
    constexpr uint64_t migration_record_bytes=32ULL*1024*1024;
    constexpr uint64_t migration_batch_bytes=migration_record_bytes;
    static_assert(migration_record_bytes<=migration_batch_bytes);
    const storage::ShieldedMigrationLimits migration_limits{
        migration_batch_bytes,4096,migration_record_bytes,10000000};
    const auto migration=storage::MigrateShieldedStateCopy(prefix,migrated,migration_limits,true);
    if(!migration.ok || !migration.ready)throw std::runtime_error("Reindex prefix migration refused: "+migration.error);
    // Only the unpublished candidate changes here. The original ChainDB and
    // DaemonApp's final promotion journal remain untouched.
    const auto old=scratch.path/("retired-"+std::to_string(generation));
    std::filesystem::rename(candidate_path,old);
    try { std::filesystem::rename(migrated,candidate_path); }
    catch(...) { std::filesystem::rename(old,candidate_path);throw; }
    Need(candidate.init(candidate_path));
    Require(candidate.hasSeparatedShieldedState(),"Reindex candidate layout unavailable");
    stats.blocks_processed+=height;
    for(const auto& [hash,v]:known) { (void)hash;StoreHeader(candidate,token,v); }
    return replay;
}
std::unique_ptr<HeaderChainSelector> Headers(const std::vector<Record>& records,const uint256& parent) {
    auto headers=std::make_unique<HeaderChainSelector>();
    const auto genesis=std::find_if(records.begin(),records.end(),[](const auto& r){return r.hash.GetHex()==Params().genesis_hash;});
    Require(genesis!=records.end() && headers->AddHeader(genesis->header),"Reindex genesis header unavailable");
    for(const auto index:SelectedFrames(records,parent))Require(headers->AddHeader(records[index].header),"Reindex ancestor header refused");
    return headers;
}
CBlockIndex Index(const ValidatedHeader& header,const ChainDB::PersistedHeaderMetadata& m) {
    CBlockIndex index(header.header,header.height);
    index.chainwork=header.work.GetHex();index.status=m.status_flags;
    index.file_number=m.file_number;index.data_pos=m.data_pos;index.data_size=m.data_size;
    index.undo_file=m.undo_file;index.undo_pos=m.undo_pos;index.undo_size=m.undo_size;
    return index;
}
} // namespace

StatusOr<consensus::BlockReindexer::Stats> OrchardReindexOwner::Run(
    const ChainDB& source,ChainDB& candidate,const ChainWriteToken& token,
    BlockStorage& files,const std::filesystem::path& datadir,
    const std::filesystem::path& candidate_path,const BlockReindexer::Config& config) {
    Stats stats;
    const auto start=std::chrono::steady_clock::now();
    try {
        const auto profile=SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);
        Require(profile.has_value(),"Orchard reindex profile unavailable");
        const auto first=ReadRuntimeOutboxUnderLock(source,*profile,{},1);
        Require(!first.events.empty(),"Orchard reindex requires retained delivery origin");
        const auto& origin=first.events.front();
        Require(origin.direction==RuntimeBlockDirection::Connect && origin.context.height==profile->activation_height &&
            origin.orchard_replay.has_value(),"Orchard reindex origin is incomplete");
        const auto source_tip=Need(source.getTip());
        const auto block_files=BlockFiles(datadir);
        auto records=Need(reindex_detail::ReadDiskBlocks(block_files,&stats));
        stats.files_scanned=block_files.size();
        Scratch scratch(candidate_path.string()+".typed-owner");
        // Close any open scratch-backed candidate before scratch cleanup.
        struct CloseOnExit {ChainDB& db;~CloseOnExit(){db.close();}} close{candidate};
        ValidatedHeaders known;
        size_t generation=0;
        auto replay=RebuildHistorical(candidate,token,files,datadir,candidate_path,config,records,
            origin.context.parent_hash,origin.context.height-1,scratch,generation++,known,stats);
        ConsensusUTXOSet live;
        Require(live.BulkLoad(replay->ProvenUtxos(),replay->Height(),origin.context.parent_hash),"Reindex coin owner initialization failed");
        live.ReplaceForestGuarded(*replay->Forest());
        AnnotatedRecursiveMutex mutex;
        RuntimeOutboxCursor verified;
        while(verified!=first.head) {
            const auto page=ReadRuntimeOutboxUnderLock(source,*profile,verified,1);
            Require(page.head==first.head && page.events.size()==1,"Reindex source changed or ended early");
            const auto& event=page.events.front();
            const auto& c=event.context;
            const bool connect=event.direction==RuntimeBlockDirection::Connect;
            const auto before_hash=connect?c.parent_hash:c.block_hash;
            const auto before_height=connect?c.height-1:c.height;
            const auto after_hash=connect?c.block_hash:c.parent_hash;
            const auto after_height=connect?c.height:c.height-1;
            const auto tip=Need(candidate.getTip());
            Require(tip.hash==before_hash && tip.height==int32_t(before_height) &&
                live.GetBestBlock()==before_hash && live.GetHeight()==before_height,"Reindex transition is not contiguous");
            const auto& frame=ExactFrame(records,c.block_hash,event.body);
            if(!event.IsOrchardProfile()) {
                // A disconnect body must already belong to the fully validated
                // current prefix. A connect is validated by the target replay.
                Require(connect || known.contains(c.block_hash),"Historical disconnect was not validated");
                replay=RebuildHistorical(candidate,token,files,datadir,candidate_path,config,records,
                    after_hash,after_height,scratch,generation++,known,stats);
                CopyValidatedOutboxPrefix(source,candidate,token,*profile,event.cursor,first.head);
                Require(live.BulkLoad(replay->ProvenUtxos(),after_height,after_hash),"Reindex historical coin reload failed");
                live.ReplaceForestGuarded(*replay->Forest());
            } else {
                Require(event.orchard_replay.has_value(),"Orchard reindex validation material missing");
                const auto body=OrchardBlockCandidate::DecodeExact(event.body);
                const bool witness=Params().enforce_witness_commitment && c.height>=Params().witness_commitment_enforcement_height;
                std::string error;
                Require(body.CheckSizeLimits(error) && body.CheckCoinbaseHeight(c.height,error) &&
                    body.CheckIdentityCommitments(witness,error),"Orchard reindex body checks failed");
                auto headers=Headers(records,c.parent_hash);
                const auto parent=Need(candidate.getHeader(c.parent_hash));
                const auto selected=SelectedOrchardBlockContext(body.Header(),c.height);
                Require(selected.has_value(),"Orchard reindex selected context unavailable");
                std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
                const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                CheckOrchardHeaderUnderChainstateLock(body.Header(),parent,*selected,*headers,now>0?uint64_t(now):0);
                const auto work=Need(candidate.getBlockWork(c.parent_hash))+GetBlockProof(body.Header().difficulty);
                Remember(known,frame,c.height,work);
                StoreHeader(candidate,token,known.at(c.block_hash));
                auto index=Index(known.at(c.block_hash),Need(candidate.getHeaderMetadata(c.block_hash)));
                const auto ancestry=SelectedFrames(records,c.parent_hash);
                const OrchardBranchMtpLookup mtp=[&](uint32_t h)->std::optional<uint64_t> {
                    if(h>=c.height)return {};
                    uint256 hash;
                    if(h==0) { if(!uint256::FromHex(Params().genesis_hash,hash))return {}; }
                    else { if(h>ancestry.size())return {};hash=records[ancestry[h-1]].hash; }
                    uint32_t time=0,height=0;
                    if(!headers->GetMedianTimePastByHash(hash,time,height) || height!=h)return {};
                    return time;
                };
                std::optional<storage::LegacyRetirementRecord> boundary;
                if(connect && c.height==c.activation_height) {
                    Require(replay && replay->Height()==c.height-1,"Activation requires independent historical replay");
                    boundary=DeriveSelectedLegacyRetirementUnderLock(candidate,&files,1000000);
                    const auto root=shielded::ComputeShieldedRoot(*replay->ShieldedTree(),*replay->ShieldedNullifiers(),*replay->ShieldedAnchors());
                    Require(root && boundary->legacy_state_root==*root && boundary->boundary_parent==c.parent_hash,
                        "Activation retirement differs from independently validated history");
                }
                const auto forest=live.GetForest();
                auto owner=connect ? PreparedOrchardChainstateWrite::ConnectIndexed(mutex,candidate,token,files,index,live,
                    *selected,body,parent,forest,mtp,witness,true,boundary,true) :
                    PreparedOrchardChainstateWrite::DisconnectIndexed(mutex,candidate,token,files,index,live,
                    *selected,body,parent,forest,witness);
                owner->Commit();
            }
            const auto reconstructed=ReadRuntimeOutboxUnderLock(candidate,*profile,verified,1);
            Require(reconstructed.head==event.cursor && reconstructed.events.size()==1 &&
                reconstructed.events.front().cursor==event.cursor && reconstructed.events.front().body==event.body,
                "Reconstructed delivery differs from retained source");
            const auto after=Need(candidate.getTip());
            Require(after.hash==after_hash && after.height==int32_t(after_height),"Reindex post-transition tip mismatch");
            verified=event.cursor;++stats.blocks_processed;
        }
        Require(ReadRuntimeOutboxUnderLock(source,*profile,verified,1).head==verified &&
            Need(source.getTip()).hash==source_tip.hash,"Reindex source changed before completion");
        const auto final=Need(candidate.getTip());
        const auto validated=Need(candidate.getValidatedTip());
        Require(final.hash==source_tip.hash && final.height==source_tip.height && validated.hash==final.hash &&
            validated.height==final.height && live.GetBestBlock()==final.hash && live.GetHeight()==uint32_t(final.height),
            "Reindex final selected state mismatch");
        rocksdb::WriteBatch finish;
        Need(candidate.putUtreexoCheckpointWithChecksum(token,final.height,live.GetForest().serialize(),&finish));
        // Retain operator invalidation policy only on independently replayed
        // inactive branches; it may not taint the validated final ancestry.
        const auto active=SelectedFrames(records,final.hash);
        uint256 genesis_hash;
        Require(uint256::FromHex(Params().genesis_hash,genesis_hash),"Invalid configured genesis identity");
        std::set<uint256> active_hashes{final.hash,genesis_hash};
        for(auto i:active)active_hashes.insert(records[i].hash);
        for(const auto& [hash,h]:known) {
            const auto original=source.getHeaderMetadata(hash);
            if(!original.ok()) {Require(original.status()==Status::NotFound,"Original header policy unavailable");continue;}
            Require(original->height==int32_t(h.height) && original->chainwork==h.work && original->parent_hash==h.header.prev_block_hash,
                "Original header policy identity mismatch");
            const auto failed=original->status_flags&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD);
            Require(!failed || !active_hashes.contains(hash),"Original invalidation conflicts with final chain");
            auto metadata=Need(candidate.getHeaderMetadata(hash));metadata.status_flags|=failed;
            Need(candidate.putHeaderMetadata(token,hash,metadata,&finish));
        }
        Need(candidate.writeBatch(token,std::move(finish),true));
        stats.success=true;
    } catch(const std::exception& e) {stats.error=e.what();stats.success=false;candidate.close();}
    stats.duration_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    // DaemonApp uses the open candidate for its existing post-reindex checks.
    if(stats.success) {const auto status=candidate.init(candidate_path);if(status!=Status::Ok)return status;}
    return stats;
}
} // namespace dinero
