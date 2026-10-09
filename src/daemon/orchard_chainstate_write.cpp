#include "consensus/orchard_header.h"
#include "consensus/block_filter.h"
#include "consensus/filter_commitment.h"
#include "storage/archival_block_reader.h"
#include "consensus/utreexo_delta_codec.h"
#include <type_traits>
#include <set>
#include "daemon/orchard_chainstate_write.h"
#include "daemon/utreexo_tx_reader.h"
#include "daemon/services/orchard_parent_catalog.h"
#include "daemon/services/historical_catalog.h"
#include "daemon/services/historical_catalog_range.h"
#include "storage/orchard_catalog_state.h"
#include "storage/orchard_storage_mode.h"
#include "consensus/orchard_catalog_validation.h"
#include "daemon/orchard_reindex.h"
#include "daemon/runtime_block_outbox.h"
#include "crypto/sha256.h"
#include "consensus/merkle_root.h"
#include <algorithm>
#include <cstring>
#include "common/annotated_mutex.h"
#include "consensus/utxo_publication.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/undo.h"
#include <rocksdb/write_batch.h>
#include <cstdio>
#include <thread>
#include <tuple>

namespace dinero {
using namespace consensus;
namespace {
template<class T> T RequiredDisk(StatusOr<T> value) {
    if (!value.ok()) throw OrchardStateLookupError(value.status());
    return std::move(value.value());
}
auto MetadataFields(const ChainDB::PersistedHeaderMetadata& m) {
    return std::tie(m.parent_hash,m.height,m.chainwork,m.status_flags,
        m.file_number,m.data_pos,m.data_size,m.undo_file,m.undo_pos,m.undo_size);
}
bool IndexMatches(const CBlockIndex& i, const BlockHeader& h,
                  const ChainDB::PersistedHeaderMetadata& m) {
    return i.hash==h.GetHash() && i.prev_hash==m.parent_hash && i.height==uint32_t(m.height) &&
        i.version==h.version && i.merkle_root==h.merkle_root && i.timestamp==h.timestamp &&
        i.bits==h.difficulty && i.nonce==h.nonce && ChainworkFromHex(i.chainwork)==m.chainwork &&
        std::tie(i.status,i.file_number,i.data_pos,i.data_size,i.undo_file,i.undo_pos,i.undo_size)==
        std::tie(m.status_flags,m.file_number,m.data_pos,m.data_size,m.undo_file,m.undo_pos,m.undo_size);
}
}

namespace outbox_detail {
constexpr const char* head_key="runtime_orchard_outbox:v1:head";
constexpr size_t maximum_record_bytes=16*1024*1024;
constexpr size_t overhead=224; // Upper bound for framing plus digest.
std::string Key(uint64_t sequence) {
    std::string key="runtime_orchard_outbox:v1:event:";
    for(int shift=60;shift>=0;shift-=4)key.push_back("0123456789abcdef"[(sequence>>shift)&15]);
    return key;
}
[[noreturn]] void Corrupt() { throw OrchardStateLookupError(Status::Corruption); }
std::optional<std::string> Raw(const ChainDB& db,const std::string& key) {
    std::string value;const auto status=db.getRaw(key,value);
    if(status==Status::NotFound)return std::nullopt;
    if(status!=Status::Ok)throw OrchardStateLookupError(status);
    return value;
}
void Number(std::string& bytes,uint64_t value,size_t width) {
    for(size_t i=0;i<width;++i)bytes.push_back(static_cast<char>(value>>(8*i)));
}
void Hash(std::string& bytes,const uint256& hash) {
    bytes.append(reinterpret_cast<const char*>(hash.data),32);
}
uint256 Digest(std::string_view bytes) {
    uint256 hash;crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()).Finalize(hash.data);
    return hash;
}
struct Reader {
    std::string_view bytes;
    std::string_view Take(size_t n) {
        if(n>bytes.size())Corrupt();auto value=bytes.substr(0,n);bytes.remove_prefix(n);return value;
    }
    uint64_t Number(size_t n) {
        auto value=Take(n);uint64_t result=0;
        for(size_t i=0;i<n;++i)result|=uint64_t(uint8_t(value[i]))<<(8*i);
        return result;
    }
    uint256 Hash() { auto value=Take(32);uint256 result;std::memcpy(result.data,value.data(),32);return result; }
};
Reader Checked(std::string_view bytes,size_t limit) {
    if(bytes.size()<32 || bytes.size()>limit)Corrupt();
    const auto payload=bytes.substr(0,bytes.size()-32);
    Reader tail{bytes.substr(bytes.size()-32)};
    if(tail.Hash()!=Digest(payload))Corrupt();
    return {payload};
}
void Seal(std::string& bytes) { Hash(bytes,Digest(bytes)); }
bool Profile(const OrchardBlockContext& c) {
    return c.domain.network_code<=2 && c.domain.branch_id && c.activation_height &&
        c.activation_height!=UINT32_MAX &&
        std::any_of(c.domain.genesis_wire.begin(),c.domain.genesis_wire.end(),[](auto x){return x!=0;});
}
bool SameProfile(const OrchardBlockContext& a,const OrchardBlockContext& b) {
    return a.domain.network_code==b.domain.network_code && a.domain.genesis_wire==b.domain.genesis_wire &&
        a.domain.branch_id==b.domain.branch_id && a.activation_height==b.activation_height;
}
RuntimeOutboxCursor Head(const std::string& bytes) {
    auto r=Checked(bytes,78);if(r.Take(6)!="DNOH01")Corrupt();
    RuntimeOutboxCursor c{r.Number(8),r.Hash()};
    if(!r.bytes.empty() || !c.sequence || c.digest.IsNull())Corrupt();return c;
}
std::string EncodeHead(const RuntimeOutboxCursor& c) {
    std::string bytes="DNOH01";Number(bytes,c.sequence,8);Hash(bytes,c.digest);Seal(bytes);return bytes;
}
void State(std::string& bytes,const storage::OrchardStoredState& s) {
    Number(bytes,s.height,4);Hash(bytes,s.block_hash);Hash(bytes,s.anchor);
    Number(bytes,s.pool_balance,8);Number(bytes,s.tree_size,8);
    Number(bytes,s.frontier.size(),4);bytes.append(s.frontier);
}
storage::OrchardStoredState State(Reader& r) {
    storage::OrchardStoredState s;
    s.height=r.Number(4);s.block_hash=r.Hash();s.anchor=r.Hash();
    s.pool_balance=r.Number(8);s.tree_size=r.Number(8);
    const auto n=r.Number(4);if(n>storage::ORCHARD_STORED_FRONTIER_LIMIT)Corrupt();
    s.frontier=std::string(r.Take(n));return s;
}
void CheckReplay(const RuntimeOrchardReplay& replay,const OrchardBlockContext& c) {
    const auto check=[](const storage::OrchardStoredState& s) {
        if(s.block_hash.IsNull() || s.pool_balance>orchard::kMaxMoneyUna ||
           s.frontier.size()>storage::ORCHARD_STORED_FRONTIER_LIMIT)Corrupt();
        const auto tree=orchard::OrchardFrontier::Decode({
            reinterpret_cast<const uint8_t*>(s.frontier.data()),s.frontier.size()});
        if(tree.Size()!=s.tree_size || !std::equal(tree.Root().begin(),tree.Root().end(),s.anchor.begin()))Corrupt();
    };
    check(replay.next);
    if(replay.next.height!=c.height || replay.next.block_hash!=c.block_hash)Corrupt();
    if(replay.parent) {
        check(*replay.parent);
        if(c.height<=c.activation_height || replay.parent->height!=c.height-1 ||
           replay.parent->block_hash!=c.parent_hash)Corrupt();
    } else if(c.height!=c.activation_height)Corrupt();
    if(replay.coin_undo.empty() || replay.coin_undo.size()>maximum_record_bytes)Corrupt();
    // Framing/canonical undo identity only. Revalidation against the body and
    // selected historical baseline remains a replay owner's obligation.
    if(UndoRecord::Deserialize(replay.coin_undo).Serialize()!=replay.coin_undo)Corrupt();
    for(const auto& [height,time]:replay.branch_mtp) {
        (void)time;if(height>=c.height)Corrupt();
    }
}
std::string EncodeReplay(const RuntimeOrchardReplay& replay) {
    std::string bytes;Number(bytes,replay.parent.has_value(),1);
    if(replay.parent)State(bytes,*replay.parent);
    State(bytes,replay.next);Number(bytes,replay.coin_undo.size(),4);
    bytes.append(reinterpret_cast<const char*>(replay.coin_undo.data()),replay.coin_undo.size());
    Number(bytes,replay.branch_mtp.size(),4);
    for(const auto& [height,time]:replay.branch_mtp){Number(bytes,height,4);Number(bytes,time,8);}
    return bytes;
}
size_t ReplayCharge(const RuntimeOrchardReplay& replay) {
    const auto state_size=[](const storage::OrchardStoredState& s){return size_t(88)+s.frontier.size();};
    return 1+(replay.parent?state_size(*replay.parent):0)+state_size(replay.next)+
        4+replay.coin_undo.size()+4+12*replay.branch_mtp.size();
}
RuntimeOrchardReplay DecodeReplay(Reader& r) {
    RuntimeOrchardReplay replay;const auto parent=r.Number(1);if(parent>1)Corrupt();
    if(parent)replay.parent=State(r);
    replay.next=State(r);const auto n=r.Number(4);const auto undo=r.Take(n);
    replay.coin_undo.assign(undo.begin(),undo.end());
    const auto count=r.Number(4);if(count>r.bytes.size()/12)Corrupt();
    uint32_t previous=0;
    for(uint64_t i=0;i<count;++i) {
        const uint32_t h=r.Number(4);const auto time=r.Number(8);
        if(i && h<=previous)Corrupt();previous=h;
        replay.branch_mtp.emplace(h,time);
    }
    return replay;
}
std::string Encode(const RuntimeOutboxEvent& e) {
    std::string bytes=e.orchard_replay?"DNOE03":(e.IsOrchardProfile()?"DNOE01":"DNOE02");Number(bytes,e.cursor.sequence,8);Hash(bytes,e.previous_digest);
    Number(bytes,e.direction==RuntimeBlockDirection::Connect?1:2,1);
    Number(bytes,e.context.domain.network_code,1);
    bytes.append(reinterpret_cast<const char*>(e.context.domain.genesis_wire.data()),32);
    Number(bytes,e.context.domain.branch_id,4);Number(bytes,e.context.activation_height,4);
    Number(bytes,e.context.height,4);Hash(bytes,e.context.block_hash);Hash(bytes,e.context.parent_hash);
    Number(bytes,e.body.size(),4);bytes.append(reinterpret_cast<const char*>(e.body.data()),e.body.size());
    if(e.orchard_replay)bytes+=EncodeReplay(*e.orchard_replay);
    if(bytes.size()>maximum_record_bytes-32)throw OrchardStateLookupError(Status::Invalid);
    Seal(bytes);return bytes;
}
RuntimeOutboxEvent Decode(const std::string& bytes,uint64_t sequence,const OrchardBlockContext& profile) {
    auto r=Checked(bytes,maximum_record_bytes);
    const auto format=r.Take(6);if(format!="DNOE01" && format!="DNOE02" && format!="DNOE03")Corrupt();
    RuntimeOutboxEvent e;e.cursor.sequence=r.Number(8);e.previous_digest=r.Hash();
    const auto direction=r.Number(1);if(direction!=1 && direction!=2)Corrupt();
    e.direction=direction==1?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect;
    e.context.domain.network_code=r.Number(1);
    const auto genesis=r.Take(32);std::copy(genesis.begin(),genesis.end(),e.context.domain.genesis_wire.begin());
    e.context.domain.branch_id=r.Number(4);e.context.activation_height=r.Number(4);
    e.context.height=r.Number(4);e.context.block_hash=r.Hash();e.context.parent_hash=r.Hash();
    const auto size=r.Number(4);
    if(!size || size>maximum_record_bytes-overhead || r.bytes.size()<size || !Profile(e.context) ||
        !SameProfile(e.context,profile) || e.cursor.sequence!=sequence || !sequence ||
        (sequence==1)!=e.previous_digest.IsNull() || !e.context.height ||
        ((format=="DNOE01" || format=="DNOE03") && !e.IsOrchardProfile()) ||
        (format=="DNOE02" && e.IsOrchardProfile()) ||
        e.context.height>INT32_MAX || e.context.block_hash.IsNull() || e.context.parent_hash.IsNull())Corrupt();
    const auto body=r.Take(size);e.body.assign(body.begin(),body.end());
    e.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));
    try {
        if(format=="DNOE03") {
            e.orchard_replay=DecodeReplay(r);CheckReplay(*e.orchard_replay,e.context);
        }
        if(!r.bytes.empty())Corrupt();
        if(e.IsOrchardProfile()) {
            const auto parsed=OrchardBlockCandidate::DecodeExact(e.body);std::string error;
            if(parsed.Header().GetHash()!=e.context.block_hash || parsed.Header().prev_block_hash!=e.context.parent_hash ||
                !parsed.CheckIdentityCommitments(false,error))Corrupt();
        } else {
            const auto parsed=Block::Deserialize(e.body);
            if(!parsed || parsed->Serialize()!=std::string(body) ||
                parsed->header.GetHash()!=e.context.block_hash || parsed->header.prev_block_hash!=e.context.parent_hash ||
                ComputeMerkleRoot(parsed->vtx)!=parsed->header.merkle_root)Corrupt();
        }
    } catch(const std::bad_alloc&) { throw; }
      catch(const OrchardStateLookupError&) { throw; }
      catch(...) { Corrupt(); }
    return e;
}
RuntimeOutboxEvent Read(const ChainDB& db,uint64_t sequence,const OrchardBlockContext& profile) {
    const auto bytes=Raw(db,Key(sequence));if(!bytes)Corrupt();return Decode(*bytes,sequence,profile);
}
using TransitionTip = std::pair<uint256,uint32_t>;
TransitionTip Before(const RuntimeOutboxEvent& e) {
    return e.direction==RuntimeBlockDirection::Connect ?
        TransitionTip{e.context.parent_hash,e.context.height-1} :
        TransitionTip{e.context.block_hash,e.context.height};
}
TransitionTip After(const RuntimeOutboxEvent& e) {
    return e.direction==RuntimeBlockDirection::Connect ?
        TransitionTip{e.context.block_hash,e.context.height} :
        TransitionTip{e.context.parent_hash,e.context.height-1};
}
void CheckCanonicalTip(const ChainDB& db,const TransitionTip& expected) {
    const auto tip=RequiredDisk(db.getTip());
    if(tip.height<0 || tip.hash!=expected.first || uint32_t(tip.height)!=expected.second)Corrupt();
}
RuntimeOutboxCursor CheckedHead(const ChainDB& db,const std::optional<std::string>& raw,
                               const OrchardBlockContext& profile) {
    if(!raw) { if(Raw(db,Key(1)))Corrupt();return {}; }
    const auto head=Head(*raw);const auto event=Read(db,head.sequence,profile);
    if(event.cursor!=head)Corrupt();
    // Even an EOF cursor must describe the current durable generation. This
    // also prevents appending onto a retained log from another canonical tip.
    CheckCanonicalTip(db,After(event));
    if(head.sequence!=UINT64_MAX && Raw(db,Key(head.sequence+1)))Corrupt();
    return head;
}
// Called only by the sealed indexed owner. No public staging API or deletion.
std::optional<std::string> Append(const ChainDB& db,rocksdb::WriteBatch& batch,
    const OrchardBlockContext& context,const OrchardBlockCandidate& block,bool connecting,
    const std::optional<RuntimeOrchardReplay>& replay) {
    if(!Profile(context))throw OrchardStateLookupError(Status::Invalid);
    auto before=Raw(db,head_key);const auto head=CheckedHead(db,before,context);
    if(head.sequence==UINT64_MAX)throw OrchardStateLookupError(Status::Invalid);
    RuntimeOutboxEvent event{{head.sequence+1,{}},head.digest,
        connecting?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect,context,block.WireBytes(),replay};
    CheckCanonicalTip(db,Before(event));
    // This is an index into the existing retained source, not another journal.
    // A disconnect must reuse the original selected validation's MTP answers.
    const auto locator_key="runtime_orchard_outbox:v1:connect:"+context.block_hash.GetHex();
    const auto locator=Raw(db,locator_key);
    if(locator) {
        const auto cursor=Head(*locator);
        if(cursor.sequence>head.sequence)Corrupt();
        const auto prior=Read(db,cursor.sequence,context);
        if(prior.cursor!=cursor || prior.direction!=RuntimeBlockDirection::Connect ||
           prior.context.height!=context.height || prior.context.block_hash!=context.block_hash ||
           prior.context.parent_hash!=context.parent_hash || prior.body!=event.body ||
           !prior.orchard_replay || !event.orchard_replay ||
           prior.orchard_replay->parent!=event.orchard_replay->parent ||
           prior.orchard_replay->next!=event.orchard_replay->next ||
           prior.orchard_replay->coin_undo!=event.orchard_replay->coin_undo)Corrupt();
        if(connecting) {
            if(prior.orchard_replay->branch_mtp!=event.orchard_replay->branch_mtp)Corrupt();
        } else event.orchard_replay->branch_mtp=prior.orchard_replay->branch_mtp;
    } else if(!connecting) {
        // Older coverage may lack validation-time replay inputs. Preserve the
        // transition, but never invent the missing context or readiness.
        event.orchard_replay.reset();
    }
    if(event.orchard_replay)CheckReplay(*event.orchard_replay,context);
    const auto key=Key(event.cursor.sequence);
    if(Raw(db,key))Corrupt();
    const auto bytes=Encode(event);
    event.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));
    batch.Put(key,bytes);batch.Put(head_key,EncodeHead(event.cursor));
    if(connecting)batch.Put(locator_key,EncodeHead(event.cursor));
    return before;
}
} // namespace outbox_detail

