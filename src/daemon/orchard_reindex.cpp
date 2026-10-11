#include "storage/archival_block_reader.h"
#include "storage/historical_compact_validation.h"
#include "daemon/services/historical_catalog.h"
#include "storage/orchard_storage_mode.h"
#include "daemon/orchard_reindex.h"
#include "daemon/orchard_chainstate_write.h"
#include "daemon/services/assumeutxo_replay.h"
#include "daemon/services/orchard_parent_catalog.h"
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
#include <fstream>
#include <map>
#include <set>
#include <regex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

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
// The startup owner has exclusive access to an unpublished candidate. Build
// the complete parent outside its selected mutex, using exact hash-linked
// archival frames and independently validated header/work records. Neither a
// copied source catalog nor a transaction index is a validation authority.
struct ReindexCatalogParent {
    OrchardParentReplay replay;
    OrchardHistoryCapture history;
    explicit ReindexCatalogParent(OrchardParentReplay::Target target)
        :replay(target,SelectedParentReplayWorkLimits()),history(target.height,target.hash) {}
};
std::unique_ptr<ReindexCatalogParent> PrepareCatalogParent(
    const std::vector<Record>& records,const ValidatedHeaders& known,
    const uint256& hash,uint32_t height,const assumeutxo::AssumeUtxoReplayEngine& expected) {
    const auto& target=known.at(hash);
    Require(target.height==height && expected.Height()==height,"Reindex catalog parent height mismatch");
    auto result=std::make_unique<ReindexCatalogParent>(OrchardParentReplay::Target{height,hash,target.work});
    const auto path=SelectedFrames(records,hash);
    Require(path.size()==height,"Reindex catalog ancestry size mismatch");
    const auto genesis=std::find_if(records.begin(),records.end(),[](const auto& r){return r.hash.GetHex()==Params().genesis_hash;});
    Require(genesis!=records.end(),"Reindex catalog genesis frame missing");
    const auto frame_at=[&](uint32_t h)->const Record& {return h?records.at(path.at(h-1)):*genesis;};
    std::vector<OrchardHistoryCapture::Header> page;
    page.reserve(OrchardHistoryCapture::HeaderBatchLimit);
    for(uint64_t next=uint64_t(height)+1;next;--next) {
        const auto h=uint32_t(next-1);const auto& frame=frame_at(h);const auto& header=known.at(frame.hash);
        Require(header.height==h && header.header.SerializeForHash()==frame.header.SerializeForHash(),
            "Reindex catalog captured header mismatch");
        page.push_back({frame.hash,header.header,header.work});
        if(page.size()==OrchardHistoryCapture::HeaderBatchLimit || next==1){result->history.CaptureReverse(page);page.clear();}
    }
    for(uint64_t h=0;h<=height;++h) {
        const auto& frame=frame_at(uint32_t(h));const auto body=Historical(frame);
        result->replay.Append(body,uint32_t(h),known.at(frame.hash).work);
        result->history.RecordBody(uint32_t(h),body);
    }
    result->history.Finish();result->replay.Finish();
    const auto& proven=result->replay.ProvenState();
    Require(proven.Forest() && expected.Forest() && proven.Forest()->serialize()==expected.Forest()->serialize(),
        "Reindex catalog verification forest mismatch");
    const auto& coins=proven.ProvenUtxos();const auto& original=expected.ProvenUtxos();
    Require(coins.size()==original.size(),"Reindex catalog coin inventory mismatch");
    for(const auto& [point,coin]:original) {
        const auto it=coins.find(point);Require(it!=coins.end(),"Reindex catalog coin missing");const auto& p=it->second;
        Require(p.value==coin.value && p.scriptPubKey==coin.scriptPubKey && p.height==coin.height &&
            p.isCoinbase==coin.isCoinbase && p.is_confidential==coin.is_confidential && p.commitment==coin.commitment,
            "Reindex catalog coin binding mismatch");
    }
    return result;
}
struct ReindexHistoricalCatalog {
    HistoricalCompactReplay replay;
    OrchardHistoryCapture history;
    explicit ReindexHistoricalCatalog(HistoricalCompactReplay::Target target)
        :replay(target,HistoricalCompactReplay::Limits{SelectedParentReplayWorkLimits().blocks,SelectedParentReplayWorkLimits().serialized_bytes}),history(target.height,target.hash) {}
};
std::unique_ptr<ReindexHistoricalCatalog> PrepareHistoricalCatalog(
    const std::vector<Record>& records,const ValidatedHeaders& known,
    const uint256& hash,uint32_t height,const assumeutxo::AssumeUtxoReplayEngine& expected) {
    const auto& target=known.at(hash);
    Require(target.height==height && expected.Height()==height,"Reindex catalog parent height mismatch");
    auto result=std::make_unique<ReindexHistoricalCatalog>(HistoricalCompactReplay::Target{height,hash,target.work});
    const auto path=SelectedFrames(records,hash);
    Require(path.size()==height,"Reindex catalog ancestry size mismatch");
    const auto genesis=std::find_if(records.begin(),records.end(),[](const auto& r){return r.hash.GetHex()==Params().genesis_hash;});
    Require(genesis!=records.end(),"Reindex catalog genesis frame missing");
    const auto frame_at=[&](uint32_t h)->const Record& {return h?records.at(path.at(h-1)):*genesis;};
    std::vector<OrchardHistoryCapture::Header> page;
    page.reserve(OrchardHistoryCapture::HeaderBatchLimit);
    for(uint64_t next=uint64_t(height)+1;next;--next) {
        const auto h=uint32_t(next-1);const auto& frame=frame_at(h);const auto& header=known.at(frame.hash);
        Require(header.height==h && header.header.SerializeForHash()==frame.header.SerializeForHash(),
            "Reindex catalog captured header mismatch");
        page.push_back({frame.hash,header.header,header.work});
        if(page.size()==OrchardHistoryCapture::HeaderBatchLimit || next==1){result->history.CaptureReverse(page);page.clear();}
    }
    for(uint64_t h=0;h<=height;++h) {
        const auto& frame=frame_at(uint32_t(h));const auto body=Historical(frame);
        result->replay.Append(body,uint32_t(h),known.at(frame.hash).work);
        result->history.RecordBody(uint32_t(h),body);
    }
    result->history.Finish();result->replay.Finish();
    const auto& proven=result->replay.ProvenState();
    Require(proven.Forest() && expected.Forest() && proven.Forest()->serialize()==expected.Forest()->serialize(),
        "Reindex catalog verification forest mismatch");
    const auto& coins=proven.ProvenUtxos();const auto& original=expected.ProvenUtxos();
    Require(coins.size()==original.size(),"Reindex catalog coin inventory mismatch");
    for(const auto& [point,coin]:original) {
        const auto it=coins.find(point);Require(it!=coins.end(),"Reindex catalog coin missing");const auto& p=it->second;
        Require(p.value==coin.value && p.scriptPubKey==coin.scriptPubKey && p.height==coin.height &&
            p.isCoinbase==coin.isCoinbase && p.is_confidential==coin.is_confidential && p.commitment==coin.commitment,
            "Reindex catalog coin binding mismatch");
    }
    return result;
}
struct CompactSelectedIdentity {
    uint32_t height;uint256 block,genesis;arith_uint256 work;std::string bytes;bool historical;
};
template<class State> CompactSelectedIdentity CompactIdentity(const State& state) {
    return {state.height,state.block,state.genesis,state.work,state.Encode(),
        std::is_same_v<State,storage::catalog::HistoricalState>};
}
CompactSelectedIdentity CompactIdentityUnderLock(const OrchardCompactChainstate& owner,AnnotatedRecursiveMutex& mutex) {
    const auto* orchard=owner.SelectedUnderLock(mutex);const auto* historical=owner.HistoricalUnderLock(mutex);
    Require(bool(orchard)!=bool(historical),"Compact selected owner is absent or ambiguous");
    return historical?CompactIdentity(*historical):CompactIdentity(*orchard);
}
CBlockIndex Index(const ValidatedHeader& header,const ChainDB::PersistedHeaderMetadata& m) {
    CBlockIndex index(header.header,header.height);
    index.chainwork=header.work.GetHex();index.status=m.status_flags;
    index.file_number=m.file_number;index.data_pos=m.data_pos;index.data_size=m.data_size;
    index.undo_file=m.undo_file;index.undo_pos=m.undo_pos;index.undo_size=m.undo_size;
    return index;
}
} // namespace

