#include "daemon/orchard_chainstate_write.h"
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
        if (!index) return;
        const auto current=RequiredDisk(db.getHeaderMetadata(indexed_header.GetHash()));
        if (MetadataFields(current)!=MetadataFields(*index_before) ||
            !IndexMatches(*index,indexed_header,*index_before))
            throw OrchardStateLookupError(Status::Corruption);
    }
    void PrepareIndex(BlockStorage& files, CBlockIndex& entry,
                      const OrchardBlockContext& context, const OrchardBlockCandidate& block,
                      bool connecting) {
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
    bool witness, bool checkpoint, const std::optional<storage::LegacyRetirementRecord>& boundary) {
    auto result=Connect(mutex,db,token,live,context,block,parent,forest,mtp,witness,checkpoint,boundary);
    result->impl_->PrepareIndex(files,index,context,block,true);
    return result;
}
std::unique_ptr<PreparedOrchardChainstateWrite> PreparedOrchardChainstateWrite::DisconnectIndexed(
    AnnotatedRecursiveMutex& mutex, ChainDB& db, const ChainWriteToken& token,
    BlockStorage& files, CBlockIndex& index, ConsensusUTXOSet& live,
    const OrchardBlockContext& context, const OrchardBlockCandidate& block,
    const BlockHeader& parent, const UtreexoForest& forest, bool witness) {
    auto result=Disconnect(mutex,db,token,live,context,block,parent,forest,witness);
    result->impl_->PrepareIndex(files,index,context,block,false);
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
