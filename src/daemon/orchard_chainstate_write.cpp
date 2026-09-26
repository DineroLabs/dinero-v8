#include "daemon/orchard_chainstate_write.h"
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
std::string Encode(const RuntimeOutboxEvent& e) {
    std::string bytes=e.IsOrchardProfile()?"DNOE01":"DNOE02";Number(bytes,e.cursor.sequence,8);Hash(bytes,e.previous_digest);
    Number(bytes,e.direction==RuntimeBlockDirection::Connect?1:2,1);
    Number(bytes,e.context.domain.network_code,1);
    bytes.append(reinterpret_cast<const char*>(e.context.domain.genesis_wire.data()),32);
    Number(bytes,e.context.domain.branch_id,4);Number(bytes,e.context.activation_height,4);
    Number(bytes,e.context.height,4);Hash(bytes,e.context.block_hash);Hash(bytes,e.context.parent_hash);
    Number(bytes,e.body.size(),4);bytes.append(reinterpret_cast<const char*>(e.body.data()),e.body.size());
    Seal(bytes);return bytes;
}
RuntimeOutboxEvent Decode(const std::string& bytes,uint64_t sequence,const OrchardBlockContext& profile) {
    auto r=Checked(bytes,maximum_record_bytes);
    const auto format=r.Take(6);if(format!="DNOE01" && format!="DNOE02")Corrupt();
    RuntimeOutboxEvent e;e.cursor.sequence=r.Number(8);e.previous_digest=r.Hash();
    const auto direction=r.Number(1);if(direction!=1 && direction!=2)Corrupt();
    e.direction=direction==1?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect;
    e.context.domain.network_code=r.Number(1);
    const auto genesis=r.Take(32);std::copy(genesis.begin(),genesis.end(),e.context.domain.genesis_wire.begin());
    e.context.domain.branch_id=r.Number(4);e.context.activation_height=r.Number(4);
    e.context.height=r.Number(4);e.context.block_hash=r.Hash();e.context.parent_hash=r.Hash();
    const auto size=r.Number(4);
    if(!size || size>maximum_record_bytes-overhead || r.bytes.size()!=size || !Profile(e.context) ||
        !SameProfile(e.context,profile) || e.cursor.sequence!=sequence || !sequence ||
        (sequence==1)!=e.previous_digest.IsNull() || !e.context.height ||
        (format=="DNOE01" && !e.IsOrchardProfile()) ||
        (format=="DNOE02" && e.IsOrchardProfile()) ||
        e.context.height>INT32_MAX || e.context.block_hash.IsNull() || e.context.parent_hash.IsNull())Corrupt();
    const auto body=r.Take(size);e.body.assign(body.begin(),body.end());
    e.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));
    try {
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
RuntimeOutboxCursor CheckedHead(const ChainDB& db,const std::optional<std::string>& raw,
                               const OrchardBlockContext& profile) {
    if(!raw) { if(Raw(db,Key(1)))Corrupt();return {}; }
    const auto head=Head(*raw);if(Read(db,head.sequence,profile).cursor!=head)Corrupt();
    if(head.sequence!=UINT64_MAX && Raw(db,Key(head.sequence+1)))Corrupt();
    return head;
}
// Called only by the sealed indexed owner. No public staging API or deletion.
std::optional<std::string> Append(const ChainDB& db,rocksdb::WriteBatch& batch,
    const OrchardBlockContext& context,const OrchardBlockCandidate& block,bool connecting) {
    if(!Profile(context))throw OrchardStateLookupError(Status::Invalid);
    auto before=Raw(db,head_key);const auto head=CheckedHead(db,before,context);
    if(head.sequence==UINT64_MAX)throw OrchardStateLookupError(Status::Invalid);
    RuntimeOutboxEvent event{{head.sequence+1,{}},head.digest,
        connecting?RuntimeBlockDirection::Connect:RuntimeBlockDirection::Disconnect,context,block.WireBytes()};
    const auto key=Key(event.cursor.sequence);
    if(Raw(db,key))Corrupt();
    const auto bytes=Encode(event);
    event.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));
    batch.Put(key,bytes);batch.Put(head_key,EncodeHead(event.cursor));return before;
}
} // namespace outbox_detail

