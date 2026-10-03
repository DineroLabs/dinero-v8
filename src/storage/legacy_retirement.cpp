#include "storage/chain_db.h"
#include "consensus/tx_validation.h"
#include <cstring>
#include <limits>
#include <string_view>

namespace dinero {
namespace {
using storage::LegacyRetirementState;
constexpr std::string_view prefix = "R1";
constexpr const char* state_key = "R1S";
constexpr size_t state_size = 237;
std::string UndoKey(const uint256& hash) {
    return std::string("R1U") + std::string(reinterpret_cast<const char*>(hash.data), 32);
}
void Number(std::string& b, uint64_t n, size_t width) {
    for (size_t i = 0; i < width; ++i) b.push_back(static_cast<char>(n >> (8 * i)));
}
void Hash(std::string& b, const uint256& h) { b.append(reinterpret_cast<const char*>(h.data), 32); }
struct Reader {
    std::string_view bytes; bool good = true;
    std::string_view take(size_t n) {
        if (n > bytes.size()) { good = false; return {}; }
        auto out = bytes.substr(0, n); bytes.remove_prefix(n); return out;
    }
    uint64_t number(size_t n) {
        auto b = take(n); uint64_t out = 0;
        for (size_t i = 0; i < b.size(); ++i) out |= uint64_t(uint8_t(b[i])) << (8 * i);
        return out;
    }
    uint256 hash() {
        auto b = take(32); uint256 out;
        if (b.size() == 32) std::memcpy(out.data, b.data(), 32);
        return out;
    }
};
bool Valid(const LegacyRetirementState& s) {
    const auto& r = s.record;
    return r.network_code <= 2 && !r.genesis.IsNull() && r.branch_id != 0 &&
        r.activation_height > 0 && r.activation_height <= uint32_t(INT32_MAX) &&
        r.legacy_epoch_height < r.activation_height && !r.boundary_parent.IsNull() &&
        r.retired_value <= consensus::MAX_MONEY && s.height >= r.activation_height &&
        s.height <= uint32_t(INT32_MAX) && !s.block_hash.IsNull() && s.block_hash != r.boundary_parent &&
        !s.parent_hash.IsNull() && s.parent_hash != s.block_hash &&
        (s.height != r.activation_height || s.parent_hash == r.boundary_parent);
}
bool Transition(const std::optional<LegacyRetirementState>& before, const LegacyRetirementState& after) {
    if (!Valid(after)) return false;
    if (!before) return after.height == after.record.activation_height;
    return Valid(*before) && before->record == after.record &&
        uint64_t(before->height) + 1 == after.height && before->block_hash == after.parent_hash;
}
std::string Encode(const LegacyRetirementState& s) {
    const auto& r = s.record; std::string b = "DLR1";
    Number(b, r.network_code, 1); Hash(b, r.genesis); Number(b, r.branch_id, 4);
    Number(b, r.activation_height, 4); Number(b, r.legacy_epoch_height, 4);
    Hash(b, r.boundary_parent); Hash(b, r.legacy_state_root); Number(b, r.retired_value, 8);
    Hash(b, r.tree_root); Number(b, r.tree_size, 8); Number(b, r.nullifier_count, 8);
    Number(b, s.height, 4); Hash(b, s.block_hash); Hash(b, s.parent_hash); return b;
}
StatusOr<LegacyRetirementState> Decode(std::string_view b) {
    if (b.size() != state_size) return Status::Corruption;
    Reader in{b}; if (in.take(4) != "DLR1") return Status::Corruption;
    LegacyRetirementState s; auto& r = s.record;
    r.network_code = uint8_t(in.number(1)); r.genesis = in.hash(); r.branch_id = uint32_t(in.number(4));
    r.activation_height = uint32_t(in.number(4)); r.legacy_epoch_height = uint32_t(in.number(4));
    r.boundary_parent = in.hash(); r.legacy_state_root = in.hash(); r.retired_value = in.number(8);
    r.tree_root = in.hash(); r.tree_size = in.number(8); r.nullifier_count = in.number(8);
    s.height = uint32_t(in.number(4)); s.block_hash = in.hash(); s.parent_hash = in.hash();
    if (!in.good || !in.bytes.empty() || !Valid(s)) return Status::Corruption;
    return s;
}
struct Undo { std::optional<LegacyRetirementState> before; LegacyRetirementState after; };
std::string EncodeUndo(const Undo& u) {
    std::string b = "DLU1"; Number(b, u.before ? 1 : 0, 1);
    if (u.before) b += Encode(*u.before);
    return b + Encode(u.after);
}
StatusOr<Undo> DecodeUndo(std::string_view b) {
    if (b.size() != 5 + state_size && b.size() != 5 + 2 * state_size) return Status::Corruption;
    Reader in{b}; if (in.take(4) != "DLU1") return Status::Corruption;
    Undo u; const auto present = in.number(1);
    if (present > 1) return Status::Corruption;
    if (present) {
        const auto s = Decode(in.take(state_size)); if (!s.ok()) return s.status(); u.before = s.value();
    }
    const auto s = Decode(in.take(state_size)); if (!s.ok()) return s.status(); u.after = s.value();
    if (!in.good || !in.bytes.empty() || !Transition(u.before, u.after)) return Status::Corruption;
    return u;
}
ChainDB::ShieldedTipMarker Marker(const LegacyRetirementState& s, bool boundary_parent = false) {
    const auto& r = s.record;
    return {int32_t(boundary_parent ? r.activation_height - 1 : s.height),
        boundary_parent ? r.boundary_parent : s.block_hash, r.tree_root, r.tree_size, r.nullifier_count};
}
Status CheckSelectedMarker(const ChainDB& db, const ChainDB::ShieldedTipMarker& expected) {
    const auto tip = db.getValidatedTip(); if (!tip.ok()) return tip.status();
    if (tip->height != expected.height || tip->hash != expected.block_hash) return Status::Invalid;
    const auto marker = db.getShieldedTipMarker(); if (!marker.ok()) return marker.status();
    if (marker->height != expected.height || marker->block_hash != expected.block_hash ||
        marker->shielded_root != expected.shielded_root || marker->tree_size != expected.tree_size ||
        marker->nullifier_count != expected.nullifier_count) return Status::Corruption;
    return Status::Ok;
}
// Any already-staged legacy-content/marker write would invalidate the frozen
// view. Orchard writes and ordinary coin/index/tip companion writes are allowed.
// The connector must not append legacy-content writes after this stage either.
class PriorWrites : public rocksdb::WriteBatch::Handler {
public:
    PriorWrites(uint32_t shielded, uint32_t meta) : shielded_(shielded), meta_(meta) {}
    rocksdb::Status PutCF(uint32_t cf, const rocksdb::Slice& k, const rocksdb::Slice&) override { return Check(cf, k); }
    rocksdb::Status DeleteCF(uint32_t cf, const rocksdb::Slice& k) override { return Check(cf, k); }
    rocksdb::Status SingleDeleteCF(uint32_t cf, const rocksdb::Slice& k) override { return Check(cf, k); }
    rocksdb::Status MergeCF(uint32_t cf, const rocksdb::Slice& k, const rocksdb::Slice&) override { return Check(cf, k); }
    rocksdb::Status DeleteRangeCF(uint32_t cf, const rocksdb::Slice&, const rocksdb::Slice&) override {
        return cf == shielded_ || cf == meta_ ? rocksdb::Status::InvalidArgument("retirement range write") : rocksdb::Status::OK();
    }
private:
    rocksdb::Status Check(uint32_t cf, const rocksdb::Slice& k) {
        const bool forbidden = (cf == shielded_ && !k.starts_with("O1")) ||
            (cf == meta_ && k == rocksdb::Slice("shielded_tip"));
        return forbidden ? rocksdb::Status::InvalidArgument("legacy state already staged") : rocksdb::Status::OK();
    }
    uint32_t shielded_, meta_;
};
class BatchGuard {
public:
    explicit BatchGuard(rocksdb::WriteBatch& b) : b_(b) { b_.SetSavePoint(); }
    ~BatchGuard() { if (!done_) (void)b_.RollbackToSavePoint(); }
    void keep() { (void)b_.PopSavePoint(); done_ = true; }
private:
    rocksdb::WriteBatch& b_; bool done_ = false;
};
} // namespace
StatusOr<storage::LegacyRetirementState> ChainDB::getLegacyRetirementState() const {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState()) return Status::Invalid;
    std::string bytes;
    const auto s = db_->Get(getReadOptions(), shieldedStateHandle(), state_key, &bytes);
    if (!s.ok()) return convertRocksDBStatus(s);
    return Decode(bytes);
}
Status ChainDB::stageLegacyRetirementConnect(const ChainWriteToken& token,
    const std::optional<LegacyRetirementState>& parent, const LegacyRetirementState& next,
    rocksdb::WriteBatch& batch) {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState() || !Transition(parent, next)) return Status::Invalid;
    auto* cf = shieldedStateHandle(); PriorWrites prior(cf->GetID(), cf_[idx_meta_]->GetID());
    if (!batch.Iterate(&prior).ok()) return Status::Invalid;
    const auto current = getLegacyRetirementState();
    if (parent) {
        if (!current.ok()) return current.status();
        if (*current != *parent) return Status::Invalid;
    } else {
        if (current.ok()) return Status::AlreadyExists;
        if (current.status() != Status::NotFound) return current.status();
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), cf));
        it->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
        if (it->Valid() && it->key().starts_with(rocksdb::Slice(prefix.data(), prefix.size()))) return Status::Corruption;
        if (!it->status().ok()) return convertRocksDBStatus(it->status());
    }
    RETURN_IF_ERROR(CheckSelectedMarker(*this, parent ? Marker(*parent) : Marker(next, true)));
    std::string retained;
    const auto exists = db_->Get(getReadOptions(), cf, UndoKey(next.block_hash), &retained);
    if (exists.ok()) return Status::AlreadyExists;
    if (!exists.IsNotFound()) return convertRocksDBStatus(exists);
    BatchGuard guard(batch);
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, state_key, Encode(next))));
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Put(cf, UndoKey(next.block_hash), EncodeUndo({parent, next}))));
    RETURN_IF_ERROR(putShieldedTipMarker(token, Marker(next), &batch));
    guard.keep(); return Status::Ok;
}
Status ChainDB::stageLegacyRetirementDisconnect(const ChainWriteToken& token,
    const LegacyRetirementState& tip, rocksdb::WriteBatch& batch) {
    if (!db_) return Status::Internal;
    if (!hasSeparatedShieldedState() || !Valid(tip)) return Status::Invalid;
    auto* cf = shieldedStateHandle(); PriorWrites prior(cf->GetID(), cf_[idx_meta_]->GetID());
    if (!batch.Iterate(&prior).ok()) return Status::Invalid;
    const auto current = getLegacyRetirementState();
    if (!current.ok()) return current.status();
    if (*current != tip) return Status::Invalid;
    RETURN_IF_ERROR(CheckSelectedMarker(*this, Marker(tip)));
    std::string bytes;
    const auto s = db_->Get(getReadOptions(), cf, UndoKey(tip.block_hash), &bytes);
    if (!s.ok()) return s.IsNotFound() ? Status::Corruption : convertRocksDBStatus(s);
    const auto undo = DecodeUndo(bytes); if (!undo.ok()) return undo.status();
    if (undo->after != tip) return Status::Corruption;
    BatchGuard guard(batch);
    RETURN_IF_ERROR(convertRocksDBStatus(undo->before ? batch.Put(cf, state_key, Encode(*undo->before)) : batch.Delete(cf, state_key)));
    RETURN_IF_ERROR(convertRocksDBStatus(batch.Delete(cf, UndoKey(tip.block_hash))));
    RETURN_IF_ERROR(putShieldedTipMarker(token, undo->before ? Marker(*undo->before) : Marker(tip, true), &batch));
    guard.keep(); return Status::Ok;
}
} // namespace dinero
