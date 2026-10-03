#include "consensus/utxo_publication.h"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace dinero::consensus {
namespace {
bool SameCoin(const UTXOEntry& a, const UTXOEntry& b) {
    return a.value == b.value && a.scriptPubKey == b.scriptPubKey &&
        a.height == b.height && a.isCoinbase == b.isCoinbase &&
        a.is_confidential == b.is_confidential && a.commitment == b.commitment;
}
bool SameRoot(const UtreexoForest& forest, const uint256& expected) {
    const auto root = forest.getCommitment();
    return root.size() == 32 && std::equal(root.begin(), root.end(), expected.begin());
}
[[noreturn]] void Mismatch() { throw std::runtime_error("UTXO publication local state mismatch"); }
}
PreparedUTXOPublication::PreparedUTXOPublication(ConsensusUTXOSet& owner,
    uint32_t from_height, uint256 from_hash, uint256 from_root,
    std::span<const UTXOPublicationChange> changes, UtreexoForest next,
    uint32_t to_height, uint256 to_hash)
    : owner_(&owner), from_height_(from_height), to_height_(to_height),
      from_hash_(from_hash), from_root_(from_root), to_hash_(to_hash),
      changes_(changes.begin(), changes.end()), forest_(std::move(next)) {}
PreparedUTXOPublication::PreparedUTXOPublication(PreparedUTXOPublication&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), from_height_(other.from_height_),
      to_height_(other.to_height_), from_hash_(other.from_hash_), from_root_(other.from_root_),
      to_hash_(other.to_hash_), changes_(std::move(other.changes_)),
      insertions_(std::move(other.insertions_)), forest_(std::move(other.forest_)),
      source_size_(other.source_size_), source_buckets_(other.source_buckets_),
      source_load_factor_(other.source_load_factor_) {}
PreparedUTXOPublication PreparedUTXOPublication::PrepareUnderLock(
    ConsensusUTXOSet& owner, uint32_t from_height, const uint256& from_hash,
    const uint256& from_root, std::span<const UTXOPublicationChange> changes,
    UtreexoForest next, uint32_t to_height, const uint256& to_hash, const uint256& to_root) {
    // Connect/disconnect exactly one height; neither a snapshot import nor a
    // pointer-only repair. Heights are widened before addition.
    if (from_hash.IsNull() || to_hash.IsNull() || from_hash == to_hash ||
        (uint64_t(from_height)+1 != to_height && uint64_t(to_height)+1 != from_height) ||
        !SameRoot(next, to_root)) Mismatch();
    PreparedUTXOPublication result(owner, from_height, from_hash, from_root,
        changes, std::move(next), to_height, to_hash);
    std::set<OutPoint> seen;
    for (const auto& change : result.changes_) {
        if ((!change.before && !change.after) || !seen.insert(change.outpoint).second) Mismatch();
        if (change.after) result.insertions_.emplace(change.outpoint, *change.after);
    }
    result.CheckReadyUnderLock();
    if (result.insertions_.size() > owner.utxos_.max_size() - owner.utxos_.size())
        throw std::length_error("UTXO publication capacity");
    const auto capacity = owner.utxos_.size() + result.insertions_.size();
    // Reserving buckets may change capacity, never the logical coin state.
    // Avoid shrinking/rebuilding the whole table on ordinary blocks.
    if (static_cast<long double>(capacity) >
        static_cast<long double>(owner.utxos_.bucket_count()) * owner.utxos_.max_load_factor())
        owner.utxos_.reserve(capacity);
    result.source_size_ = owner.utxos_.size();
    result.source_buckets_ = owner.utxos_.bucket_count();
    result.source_load_factor_ = owner.utxos_.max_load_factor();
    return result;
}
void PreparedUTXOPublication::CheckReadyUnderLock() const {
    if (!owner_ || owner_->height_ != from_height_ || owner_->best_block_ != from_hash_) Mismatch();
    if (source_load_factor_ != 0 &&
        (owner_->utxos_.size() != source_size_ || owner_->utxos_.bucket_count() != source_buckets_ ||
         owner_->utxos_.max_load_factor() != source_load_factor_)) Mismatch();
    {
        const auto lock = owner_->LockForestShared();
        if (!SameRoot(owner_->forest_, from_root_)) Mismatch();
    }
    for (const auto& change : changes_) {
        const auto it = owner_->utxos_.find(change.outpoint);
        if (change.before ? it == owner_->utxos_.end() || !SameCoin(it->second, *change.before)
                          : it != owner_->utxos_.end()) Mismatch();
    }
}
void PreparedUTXOPublication::PublishAfterCommitUnderLock() && noexcept {
    if (!owner_ || owner_->height_ != from_height_ || owner_->best_block_ != from_hash_ ||
        owner_->utxos_.size() != source_size_ || owner_->utxos_.bucket_count() != source_buckets_ ||
        owner_->utxos_.max_load_factor() != source_load_factor_) std::terminate();
    // Lock acquisition failure is fatal after commit. Never acquire a database
    // or activation lock while holding this leaf forest lock.
    std::unique_lock<std::shared_mutex> lock(owner_->forest_mutex_);
    // Recheck touched coins without allocating. Continuing after a changed
    // prepared view would overwrite state outside the committed transaction.
    for (const auto& change : changes_) {
        const auto it = owner_->utxos_.find(change.outpoint);
        if (change.before ? it == owner_->utxos_.end() || !SameCoin(it->second, *change.before)
                          : it != owner_->utxos_.end()) std::terminate();
    }
    for (const auto& change : changes_) if (change.before) owner_->utxos_.erase(change.outpoint);
    while (!insertions_.empty()) {
        auto node = insertions_.extract(insertions_.begin());
        if (!owner_->utxos_.insert(std::move(node)).inserted) std::terminate();
    }
    owner_->forest_ = std::move(forest_);
    owner_->best_block_ = to_hash_;
    owner_->height_ = to_height_;
    owner_ = nullptr;
}
} // namespace dinero::consensus