OrchardCompactStartup::OrchardCompactStartup()=default;
OrchardCompactStartup::~OrchardCompactStartup()=default;

// The existing global objects retain their addresses. Preparation owns copies
// of values and membership, never a replacement live graph. All allocation,
// source checks and graph conflicts precede the no-throw publication below.
std::vector<CBlockIndex*> OrchardCompactStartup::PublishIndexUnderLock() {
    Require(source_ && mutex_ && owner_,"Compact index publication lacks reconstructed owner");
    mutex_->AssertHeld("Compact index publication");
    const auto selected=CompactIdentityUnderLock(*owner_,*mutex_);
    Require(!selected_index_.empty() &&
        selected_index_.size()==selected_headers_.size() &&
        selected_undo_.size()==selected_headers_.size(),"Compact index handoff incomplete or consumed");
    const auto tip=Need(source_->getTip()),validated=Need(source_->getValidatedTip());
    Require(tip.height==int64_t(selected.height) && tip.hash==selected.block &&
        tip.work==selected.work && validated.hash==tip.hash && validated.height==tip.height &&
        (selected.historical?Need(source_->getHistoricalCompactCatalogState(tip.hash)):Need(source_->getOrchardCatalogState(tip.hash)))==selected.bytes,
        "Compact index source differs from reconstructed owner");
    std::lock_guard<std::recursive_mutex> lock(g_block_index_mutex);
    using Index=decltype(g_block_index);
    Index staged_index;
    staged_index.reserve(g_block_index.size()+selected_index_.size());
    std::unordered_map<uint256,CBlockIndex*> pointers;
    pointers.reserve(g_block_index.size()+selected_index_.size());
    std::unordered_map<CBlockIndex*,CBlockIndex> values;
    values.reserve(g_block_index.size()+selected_index_.size());
    // Destination unique_ptr slots are allocated before any live ownership moves.
    std::vector<std::pair<std::unique_ptr<CBlockIndex>*,std::unique_ptr<CBlockIndex>*>> transfers;
    transfers.reserve(g_block_index.size());
    for(auto& [hash,node]:g_block_index) {
        Require(node && node->hash==hash,"Compact index has conflicting stored identity");
        Require(pointers.emplace(hash,node.get()).second,"Compact index duplicate identity");
        values.emplace(node.get(),*node);
        auto [slot,inserted]=staged_index.emplace(hash,nullptr);
        Require(inserted,"Compact index duplicate ownership slot");
        transfers.emplace_back(&node,&slot->second);
    }
    std::vector<CBlockIndex*> result;result.reserve(selected_index_.size());
    std::unordered_set<CBlockIndex*> authenticated;
    authenticated.reserve(selected_index_.size());
    for(size_t height=0;height<selected_index_.size();++height) {
        const auto& proved=*selected_index_[height];
        const auto& h=selected_headers_[height];
        Require(proved.hash==h.header.GetHash() && proved.height==height && h.height==height &&
            proved.chainwork==h.work.GetHex(),"Compact index reconstructed identity mismatch");
        auto found=pointers.find(proved.hash);CBlockIndex* live=nullptr;
        if(found==pointers.end()) {
            auto node=std::make_unique<CBlockIndex>(proved);
            // Links will use permanent objects, never temporary proof nodes.
            node->pprev=nullptr;node->children.clear();live=node.get();
            staged_index.emplace(proved.hash,std::move(node));
            pointers.emplace(proved.hash,live);values.emplace(live,*live);
        } else live=found->second;
        auto& v=values.at(live);
        Require(v.hash==proved.hash && v.prev_hash==proved.prev_hash &&
            v.height==proved.height && v.version==proved.version && v.merkle_root==proved.merkle_root &&
            v.timestamp==proved.timestamp && v.bits==proved.bits && v.nonce==proved.nonce &&
            !(v.status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD)),"Compact index existing header conflicts with replay");
        if(v.status&BLOCK_HAVE_DATA)
            Require(v.file_number==h.file_number && v.data_pos==h.data_pos && v.data_size==h.data_size,
                "Compact index existing body locator conflicts with replay");
        const auto& undo=selected_undo_[height];
        if(v.status&BLOCK_HAVE_UNDO)
            Require(undo && v.undo_file==undo->file_number && v.undo_pos==undo->data_pos &&
                v.undo_size==undo->data_size,"Compact index existing undo lacks matching authentication");
        v.chainwork=proved.chainwork;v.status|=proved.status;
        v.file_number=h.file_number;v.data_pos=h.data_pos;v.data_size=h.data_size;
        if(undo) {v.undo_file=undo->file_number;v.undo_pos=undo->data_pos;v.undo_size=undo->data_size;v.status|=BLOCK_HAVE_UNDO;}
        authenticated.insert(live);result.push_back(live);
    }
    Require(result.back()->hash==selected.block && result.back()->height==selected.height,
        "Compact index selected tip mismatch");
    auto candidates=g_candidates;auto orphans=g_orphan_pool;
    std::unordered_set<CBlockIndex*> queued;
    for(const auto& [parent,children]:orphans)
        for(auto* child:children) {
            Require(child && values.contains(child) && values.at(child).prev_hash==parent,
                "Compact index orphan membership conflicts with graph");queued.insert(child);
        }
    for(auto* candidate:candidates.Snapshot())
        Require(values.contains(candidate),"Compact index candidate is not owned by graph");
    std::vector<CBlockIndex*> order;order.reserve(values.size());
    for(const auto& [node,v]:values) {
        Require(!v.pprev || (values.contains(v.pprev) && values.at(v.pprev).hash==v.prev_hash),
            "Compact index existing parent pointer conflicts with graph");
        std::unordered_set<CBlockIndex*> children;
        for(auto* child:v.children)
            Require(child && values.contains(child) && values.at(child).prev_hash==v.hash &&
                children.insert(child).second,"Compact index existing child pointer conflicts with graph");
        order.push_back(node);
    }
    std::sort(order.begin(),order.end(),[&](auto* a,auto* b){
        const auto& x=values.at(a);const auto& y=values.at(b);
        return x.height!=y.height?x.height<y.height:x.hash<y.hash;
    });
    for(auto* node:order) {
        auto& v=values.at(node);const auto parent=pointers.find(v.prev_hash);
        if(!v.prev_hash.IsNull() && parent!=pointers.end()) {
            auto* p=parent->second;auto& pv=values.at(p);
            Require(uint64_t(pv.height)+1==v.height,"Compact index parent height mismatch");
            if(!v.pprev)v.pprev=p;
            Require(v.pprev==p,"Compact index parent is not permanent object");
            if(std::find(pv.children.begin(),pv.children.end(),node)==pv.children.end())pv.children.push_back(node);
            // Preserve existing orphan reconnection semantics, without granting
            // any new body/script/undo authority to a competing branch.
            v.chainwork=chainwork::AddWork(pv.chainwork,chainwork::WorkForBits(v.bits));
            if(queued.contains(node) && (pv.status&BLOCK_VALID_CHAIN))v.status|=BLOCK_VALID_CHAIN;
        } else Require(!v.pprev,"Compact index parent pointer has no owned identity");
        if(authenticated.contains(node))
            Require(v.chainwork==selected_headers_[v.height].work.GetHex() &&
                (v.height?v.pprev==result[v.height-1]:!v.pprev),"Compact index selected ancestry differs from replay");
        if((authenticated.contains(node)||queued.contains(node)) && IsEligibleForCandidacy(v.status)) {
            if(v.pprev)candidates.erase(v.pprev);candidates.insert(node);
        }
    }
    for(auto it=orphans.begin();it!=orphans.end();) {
        auto& children=it->second;
        children.erase(std::remove_if(children.begin(),children.end(),[&](auto* child){
            const auto& v=values.at(child);
            return v.pprev && (values.at(v.pprev).status&BLOCK_VALID_CHAIN);
        }),children.end());
        if(children.empty())it=orphans.erase(it);else ++it;
    }
    // All references and destination slots are final before the first mutation.
    // Cache invalidation can acquire only the already-held recursive inner lock.
    // It precedes the no-throw phase and never grants graph authority.
    InvalidateAncestryCache();
    static_assert(std::is_nothrow_swappable_v<CBlockIndex>);
    static_assert(noexcept(staged_index.swap(g_block_index)));
    static_assert(noexcept(orphans.swap(g_orphan_pool)));
    for(auto& [node,value]:values) {using std::swap;swap(*node,value);}
    for(auto& [from,to]:transfers)*to=std::move(*from);
    staged_index.swap(g_block_index);candidates.Swap(g_candidates);orphans.swap(g_orphan_pool);
    selected_index_.clear();
    return result;
}