namespace outbox_detail {
// Both entry points use the same checked page walk. The boundary was verified
// against durable records by the caller while holding the selected writer lock.
RuntimeOutboxPage ReadPage(const ChainDB& db,const OrchardBlockContext& profile,
    RuntimeOutboxCursor head,RuntimeOutboxCursor after,size_t maximum_events,size_t maximum_bytes) {
    RuntimeOutboxPage page;page.head=head;page.next=after;
    if(after.sequence>page.head.sequence || (!after.sequence && !after.digest.IsNull()))Corrupt();
    std::optional<TransitionTip> previous_tip;
    if(after.sequence) {
        const auto previous=Read(db,after.sequence,profile);
        if(previous.cursor!=after)Corrupt();
        previous_tip=After(previous);page.after_tip=previous_tip;
    }
    size_t used=0;
    while(page.next.sequence<page.head.sequence && page.events.size()<maximum_events) {
        auto event=Read(db,page.next.sequence+1,profile);
        if(event.previous_digest!=page.next.digest ||
            (previous_tip && *previous_tip!=Before(event)))Corrupt();
        const auto charge=event.body.size()+overhead+
            (event.orchard_replay?ReplayCharge(*event.orchard_replay):0);
        if(charge>maximum_bytes-used) {
            if(page.events.empty())throw OrchardStateLookupError(Status::Invalid);
            break;
        }
        used+=charge;previous_tip=After(event);page.next=event.cursor;page.events.push_back(std::move(event));
    }
    if(page.next.sequence==page.head.sequence && page.next!=page.head)Corrupt();
    return page;
}
} // namespace outbox_detail

RuntimeOutboxPage ReadRuntimeOutboxUnderLock(const ChainDB& db,const OrchardBlockContext& profile,
    RuntimeOutboxCursor after,size_t maximum_events,size_t maximum_bytes) {
    using namespace outbox_detail;
    if(!Profile(profile) || !maximum_events || maximum_events>128 || !maximum_bytes ||
        maximum_bytes>16*1024*1024)throw OrchardStateLookupError(Status::Invalid);
    const auto head=CheckedHead(db,Raw(db,head_key),profile);
    return ReadPage(db,profile,head,after,maximum_events,maximum_bytes);
}

RuntimeOutboxPage ReadRuntimeOutboxPrefixUnderLock(const ChainDB& db,const OrchardBlockContext& profile,
    RuntimeOutboxCursor captured_head,RuntimeOutboxCursor after,size_t maximum_events,size_t maximum_bytes) {
    using namespace outbox_detail;
    if(!Profile(profile) || !captured_head.sequence || !maximum_events || maximum_events>128 ||
        !maximum_bytes || maximum_bytes>16*1024*1024)throw OrchardStateLookupError(Status::Invalid);
    const auto current=CheckedHead(db,Raw(db,head_key),profile);
    if(captured_head.sequence>current.sequence || Read(db,captured_head.sequence,profile).cursor!=captured_head)
        Corrupt();
    return ReadPage(db,profile,captured_head,after,maximum_events,maximum_bytes);
}

// This private entry is reachable only from the startup replay owner, after
// independent consensus application through `through` into a new candidate.
void OrchardReindexOwner::CopyValidatedOutboxPrefix(const ChainDB& source, ChainDB& candidate,
    const ChainWriteToken& token, const consensus::OrchardBlockContext& profile,
    RuntimeOutboxCursor through, RuntimeOutboxCursor source_head) {
    using namespace outbox_detail;
    if (&source==&candidate || !through.sequence || through.sequence>source_head.sequence ||
        Raw(candidate,head_key) || Raw(candidate,Key(1))) Corrupt();
    const auto last=Read(source,through.sequence,profile);
    if(last.cursor!=through)Corrupt();
    CheckCanonicalTip(candidate,After(last));
    RuntimeOutboxCursor copied;
    while(copied!=through) {
        const auto page=ReadRuntimeOutboxUnderLock(source,profile,copied,1);
        if(page.head!=source_head || page.events.size()!=1)Corrupt();
        const auto& event=page.events.front();
        const auto raw=Raw(source,Key(event.cursor.sequence));
        if(!raw || Decode(*raw,event.cursor.sequence,profile).cursor!=event.cursor)Corrupt();
        rocksdb::WriteBatch batch;
        batch.Put(Key(event.cursor.sequence),*raw);
        if(event.IsOrchardProfile() && event.direction==RuntimeBlockDirection::Connect)
            batch.Put("runtime_orchard_outbox:v1:connect:"+event.context.block_hash.GetHex(),EncodeHead(event.cursor));
        if(candidate.writeBatch(token,std::move(batch),true)!=Status::Ok)
            throw OrchardStateLookupError(Status::Io);
        copied=event.cursor;
    }
    if(ReadRuntimeOutboxUnderLock(source,profile,through,1).head!=source_head)Corrupt();
    rocksdb::WriteBatch head;head.Put(head_key,EncodeHead(through));
    if(candidate.writeBatch(token,std::move(head),true)!=Status::Ok)
        throw OrchardStateLookupError(Status::Io);
    if(ReadRuntimeOutboxUnderLock(candidate,profile,through,1).head!=through)Corrupt();
}

