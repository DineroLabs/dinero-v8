#include "storage/chain_db.h"
#include "consensus/orchard_pool_balance.h"
#include "consensus/tx_validation.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace dinero {
namespace {
using storage::OrchardStoredState;
constexpr std::string_view prefix = "O1";
constexpr const char* state_key = "O1S";
constexpr size_t state_limit = 92 + storage::ORCHARD_STORED_FRONTIER_LIMIT;
constexpr size_t undo_limit = 16 + 2 * state_limit + 32 * storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT;
std::string Key(char kind, const uint256& hash) {
    return std::string(prefix) + kind + std::string(reinterpret_cast<const char*>(hash.data), 32);
}
void Number(std::string& bytes, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes.push_back(static_cast<char>(value >> (8 * i)));
}
void Hash(std::string& bytes, const uint256& hash) {
    bytes.append(reinterpret_cast<const char*>(hash.data), 32);
}
struct OrchardRecordReader {
    std::string_view bytes;
    bool good = true;
    std::string_view take(size_t count) {
        if (count > bytes.size()) { good = false; return {}; }
        auto result = bytes.substr(0, count); bytes.remove_prefix(count); return result;
    }
    uint64_t number(size_t width) {
        auto data = take(width); uint64_t result = 0;
        for (size_t i = 0; i < data.size(); ++i)
            result |= uint64_t(static_cast<uint8_t>(data[i])) << (8 * i);
        return result;
    }
    uint256 hash() {
        auto data = take(32); uint256 result;
        if (good) std::memcpy(result.data, data.data(), 32);
        return result;
    }
};
bool Valid(const OrchardStoredState& s) {
    return s.height > 0 && s.height <= uint32_t(std::numeric_limits<int32_t>::max()) &&
        !s.block_hash.IsNull() &&
        s.pool_balance <= consensus::MAX_MONEY && s.tree_size <= (uint64_t{1} << 32) &&
        !s.frontier.empty() && s.frontier.size() <= storage::ORCHARD_STORED_FRONTIER_LIMIT;
}
std::string Encode(const OrchardStoredState& s) {
    std::string bytes = "DOS1";
    Number(bytes, s.height, 4); Hash(bytes, s.block_hash); Hash(bytes, s.anchor);
    Number(bytes, s.pool_balance, 8); Number(bytes, s.tree_size, 8);
    Number(bytes, s.frontier.size(), 4); bytes += s.frontier; return bytes;
}
StatusOr<OrchardStoredState> Decode(std::string_view bytes) {
    if (bytes.size() > state_limit) return Status::Corruption;
    OrchardRecordReader r{bytes}; if (r.take(4) != "DOS1") return Status::Corruption;
    OrchardStoredState s;
    s.height = static_cast<uint32_t>(r.number(4)); s.block_hash = r.hash(); s.anchor = r.hash();
    s.pool_balance = r.number(8); s.tree_size = r.number(8);
    const auto size = r.number(4);
    if (size > storage::ORCHARD_STORED_FRONTIER_LIMIT) return Status::Corruption;
    s.frontier = r.take(static_cast<size_t>(size));
    if (!r.good || !r.bytes.empty() || !Valid(s)) return Status::Corruption;
    return s;
}
bool Transition(const std::optional<OrchardStoredState>& before, const OrchardStoredState& after) {
    if (!Valid(after)) return false;
    if (!before) return true; // Activation/initial empty-pool rules belong to the host validator.
    return Valid(*before) && before->height + uint64_t{1} == after.height &&
        before->block_hash != after.block_hash && before->tree_size <= after.tree_size &&
        (before->tree_size != after.tree_size ||
         (before->anchor == after.anchor && before->frontier == after.frontier));
}
struct Undo {
    std::optional<OrchardStoredState> before;
    OrchardStoredState after;
    std::vector<uint256> nullifiers; // Canonically sorted, unique; original action order is not needed for deletion.
};
std::string EncodeUndo(const Undo& u) {
    std::string bytes = "DOU1";
    auto before = u.before ? Encode(*u.before) : std::string{};
    auto after = Encode(u.after);
    Number(bytes, before.size(), 4); bytes += before;
    Number(bytes, after.size(), 4); bytes += after;
    Number(bytes, u.nullifiers.size(), 4);
    for (const auto& nf : u.nullifiers) Hash(bytes, nf);
    return bytes;
}
StatusOr<Undo> DecodeUndo(std::string_view bytes) {
    if (bytes.size() > undo_limit) return Status::Corruption;
    OrchardRecordReader r{bytes}; if (r.take(4) != "DOU1") return Status::Corruption;
    Undo u;
    auto size = r.number(4);
    if (size > state_limit) return Status::Corruption;
    if (size) {
        auto s = Decode(r.take(static_cast<size_t>(size)));
        if (!s.ok()) return s.status(); u.before = s.value();
    }
    size = r.number(4); if (size > state_limit) return Status::Corruption;
    auto after = Decode(r.take(static_cast<size_t>(size)));
    if (!after.ok()) return after.status(); u.after = after.value();
    auto count = r.number(4);
    if (count > storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT || r.bytes.size() != 32 * count)
        return Status::Corruption;
    for (uint64_t i = 0; i < count; ++i) {
        auto nf = r.hash();
        if (!u.nullifiers.empty() && !(u.nullifiers.back() < nf)) return Status::Corruption;
        u.nullifiers.push_back(nf);
    }
    if (!r.good || !r.bytes.empty() || !Transition(u.before, u.after)) return Status::Corruption;
    return u;
}
// Reject a second Orchard transition in one outer batch rather than reading
// committed state while accidentally ignoring a previously staged transition.
class PriorWrites : public rocksdb::WriteBatch::Handler {
public:
    explicit PriorWrites(uint32_t cf) : cf_(cf) {}
    rocksdb::Status PutCF(uint32_t cf, const rocksdb::Slice& key, const rocksdb::Slice&) override {
        return Check(cf, key);
    }
    rocksdb::Status DeleteCF(uint32_t cf, const rocksdb::Slice& key) override { return Check(cf, key); }
    rocksdb::Status SingleDeleteCF(uint32_t cf, const rocksdb::Slice& key) override { return Check(cf, key); }
    rocksdb::Status MergeCF(uint32_t cf, const rocksdb::Slice& key, const rocksdb::Slice&) override {
        return Check(cf, key);
    }
    rocksdb::Status DeleteRangeCF(uint32_t cf, const rocksdb::Slice&, const rocksdb::Slice&) override {
        return cf == cf_ ? rocksdb::Status::InvalidArgument("range delete in Orchard family") : rocksdb::Status::OK();
    }
private:
    rocksdb::Status Check(uint32_t cf, const rocksdb::Slice& key) {
        return cf == cf_ && key.starts_with(rocksdb::Slice(prefix.data(), prefix.size()))
            ? rocksdb::Status::InvalidArgument("Orchard transition already staged") : rocksdb::Status::OK();
    }
    uint32_t cf_;
};
class BatchGuard {
public:
    explicit BatchGuard(rocksdb::WriteBatch& b) : batch(b) { batch.SetSavePoint(); }
    ~BatchGuard() { if (!done) (void)batch.RollbackToSavePoint(); }
    void keep() { (void)batch.PopSavePoint(); done = true; }
private:
    rocksdb::WriteBatch& batch; bool done = false;
};
} // namespace