// This proof result is private to the reconstruction owner. Callers cannot
// manufacture it from persisted metadata or carry it across database promotion.
struct OrchardReindexOwner::CompactReconstruction {
    const BlockStorage& source_files;
    std::optional<storage::LegacyRetirementRecord> retirement;
    std::optional<UtreexoForest> forest;
    std::optional<storage::catalog::HistoricalState> historical;
    std::set<uint256> checked_nodes;
    const bool retain_selected_headers;
    std::vector<OrchardCompactStartup::SelectedHeader> selected_headers;
    std::map<uint256,OrchardCompactStartup::RetainedUndoLocation> checked_undo;
    explicit CompactReconstruction(const BlockStorage& files,bool retain_headers):
        source_files(files),retain_selected_headers(retain_headers) {}
    void Catalog(const ChainDB& source,const ChainDB& candidate,const uint256& hash) {
        namespace c=storage::catalog;
        const auto expected=Need(candidate.getOrchardCatalogState(hash));
        Require(Need(source.getOrchardCatalogState(hash))==expected,
            "Compact startup catalog differs from independent reconstruction");
        CatalogValue(source,candidate,c::State::Decode(expected));
    }
    void HistoricalCatalog(const ChainDB& source,const ChainDB& candidate,const storage::catalog::HistoricalState& state) {
        Require(Need(source.getHistoricalCompactCatalogState(state.block))==state.Encode(),
            "Compact historical catalog differs from independently completed replay");
        CatalogValue(source,candidate,state);
    }
    template<class State> void CatalogValue(const ChainDB& source,const ChainDB& candidate,const State& state) {
        namespace c=storage::catalog;
        std::vector<std::pair<uint256,c::Kind>> pending;
        for(const auto& item:{std::pair{state.transactions,c::Kind::Transactions},
            std::pair{state.legacy,c::Kind::LegacyCoins},std::pair{state.nontransparent,c::Kind::NonTransparentCoins}})
            if(!item.first.IsNull())pending.push_back(item);
        while(!pending.empty()) {
            const auto [id,kind]=pending.back();pending.pop_back();
            if(!checked_nodes.insert(id).second)continue;
            const auto bytes=Need(candidate.getOrchardCatalogNode(id));
            Require(Need(source.getOrchardCatalogNode(id))==bytes,
                "Compact startup reachable catalog node differs from replay");
            const auto node=c::Node::Decode(bytes);
            Require(node.kind==kind,"Compact startup catalog kind mismatch");
            if(node.bit!=256) {pending.emplace_back(node.zero,kind);pending.emplace_back(node.one,kind);}
        }
    }
    void Connected(const ChainDB& source,const ChainDB& candidate,const OrchardBlockContext& c) {
        Catalog(source,candidate,c.parent_hash);Catalog(source,candidate,c.block_hash);
        const auto expected=Need(candidate.getUndo(c.block_hash)).Serialize();
        Require(Need(source.getUndo(c.block_hash)).Serialize()==expected,
            "Compact startup retained undo differs from replay");
        const auto m=Need(source.getHeaderMetadata(c.block_hash));
        Require((m.status_flags&(BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO))==(BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO) &&
            m.undo_size && Need(source_files.readUndo({m.undo_file,m.undo_pos,m.undo_size}))==expected,
            "Compact startup archival undo differs from replay");
        if(retain_selected_headers)
            checked_undo[c.block_hash]={m.undo_file,m.undo_pos,m.undo_size};
        // Orchard undo belongs to the currently selected chain. Disconnect
        // deletes it, while the outbox retains the historical connect. Compare
        // those owners against the final replayed ancestry in RestoreCompact.
    }
};