std::optional<PreparedHistoricalRuntimeOutbox> PreparedHistoricalRuntimeOutbox::PrepareUnderLock(
    const ChainDB& db,const OrchardBlockContext& profile,const Block& block,uint32_t height,
    RuntimeBlockDirection direction) {
    using namespace outbox_detail;
    const auto raw=Raw(db,head_key);
    // Preserve historical databases without a coverage origin, but never hide
    // an orphaned first record after a missing head.
    if(!raw) { if(Raw(db,Key(1)))Corrupt();return {}; }
    if(!Profile(profile) || !height || height>=profile.activation_height || height>INT32_MAX ||
        (direction!=RuntimeBlockDirection::Connect && direction!=RuntimeBlockDirection::Disconnect))
        throw OrchardStateLookupError(Status::Invalid);
    const auto previous=CheckedHead(db,raw,profile);
    if(previous.sequence==UINT64_MAX)throw OrchardStateLookupError(Status::Invalid);
    auto context=profile;context.height=height;context.block_hash=block.header.GetHash();context.parent_hash=block.header.prev_block_hash;
    const auto tip=RequiredDisk(db.getTip());
    if(tip.hash!=(direction==RuntimeBlockDirection::Connect?context.parent_hash:context.block_hash) ||
       tip.height!=int32_t(direction==RuntimeBlockDirection::Connect?height-1:height))Corrupt();
    const auto wire=block.Serialize();
    if(wire.size()>maximum_record_bytes-overhead)throw OrchardStateLookupError(Status::Invalid);
    RuntimeOutboxEvent event{{previous.sequence+1,{}},previous.digest,direction,context,{wire.begin(),wire.end()}};
    PreparedHistoricalRuntimeOutbox prepared;
    prepared.before_=*raw;prepared.key_=Key(event.cursor.sequence);prepared.record_=Encode(event);
    // Validate the exact retained representation before any historical state
    // mutation. This is body identity, not historical consensus replay.
    event=Decode(prepared.record_,event.cursor.sequence,profile);
    if(Raw(db,prepared.key_))Corrupt();
    prepared.head_=EncodeHead(event.cursor);prepared.tip_hash_=tip.hash;prepared.tip_height_=tip.height;
    prepared.thread_=std::this_thread::get_id();return prepared;
}
void PreparedHistoricalRuntimeOutbox::StageOrTerminateUnderLock(const ChainDB& db,rocksdb::WriteBatch& batch) noexcept {
    try {
        using namespace outbox_detail;
        const auto tip=RequiredDisk(db.getTip());
        if(staged_ || thread_!=std::this_thread::get_id() || Raw(db,head_key)!=std::optional<std::string>(before_) ||
           Raw(db,key_) || tip.hash!=tip_hash_ || tip.height!=tip_height_)std::terminate();
        batch.Put(key_,record_);batch.Put(head_key,head_);staged_=true;
    } catch(...) { std::terminate(); }
}

struct PreparedOrchardChainstateWrite::Impl {
    enum class Phase { Preparing, Prepared, Aborted, Writing, Committed };
    std::unique_lock<AnnotatedRecursiveMutex> lock;
    const std::thread::id thread = std::this_thread::get_id();
    ChainDB& db;
    const ChainWriteToken& token;
    rocksdb::WriteBatch batch;
    std::optional<PreparedUTXOPublication> publication;
    OrchardCompactChainstate* compact_live=nullptr;
    std::shared_ptr<const OrchardCompactChainstate::Selected> compact_before,compact_after;
    std::vector<uint8_t> undo_bytes;
    std::optional<RuntimeOrchardReplay> replay;
    CBlockIndex* index = nullptr;
    BlockHeader indexed_header;
    std::optional<ChainDB::PersistedHeaderMetadata> index_before, index_after;
    // Historical replay proves live validation levels without rewriting the
    // stored before-image. Retain both exact representations through commit.
    std::optional<ChainDB::PersistedHeaderMetadata> historical_index_live_before, historical_index_live_after;
    Phase phase = Phase::Preparing;
    std::optional<std::string> outbox_before;
    bool outbox_staged = false;
    std::function<void()> historical_ready;
    bool storage_mode_captured=false;
    std::optional<std::string> storage_mode_before,storage_mode_after;
    void CaptureStorageMode(bool compact,const storage::catalog::State* state=nullptr,bool enroll=false) {
        CatalogRequire(!storage_mode_captured);
        storage_mode_before=storage::ReadOrchardCompactStorageBinding(db);
        if(compact) {
            CatalogRequire(state!=nullptr);
            const auto expected=storage::EncodeOrchardCompactStorageBinding(*state);
            if(enroll) {
                CatalogRequire(!storage_mode_before);
                storage_mode_after=expected;
            } else CatalogRequire(storage_mode_before&&*storage_mode_before==expected);
        } else storage::RequireFullOrchardStorage(db);
        storage_mode_captured=true;
    }
    std::map<uint256,std::optional<std::string>> catalog_before;
    std::optional<TipInfo> catalog_tip_before;
    std::optional<std::string> CaptureCatalog(const uint256& hash) {
        if(!catalog_tip_before)catalog_tip_before=RequiredDisk(db.getTip());
        const auto value=db.getOrchardCatalogState(hash);std::optional<std::string> bytes;
        if(value.ok())bytes=*value;
        else if(value.status()!=Status::NotFound)throw OrchardStateLookupError(value.status(),"catalog/read");
        const auto [it,added]=catalog_before.emplace(hash,bytes);
        if(!added&&it->second!=bytes)throw OrchardStateLookupError(Status::Corruption,"catalog/changed");
        return bytes;
    }
    static void CatalogRequire(bool ok) {
        if(!ok)throw OrchardStateLookupError(Status::Corruption,"catalog/binding");
    }
    storage::catalog::State SelectCatalog(const PreparedOrchardCatalog* initial,
        const std::optional<storage::LegacyRetirementRecord>& boundary,
        const OrchardBlockContext& context,const BlockHeader& parent) {
        namespace c=storage::catalog;
        const auto prior=CaptureCatalog(context.parent_hash);
        CatalogRequire(bool(prior)||initial);
        c::State state;
        if(initial) {
            initial->Check();CatalogRequire(&initial->db_==&db&&boundary&&initial->record_==*boundary&&
                context.height==context.activation_height&&initial->target_.hash==context.parent_hash&&
                uint64_t(initial->target_.height)+1==context.height&&
                initial->target_.chainwork==RequiredDisk(db.getBlockWork(context.parent_hash)));
            state.network=initial->record_.network_code;state.genesis=initial->record_.genesis;
            state.branch=initial->record_.branch_id;state.activation=initial->record_.activation_height;
            state.leaf_activation=initial->leaf_activation_;state.height=initial->target_.height;
            state.block=initial->target_.hash;state.parent=parent.prev_block_hash;state.work=initial->target_.chainwork;
            state.transactions=initial->transactions_;state.legacy=initial->legacy_;state.nontransparent=initial->nontransparent_;
            state.transaction_count=initial->transaction_count_;state.legacy_count=initial->legacy_count_;state.nontransparent_count=initial->nontransparent_count_;
            state.stump=initial->stump_;
            if(prior)CatalogRequire(*prior==state.Encode());
        } else state=c::State::Decode(*prior);
        const auto stump=UtreexoStump::deserialize(state.stump);
        CheckOrchardCatalogStump(db,state,context,parent,context.height-1,stump);
        return state;
    }
    std::optional<storage::catalog::State> ConnectCatalog(const PreparedOrchardCatalog* initial,bool required,
        const std::optional<storage::LegacyRetirementRecord>& boundary,
        const OrchardBlockContext& context,const OrchardBlockCandidate& block,const BlockHeader& parent,
        const UtreexoStump& before,const PreparedOrchardBlockCoins& coins,const UtreexoStump& after,bool verify_only=false) {
        namespace c=storage::catalog;
        const auto prior=CaptureCatalog(context.parent_hash);
        const auto existing=CaptureCatalog(context.block_hash);
        if(!prior&&!initial){CatalogRequire(!required&&!existing);return {};}
        auto state=SelectCatalog(initial,boundary,context,parent);
        CheckOrchardCatalogStump(db,state,context,parent,context.height-1,before);
        const auto parent_bytes=state.Encode();
        if(!prior) {
            CatalogRequire(!verify_only);
            CatalogRequire(db.stageOrchardCatalogState(token,state.block,parent_bytes,batch)==Status::Ok);
        }
        std::map<uint256,std::string> pending;
        const auto read=[&](const uint256& hash){
            const auto found=pending.find(hash);return found==pending.end()?RequiredDisk(db.getOrchardCatalogNode(hash)):found->second;
        };
        const auto write=[&](const uint256& hash,const std::string& bytes){
            const auto [it,added]=pending.emplace(hash,bytes);CatalogRequire(added||it->second==bytes);
            if(added) {
                if(verify_only)CatalogRequire(RequiredDisk(db.getOrchardCatalogNode(hash))==bytes);
                else CatalogRequire(db.stageOrchardCatalogNode(token,hash,bytes,batch)==Status::Ok);
            }
        };
        c::Tree transactions(c::Kind::Transactions,read,write),legacy(c::Kind::LegacyCoins,read,write);
        c::Tree nontransparent(c::Kind::NonTransparentCoins,read,write);
        // Includes transactions whose outputs were all spent, and same-block
        // transactions with no surviving output. TxIndex is not this authority.
        for(const auto& tx:coins.Transactions()) {
            CatalogRequire(state.transaction_count!=UINT64_MAX);
            state.transactions=transactions.Insert(state.transactions,c::TransactionKey(tx.txid),{});++state.transaction_count;
        }
        for(const auto& change:coins.Changes()) {
            const auto point=c::OutpointBytes(change.outpoint);const auto key=c::LegacyKey(point);
            const auto found=legacy.Find(state.legacy,key);
            const auto nontransparent_key=c::NonTransparentKey(point);
            const auto special=nontransparent.Find(state.nontransparent,nontransparent_key);
            const bool was_nontransparent=change.before&&(change.before->is_confidential||!change.before->commitment.empty());
            CatalogRequire(special.has_value()==was_nontransparent);
            if(special) {
                CatalogRequire(*special==point&&state.nontransparent_count>0);
                state.nontransparent=nontransparent.Erase(state.nontransparent,nontransparent_key);--state.nontransparent_count;
            }
            if(change.after&&(change.after->is_confidential||!change.after->commitment.empty())) {
                CatalogRequire(change.after->height==context.height&&state.nontransparent_count!=UINT64_MAX);
                state.nontransparent=nontransparent.Insert(state.nontransparent,nontransparent_key,point);++state.nontransparent_count;
            }
            const auto metadata=[&](const UTXOEntry& coin){auto v=point;c::Number(v,coin.height,4);v.push_back(coin.isCoinbase?1:0);return v;};
            if(change.before&&change.before->height<state.leaf_activation) {
                CatalogRequire(found&&*found==metadata(*change.before)&&state.legacy_count>0);
                state.legacy=legacy.Erase(state.legacy,key);--state.legacy_count;
            } else CatalogRequire(!found);
            if(change.after&&change.after->height<state.leaf_activation) {
                CatalogRequire(change.after->height==context.height&&state.legacy_count!=UINT64_MAX);
                state.legacy=legacy.Insert(state.legacy,key,metadata(*change.after));++state.legacy_count;
            }
        }
        state.previous_record=c::Hash(parent_bytes);
        state.undo=c::Hash(std::string(undo_bytes.begin(),undo_bytes.end()));
        state.height=context.height;state.block=context.block_hash;state.parent=context.parent_hash;
        state.work=RequiredDisk(db.getBlockWork(context.block_hash));
        state.stump=after.serialize();
        CheckOrchardCatalogStump(db,state,context,block.Header(),context.height,after);
        const auto bytes=state.Encode();if(existing)CatalogRequire(*existing==bytes);
        if(verify_only)CatalogRequire(existing&&*existing==bytes);
        else CatalogRequire(db.stageOrchardCatalogState(token,state.block,bytes,batch)==Status::Ok);
        return state;
    }
    void DisconnectCatalog(bool required,const OrchardBlockContext& context,
        const OrchardBlockCandidate& block,const BlockHeader& parent,
        const UtreexoForest& before,const UtreexoForest& after) {
        namespace c=storage::catalog;const auto current=CaptureCatalog(context.block_hash);
        const auto prior=CaptureCatalog(context.parent_hash);
        CheckOrchardCatalogUndo(db,context,block.Header(),parent,before,after,undo_bytes,current,prior,required);
        // No second catalog tip and no mutable root restoration: the same
        // canonical batch restores the chain tip to this retained parent.
    }