StatusOr<storage::OrchardStoredState> ChainDB::getOrchardState() const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    std::string bytes;
    auto status = db_->Get(getReadOptions(), shieldedStateHandle(), state_key, &bytes);
    if (!status.ok()) return convertRocksDBStatus(status);
    return Decode(bytes);
}
StatusOr<uint256> ChainDB::getOrchardNullifierOwner(const uint256& nullifier) const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    std::string bytes;
    auto status = db_->Get(getReadOptions(), shieldedStateHandle(), Key('N', nullifier), &bytes);
    if (!status.ok()) return convertRocksDBStatus(status);
    if (bytes.size() != 32) return Status::Corruption;
    OrchardRecordReader r{bytes}; auto owner = r.hash();
    if (owner.IsNull()) return Status::Corruption;
    return owner;
}
StatusOr<std::optional<storage::OrchardStoredState>> ChainDB::getOrchardUndoParent(
    const OrchardStoredState& expected_tip) const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState() || !Valid(expected_tip)) return Status::Invalid;
    std::string bytes;
    const auto status = db_->Get(getReadOptions(), shieldedStateHandle(), Key('U', expected_tip.block_hash), &bytes);
    if (!status.ok()) return status.IsNotFound() ? Status::Corruption : convertRocksDBStatus(status);
    const auto undo = DecodeUndo(bytes);
    if (!undo.ok()) return undo.status();
    if (undo->after != expected_tip) return Status::Corruption;
    return undo->before;
}
StatusOr<uint64_t> ChainDB::getOrchardAnchorReferences(const uint256& anchor) const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    std::string bytes;
    auto status = db_->Get(getReadOptions(), shieldedStateHandle(), Key('A', anchor), &bytes);
    if (!status.ok()) return convertRocksDBStatus(status);
    if (bytes.size() != 8) return Status::Corruption;
    OrchardRecordReader r{bytes}; auto count = r.number(8);
    if (count == 0 || count > uint64_t(std::numeric_limits<int32_t>::max())) return Status::Corruption;
    return count;
}
Status ChainDB::stageOrchardConnect(const ChainWriteToken& token,
    const std::optional<OrchardStoredState>& expected_parent, const OrchardStoredState& next,
    const std::vector<uint256>& nullifiers, const std::vector<consensus::OrchardValueFlow>& value_flows,
    rocksdb::WriteBatch& batch) {
    (void)token;
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    if (!Transition(expected_parent, next) || nullifiers.size() > storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT)
        return Status::Invalid;
    // The Orchard pool begins at zero, independent of every historical pool.
    // Never accept a caller-provided next pool total without accounting for it.
    const auto balance = consensus::ApplyOrchardValueFlows(
        expected_parent ? expected_parent->pool_balance : 0, value_flows);
    if (!balance.ok() || balance.value() != next.pool_balance) return Status::Invalid;
    auto* cf = shieldedStateHandle(); PriorWrites prior(cf->GetID());
    if (!batch.Iterate(&prior).ok()) return Status::Invalid;
    auto current = getOrchardState();
    if (expected_parent) {
        if (!current.ok()) return current.status();
        if (current.value() != *expected_parent) return Status::Invalid;
    } else {
        if (current.ok()) return Status::AlreadyExists;
        if (current.status() != Status::NotFound) return current.status();
        // Missing tip plus leftover Orchard rows is damaged state, not an
        // empty pool. Never silently initialize over it.
        // getReadOptions() explicitly labels a point Get; RocksDB rejects
        // that activity on an iterator. Use iterator-default read options.
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), cf));
        it->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
        if (it->Valid() && it->key().starts_with(rocksdb::Slice(prefix.data(), prefix.size()))) return Status::Corruption;
        if (!it->status().ok()) return convertRocksDBStatus(it->status());
    }
    Undo undo{expected_parent, next, nullifiers};
    std::sort(undo.nullifiers.begin(), undo.nullifiers.end());
    if (std::adjacent_find(undo.nullifiers.begin(), undo.nullifiers.end()) != undo.nullifiers.end()) return Status::Invalid;
    for (const auto& nf : undo.nullifiers) {
        auto owner = getOrchardNullifierOwner(nf);
        if (owner.ok()) return Status::AlreadyExists;
        if (owner.status() != Status::NotFound) return owner.status();
    }
    std::string old_undo;
    auto exists = db_->Get(getReadOptions(), cf, Key('U', next.block_hash), &old_undo);
    if (exists.ok()) return Status::AlreadyExists;
    if (!exists.IsNotFound()) return convertRocksDBStatus(exists);
    auto refs = getOrchardAnchorReferences(next.anchor);
    if (!refs.ok() && refs.status() != Status::NotFound) return refs.status();
    uint64_t count = refs.ok() ? refs.value() : 0;
    if (count >= next.height) return Status::Corruption;
    std::string count_bytes; Number(count_bytes, count + 1, 8);
    BatchGuard guard(batch);
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, state_key, Encode(next))));
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, Key('U', next.block_hash), EncodeUndo(undo))));
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, Key('A', next.anchor), count_bytes)));
    std::string owner; Hash(owner, next.block_hash);
    for (const auto& nf : undo.nullifiers)
        RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, Key('N', nf), owner)));
    guard.keep(); return Status::Ok;
}
Status ChainDB::stageOrchardDisconnect(const ChainWriteToken& token,
    const OrchardStoredState& expected_tip, rocksdb::WriteBatch& batch) {
    (void)token;
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState() || !Valid(expected_tip)) return Status::Invalid;
    auto* cf = shieldedStateHandle(); PriorWrites prior(cf->GetID());
    if (!batch.Iterate(&prior).ok()) return Status::Invalid;
    auto current = getOrchardState();
    if (!current.ok()) return current.status();
    if (current.value() != expected_tip) return Status::Invalid;
    std::string bytes;
    auto status = db_->Get(getReadOptions(), cf, Key('U', expected_tip.block_hash), &bytes);
    if (!status.ok()) return status.IsNotFound() ? Status::Corruption : convertRocksDBStatus(status);
    auto undo = DecodeUndo(bytes);
    if (!undo.ok()) return undo.status();
    if (undo->after != expected_tip) return Status::Corruption;
    for (const auto& nf : undo->nullifiers) {
        auto owner = getOrchardNullifierOwner(nf);
        if (!owner.ok()) return owner.status() == Status::NotFound ? Status::Corruption : owner.status();
        if (owner.value() != expected_tip.block_hash) return Status::Corruption;
    }
    auto refs = getOrchardAnchorReferences(expected_tip.anchor);
    if (!refs.ok()) return refs.status() == Status::NotFound ? Status::Corruption : refs.status();
    if (refs.value() > expected_tip.height) return Status::Corruption;
    BatchGuard guard(batch);
    RETURN_IF_ERROR(convertRocksDBStatus(undo->before
        ? batch.Put(cf, state_key, Encode(*undo->before)) : batch.Delete(cf, state_key)));
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Delete(cf, Key('U', expected_tip.block_hash))));
    std::string count; Number(count, refs.value() - 1, 8);
    RETURN_IF_ERROR(convertRocksDBStatus(refs.value() > 1
        ? batch.Put(cf, Key('A', expected_tip.anchor), count) : batch.Delete(cf, Key('A', expected_tip.anchor))));
    for (const auto& nf : undo->nullifiers)
        RETURN_IF_ERROR(convertRocksDBStatus(batch.Delete(cf, Key('N', nf))));
    guard.keep(); return Status::Ok;
}