StatusOr<consensus::BlockReindexer::Stats> OrchardReindexOwner::Run(
    const ChainDB& source,ChainDB& candidate,const ChainWriteToken& token,
    BlockStorage& files,const std::filesystem::path& datadir,
    const std::filesystem::path& candidate_path,const BlockReindexer::Config& config) {
    return Reconstruct(source,candidate,token,files,datadir,candidate_path,config,nullptr);
}

StatusOr<consensus::BlockReindexer::Stats> OrchardReindexOwner::Reconstruct(
    const ChainDB& source,ChainDB& candidate,const ChainWriteToken& token,
    BlockStorage& files,const std::filesystem::path& datadir,
    const std::filesystem::path& candidate_path,const BlockReindexer::Config& config,
    CompactReconstruction* compact) {
    Stats stats;
    const auto start=std::chrono::steady_clock::now();
    try {
        if(!compact)storage::RequireFullOrchardStorage(source);
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
                std::unique_ptr<ReindexCatalogParent> catalog_parent;
                std::unique_ptr<PreparedOrchardCatalog> catalog;
                if(connect && c.height==c.activation_height) {
                    Require(replay && replay->Height()==c.height-1,"Activation requires independent historical replay");
                    catalog_parent=PrepareCatalogParent(records,known,c.parent_hash,c.height-1,*replay);
                    catalog=PreparedOrchardCatalog::Create(candidate,token,catalog_parent->replay,catalog_parent->history);
                }
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
                    Require(catalog_parent && catalog && catalog_parent->replay.Record()==*boundary,
                        "Reindex catalog retirement differs from selected state");
                    const auto root=shielded::ComputeShieldedRoot(*replay->ShieldedTree(),*replay->ShieldedNullifiers(),*replay->ShieldedAnchors());
                    Require(root && boundary->legacy_state_root==*root && boundary->boundary_parent==c.parent_hash,
                        "Activation retirement differs from independently validated history");
                    if(compact)compact->retirement=*boundary;
                }
                const auto forest=live.GetForest();
                auto owner=connect ? PreparedOrchardChainstateWrite::ConnectIndexed(mutex,candidate,token,files,index,live,
                    *selected,body,parent,forest,mtp,witness,true,boundary,true,nullptr,catalog.get(),true) :
                    PreparedOrchardChainstateWrite::DisconnectIndexed(mutex,candidate,token,files,index,live,
                    *selected,body,parent,forest,witness,true);
                owner->Commit();
                if(compact && connect)compact->Connected(source,candidate,c);
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
        if(compact) {
            // Bind every validated archival body to the ORIGINAL locator domain.
            // Scratch offsets are never accepted as source offsets.
            for(const auto& [hash,h]:known) {
                const auto m=Need(source.getHeaderMetadata(hash));
                Require(Need(source.getHeader(hash)).SerializeForHash()==h.header.SerializeForHash() &&
                    m.height==int32_t(h.height) && m.parent_hash==h.header.prev_block_hash && m.chainwork==h.work &&
                    (m.status_flags&BLOCK_HAVE_DATA) && m.data_size,
                    "Compact startup source header/locator mismatch");
                const auto raw=Need(compact->source_files.readBlockBytes({m.file_number,m.data_pos,m.data_size}));
                (void)ExactFrame(records,hash,std::vector<uint8_t>(raw.begin(),raw.end()));
                if(active_hashes.contains(hash)) {
                    Require(Need(source.getBlockHashByHeight(h.height))==hash,
                        "Compact startup selected height index mismatch");
                    // h belongs to independent validation; m and its ORIGINAL
                    // archive location were bound to that exact body above.
                    if(compact->retain_selected_headers)
                        compact->selected_headers.push_back({h.header,h.height,h.work,
                            m.file_number,m.data_pos,m.data_size});
                }
            }
            if(compact->retain_selected_headers) {
                auto& headers=compact->selected_headers;
                std::sort(headers.begin(),headers.end(),[](const auto& a,const auto& b) {
                    return a.height<b.height;
                });
                Require(final.height>=0 && headers.size()==uint64_t(final.height)+1,
                    "Compact startup selected header inventory incomplete");
                for(size_t height=0;height<headers.size();++height) {
                    const auto& entry=headers[height];
                    Require(entry.height==height &&
                        (height?entry.header.prev_block_hash==headers[height-1].header.GetHash():
                            entry.header.GetHash()==genesis_hash),
                        "Compact startup selected header inventory is not contiguous");
                }
                Require(headers.back().header.GetHash()==final.hash &&
                    headers.back().work==final.work,
                    "Compact startup selected header tip mismatch");
            }
            if(uint64_t(final.height)+1<profile->activation_height) {
                Require(replay&&replay->Height()==uint32_t(final.height),"Historical startup proof missing");
                const auto prepared=PrepareHistoricalCatalog(records,known,final.hash,uint32_t(final.height),*replay);
                const auto catalog=PreparedHistoricalCatalog::Create(candidate,token,prepared->replay,prepared->history);
                compact->historical=catalog->State();
                compact->HistoricalCatalog(source,candidate,*compact->historical);
                // Check the actual conventional transaction index for every
                // independently validated selected transaction, including genesis.
                for(const auto& [hash,h]:known)if(active_hashes.contains(hash)) {
                    const auto& frame=ExactFrame(records,hash,[&]{
                        const auto body=Need(storage::ReadArchivalBlock(candidate,&files,hash));const auto wire=body.Serialize();
                        return std::vector<uint8_t>(wire.begin(),wire.end());}());
                    const auto body=Historical(frame);Require(body.vtx.size()<=UINT32_MAX,"Historical transaction index overflow");
                    for(size_t n=0;n<body.vtx.size();++n) {
                        const auto id=body.vtx[n].GetTxid().AsUint256();
                        const auto expected=candidate.getTxLocation(id),original=source.getTxLocation(id);
                        // Both established genesis writers omit its tx-index entry.
                        // Catalog membership still includes the actual genesis tx.
                        if(h.height==0)Require(expected.status()==Status::NotFound&&original.status()==Status::NotFound,
                            "Historical genesis transaction index differs from its writer contract");
                        else Require(expected.ok()&&original.ok()&&*expected==*original&&
                            expected->first==hash&&expected->second==uint32_t(n),
                            "Historical selected transaction index differs from replay");
                    }
                }
            }
            if(uint64_t(final.height)+1==profile->activation_height) {
                // A historical rebuild replaces the unpublished candidate and
                // therefore discards its earlier boundary catalog. Reconstruct
                // that exact parent from independently validated bodies again;
                // never copy the source catalog or activate Orchard to fill it.
                Require(replay&&replay->Height()==uint32_t(final.height),
                    "Boundary startup historical proof missing");
                const auto prepared=PrepareCatalogParent(records,known,final.hash,uint32_t(final.height),*replay);
                const auto catalog=PreparedOrchardCatalog::Create(candidate,token,prepared->replay,prepared->history);
                catalog->Check();
                const auto& record=prepared->replay.Record();
                Require(record==DeriveSelectedLegacyRetirementUnderLock(candidate,&files,1000000),
                    "Boundary startup retirement differs from independent replay");
                storage::catalog::State state;
                state.network=record.network_code;state.genesis=record.genesis;
                state.branch=record.branch_id;state.activation=record.activation_height;
                state.leaf_activation=catalog->leaf_activation_;state.height=catalog->target_.height;
                state.block=catalog->target_.hash;state.parent=Need(candidate.getHeader(final.hash)).prev_block_hash;
                state.work=catalog->target_.chainwork;
                state.transactions=catalog->transactions_;state.legacy=catalog->legacy_;state.nontransparent=catalog->nontransparent_;
                state.transaction_count=catalog->transaction_count_;state.legacy_count=catalog->legacy_count_;
                state.nontransparent_count=catalog->nontransparent_count_;state.stump=catalog->stump_;
                const auto bytes=state.Encode();const auto prior=candidate.getOrchardCatalogState(final.hash);
                Require(prior.ok()?*prior==bytes:prior.status()==Status::NotFound,
                    "Boundary startup existing catalog differs from replay");
                if(!prior.ok()) {
                    rocksdb::WriteBatch catalog_batch;
                    Need(candidate.stageOrchardCatalogState(token,final.hash,bytes,catalog_batch));
                    Need(candidate.writeBatch(token,std::move(catalog_batch),true));
                }
                compact->retirement=record;
                compact->Catalog(source,candidate,final.hash);
            }
            compact->forest=live.GetForest();
        }
        stats.success=true;
    } catch(const std::exception& e) {stats.error=e.what();stats.success=false;candidate.close();}
    stats.duration_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    // DaemonApp uses the open candidate for its existing post-reindex checks.
    if(stats.success) {const auto status=candidate.init(candidate_path);if(status!=Status::Ok)return status;}
    return stats;
}
namespace {
uint256 StartupFileDigest(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    Require(input.is_open(),"Compact startup archive unavailable");
    crypto::CSHA256 digest;std::array<uint8_t,65536> buffer;
    while(input) {
        input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());
        if(input.gcount())digest.Write(buffer.data(),size_t(input.gcount()));
    }
    Require(input.eof()&&!input.bad(),"Compact startup archive read failed");
    uint256 result;digest.Finalize(result.data);return result;
}
}
std::unique_ptr<OrchardCompactChainstate> OrchardReindexOwner::RestoreCompact(
    AnnotatedRecursiveMutex& mutex,ChainDB& source,const ChainWriteToken& token,
    const BlockStorage& source_files,const std::filesystem::path& datadir,
    const std::filesystem::path& scratch_path) {
    auto restored=RestoreCompactPrepared(mutex,source,token,source_files,datadir,scratch_path,false);
    return std::move(restored->owner_);
}