    Impl(AnnotatedRecursiveMutex& mutex, ChainDB& database, const ChainWriteToken& capability)
        : lock(mutex), db(database), token(capability) {}
    ~Impl() {
        // unique_lock must never unlock a recursive_mutex from another thread.
        if (thread != std::this_thread::get_id()) std::terminate();
    }
    void CheckOwner() const {
        if (thread != std::this_thread::get_id() || !lock.owns_lock())
            throw std::logic_error("Orchard chainstate write used outside its owner thread");
        lock.mutex()->AssertHeld("Orchard chainstate write");
    }
    void CheckIndex() const {
        if(historical_ready)historical_ready();
        CatalogRequire(storage_mode_captured&&
            storage::ReadOrchardCompactStorageBinding(db)==storage_mode_before);
        if(catalog_tip_before) {
            const auto now=RequiredDisk(db.getTip());const auto& before=*catalog_tip_before;
            CatalogRequire(std::tie(now.hash,now.height,now.work,now.timestamp)==
                std::tie(before.hash,before.height,before.work,before.timestamp));
        }
        for(const auto& [hash,before]:catalog_before) {
            const auto value=db.getOrchardCatalogState(hash);
            if(before)CatalogRequire(value.ok()&&*value==*before);
            else CatalogRequire(value.status()==Status::NotFound);
        }
        if (outbox_staged && outbox_detail::Raw(db,outbox_detail::head_key)!=outbox_before)
            throw OrchardStateLookupError(Status::Corruption);
        if (!index) return;
        const auto current=RequiredDisk(db.getHeaderMetadata(indexed_header.GetHash()));
        if (MetadataFields(current)!=MetadataFields(*index_before) ||
            !IndexMatches(*index,indexed_header,historical_index_live_before ? *historical_index_live_before : *index_before))
            throw OrchardStateLookupError(Status::Corruption);
    }
    void PrepareIndex(BlockStorage& files, CBlockIndex& entry,
                      const OrchardBlockContext& context, const OrchardBlockCandidate& block,
                      bool connecting, bool contextual_header_validated=false) {
        const char* operation="index/read-metadata";
        try {
        auto before=RequiredDisk(db.getHeaderMetadata(context.block_hash));
        if (before.height<0 || uint32_t(before.height)!=context.height ||
            before.parent_hash!=context.parent_hash ||
            before.chainwork!=RequiredDisk(db.getBlockWork(context.block_hash)) ||
            (before.status_flags & (BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD)) ||
            !IndexMatches(entry,block.Header(),before)) {
            // Preserve the refusal predicate; report its mismatched field without
            // another database read or publishing any state.
            const auto& header=block.Header();
            const char* field="index/durable-work";
            if(before.height<0 || uint32_t(before.height)!=context.height)field="index/durable-height";
            else if(before.parent_hash!=context.parent_hash)field="index/durable-parent";
            else if(before.status_flags&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD))field="index/durable-failure-flags";
            else if(entry.hash!=header.GetHash())field="index/live-hash";
            else if(entry.prev_hash!=before.parent_hash)field="index/live-parent";
            else if(entry.height!=uint32_t(before.height))field="index/live-height";
            else if(entry.version!=header.version || entry.merkle_root!=header.merkle_root ||
                entry.timestamp!=header.timestamp || entry.bits!=header.difficulty || entry.nonce!=header.nonce)
                field="index/live-header";
            else if(ChainworkFromHex(entry.chainwork)!=before.chainwork)field="index/live-work";
            else if(entry.status!=before.status_flags)field="index/live-status";
            else if(std::tie(entry.file_number,entry.data_pos,entry.data_size)!=
                std::tie(before.file_number,before.data_pos,before.data_size))field="index/live-body-locator";
            else if(std::tie(entry.undo_file,entry.undo_pos,entry.undo_size)!=
                std::tie(before.undo_file,before.undo_pos,before.undo_size))field="index/live-undo-locator";
            throw OrchardStateLookupError(Status::Corruption,field);
        }
        auto after=before;
        const auto& wire=block.WireBytes();
        const std::string exact(wire.begin(),wire.end());
        operation="index/retained-body";
        if (before.status_flags & BLOCK_HAVE_DATA) {
            if (!before.data_size || RequiredDisk(files.readBlockBytes(
                    {before.file_number,before.data_pos,before.data_size}))!=exact)
                throw OrchardStateLookupError(Status::Corruption);
        } else {
            operation="index/new-body-locator";
            if (!connecting || before.data_size || before.data_pos || before.file_number)
                throw OrchardStateLookupError(Status::Corruption);
            operation="index/write-body";
            const auto pos=RequiredDisk(files.writeBlockBytes(context.block_hash,exact));
            if (pos.offset>UINT32_MAX) throw OrchardStateLookupError(Status::Invalid);
            after.file_number=pos.file_number;after.data_pos=uint32_t(pos.offset);after.data_size=pos.size;
            after.status_flags|=BLOCK_HAVE_DATA;
        }
        operation="index/retained-undo";
        if (before.status_flags & BLOCK_HAVE_UNDO) {
            if (!before.undo_size || RequiredDisk(files.readUndo(
                    {before.undo_file,before.undo_pos,before.undo_size}))!=undo_bytes)
                throw OrchardStateLookupError(Status::Corruption);
        } else {
            operation="index/new-undo-locator";
            if (!connecting || before.undo_size || before.undo_pos || before.undo_file)
                throw OrchardStateLookupError(Status::Corruption);
            operation="index/write-undo";
            const auto pos=RequiredDisk(files.writeUndo(context.block_hash,undo_bytes));
            if (pos.offset>UINT32_MAX) throw OrchardStateLookupError(Status::Invalid);
            after.undo_file=pos.file_number;after.undo_pos=uint32_t(pos.offset);after.undo_size=pos.size;
            after.status_flags|=BLOCK_HAVE_UNDO;
        }
        if (connecting && contextual_header_validated) after.status_flags|=BLOCK_VALID_MASK;
        operation="index/write-metadata";
        if (connecting && db.putHeaderMetadata(token,context.block_hash,after,&batch)!=Status::Ok)
            throw OrchardStateLookupError(Status::Internal);
        indexed_header=block.Header(); index_before=before; index_after=after; index=&entry;
        } catch(const OrchardStateLookupError& error) {
            if(error.Operation())throw;
            throw OrchardStateLookupError(error.SourceStatus(),operation);
        }
    }
    void PublishIndex() noexcept {
        if (!index) return;
        const auto& m=historical_index_live_after ? *historical_index_live_after : *index_after;
        index->status=m.status_flags;
        index->file_number=m.file_number;index->data_pos=m.data_pos;index->data_size=m.data_size;
        index->undo_file=m.undo_file;index->undo_pos=m.undo_pos;index->undo_size=m.undo_size;
    }
};

PreparedOrchardChainstateWrite::PreparedOrchardChainstateWrite(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token)
    : impl_(std::make_unique<Impl>(mutex, db, token)) {}
PreparedOrchardChainstateWrite::~PreparedOrchardChainstateWrite() = default;

const storage::catalog::State* OrchardCompactChainstate::SelectedUnderLock(AnnotatedRecursiveMutex& mutex) const {
    mutex.AssertHeld("Orchard compact selected state");
    if(!selected_)return nullptr;
    if(selected_->mutex!=&mutex)throw std::logic_error("Orchard compact owner mutex mismatch");
    const auto* state=std::get_if<Selected::Orchard>(&selected_->value);
    return state?&state->catalog:nullptr;
}
const storage::catalog::HistoricalState* OrchardCompactChainstate::HistoricalUnderLock(AnnotatedRecursiveMutex& mutex) const {
    mutex.AssertHeld("Historical compact selected state");
    if(!selected_)return nullptr;
    if(selected_->mutex!=&mutex)throw std::logic_error("Historical compact owner mutex mismatch");
    return std::get_if<storage::catalog::HistoricalState>(&selected_->value);
}
const storage::LegacyRetirementRecord* OrchardCompactChainstate::RetirementUnderLock(AnnotatedRecursiveMutex& mutex) const {
    if(!SelectedUnderLock(mutex))return nullptr;
    return &selected_->OrchardState().retirement;
}