StatusOr<storage::OrchardCommitmentSets> ChainDB::previewOrchardDisconnectCommitmentSets(
    const OrchardStoredState& expected_tip) const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState() || !Valid(expected_tip)) return Status::Invalid;
    const auto current=getOrchardState();
    if (!current.ok()) return current.status();
    if (*current!=expected_tip) return Status::Invalid;
    std::string bytes;
    const auto status=db_->Get(getReadOptions(),shieldedStateHandle(),Key('U',expected_tip.block_hash),&bytes);
    if (!status.ok()) return status.IsNotFound()?Status::Corruption:convertRocksDBStatus(status);
    const auto undo=DecodeUndo(bytes);
    if (!undo.ok()) return undo.status();
    if (undo->after!=expected_tip ||
        (undo->before?undo->before->tree_size:0)+uint64_t(undo->nullifiers.size())!=expected_tip.tree_size)
        return Status::Corruption;
    const auto result=readOrchardCommitmentSets(expected_tip,undo->nullifiers,expected_tip.anchor,true,
        undo->before?std::optional(undo->before->anchor):std::nullopt);
    if (!result.ok()) return result.status();
    if (!undo->before && (result->nullifier_count || result->anchor_count || result->anchor_references))
        return Status::Corruption;
    return result;
}

StatusOr<storage::OrchardCommitmentSets> ChainDB::getOrchardCommitmentSets(
    const OrchardStoredState& expected) const {
    if (!Valid(expected)) return Status::Invalid;
    return readOrchardCommitmentSets(expected, {}, std::nullopt);
}
StatusOr<storage::OrchardCommitmentSets> ChainDB::previewOrchardCommitmentSets(
    const std::optional<OrchardStoredState>& parent, const OrchardStoredState& next,
    const std::vector<uint256>& nullifiers) const {
    if (!Transition(parent, next) || nullifiers.size() > storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT ||
        next.tree_size != (parent ? parent->tree_size : 0) + uint64_t(nullifiers.size()))
        return Status::Invalid;
    return readOrchardCommitmentSets(parent, nullifiers, next.anchor);
}
StatusOr<storage::OrchardCommitmentSets> ChainDB::readOrchardCommitmentSets(
    const std::optional<OrchardStoredState>& parent, const std::vector<uint256>& added,
    const std::optional<uint256>& added_anchor, bool removing,
    const std::optional<uint256>& restored_anchor) const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    const auto current = getOrchardState();
    if (parent) {
        if (!current.ok()) return current.status();
        if (*current != *parent) return Status::Invalid;
    } else {
        if (current.ok()) return Status::AlreadyExists;
        if (current.status() != Status::NotFound) return current.status();
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), shieldedStateHandle()));
        it->Seek("O1");
        if (it->Valid() && it->key().starts_with("O1")) return Status::Corruption;
        if (!it->status().ok()) return convertRocksDBStatus(it->status());
    }
    const auto less = [](const uint256& a, const uint256& b) { return std::memcmp(a.data,b.data,32) < 0; };
    auto inserted = added; std::sort(inserted.begin(),inserted.end(),less);
    if (std::adjacent_find(inserted.begin(),inserted.end()) != inserted.end()) return Status::Invalid;
    storage::OrchardCommitmentSets result;
    crypto::CSHA256 nf_hash, anchor_hash;
    nf_hash.Write(std::string("ONF1\x01",5)); anchor_hash.Write(std::string("OAN1\x01",5));
    const auto hash_number = [](crypto::CSHA256& h, uint64_t n) {
        std::string b; Number(b,n,8); h.Write(b);
    };
    const auto emit_nf = [&](const uint256& nf) {
        if (result.nullifier_count == UINT64_MAX) return false;
        nf_hash.Write(nf.data,32); ++result.nullifier_count; return true;
    };
    if (removing && (!parent || !added_anchor || inserted.size()>parent->tree_size)) return Status::Invalid;
    size_t inserted_pos = 0;
    uint64_t stored_nullifiers=0;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(),shieldedStateHandle()));
    std::optional<uint256> previous;
    for (it->Seek("O1N"); it->Valid() && it->key().starts_with("O1N"); it->Next()) {
        if (it->key().size()!=35 || it->value().size()!=32) return Status::Corruption;
        uint256 nf, owner; std::memcpy(nf.data,it->key().data()+3,32); std::memcpy(owner.data,it->value().data(),32);
        if (owner.IsNull() || (previous && !less(*previous,nf))) return Status::Corruption;
        previous=nf;
        if (stored_nullifiers==UINT64_MAX) return Status::Corruption;
        ++stored_nullifiers;
        while (inserted_pos<inserted.size() && less(inserted[inserted_pos],nf)) {
            if (removing || !emit_nf(inserted[inserted_pos++])) return Status::Corruption;
        }
        if (inserted_pos<inserted.size() && inserted[inserted_pos]==nf) {
            if (!removing) return Status::AlreadyExists;
            if (owner!=parent->block_hash) return Status::Corruption;
            ++inserted_pos;
        } else if (!emit_nf(nf)) return Status::Corruption;
    }
    if (!it->status().ok()) return convertRocksDBStatus(it->status());
    if (stored_nullifiers!=(parent?parent->tree_size:0)) return Status::Corruption;
    if (removing && inserted_pos!=inserted.size()) return Status::Corruption;
    while (inserted_pos<inserted.size()) if (!emit_nf(inserted[inserted_pos++])) return Status::Corruption;
    const auto expected_count=removing ? parent->tree_size-uint64_t(inserted.size())
        : (parent?parent->tree_size:0)+uint64_t(inserted.size());
    if (result.nullifier_count!=expected_count) return Status::Corruption;
    hash_number(nf_hash,result.nullifier_count); nf_hash.Finalize(result.nullifiers.data);
    bool emitted_new = !added_anchor.has_value(), found_current = !parent.has_value();
    bool found_restored=!restored_anchor.has_value();
    uint64_t old_references=0;
    const auto emit_anchor = [&](const uint256& a, uint64_t count) {
        if (!count || count > UINT64_MAX-result.anchor_references || result.anchor_count==UINT64_MAX) return false;
        if (restored_anchor && a==*restored_anchor) found_restored=true;
        anchor_hash.Write(a.data,32); hash_number(anchor_hash,count);
        ++result.anchor_count; result.anchor_references+=count; return true;
    };
    previous.reset();
    for (it->Seek("O1A"); it->Valid() && it->key().starts_with("O1A"); it->Next()) {
        if (it->key().size()!=35 || it->value().size()!=8) return Status::Corruption;
        uint256 anchor; std::memcpy(anchor.data,it->key().data()+3,32);
        OrchardRecordReader reader{std::string_view(it->value().data(),it->value().size())};
        auto count=reader.number(8);
        if (!parent || !count || count>parent->height || count>parent->height-old_references ||
            (previous && !less(*previous,anchor))) return Status::Corruption;
        previous=anchor; old_references+=count;
        if (anchor==parent->anchor) found_current=true;
        if (!emitted_new && less(*added_anchor,anchor)) {
            if (removing || !emit_anchor(*added_anchor,1)) return Status::Corruption;
            emitted_new=true;
        }
        if (!emitted_new && *added_anchor==anchor) {
            count=removing?count-1:count+1; emitted_new=true;
        }
        if (count && !emit_anchor(anchor,count)) return Status::Corruption;
    }
    if (!it->status().ok()) return convertRocksDBStatus(it->status());
    if (!found_current || !found_restored) return Status::Corruption;
    if (!emitted_new && (removing || !emit_anchor(*added_anchor,1))) return Status::Corruption;
    hash_number(anchor_hash,result.anchor_count); hash_number(anchor_hash,result.anchor_references);
    anchor_hash.Finalize(result.anchors.data);
    return result;
}
} // namespace dinero