RuntimeOutboxPage ReadRuntimeOutboxUnderLock(const ChainDB& db,const OrchardBlockContext& profile,
    RuntimeOutboxCursor after,size_t maximum_events,size_t maximum_bytes) {
    using namespace outbox_detail;
    if(!Profile(profile) || !maximum_events || maximum_events>128 || !maximum_bytes ||
        maximum_bytes>16*1024*1024)throw OrchardStateLookupError(Status::Invalid);
    RuntimeOutboxPage page;page.head=CheckedHead(db,Raw(db,head_key),profile);page.next=after;
    if(after.sequence>page.head.sequence || (!after.sequence && !after.digest.IsNull()))Corrupt();
    if(after.sequence && Read(db,after.sequence,profile).cursor!=after)Corrupt();
    size_t used=0;
    while(page.next.sequence<page.head.sequence && page.events.size()<maximum_events) {
        auto event=Read(db,page.next.sequence+1,profile);
        if(event.previous_digest!=page.next.digest)Corrupt();
        const auto charge=event.body.size()+overhead;
        if(charge>maximum_bytes-used) {
            if(page.events.empty())throw OrchardStateLookupError(Status::Invalid);
            break;
        }
        used+=charge;page.next=event.cursor;page.events.push_back(std::move(event));
    }
    if(page.next.sequence==page.head.sequence && page.next!=page.head)Corrupt();
    return page;
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
    std::vector<uint8_t> undo_bytes;
    CBlockIndex* index = nullptr;
    BlockHeader indexed_header;
    std::optional<ChainDB::PersistedHeaderMetadata> index_before, index_after;
    Phase phase = Phase::Preparing;
    std::optional<std::string> outbox_before;
    bool outbox_staged = false;

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
        if (outbox_staged && outbox_detail::Raw(db,outbox_detail::head_key)!=outbox_before)
            throw OrchardStateLookupError(Status::Corruption);
        if (!index) return;
        const auto current=RequiredDisk(db.getHeaderMetadata(indexed_header.GetHash()));
        if (MetadataFields(current)!=MetadataFields(*index_before) ||
            !IndexMatches(*index,indexed_header,*index_before))
            throw OrchardStateLookupError(Status::Corruption);
    }
    void PrepareIndex(BlockStorage& files, CBlockIndex& entry,
                      const OrchardBlockContext& context, const OrchardBlockCandidate& block,
                      bool connecting, bool contextual_header_validated=false) {
        auto before=RequiredDisk(db.getHeaderMetadata(context.block_hash));
        if (before.height<0 || uint32_t(before.height)!=context.height ||
            before.parent_hash!=context.parent_hash ||
            before.chainwork!=RequiredDisk(db.getBlockWork(context.block_hash)) ||
            (before.status_flags & (BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD)) ||
            !IndexMatches(entry,block.Header(),before))
            throw OrchardStateLookupError(Status::Corruption);
        auto after=before;
        const auto& wire=block.WireBytes();
        const std::string exact(wire.begin(),wire.end());
        if (before.status_flags & BLOCK_HAVE_DATA) {
            if (!before.data_size || RequiredDisk(files.readBlockBytes(
                    {before.file_number,before.data_pos,before.data_size}))!=exact)
                throw OrchardStateLookupError(Status::Corruption);
        } else {
            if (!connecting || before.data_size || before.data_pos || before.file_number)
                throw OrchardStateLookupError(Status::Corruption);
            const auto pos=RequiredDisk(files.writeBlockBytes(context.block_hash,exact));
            if (pos.offset>UINT32_MAX) throw OrchardStateLookupError(Status::Invalid);
            after.file_number=pos.file_number;after.data_pos=uint32_t(pos.offset);after.data_size=pos.size;
            after.status_flags|=BLOCK_HAVE_DATA;
        }
        if (before.status_flags & BLOCK_HAVE_UNDO) {
            if (!before.undo_size || RequiredDisk(files.readUndo(
                    {before.undo_file,before.undo_pos,before.undo_size}))!=undo_bytes)
                throw OrchardStateLookupError(Status::Corruption);
        } else {
            if (!connecting || before.undo_size || before.undo_pos || before.undo_file)
                throw OrchardStateLookupError(Status::Corruption);
            const auto pos=RequiredDisk(files.writeUndo(context.block_hash,undo_bytes));
            if (pos.offset>UINT32_MAX) throw OrchardStateLookupError(Status::Invalid);
            after.undo_file=pos.file_number;after.undo_pos=uint32_t(pos.offset);after.undo_size=pos.size;
            after.status_flags|=BLOCK_HAVE_UNDO;
        }
        if (connecting && contextual_header_validated) after.status_flags|=BLOCK_VALID_MASK;
        if (connecting && db.putHeaderMetadata(token,context.block_hash,after,&batch)!=Status::Ok)
            throw OrchardStateLookupError(Status::Internal);
        indexed_header=block.Header(); index_before=before; index_after=after; index=&entry;
    }
    void PublishIndex() noexcept {
        if (!index) return;
        const auto& m=*index_after;
        index->status=m.status_flags;
        index->file_number=m.file_number;index->data_pos=m.data_pos;index->data_size=m.data_size;
        index->undo_file=m.undo_file;index->undo_pos=m.undo_pos;index->undo_size=m.undo_size;
    }
};

