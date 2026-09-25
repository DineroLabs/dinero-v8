#include "daemon/orchard_chainstate_write.h"
#include "common/annotated_mutex.h"
#include "consensus/utxo_publication.h"
#include "storage/chain_db.h"
#include <rocksdb/write_batch.h>
#include <cstdio>
#include <thread>

namespace dinero {
using namespace consensus;

struct PreparedOrchardChainstateWrite::Impl {
    enum class Phase { Preparing, Prepared, Aborted, Writing, Committed };
    std::unique_lock<AnnotatedRecursiveMutex> lock;
    const std::thread::id thread = std::this_thread::get_id();
    ChainDB& db;
    const ChainWriteToken& token;
    rocksdb::WriteBatch batch;
    std::optional<PreparedUTXOPublication> publication;
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

void PreparedOrchardChainstateWrite::Commit() {
    auto& owner = *impl_;
    owner.CheckOwner();
    if (owner.phase != Impl::Phase::Prepared)
        throw std::logic_error("Orchard chainstate write is not prepared or was already consumed");
    try {
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
    owner.phase = Impl::Phase::Committed;
}
} // namespace dinero