OrchardPoolCoinView OrchardPoolCoinView::Capture(const OrchardTransactionContext& context,
    const BlockHeader& header,const storage::catalog::State& catalog,
    const storage::catalog::Tree::Read& read,std::span<const MempoolProofView> entries) {
    namespace c=storage::catalog;
    catalog.Validate();
    RequireOrchardCatalog(read&&context.height&&context.height<=INT32_MAX&&
        context.height>=context.activation_height&&catalog.height==context.height-1&&
        catalog.block==context.parent_hash&&catalog.block==header.GetHash()&&
        catalog.parent==header.prev_block_hash&&catalog.activation==context.activation_height&&
        catalog.network==context.domain.network_code&&catalog.branch==context.domain.branch_id&&
        std::equal(catalog.genesis.begin(),catalog.genesis.end(),context.domain.genesis_wire.begin())&&
        catalog.leaf_activation==GetUtreexoMaturityLeafActivationHeight());
    const auto stump=UtreexoStump::deserialize(catalog.stump);
    const auto root=stump.getCommitment();
    RequireOrchardCatalog(root.size()==32&&std::equal(root.begin(),root.end(),header.utreexo_root.begin()));
    c::Tree transactions(c::Kind::Transactions,read),legacy(c::Kind::LegacyCoins,read);
    c::Tree nontransparent(c::Kind::NonTransparentCoins,read);
    OrchardPoolCoinView result(catalog.height,catalog.block);
    std::set<TxId> ids;
    for(const auto& entry:entries) {
        RequireOrchardCatalog(entry.body.HasBody());
        const auto id=entry.body.GetTxid();
        RequireOrchardCatalog(ids.insert(id).second&&!transactions.Find(catalog.transactions,c::TransactionKey(id)));
        RequireOrchardCatalog(entry.body.OutputCount()<=UINT32_MAX);
        for(size_t i=0;i<entry.body.OutputCount();++i)result.absent_.emplace(id,uint32_t(i));
    }
    for(const auto& entry:entries) {
        // There is no pending-parent proof representation in this transport.
        // Do not invent confirmed absence or permit omitted input proofs.
        for(const auto& point:entry.body.Inputs())
            RequireOrchardCatalog(!point.txid.IsNull()&&!ids.contains(point.txid));
        if(entry.proof.empty()) {
            RequireOrchardCatalog(entry.body.Inputs().empty());
            continue;
        }
        const auto payload=UtreexoTransactionPayload::Decode(entry.proof,RelayTransactionReadMode::AvailableFamilies);
        RequireOrchardCatalog(payload.Body().Serialize()==entry.body.Serialize());
        for(size_t i=0;i<payload.data_->proofs.size();++i) {
            const auto& point=entry.body.Inputs().at(i);
            const auto& claimed=payload.data_->proofs[i].second;
            const auto bytes=c::OutpointBytes(point);
            RequireOrchardCatalog(!nontransparent.Find(catalog.nontransparent,c::NonTransparentKey(bytes))&&
                !claimed.is_confidential&&claimed.commitment.empty());
            const auto old=legacy.Find(catalog.legacy,c::LegacyKey(bytes));
            uint32_t height=claimed.created_height;bool coinbase=claimed.is_coinbase;
            if(old) {
                RequireOrchardCatalog(old->size()==41&&old->compare(0,36,bytes)==0&&uint8_t((*old)[40])<=1);
                height=uint32_t(c::Number(*old,36,4));coinbase=uint8_t((*old)[40])!=0;
                RequireOrchardCatalog(height<catalog.leaf_activation&&height==claimed.created_height&&coinbase==claimed.is_coinbase);
            } else RequireOrchardCatalog(height>=catalog.leaf_activation);
            RequireOrchardCatalog(height<=catalog.height&&claimed.value<=orchard::kMaxMoneyUna&&
                transactions.Find(catalog.transactions,c::TransactionKey(point.txid)).has_value());
            RequireOrchardCatalog(result.inputs_.emplace(point,
                UTXOEntry(AmountUna::Una(claimed.value),claimed.scriptPubKey,height,coinbase)).second);
        }
        // The provisional map is local and never escapes on any failed proof.
        // Modern metadata becomes authenticated by its exact maturity-bound leaf;
        // legacy metadata was independently bound above before this check.
        RequireOrchardCatalog(payload.VerifyInputs(stump,catalog.height,result).has_value());
    }
    return result;
}