std::unique_ptr<OrchardCompactStartup> OrchardReindexOwner::RestoreCompactPrepared(
    AnnotatedRecursiveMutex& mutex,ChainDB& source,const ChainWriteToken& token,
    const BlockStorage& source_files,const std::filesystem::path& datadir,
    const std::filesystem::path& scratch_path,bool retain_selected_headers) {
    std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
    const auto mode=storage::ReadOrchardCompactStorageBinding(source);
    Require(mode.has_value(),"Compact startup requires an existing storage binding");
    const auto tip=Need(source.getTip()),validated=Need(source.getValidatedTip());
    const auto profile=SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);
    Require(profile.has_value() && tip.height>=0,"Compact startup selected profile or height unavailable");
    const bool historical=uint64_t(tip.height)+1<profile->activation_height;
    Require(validated.hash==tip.hash&&validated.height==tip.height,"Compact startup selected tip is not validated");
    const auto selected_bytes=historical?Need(source.getHistoricalCompactCatalogState(tip.hash)):
        Need(source.getOrchardCatalogState(tip.hash));
    using SelectedCatalog=std::variant<storage::catalog::State,storage::catalog::HistoricalState>;
    const SelectedCatalog selected=historical?SelectedCatalog{storage::catalog::HistoricalState::Decode(selected_bytes)}:
        SelectedCatalog{storage::catalog::State::Decode(selected_bytes)};
    const auto identity=std::visit([](const auto& state){return CompactIdentity(state);},selected);
    Require(*mode==std::visit([](const auto& state){return storage::EncodeOrchardCompactStorageBinding(state);},selected),
        "Compact startup storage binding mismatch");
    const auto head=ReadRuntimeOutboxUnderLock(source,*profile,{},1).head;
    // No caller-supplied reindex config: every writable destination, including
    // shielded sidecars and block/undo outputs, belongs to this new scratch.
    Scratch scratch(scratch_path);
    const auto input=scratch.path/"archives";
    Require(std::filesystem::create_directories(input/"blocks"),"Compact startup scratch archive creation failed");
    const auto original_files=BlockFiles(datadir);
    std::map<std::filesystem::path,uint256> digests;
    for(const auto& path:original_files) {
        const auto id=uint32_t(std::stoul(path.stem().string().substr(3)));
        Require(std::filesystem::equivalent(path,source_files.getBlockFilePath(id)),
            "Compact startup archive reader domain mismatch");
        const auto digest=StartupFileDigest(path);
        const auto copy=input/"blocks"/path.filename();
        Require(std::filesystem::copy_file(path,copy),"Compact startup archive copy failed");
        Require(StartupFileDigest(copy)==digest,"Compact startup archive changed during copy");
        digests.emplace(path,digest);
    }
    BlockStorage files;Need(files.init(input));
    const auto candidate_path=scratch.path/"candidate";ChainDB candidate;Need(candidate.init(candidate_path));
    BlockReindexer::Config config;config.mode=BlockReindexer::Mode::FULL;config.use_assumevalid=false;
    config.shielded_frontier_output_path=scratch.path/"frontier.bin";
    config.shielded_nullifier_db_path=scratch.path/"nullifiers.db";
    CompactReconstruction proof(source_files,retain_selected_headers);
    const auto result=Need(Reconstruct(source,candidate,token,files,input,candidate_path,config,&proof));
    if(!result.success)throw std::runtime_error("Compact startup reconstruction refused: "+result.error);
    Require(proof.forest.has_value()&&(historical?proof.historical.has_value():proof.retirement.has_value()),"Compact startup replay proof incomplete");
    // Compare the complete current Orchard namespace, including orphan undo or
    // unknown rows and the truly empty boundary state. Expected bytes belong to
    // the independently reconstructed candidate, never a caller-supplied digest.
    Need(source.compareOrchardStorage(candidate));
    if(historical) {
        proof.HistoricalCatalog(source,candidate,*proof.historical);
        Require(proof.historical->Encode()==selected_bytes,"Compact historical selected state differs from replay");
        storage::CheckHistoricalCompactSelection(source,*proof.historical);
        for(const auto record:{ChainDB::ShieldedStateRecord::Frontier,ChainDB::ShieldedStateRecord::AnchorHistory})
            Require(Need(source.getShieldedState(record))==Need(candidate.getShieldedState(record)),
                "Compact historical legacy bytes differ from replay");
    } else {
        proof.Catalog(source,candidate,tip.hash);
        Require(Need(candidate.getOrchardCatalogState(tip.hash))==selected_bytes,"Compact selected catalog differs from replay");
    }
    Require(identity.height==uint32_t(tip.height)&&identity.block==tip.hash&&identity.work==tip.work,
        "Compact startup selected identity differs from replay");
    const auto marker=Need(source.getForestTipMarker());
    Require(marker.height==tip.height&&marker.block_hash==tip.hash&&
        marker.forest_root==Need(candidate.getForestTipMarker()).forest_root,
        "Compact startup accumulator marker differs from replay");
    if(uint32_t(tip.height)>=profile->activation_height) {
        auto state=Need(candidate.getOrchardState());
        Require(Need(source.getOrchardState())==state &&
            Need(source.getOrchardCommitmentSets(state))==Need(candidate.getOrchardCommitmentSets(state)) &&
            Need(source.getLegacyRetirementState())==Need(candidate.getLegacyRetirementState()),
            "Compact startup selected shielded owners differ from replay");
        // Authenticate every retained selected predecessor, undo, journal,
        // transaction index and original body/undo locator without source coins.
        auto forest=*proof.forest;
        for(uint32_t height=uint32_t(tip.height);height>=profile->activation_height;--height) {
            const auto header=Need(source.getHeader(state.block_hash));
            const auto parent=Need(source.getHeader(header.prev_block_hash));
            const auto c=SelectedOrchardBlockContext(header,height);
            Require(c.has_value(),"Compact startup selected predecessor profile missing");
            const bool witness=Params().enforce_witness_commitment&&height>=Params().witness_commitment_enforcement_height;
            const auto block=ReadStoredOrchardBlock(source,state.block_hash,witness);
            const auto previous=AuditOrchardUndoStepUnderLock(source,source_files,*c,block,parent,state,forest,witness,true);
            Require(previous.parent_state==Need(candidate.getOrchardUndoParent(state)),
                "Compact startup selected Orchard predecessor differs from replay");
            if(height==uint32_t(tip.height)) {
                // Also exercise the compact reversible transition against the
                // actual selected sets and frozen legacy owner. Batch is abandoned.
                const auto prior=storage::catalog::State::Decode(Need(source.getOrchardCatalogState(c->parent_hash)));
                Stats ignored;const auto records=Need(reindex_detail::ReadDiskBlocks(BlockFiles(input),&ignored));
                auto headers=Headers(records,c->parent_hash);
                const auto ancestry=SelectedFrames(records,c->parent_hash);
                const OrchardBranchMtpLookup mtp=[&](uint32_t h)->std::optional<uint64_t> {
                    if(h>=height)return {};uint256 hash;
                    if(!h){if(!uint256::FromHex(Params().genesis_hash,hash))return {};}
                    else {if(h>ancestry.size())return {};hash=records[ancestry[h-1]].hash;}
                    uint32_t time=0,found=0;
                    if(!headers->GetMedianTimePastByHash(hash,time,found)||found!=h)return {};return time;
                };
                rocksdb::WriteBatch abandoned;
                (void)StageOrchardCompactChainstateDisconnectUnderLock(source,token,*c,block,parent,std::get<storage::catalog::State>(selected),prior,mtp,witness,abandoned);
            }
            forest=previous.parent_forest;
            if(previous.parent_state)state=*previous.parent_state;
            else Require(height==profile->activation_height,"Compact startup predecessor ended early");
        }
    } else if(!historical) {
        Require(Need(candidate.getTip()).hash==proof.retirement->boundary_parent,
            "Compact startup boundary retirement does not belong to selected parent");
        CheckPreparedLegacyRetirementUnderLock(source,*proof.retirement);
    }
    // Held writer mutex plus exclusive startup/datadir ownership covers the
    // complete operation. No caller callbacks or unlocked publication gap.
    const auto current=Need(source.getTip()),current_validated=Need(source.getValidatedTip());
    Require(current.hash==tip.hash&&current.height==tip.height&&current.work==tip.work&&
        current_validated.hash==validated.hash&&current_validated.height==validated.height&&
        storage::ReadOrchardCompactStorageBinding(source)==mode&&
        (historical?Need(source.getHistoricalCompactCatalogState(tip.hash)):Need(source.getOrchardCatalogState(tip.hash)))==selected_bytes&&
        ReadRuntimeOutboxUnderLock(source,*profile,head,1).head==head,
        "Compact startup source changed before enrollment");
    Require(BlockFiles(datadir)==original_files,"Compact startup archive inventory changed");
    for(const auto& [path,digest]:digests)
        Require(StartupFileDigest(path)==digest,"Compact startup source archive changed");
    auto owner=std::make_unique<OrchardCompactChainstate>();
    if(historical) {
        storage::CheckHistoricalCompactSelection(source,*proof.historical);
        owner->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(&source,&mutex,*proof.historical);
    } else owner->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
        &source,&mutex,std::get<storage::catalog::State>(selected),*proof.retirement);
    auto restored=std::unique_ptr<OrchardCompactStartup>(new OrchardCompactStartup());
    restored->source_=&source;restored->mutex_=&mutex;
    restored->owner_=std::move(owner);
    restored->selected_headers_=std::move(proof.selected_headers);
    if(retain_selected_headers) {
        restored->selected_undo_.resize(restored->selected_headers_.size());
        for(const auto& entry:restored->selected_headers_) {
            const auto hash=entry.header.GetHash();
            if(entry.height<profile->activation_height) {
                // Historical FULL replay persists conventional undo in its
                // own flatfiles, not the Orchard undo key/value namespace.
                // Absence of an original availability bit grants no location.
                const auto original=Need(source.getHeaderMetadata(hash));
                if(!(original.status_flags&BLOCK_HAVE_UNDO))continue;
                const auto expected=Need(candidate.getHeaderMetadata(hash));
                Require(entry.height && original.undo_size &&
                    (expected.status_flags&BLOCK_HAVE_UNDO) && expected.undo_size,
                    "Compact startup historical undo owner unavailable");
                const auto rebuilt=Need(files.readUndo(
                    {expected.undo_file,expected.undo_pos,expected.undo_size}));
                Require(Need(source_files.readUndo(
                    {original.undo_file,original.undo_pos,original.undo_size}))==rebuilt,
                    "Compact startup historical undo differs from independent replay");
                Require(entry.height<restored->selected_undo_.size(),
                    "Compact startup historical undo height out of range");
                restored->selected_undo_[entry.height]=OrchardCompactStartup::RetainedUndoLocation{
                    original.undo_file,original.undo_pos,original.undo_size};
                continue;
            }
            const auto found=proof.checked_undo.find(hash);
            Require(found!=proof.checked_undo.end(),"Compact startup selected undo was not independently checked");
            const auto& loc=found->second;const auto m=Need(source.getHeaderMetadata(hash));
            Require((m.status_flags&BLOCK_HAVE_UNDO) && loc.data_size &&
                m.undo_file==loc.file_number && m.undo_pos==loc.data_pos && m.undo_size==loc.data_size,
                "Compact startup checked undo location changed");
            Require(Need(source_files.readUndo({loc.file_number,loc.data_pos,loc.data_size}))==
                Need(candidate.getUndo(hash)).Serialize(),"Compact startup checked undo bytes changed");
            Require(entry.height<restored->selected_undo_.size(),"Compact startup undo height out of range");
            restored->selected_undo_[entry.height]=loc;
        }
    }
    if(retain_selected_headers) {
        auto& nodes=restored->selected_index_;
        const auto& entries=restored->selected_headers_;
        Require(entries.size()==uint64_t(identity.height)+1,
            "Compact startup index history is incomplete");
        nodes.reserve(entries.size());
        arith_uint256 work{0};
        for(size_t height=0;height<entries.size();++height) {
            const auto& entry=entries[height];
            auto node=std::make_unique<CBlockIndex>(entry.header,entry.height);
            work+=GetBlockProof(entry.header.difficulty);
            Require(entry.height==height && entry.work==work && entry.data_size &&
                (height?node->prev_hash==nodes.back()->hash:
                    node->hash==identity.genesis && node->prev_hash.IsNull()),
                "Compact startup index header domain mismatch");
            node->chainwork=entry.work.GetHex();
            // Reconstruction has independently checked this header AND body.
            // The source archive locator was bound to those exact body bytes.
            // Undo availability remains a separate obligation; none is inferred.
            node->status=BLOCK_VALID_HEADER|BLOCK_VALID_TREE|BLOCK_VALID_TRANSACTIONS|
                BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS|BLOCK_HAVE_DATA;
            node->file_number=entry.file_number;
            node->data_pos=entry.data_pos;node->data_size=entry.data_size;
            node->children.reserve(1);
            if(height) {
                node->pprev=nodes.back().get();
                node->pprev->children.push_back(node.get());
            }
            nodes.push_back(std::move(node));
        }
        Require(!nodes.empty() && nodes.back()->hash==identity.block &&
            nodes.back()->height==identity.height && work==identity.work,
            "Compact startup index tip differs from selected owner");
    }
    return restored;
}
} // namespace dinero