PreparedOrchardChainstateWrite::PreparedOrchardChainstateWrite(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token)
    : impl_(std::make_unique<Impl>(mutex, db, token)) {}
PreparedOrchardChainstateWrite::~PreparedOrchardChainstateWrite() = default;

std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::Connect(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    ConsensusUTXOSet& live, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, const BlockHeader& parent,
    const UtreexoForest& forest, const OrchardBranchMtpLookup& mtp,
    bool witness, bool checkpoint,
    const std::optional<storage::LegacyRetirementRecord>& boundary) {
    auto result = std::unique_ptr<PreparedOrchardChainstateWrite>(
        new PreparedOrchardChainstateWrite(mutex, db, token));
    auto& owner = *result->impl_;
    auto staged = StageOrchardChainstateConnectUnderLock(db, token, context, block,
        parent, forest, mtp, witness, checkpoint, owner.batch, boundary);
    owner.undo_bytes=staged.block.undo.Serialize();
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
    const UtreexoForest& forest, bool witness) {
    auto result = std::unique_ptr<PreparedOrchardChainstateWrite>(
        new PreparedOrchardChainstateWrite(mutex, db, token));
    auto& owner = *result->impl_;
    auto staged = StageOrchardChainstateDisconnectUnderLock(db, token, context,
        block, parent, forest, witness, owner.batch);
    owner.undo_bytes=RequiredDisk(db.getUndo(context.block_hash)).Serialize();
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
    bool witness, bool checkpoint, const std::optional<storage::LegacyRetirementRecord>& boundary, bool contextual_header_validated) {
    auto result=Connect(mutex,db,token,live,context,block,parent,forest,mtp,witness,checkpoint,boundary);
    result->impl_->PrepareIndex(files,index,context,block,true,contextual_header_validated);
    result->impl_->outbox_before=outbox_detail::Append(db,result->impl_->batch,context,block,true);
    result->impl_->outbox_staged=true;
    return result;
}
std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::DisconnectIndexed(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    BlockStorage& files, CBlockIndex& index, ConsensusUTXOSet& live,
    const OrchardBlockContext& context, const OrchardBlockCandidate& block,
    const BlockHeader& parent, const UtreexoForest& forest, bool witness) {
    auto result=Disconnect(mutex,db,token,live,context,block,parent,forest,witness);
    result->impl_->PrepareIndex(files,index,context,block,false);
    result->impl_->outbox_before=outbox_detail::Append(db,result->impl_->batch,context,block,false);
    result->impl_->outbox_staged=true;
    return result;
}

void PreparedOrchardChainstateWrite::Commit() {
    auto& owner = *impl_;
    owner.CheckOwner();
    if (owner.phase != Impl::Phase::Prepared)
        throw std::logic_error("Orchard chainstate write is not prepared or was already consumed");
    try {
        owner.CheckIndex();
        owner.publication->CheckReadyUnderLock();
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
    std::move(*owner.publication).PublishAfterCommitUnderLock();
    owner.PublishIndex();
    owner.phase = Impl::Phase::Committed;
}
} // namespace dinero