OrchardPoolCoinView OrchardCompactChainstate::CapturePoolCoinsUnderLock(AnnotatedRecursiveMutex& mutex,
    const OrchardTransactionContext& context,const BlockHeader& header,
    std::span<const MempoolProofView> entries) const {
    mutex.AssertHeld("Orchard compact pool input capture");
    RequireOrchardCatalog(selected_&&selected_->mutex==&mutex&&selected_->database);
    const auto selected=selected_;const auto& db=*selected->database;const auto& state=selected->OrchardState().catalog;
    const auto check=[&] {
        const auto tip=RequiredDisk(db.getTip()),validated=RequiredDisk(db.getValidatedTip());
        const auto durable_header=RequiredDisk(db.getHeader(state.block));
        RequireOrchardCatalog(tip.height>=0&&uint32_t(tip.height)==state.height&&tip.hash==state.block&&
            tip.work==state.work&&validated.height==tip.height&&validated.hash==tip.hash&&
            RequiredDisk(db.getBlockWork(state.block))==state.work&&
            durable_header.SerializeForHash()==header.SerializeForHash()&&
            RequiredDisk(db.getOrchardCatalogState(state.block))==state.Encode()&&
            storage::ReadOrchardCompactStorageBinding(db)==storage::EncodeOrchardCompactStorageBinding(state));
        if(state.height>=state.activation) {
            const auto orchard=RequiredDisk(db.getOrchardState());
            const auto retired=RequiredDisk(db.getLegacyRetirementState());
            RequireOrchardCatalog(orchard.height==state.height&&orchard.block_hash==state.block&&
                retired.height==state.height&&retired.block_hash==state.block&&
                retired.parent_hash==state.parent&&retired.record==selected->OrchardState().retirement);
        } else RequireOrchardCatalog(db.getOrchardState().status()==Status::NotFound&&
            db.getLegacyRetirementState().status()==Status::NotFound);
    };
    check();
    const auto read=[&](const uint256& id){return RequiredDisk(db.getOrchardCatalogNode(id));};
    auto result=OrchardPoolCoinView::Capture(context,header,state,read,entries);
    check();RequireOrchardCatalog(selected_==selected);
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::Connect(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    ConsensusUTXOSet& live, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, const BlockHeader& parent,
    const UtreexoForest& forest, const OrchardBranchMtpLookup& mtp,
    bool witness, bool checkpoint,
    const std::optional<storage::LegacyRetirementRecord>& boundary,
    const ValidatedOrchardBlock* detached,const PreparedOrchardCatalog* initial_catalog,bool require_catalog) {
    auto result = std::unique_ptr<PreparedOrchardChainstateWrite>(
        new PreparedOrchardChainstateWrite(mutex, db, token));
    auto& owner = *result->impl_;
    owner.CaptureStorageMode(false);
    std::map<uint32_t,uint64_t> recorded_mtp;
    const OrchardBranchMtpLookup capture_mtp=[&](uint32_t height) {
        const auto value=mtp?mtp(height):std::nullopt;
        if(value) {
            const auto [it,inserted]=recorded_mtp.emplace(height,*value);
            if(!inserted && it->second!=*value)throw OrchardStateLookupError(Status::Corruption);
        }
        return value;
    };
    auto staged = StageOrchardChainstateConnectUnderLock(db, token, context, block,
        parent, forest, capture_mtp, witness, checkpoint, owner.batch, boundary, detached);
    owner.undo_bytes=staged.block.undo.Serialize();
    owner.ConnectCatalog(initial_catalog,require_catalog,boundary,context,block,parent,
        UtreexoStump::fromForest(forest),staged.block.coins,UtreexoStump::fromForest(staged.forest.After()));
    owner.replay=RuntimeOrchardReplay{staged.block.orchard.Parent(),staged.block.orchard.Next(),
        owner.undo_bytes,std::move(recorded_mtp)};
    std::vector<UTXOPublicationChange> changes;
    changes.reserve(staged.block.coins.Changes().size());
    for (const auto& change : staged.block.coins.Changes())
        changes.push_back({change.outpoint, change.before, change.after});
    owner.publication.emplace(PreparedUTXOPublication::PrepareUnderLock(live,
        context.height - 1, parent.GetHash(), parent.utreexo_root, changes,
        staged.forest.After(), context.height, context.block_hash, block.Header().utreexo_root));
    owner.phase = Impl::Phase::Prepared;
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::Disconnect(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    ConsensusUTXOSet& live, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, const BlockHeader& parent,
    const UtreexoForest& forest, bool witness,bool require_catalog) {
    auto result = std::unique_ptr<PreparedOrchardChainstateWrite>(
        new PreparedOrchardChainstateWrite(mutex, db, token));
    auto& owner = *result->impl_;
    owner.CaptureStorageMode(false);
    const auto before_state=RequiredDisk(db.getOrchardState());
    const auto parent_state=RequiredDisk(db.getOrchardUndoParent(before_state));
    auto staged = StageOrchardChainstateDisconnectUnderLock(db, token, context,
        block, parent, forest, witness, owner.batch);
    const auto undo=RequiredDisk(db.getUndo(context.block_hash));
    owner.undo_bytes=undo.Serialize();
    owner.DisconnectCatalog(require_catalog,context,block,parent,forest,staged.forest);
    owner.replay=RuntimeOrchardReplay{parent_state,before_state,owner.undo_bytes,{}};
    std::vector<UTXOPublicationChange> changes;
    changes.reserve(staged.coins.size());
    for (const auto& change : staged.coins)
        changes.push_back({change.outpoint, change.before, change.after});
    owner.publication.emplace(PreparedUTXOPublication::PrepareUnderLock(live,
        context.height, context.block_hash, block.Header().utreexo_root, changes,
        std::move(staged.forest), context.height - 1, parent.GetHash(), parent.utreexo_root));
    owner.phase = Impl::Phase::Prepared;
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::ConnectIndexed(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    BlockStorage& files, CBlockIndex& index, ConsensusUTXOSet& live,
    const OrchardBlockContext& context, const OrchardBlockCandidate& block,
    const BlockHeader& parent, const UtreexoForest& forest, const OrchardBranchMtpLookup& mtp,
    bool witness, bool checkpoint, const std::optional<storage::LegacyRetirementRecord>& boundary, bool contextual_header_validated,
    const ValidatedOrchardBlock* detached,const PreparedOrchardCatalog* initial_catalog,bool require_catalog) {
    const char* operation="indexed/coins-and-state";
    try {
    auto result=Connect(mutex,db,token,live,context,block,parent,forest,mtp,witness,checkpoint,boundary,detached,initial_catalog,require_catalog);
    operation="indexed/body-and-undo-locators";
    result->impl_->PrepareIndex(files,index,context,block,true,contextual_header_validated);
    operation="indexed/delivery-append";
    result->impl_->outbox_before=outbox_detail::Append(db,result->impl_->batch,context,block,true,result->impl_->replay);
    result->impl_->outbox_staged=true;
    return result;
    } catch(const OrchardStateLookupError& error) {
        if(error.Operation())throw;
        throw OrchardStateLookupError(error.SourceStatus(),operation);
    }
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::DisconnectIndexed(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    BlockStorage& files, CBlockIndex& index, ConsensusUTXOSet& live,
    const OrchardBlockContext& context, const OrchardBlockCandidate& block,
    const BlockHeader& parent, const UtreexoForest& forest, bool witness,bool require_catalog) {
    auto result=Disconnect(mutex,db,token,live,context,block,parent,forest,witness,require_catalog);
    result->impl_->PrepareIndex(files,index,context,block,false);
    result->impl_->outbox_before=outbox_detail::Append(db,result->impl_->batch,context,block,false,result->impl_->replay);
    result->impl_->outbox_staged=true;
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::ConnectCompactIndexed(
    AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
    BlockStorage& files,CBlockIndex& index,OrchardCompactChainstate& live,
    const OrchardBlockContext& context,const OrchardBlockCandidate& block,
    const BlockHeader& parent,const OrchardBranchMtpLookup& mtp,bool witness,
    const std::optional<storage::LegacyRetirementRecord>& boundary,bool contextual_header_validated,
    const PreparedOrchardCatalog* initial,const ValidatedOrchardBlock* detached) {
    auto result=std::unique_ptr<PreparedOrchardChainstateWrite>(
        new PreparedOrchardChainstateWrite(mutex,db,token));
    auto& owner=*result->impl_;
    owner.compact_live=&live;owner.compact_before=live.selected_;
    // A decoded database row cannot enroll a new live owner. The first
    // transition consumes genuine independent replay; descendants must match
    // the already-published selected catalog and its database/mutex binding.
    Impl::CatalogRequire(bool(owner.compact_before)!=bool(initial));
    auto effective_boundary=boundary;
    if(owner.compact_before) {
        Impl::CatalogRequire(owner.compact_before->database==&db&&owner.compact_before->mutex==&mutex);
        if(context.height==context.activation_height) {
            if(boundary)Impl::CatalogRequire(*boundary==owner.compact_before->OrchardState().retirement);
            effective_boundary=owner.compact_before->OrchardState().retirement;
        } else {
            Impl::CatalogRequire(!boundary&&
                RequiredDisk(db.getLegacyRetirementState()).record==owner.compact_before->OrchardState().retirement);
        }
    }
    const auto selected=owner.SelectCatalog(initial,effective_boundary,context,parent);
    owner.CaptureStorageMode(true,&selected,!owner.compact_before);
    if(owner.compact_before)
        Impl::CatalogRequire(owner.compact_before->OrchardState().catalog.Encode()==selected.Encode());
    std::map<uint32_t,uint64_t> replay_mtp;
    const OrchardBranchMtpLookup captured_mtp=[&](uint32_t height) {
        const auto value=mtp?mtp(height):std::nullopt;
        if(value) {
            const auto [at,inserted]=replay_mtp.emplace(height,*value);
            if(!inserted&&at->second!=*value)throw OrchardStateLookupError(Status::Corruption);
        }
        return value;
    };
    auto staged=StageOrchardCompactChainstateConnectUnderLock(db,token,context,block,parent,
        selected,captured_mtp,witness,owner.batch,effective_boundary,detached);
    owner.undo_bytes=staged.block.undo.Serialize();
    auto next=owner.ConnectCatalog(initial,true,effective_boundary,context,block,parent,
        UtreexoStump::deserialize(selected.stump),staged.block.coins,staged.stump);
    Impl::CatalogRequire(bool(next));
    if(owner.storage_mode_after)
        Impl::CatalogRequire(owner.batch.Put(storage::OrchardCompactStorageKey,*owner.storage_mode_after).ok());
    owner.replay=RuntimeOrchardReplay{staged.block.orchard.Parent(),staged.block.orchard.Next(),
        owner.undo_bytes,std::move(replay_mtp)};
    owner.compact_after=std::make_shared<const OrchardCompactChainstate::Selected>(
        OrchardCompactChainstate::Selected{&db,&mutex,std::move(*next),
            owner.compact_before?owner.compact_before->OrchardState().retirement:*effective_boundary});
    owner.PrepareIndex(files,index,context,block,true,contextual_header_validated);
    owner.outbox_before=outbox_detail::Append(db,owner.batch,context,block,true,owner.replay);
    owner.outbox_staged=true;
    owner.phase=Impl::Phase::Prepared;
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::DisconnectCompactIndexed(
    AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
    BlockStorage& files,CBlockIndex& index,OrchardCompactChainstate& live,
    const OrchardBlockContext& context,const OrchardBlockCandidate& block,
    const BlockHeader& parent,const OrchardBranchMtpLookup& mtp,bool witness,
    const ValidatedOrchardBlock* detached) {
    auto result=std::unique_ptr<PreparedOrchardChainstateWrite>(new PreparedOrchardChainstateWrite(mutex,db,token));
    auto& owner=*result->impl_;owner.compact_live=&live;owner.compact_before=live.selected_;
    Impl::CatalogRequire(owner.compact_before&&owner.compact_before->database==&db&&
        owner.compact_before->mutex==&mutex);
    owner.CaptureStorageMode(true,&owner.compact_before->OrchardState().catalog);
    const auto current=owner.CaptureCatalog(context.block_hash);
    Impl::CatalogRequire(current&&*current==owner.compact_before->OrchardState().catalog.Encode());
    Impl::CatalogRequire(RequiredDisk(db.getLegacyRetirementState()).record==owner.compact_before->OrchardState().retirement);
    const auto previous=owner.SelectCatalog(nullptr,std::nullopt,context,parent);
    const auto before_state=RequiredDisk(db.getOrchardState());
    const auto parent_state=RequiredDisk(db.getOrchardUndoParent(before_state));
    auto staged=StageOrchardCompactChainstateDisconnectUnderLock(db,token,context,block,parent,
        owner.compact_before->OrchardState().catalog,previous,mtp,witness,owner.batch,detached);
    owner.undo_bytes=RequiredDisk(db.getUndo(context.block_hash)).Serialize();
    // Rebuild every catalog effect against the retained parent and require
    // the exact child plus already-present immutable nodes. Undo never repairs
    // missing nodes or fabricates a new predecessor from a decoded stump.
    owner.ConnectCatalog(nullptr,true,std::nullopt,context,block,parent,staged.stump,
        staged.forward_coins,UtreexoStump::deserialize(owner.compact_before->OrchardState().catalog.stump),true);
    owner.replay=RuntimeOrchardReplay{parent_state,before_state,owner.undo_bytes,{}};
    owner.compact_after=std::make_shared<const OrchardCompactChainstate::Selected>(
        OrchardCompactChainstate::Selected{&db,&mutex,previous,owner.compact_before->OrchardState().retirement});
    owner.PrepareIndex(files,index,context,block,false);
    owner.outbox_before=outbox_detail::Append(db,owner.batch,context,block,false,owner.replay);
    owner.outbox_staged=true;owner.phase=Impl::Phase::Prepared;
    return result;
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::HistoricalCompactIndexed(
    AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,BlockStorage& files,
    CBlockIndex& index,OrchardCompactChainstate& live,const Block& block,
    const PreparedHistoricalCatalog& lower,const HigherHistoricalCatalog& higher,bool connecting) {
    return HistoricalCompactPrepared(mutex,db,token,files,index,live,block,&lower,nullptr,&higher,connecting);
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::HistoricalCompactRangeIndexed(
    AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,BlockStorage& files,
    CBlockIndex& index,OrchardCompactChainstate& live,const Block& block,
    const PreparedHistoricalCatalogRange& range,const PreparedOrchardCatalog* boundary,bool connecting) {
    std::optional<HigherHistoricalCatalog> higher;
    if(boundary)higher.emplace(std::cref(*boundary));
    return HistoricalCompactPrepared(mutex,db,token,files,index,live,block,nullptr,&range,higher?&*higher:nullptr,connecting);
}

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::HistoricalCompactPrepared(
    AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,BlockStorage& files,
    CBlockIndex& index,OrchardCompactChainstate& live,const Block& block,
    const PreparedHistoricalCatalog* lower,const PreparedHistoricalCatalogRange* range,
    const HigherHistoricalCatalog* higher,bool connecting) {
    namespace c=storage::catalog;
    auto result=std::unique_ptr<PreparedOrchardChainstateWrite>(new PreparedOrchardChainstateWrite(mutex,db,token));
    auto& owner=*result->impl_;
    const char* operation="historical/catalog-capture";
    const auto require=[&operation](bool ok){if(!ok)throw OrchardStateLookupError(Status::Corruption,operation);};
    const auto checked=[&](Status status){require(status==Status::Ok);};
    struct View {
        std::variant<c::State,c::HistoricalState> state;
        std::optional<storage::LegacyRetirementRecord> retirement;
        std::vector<uint8_t> stump,undo;
        std::string delta;
        uint32_t height=0;uint256 hash,parent,wire;arith_uint256 work{0};
        std::string bytes,frontier,anchors;
        ChainDB::ShieldedTipMarker marker;
        std::vector<std::pair<uint32_t,uint256>> nullifiers;
    };
    const auto fill=[&](auto state,const auto& proof,const uint256& wire,
        std::optional<storage::LegacyRetirementRecord> retirement)->View {
        View v;state.Validate();v.height=state.height;v.hash=state.block;v.parent=state.parent;v.work=state.work;
        v.bytes=state.Encode();v.state=std::move(state);v.retirement=std::move(retirement);v.wire=wire;
        require(v.height<=INT32_MAX && proof.Height()==v.height && proof.Forest() && !wire.IsNull());
        const auto f=proof.ShieldedTree()->SerializeFrontier(),a=proof.ShieldedAnchors()->SerializePersistenceBytes();
        v.frontier.assign(f.begin(),f.end());v.anchors.assign(a.begin(),a.end());
        const auto root=proof.ShieldedTree()->Root();uint256 tree_root;std::copy(root.begin(),root.end(),tree_root.begin());
        const auto count=proof.ShieldedNullifiers()->TryCount();require(bool(count));
        v.marker={int32_t(v.height),v.hash,tree_root,proof.ShieldedTree()->Size(),*count};
        constexpr auto limits=SelectedParentReplayWorkLimits();size_t charge=v.frontier.size();
        require(charge<=limits.serialized_bytes && v.anchors.size()<=limits.serialized_bytes-charge);charge+=v.anchors.size();
        std::set<uint256> seen;
        require(proof.ShieldedNullifiers()->ForEach([&](uint32_t height,const uint8_t* bytes) {
            if(height>v.height || v.nullifiers.size()>=*count || limits.serialized_bytes-charge<36)return false;
            uint256 hash;std::copy_n(bytes,32,hash.begin());if(!seen.insert(hash).second)return false;
            v.nullifiers.emplace_back(height,hash);charge+=36;return true;
        }) && v.nullifiers.size()==*count);
        std::sort(v.nullifiers.begin(),v.nullifiers.end());
        v.stump=UtreexoStump::fromForest(*proof.Forest()).serialize();
        const auto forest_root=proof.Forest()->getCommitment();
        require(UtreexoStump::deserialize(v.stump).getCommitment()==forest_root);
        if(v.height) {
            const auto& tail=proof.UndoTail();
            require(tail.size()==1 && tail.front().height==v.height && tail.front().block_hash==v.hash);
            const auto& undo=tail.front().undo;
            require(undo.height==v.height && undo.block_hash==v.hash && undo.utreexo_delta);
            const auto body=RequiredDisk(storage::ReadArchivalBlock(db,&files,v.hash));
            uint256 digest;crypto::CSHA256().Write(body.Serialize()).Finalize(digest.data);
            require(body.GetHash()==v.hash && digest==wire);
            UndoRecord stored;
            for(const auto& e:undo.spent_coins)stored.spent.emplace_back(e.txid,e.vout,e.coin.value.GetUna(),
                e.coin.scriptPubKey,e.coin.isCoinbase,e.coin.height,e.coin.is_confidential,e.coin.commitment);
            for(const auto& tx:body.vtx) {
                require(tx.vout.size()<=UINT32_MAX);
                for(size_t n=0;n<tx.vout.size();++n)stored.created.emplace_back(tx.GetTxid().AsUint256(),uint32_t(n));
            }
            stored.pre_block_shielded_frontier=undo.pre_block_shielded_frontier;
            stored.pre_block_shielded_anchors=undo.pre_block_shielded_anchors;
            stored.pre_reset_shielded_epoch=undo.pre_reset_shielded_epoch;
            v.undo=stored.Serialize();std::string error;
            require(SerializeUtreexoDelta(*undo.utreexo_delta,v.delta,error));
        }
        return v;
    };
    const auto historical=[&](const PreparedHistoricalCatalog& p) {
        p.Check();require(&p.db_==&db);
        return fill(p.State(),p.proof_.ProvenState(),p.wire_,std::nullopt);
    };
    const auto boundary=[&](const PreparedOrchardCatalog& p) {
        p.Check();require(&p.db_==&db);
        const auto header=RequiredDisk(db.getHeader(p.target_.hash));require(header.GetHash()==p.target_.hash);
        c::State s;s.network=p.record_.network_code;s.genesis=p.record_.genesis;
        s.branch=p.record_.branch_id;s.activation=p.record_.activation_height;s.leaf_activation=p.leaf_activation_;
        s.height=p.target_.height;s.block=p.target_.hash;s.parent=header.prev_block_hash;s.work=p.target_.chainwork;
        s.transactions=p.transactions_;s.legacy=p.legacy_;s.nontransparent=p.nontransparent_;
        s.transaction_count=p.transaction_count_;s.legacy_count=p.legacy_count_;s.nontransparent_count=p.nontransparent_count_;
        s.stump=p.stump_;return fill(std::move(s),p.proof_.ProvenState(),p.wire_,p.record_);
    };
    const auto ranged=[&](const PreparedHistoricalCatalogRange& range,uint32_t height) {
        range.Check();require(&range.db_==&db);
        const auto state=range.At(height);const auto checkpoint=range.proof_.CheckpointAt(height);
        require(state.height==checkpoint.target.height && state.block==checkpoint.target.hash &&
            state.parent==checkpoint.parent && state.work==checkpoint.target.chainwork &&
            state.stump==checkpoint.snapshot.stump && height<=INT32_MAX);
        View v;v.state=state;v.height=state.height;v.hash=state.block;v.parent=state.parent;v.work=state.work;
        v.bytes=state.Encode();v.wire=checkpoint.wire_hash;v.stump=checkpoint.snapshot.stump;
        v.frontier.assign(checkpoint.frontier.begin(),checkpoint.frontier.end());
        v.anchors.assign(checkpoint.anchors.begin(),checkpoint.anchors.end());
        v.marker={int32_t(height),state.block,checkpoint.snapshot.tree_root,
            checkpoint.snapshot.tree_size,checkpoint.snapshot.nullifier_count};
        v.nullifiers=checkpoint.nullifiers;v.undo=checkpoint.undo;v.delta=checkpoint.delta;
        return v;
    };
    require(bool(lower)!=bool(range));
    if(range) {
        range->Check();require(index.height>0 && &range->db_==&db);
        if(index.height>range->Last()) {
            require(higher && std::holds_alternative<std::reference_wrapper<const PreparedOrchardCatalog>>(*higher));
            const auto& top=std::get<std::reference_wrapper<const PreparedOrchardCatalog>>(*higher).get();
            top.Check();require(&top.db_==&db && uint64_t(range->Last())+1==top.target_.height &&
                top.target_.height==index.height);
            const auto header=RequiredDisk(db.getHeader(top.target_.hash));
            require(header.GetHash()==top.target_.hash && header.prev_block_hash==range->At(range->Last()).block);
        }
    } else require(higher!=nullptr);
    const auto low=range?ranged(*range,index.height-1):historical(*lower);
    const auto high=range && index.height<=range->Last()?ranged(*range,index.height):
        std::visit([&](const auto& reference)->View {
            using T=std::remove_cvref_t<decltype(reference.get())>;
            if constexpr(std::is_same_v<T,PreparedHistoricalCatalog>)return historical(reference.get());
            else return boundary(reference.get());
        },*higher);
    operation="historical/adjacent-proofs";
    require(uint64_t(low.height)+1==high.height && high.parent==low.hash && high.work>low.work &&
        high.height<Params().orchard_activation_height && high.hash==block.GetHash() && !block.vtx.empty());
    operation="historical/exact-body";
    const auto wire=block.Serialize();uint256 wire_hash;crypto::CSHA256().Write(wire).Finalize(wire_hash.data);
    require(wire_hash==high.wire);
    const auto& before=connecting?low:high;const auto& after=connecting?high:low;
    operation="historical/selected-owner";
    owner.compact_live=&live;owner.compact_before=live.selected_;
    require(owner.compact_before && owner.compact_before->database==&db && owner.compact_before->mutex==&mutex);
    const auto selected_bytes=std::visit([](const auto& value) {
        using T=std::remove_cvref_t<decltype(value)>;
        if constexpr(std::is_same_v<T,OrchardCompactChainstate::Selected::Orchard>)return value.catalog.Encode();
        else return value.Encode();
    },owner.compact_before->value);
    operation="historical/selected-catalog";
    require(selected_bytes==before.bytes);
    operation="historical/storage-mode";
    const auto mode=std::visit([](const auto& s){return storage::EncodeOrchardCompactStorageBinding(s);},before.state);
    require(mode==std::visit([](const auto& s){return storage::EncodeOrchardCompactStorageBinding(s);},after.state));
    owner.storage_mode_before=storage::ReadOrchardCompactStorageBinding(db);
    require(owner.storage_mode_before && *owner.storage_mode_before==mode);owner.storage_mode_captured=true;
    const auto read_catalog=[&](const View& v) {
        return v.retirement?db.getOrchardCatalogState(v.hash):db.getHistoricalCompactCatalogState(v.hash);
    };
    operation="historical/durable-catalog";
    require(RequiredDisk(read_catalog(before))==before.bytes);
    const auto existing_after=read_catalog(after);
    require(existing_after.ok()?*existing_after==after.bytes:existing_after.status()==Status::NotFound);
    operation="historical/headers-work-stumps";
    const auto low_header=RequiredDisk(db.getHeader(low.hash));
    require(low_header.GetHash()==low.hash && low_header.prev_block_hash==low.parent);
    require(RequiredDisk(db.getHeader(high.hash)).SerializeForHash()==block.header.SerializeForHash());
    for(const auto* view:{&low,&high}) {
        require(RequiredDisk(db.getBlockHeight(view->hash))==int(view->height) && RequiredDisk(db.getBlockWork(view->hash))==view->work);
        const auto stump=UtreexoStump::deserialize(view->stump);
        require(stump.serialize()==view->stump);const auto root=stump.getCommitment();
        const auto header=RequiredDisk(db.getHeader(view->hash));
        require(root.size()==32 && std::equal(root.begin(),root.end(),header.utreexo_root.begin()));
        require(std::visit([&](const auto& s){return view->stump==s.stump;},view->state));
    }
    require(high.work==low.work+GetBlockProof(block.header.difficulty));
    operation="historical/archival-body";
    const auto durable_body=RequiredDisk(storage::ReadArchivalBlock(db,&files,high.hash));
    require(durable_body.Serialize()==wire);
    operation="historical/global-parent";
    require(index.pprev && index.pprev->hash==low.hash && index.pprev->height==low.height);
    operation="historical/global-selected";
    auto metadata=RequiredDisk(db.getHeaderMetadata(high.hash));
    // The actual completed historical/boundary proofs above bind both bodies
    // and work. Only their reconstructed validation levels may differ from
    // disk. Failure, availability and every locator remain exact.
    const auto live_before=[&](const CBlockIndex& entry,const BlockHeader& header,
        const ChainDB::PersistedHeaderMetadata& durable) {
        require(!(durable.status_flags&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD)) &&
            (entry.status==durable.status_flags || entry.status==(durable.status_flags|BLOCK_VALID_MASK)));
        auto captured=durable;captured.status_flags=entry.status;
        require(IndexMatches(entry,header,captured));return captured;
    };
    const auto high_live_before=live_before(index,block.header,metadata);
    require(metadata.height==int(high.height) && metadata.chainwork==high.work &&
        metadata.parent_hash==low.hash && (metadata.status_flags&BLOCK_HAVE_DATA));
    operation="historical/independent-undo";
    require(!high.undo.empty() && !high.delta.empty());
    auto stored_undo=UndoRecord::Deserialize(high.undo);
    require(stored_undo.Serialize()==high.undo);
    size_t created=0;
    for(const auto& tx:block.vtx) {
        require(tx.vout.size()<=UINT32_MAX);
        for(size_t n=0;n<tx.vout.size();++n) {
            require(created<stored_undo.created.size());const auto& output=stored_undo.created[created++];
            require(output.txid==tx.GetTxid().AsUint256() && output.vout==uint32_t(n));
        }
    }
    require(created==stored_undo.created.size());
    operation="historical/retained-undo";
    owner.undo_bytes=stored_undo.Serialize();const auto existing_undo=db.getUndo(high.hash);
    require(existing_undo.ok()?existing_undo->Serialize()==owner.undo_bytes:existing_undo.status()==Status::NotFound);
    operation="historical/retained-delta";
    const auto& delta=high.delta;UtreexoDelta parsed_delta;std::string error;
    require(DeserializeUtreexoDelta(delta,parsed_delta,error));
    const auto delta_key=MakeUtreexoDeltaUndoKey(high.hash);std::string old_delta;
    const auto delta_status=db.getRaw(delta_key,old_delta);require(delta_status==Status::Ok?old_delta==delta:delta_status==Status::NotFound);
    // Reconstruct the historical BIP158 filter from the independently validated
    // block and spent coins, including outputs spent later in this same block.
    operation="historical/filter-inputs";
    std::map<OutPoint,std::vector<uint8_t>> scripts_by_coin;
    std::vector<std::vector<uint8_t>> scripts;
    for(const auto& coin:stored_undo.spent)
        require(scripts_by_coin.emplace(OutPoint(TxId(coin.prev_txid),coin.prev_vout),coin.scriptPubKey).second);
    for(const auto& tx:block.vtx) {
        for(size_t n=0;n<tx.vout.size();++n) {
            const auto& script=tx.vout[n].scriptPubKey;
            const auto [it,added]=scripts_by_coin.emplace(OutPoint(tx.GetTxid(),uint32_t(n)),script);
            require(added || it->second==script);
            if(!script.empty() && script.front()!=0x6a)scripts.push_back(script);
        }
    }
    for(size_t n=1;n<block.vtx.size();++n)for(const auto& input:block.vtx[n].vin) {
        const auto found=scripts_by_coin.find(OutPoint(input.prevout.txid,input.prevout.vout));require(found!=scripts_by_coin.end());
        if(!found->second.empty())scripts.push_back(found->second);
    }
    const auto filter=GCSFilter::Build(scripts,low.hash);
    std::string filter_error;
    operation="historical/filter-commitment";
    require(ValidateFilterCommitment(block.vtx.front(),filter.GetHash(),high.height,filter_error));
    operation="historical/retained-filter";
    const auto old_filter=db.getBlockFilter(high.hash);
    require(old_filter.ok()?(old_filter->data==filter.encoded_data&&old_filter->element_count==filter.element_count):
        old_filter.status()==Status::NotFound);
    operation="historical/parent-metadata";
    const auto low_metadata=RequiredDisk(db.getHeaderMetadata(low.hash));
    const auto low_live_before=live_before(*index.pprev,low_header,low_metadata);
    require(low_metadata.height==int(low.height)&&low_metadata.chainwork==low.work);
    operation="historical/transaction-index";
    std::vector<std::pair<uint256,std::optional<std::pair<uint256,uint32_t>>>> tx_before;
    require(block.vtx.size()<=UINT32_MAX);
    for(size_t n=0;n<block.vtx.size();++n) {
        const auto id=block.vtx[n].GetTxid().AsUint256();const auto location=db.getTxLocation(id);
        if(connecting)require(location.status()==Status::NotFound);
        else require(location.ok()&&location->first==high.hash&&location->second==uint32_t(n));
        tx_before.emplace_back(id,location.ok()?std::make_optional(*location):std::nullopt);
    }
    operation="historical/height-index";
    const auto high_height_before=db.getBlockHashByHeight(int(high.height));
    require(connecting?high_height_before.status()==Status::NotFound:
        (high_height_before.ok()&&*high_height_before==high.hash));
    // Recheck all mutable disk inputs at commit after external preparation.
    // No replay engine is borrowed by the callback; only captured values remain.
    const auto durable_ready=[&db,&files,parent_index=index.pprev,low_header,low_metadata,low_live_before,
        low_hash=low.hash,high_hash=high.hash,low_height=low.height,high_height=high.height,
        low_work=low.work,high_work=high.work,wire,existing_after,after_hash=after.hash,
        after_is_boundary=bool(after.retirement),existing_undo,delta_key,delta_status,old_delta,
        old_filter,tx_before,high_height_before]() {
        const auto check=[](bool ok){Impl::CatalogRequire(ok);};
        check(IndexMatches(*parent_index,low_header,low_live_before));
        const auto metadata_now=RequiredDisk(db.getHeaderMetadata(low_hash));
        check(MetadataFields(metadata_now)==MetadataFields(low_metadata));
        check(RequiredDisk(db.getHeader(low_hash)).SerializeForHash()==low_header.SerializeForHash());
        check(RequiredDisk(db.getBlockHeight(low_hash))==int(low_height)&&
            RequiredDisk(db.getBlockHeight(high_hash))==int(high_height)&&
            RequiredDisk(db.getBlockWork(low_hash))==low_work&&RequiredDisk(db.getBlockWork(high_hash))==high_work);
        check(RequiredDisk(storage::ReadArchivalBlock(db,&files,high_hash)).Serialize()==wire);
        const auto height_now=db.getBlockHashByHeight(int(high_height));
        check(height_now.status()==high_height_before.status()&&(!height_now.ok()||*height_now==*high_height_before));
        const auto catalog_now=after_is_boundary?db.getOrchardCatalogState(after_hash):db.getHistoricalCompactCatalogState(after_hash);
        check(catalog_now.status()==existing_after.status()&&(!catalog_now.ok()||*catalog_now==*existing_after));
        const auto undo_now=db.getUndo(high_hash);
        check(undo_now.status()==existing_undo.status()&&(!undo_now.ok()||undo_now->Serialize()==existing_undo->Serialize()));
        std::string delta_now;check(db.getRaw(delta_key,delta_now)==delta_status&&
            (delta_status!=Status::Ok||delta_now==old_delta));
        const auto filter_now=db.getBlockFilter(high_hash);
        check(filter_now.status()==old_filter.status()&&(!filter_now.ok()||
            (filter_now->data==old_filter->data&&filter_now->element_count==old_filter->element_count)));
        for(const auto& [id,location]:tx_before) {
            const auto now=db.getTxLocation(id);
            check(location?(now.ok()&&*now==*location):now.status()==Status::NotFound);
        }
    };
    durable_ready();
    // Capture exact selected legacy bytes and every nullifier before preparing
    // any durable change. A marker/count alone cannot certify those rows.
    const auto check_before=[&db,before=before]() {
        const auto check=[](bool ok){Impl::CatalogRequire(ok);};
        const auto tip=RequiredDisk(db.getTip()),validated=RequiredDisk(db.getValidatedTip());
        check(tip.height==int(before.height)&&tip.hash==before.hash&&tip.work==before.work&&
            validated.height==tip.height&&validated.hash==tip.hash&&
            RequiredDisk(db.getBlockHashByHeight(int(before.height)))==before.hash);
        // Existing checked iterators refuse malformed records. A single
        // present full/prebase coin is enough to refuse compact publication.
        bool full_coin=false,prebase_coin=false;
        check(db.forEachUTXO([&](const auto&,uint32_t,const auto&){full_coin=true;return false;})==Status::Ok&&!full_coin);
        check(db.forEachPreBaseCoin([&](const auto&,uint32_t,const auto&){prebase_coin=true;return false;})==Status::Ok&&!prebase_coin);
        check(db.getPreBaseCoinSetBase().status()==Status::NotFound);
        check(db.hasSeparatedShieldedState() && db.getOrchardState().status()==Status::NotFound &&
            db.getLegacyRetirementState().status()==Status::NotFound);
        check(RequiredDisk(db.getShieldedState(ChainDB::ShieldedStateRecord::Frontier))==before.frontier&&
            RequiredDisk(db.getShieldedState(ChainDB::ShieldedStateRecord::AnchorHistory))==before.anchors);
        const auto marker=RequiredDisk(db.getShieldedTipMarker());const auto& m=before.marker;
        check(std::tie(marker.height,marker.block_hash,marker.shielded_root,marker.tree_size,marker.nullifier_count)==
            std::tie(m.height,m.block_hash,m.shielded_root,m.tree_size,m.nullifier_count));
        size_t at=0;bool valid=true;
        const auto status=db.forEachShieldedNullifier([&](uint32_t height,const uint8_t* p) {
            if(at==before.nullifiers.size() || height!=before.nullifiers[at].first ||
                !std::equal(p,p+32,before.nullifiers[at].second.begin())){valid=false;return false;}++at;return true;
        });check(status==Status::Ok&&valid&&at==before.nullifiers.size());
        const auto catalog=before.retirement?db.getOrchardCatalogState(before.hash):db.getHistoricalCompactCatalogState(before.hash);
        check(catalog.ok()&&*catalog==before.bytes);
        const auto forest=RequiredDisk(db.getForestTipMarker());
        const auto header=RequiredDisk(db.getHeader(before.hash));
        check(forest.height==int(before.height)&&forest.block_hash==before.hash&&forest.forest_root==header.utreexo_root);
    };
    check_before();owner.historical_ready=[check_before,durable_ready](){check_before();durable_ready();};
    operation="historical/retirement-owner";
    if(before.retirement)require(owner.compact_before->OrchardState().retirement==*before.retirement);
    operation="historical/delivery-profile";
    const auto profile=SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);require(bool(profile));
    operation="historical/delivery-prepare";
    auto delivery=PreparedHistoricalRuntimeOutbox::PrepareUnderLock(db,*profile,block,high.height,
        connecting?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect);require(bool(delivery));
    owner.outbox_before=outbox_detail::Raw(db,outbox_detail::head_key);require(bool(owner.outbox_before));
    // All fallible preparation precedes the single durable commit. Immutable
    // undo append records may be abandoned, never published without the batch.
    operation="historical/undo-locator";
    auto next_metadata=metadata;
    if(metadata.status_flags&BLOCK_HAVE_UNDO)require(metadata.undo_size&&
        RequiredDisk(files.readUndo({metadata.undo_file,metadata.undo_pos,metadata.undo_size}))==owner.undo_bytes);
    else {
        require(!metadata.undo_file&&!metadata.undo_pos&&!metadata.undo_size);
        const auto pos=RequiredDisk(files.writeUndo(high.hash,owner.undo_bytes));require(pos.offset<=UINT32_MAX);
        next_metadata.undo_file=pos.file_number;next_metadata.undo_pos=uint32_t(pos.offset);next_metadata.undo_size=pos.size;
        next_metadata.status_flags|=BLOCK_HAVE_UNDO;
    }
    checked(db.putBlockFilter(token,high.hash,filter.encoded_data,filter.element_count,&owner.batch));
    checked(db.putHeaderMetadata(token,high.hash,next_metadata,&owner.batch));
    owner.indexed_header=block.header;owner.index_before=metadata;owner.index_after=next_metadata;owner.index=&index;
    owner.historical_index_live_before=high_live_before;
    auto high_live_after=next_metadata;
    // Both adjacent catalogs above carry completed independent replay proofs
    // for these exact bodies. Publish the proven live validation levels only
    // after commit, including a boundary parent first reached from below.
    // Durable status, failure flags and every locator retain their exact checks.
    high_live_after.status_flags|=BLOCK_VALID_MASK;
    owner.historical_index_live_after=high_live_after;
    checked(db.putUndo(token,high.hash,stored_undo,&owner.batch));owner.batch.Put(delta_key,delta);
    std::visit([&](const auto& state) {
        using T=std::remove_cvref_t<decltype(state)>;
        if constexpr(std::is_same_v<T,c::State>)checked(db.stageOrchardCatalogState(token,after.hash,after.bytes,owner.batch));
        else checked(db.stageHistoricalCompactCatalogState(token,after.hash,after.bytes,owner.batch));
    },after.state);
    checked(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,after.frontier,&owner.batch));
    checked(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,after.anchors,&owner.batch));
    require(RequiredDisk(db.deleteAllShieldedNullifiers(token,&owner.batch))==before.nullifiers.size());
    for(const auto& [height,hash]:after.nullifiers)checked(db.putShieldedNullifier(token,height,hash.data,&owner.batch));
    checked(db.putShieldedTipMarker(token,after.marker,&owner.batch));
    const auto after_header=connecting?block.header:low_header;
    checked(db.putForestTipMarker(token,{int32_t(after.height),after.hash,after_header.utreexo_root},&owner.batch));
    for(size_t i=0;i<block.vtx.size();++i) {
        const auto id=block.vtx[i].GetTxid().AsUint256();const auto location=db.getTxLocation(id);
        if(connecting) {
            require(location.status()==Status::NotFound);
            checked(db.putTxIndex(token,id,high.hash,uint32_t(i),&owner.batch));
        } else {
            require(location.ok()&&location->first==high.hash&&location->second==uint32_t(i));
            checked(db.deleteTxIndex(token,id,&owner.batch));
        }
    }
    if(connecting) {
        require(db.getBlockHashByHeight(int(high.height)).status()==Status::NotFound);
        checked(db.putHeightIndex(token,int(high.height),high.hash,&owner.batch));
    } else checked(db.deleteHeightIndex(token,int(high.height),&owner.batch));
    checked(db.setTip(token,after.hash,int(after.height),after.work,&owner.batch));
    checked(db.setValidatedTip(token,after.hash,int(after.height),&owner.batch));
    owner.compact_after=std::visit([&](const auto& state)->std::shared_ptr<const OrchardCompactChainstate::Selected> {
        using T=std::remove_cvref_t<decltype(state)>;
        if constexpr(std::is_same_v<T,c::State>) {
            require(bool(after.retirement));return std::make_shared<const OrchardCompactChainstate::Selected>(&db,&mutex,state,*after.retirement);
        } else return std::make_shared<const OrchardCompactChainstate::Selected>(&db,&mutex,state);
    },after.state);
    delivery->StageOrTerminateUnderLock(db,owner.batch);owner.outbox_staged=true;
    owner.phase=Impl::Phase::Prepared;return result;
}

void PreparedOrchardChainstateWrite::Commit() {
    auto& owner = *impl_;
    owner.CheckOwner();
    if (owner.phase != Impl::Phase::Prepared)
        throw std::logic_error("Orchard chainstate write is not prepared or was already consumed");
    try {
        owner.CheckIndex();
        if(owner.compact_live) {
            Impl::CatalogRequire(!owner.publication&&owner.compact_after&&
                owner.compact_live->selected_==owner.compact_before);
        } else {
            if(!owner.publication)throw std::logic_error("Orchard publication is missing");
            owner.publication->CheckReadyUnderLock();
        }
    } catch (...) {
        owner.phase = Impl::Phase::Aborted;
        throw;
    }
    owner.phase = Impl::Phase::Writing;
    try {
        if (owner.db.writeBatch(owner.token, std::move(owner.batch), true) != Status::Ok)
            throw std::runtime_error("Orchard durable chainstate write failed");
    } catch (...) {
        std::fputs("FATAL: Orchard chainstate write failed; restart from durable state required\n", stderr);
        std::terminate();
    }
    if(owner.compact_live) {
        // All allocation and readiness checks precede durability. Swapping
        // shared owners publishes the complete immutable catalog without a
        // potentially throwing copy or a second database write.
        owner.compact_live->selected_.swap(owner.compact_after);
    } else std::move(*owner.publication).PublishAfterCommitUnderLock();
    owner.PublishIndex();
    owner.phase = Impl::Phase::Committed;
}
} // namespace dinero
