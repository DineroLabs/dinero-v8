#include "consensus/orchard_block_staging.h"
#include "storage/chain_db.h"
#include <set>
#include <exception>
#include <map>
#include "consensus/undo.h"
#include "util/hex.h"

namespace dinero::consensus {
namespace {
class DatabaseCoins final : public ChainStateView {
public:
    DatabaseCoins(const ChainDB& db, uint32_t height) : db_(db), height_(height) {}
    StatusOr<UTXOEntry> getCoin(const OutPoint& point) const override {
        const auto result = db_.getCoin(point.txid.AsUint256(), point.vout);
        if (!result.ok()) return result.status();
        const auto& coin = *result;
        if (coin.height < 0 || uint32_t(coin.height) > height_ || coin.amount > orchard::kMaxMoneyUna)
            return Status::Corruption;
        // ChainDB Coin uses hex text, as written by BlockAcceptor and the
        // persistent UTXO adapter. Never reinterpret that text as script bytes.
        // Validate ASCII first: util::unhex's case folding takes a char.
        if (!std::all_of(coin.script_pubkey.begin(),coin.script_pubkey.end(),[](unsigned char c) {
            return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        })) return Status::Corruption;
        std::vector<uint8_t> script;
        if (!util::unhex(coin.script_pubkey,script)) return Status::Corruption;
        return UTXOEntry(AmountUna::Una(coin.amount),script, uint32_t(coin.height),
            coin.coinbase, coin.is_confidential, coin.commitment);
    }
    bool hasCoin(const OutPoint& point) const override {
        const auto coin = getCoin(point);
        if (!coin.ok() && coin.status() != Status::NotFound) throw OrchardCoinLookupError(coin.status());
        return coin.ok();
    }
    uint32_t getHeight() const override { return height_; }
private:
    const ChainDB& db_;
    const uint32_t height_;
};
Coin StoredCoin(const UTXOEntry& coin) {
    Coin result; result.amount=coin.value.GetUna();
    result.script_pubkey=util::hex(coin.scriptPubKey);
    result.height=static_cast<int>(coin.height);result.coinbase=coin.isCoinbase;
    result.is_confidential=coin.is_confidential;result.commitment=coin.commitment;
    return result;
}
void StorageCheck(Status status) { if (status != Status::Ok) throw OrchardStateLookupError(status); }
class EmptyBatchGuard {
public:
    explicit EmptyBatchGuard(rocksdb::WriteBatch& batch) : batch_(batch), exceptions_(std::uncaught_exceptions()) {
        if (batch.Count()!=0) throw OrchardStateLookupError(Status::Invalid);
        batch_.SetSavePoint();
    }
    ~EmptyBatchGuard() {
        // Return-value construction may allocate. Release the savepoint only
        // after that succeeds; unwinding must still roll back the entire batch.
        const auto status=kept_ && std::uncaught_exceptions()==exceptions_
            ? batch_.PopSavePoint() : batch_.RollbackToSavePoint();
        if (!status.ok()) std::terminate();
    }
    void Keep() noexcept { kept_=true; }
private:
    rocksdb::WriteBatch& batch_;int exceptions_;bool kept_=false;
};
}
PreparedOrchardState StageOrchardBlockUnderChainstateLock(ChainDB& db,
    const ChainWriteToken& token, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, bool require_witness_commitment,
    std::span<const VerifiedOrchardAuthorizations> transactions, rocksdb::WriteBatch& batch) {
    if (context.activation_height == UINT32_MAX || context.activation_height == 0 ||
        context.height < context.activation_height) throw OrchardStateError(OrchardStateErrorCode::Inactive);
    if (block.Header().GetHash() != context.block_hash || block.Header().prev_block_hash != context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    std::string body_error;
    if (!block.Header().IsReservedValid() || !block.CheckIdentityCommitments(require_witness_commitment, body_error))
        throw OrchardStateError(OrchardStateErrorCode::BlockBody);
    std::set<TxId> ids;
    size_t checked = 0;
    for (size_t i = 0; i < block.Transactions().size(); ++i) {
        const auto& tx = block.Transactions()[i];
        if (!ids.insert(tx.GetTxid()).second) throw OrchardStateError(OrchardStateErrorCode::DuplicateTransaction);
        if (tx.IsOrchard()) {
            // Compare full canonical bytes, including transparent witnesses.
            // A valid subset or reordered list cannot produce a partial update.
            if (checked == transactions.size() ||
                tx.Orchard().CanonicalBytes() != transactions[checked].Orchard().CanonicalBytes())
                throw OrchardStateError(OrchardStateErrorCode::AuthorizationCoverage);
            ++checked;
        } else {
            const auto& old = tx.Historical();
            if (Transaction::IsShieldedVersion(old.version) || !old.shielded_bundle_bytes.empty())
                throw OrchardStateError(OrchardStateErrorCode::RetiredLegacyPool);
            if (i != 0 && old.IsCoinbase()) throw OrchardStateError(OrchardStateErrorCode::BlockBody);
        }
    }
    if (checked != transactions.size()) throw OrchardStateError(OrchardStateErrorCode::AuthorizationCoverage);
    const auto tip = db.getTip();
    if (!tip.ok()) throw OrchardStateLookupError(tip.status());
    if (context.height == 0 || tip->height < 0 ||
        static_cast<uint32_t>(tip->height) != context.height - 1 || tip->hash != context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const auto stored = db.getOrchardState();
    std::optional<storage::OrchardStoredState> parent;
    if (stored.ok()) parent = stored.value();
    else if (stored.status() != Status::NotFound) throw OrchardStateLookupError(stored.status());
    if (parent) {
        // The committed tip itself must be represented in anchor history.
        // A missing local row is not an unknown anchor supplied by a peer.
        const auto refs = db.getOrchardAnchorReferences(parent->anchor);
        if (!refs.ok()) throw OrchardStateLookupError(
            refs.status() == Status::NotFound ? Status::Corruption : refs.status());
    }
    OrchardStateLookups lookups{
        [&](const uint256& root) -> StatusOr<bool> {
            const auto references = db.getOrchardAnchorReferences(root);
            if (references.status() == Status::NotFound) return false;
            if (!references.ok()) return references.status();
            return references.value() > 0;
        },
        [&](const uint256& nullifier) -> StatusOr<bool> {
            const auto owner = db.getOrchardNullifierOwner(nullifier);
            if (owner.status() == Status::NotFound) return false;
            if (!owner.ok()) return owner.status();
            return true;
        }};
    const auto prepared = [&] {
        try {
            return PrepareOrchardStateTransition(context, parent, transactions, lookups);
        } catch (const OrchardStateError& e) {
            // These bytes came from our committed database. Quarantine/recover
            // local state rather than marking the candidate block invalid.
            if (e.Code() == OrchardStateErrorCode::ParentState)
                throw OrchardStateLookupError(Status::Corruption);
            throw;
        }
    }();
    const auto staged = db.stageOrchardConnect(token, prepared.Parent(), prepared.Next(),
        prepared.Nullifiers(), prepared.Flows(), batch);
    if (staged != Status::Ok) throw OrchardStateLookupError(staged);
    return prepared;
}

StagedOrchardBlock StageOrchardBlockCoinsAndStateUnderChainstateLock(ChainDB& db,
    const ChainWriteToken& token, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, const OrchardBranchMtpLookup& mtp,
    bool require_witness_commitment, rocksdb::WriteBatch& batch) {
    EmptyBatchGuard guard(batch);
    const auto tip=db.getTip();
    if (!tip.ok()) throw OrchardStateLookupError(tip.status());
    if (context.height==0 || tip->height<0 || uint32_t(tip->height)!=context.height-1 || tip->hash!=context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const DatabaseCoins view(db,static_cast<uint32_t>(tip->height));
    auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(block,context,view,mtp,require_witness_commitment);
    auto state=StageOrchardBlockUnderChainstateLock(db,token,context,block,
        require_witness_commitment,coins.Authorizations(),batch);
    UndoRecord undo;
    for (const auto& change:coins.Changes()) {
        if (change.before) {
            const auto& coin=*change.before;
            undo.spent.emplace_back(change.outpoint.txid.AsUint256(),change.outpoint.vout,
                coin.value.GetUna(),coin.scriptPubKey,coin.isCoinbase,coin.height,
                coin.is_confidential,coin.commitment);
        }
        if (change.after) {
            undo.created.emplace_back(change.outpoint.txid.AsUint256(),change.outpoint.vout);
            StorageCheck(db.putCoin(token,change.outpoint.txid.AsUint256(),change.outpoint.vout,StoredCoin(*change.after),&batch));
        } else StorageCheck(db.deleteCoin(token,change.outpoint.txid.AsUint256(),change.outpoint.vout,&batch));
    }
    // Reconnection may encounter a retained conventional undo row. It must
    // describe this exact transition; never overwrite different local history.
    const auto existing=db.getUndo(context.block_hash);
    if (existing.ok()) {
        if (existing->Serialize()!=undo.Serialize()) throw OrchardStateLookupError(Status::Corruption);
    } else if (existing.status()!=Status::NotFound) throw OrchardStateLookupError(existing.status());
    StorageCheck(db.putUndo(token,context.block_hash,undo,&batch));
    guard.Keep();
    return {std::move(coins),std::move(state)};
}

void StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(ChainDB& db,
    const ChainWriteToken& token, const OrchardBlockContext& context,
    const OrchardBlockCandidate& block, bool require_witness_commitment, rocksdb::WriteBatch& batch) {
    EmptyBatchGuard guard(batch);
    const auto tip=db.getTip();
    if (!tip.ok()) throw OrchardStateLookupError(tip.status());
    if (context.height==0 || context.activation_height==0 || context.activation_height==UINT32_MAX || context.height<context.activation_height ||
        tip->height<0 || uint32_t(tip->height)!=context.height || tip->hash!=context.block_hash ||
        block.Header().GetHash()!=context.block_hash || block.Header().prev_block_hash!=context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const auto corrupt=[] { throw OrchardStateLookupError(Status::Corruption); };
    std::string error;
    if (!block.Header().IsReservedValid() || !block.CheckIdentityCommitments(require_witness_commitment,error)) corrupt();
    const auto state=db.getOrchardState();
    if (!state.ok()) throw OrchardStateLookupError(state.status()==Status::NotFound?Status::Corruption:state.status());
    if (state->height!=context.height || state->block_hash!=context.block_hash) corrupt();
    const auto undo=db.getUndo(context.block_hash);
    if (!undo.ok()) throw OrchardStateLookupError(undo.status()==Status::NotFound?Status::Corruption:undo.status());
    if (undo->pre_block_shielded_frontier || undo->pre_block_shielded_anchors || undo->pre_reset_shielded_epoch) corrupt();
    // Reconstruct exact net row identities from the authenticated body, without
    // re-running signatures against coins that have already been spent.
    std::map<OutPoint,UTXOEntry> created;
    std::set<OutPoint> spent;
    std::set<TxId> ids;
    for (size_t index=0;index<block.Transactions().size();++index) {
        const auto& parsed=block.Transactions()[index];const auto id=parsed.GetTxid();
        if (!ids.insert(id).second) corrupt();
        if (parsed.IsOrchard()) {
            for (const auto& input:parsed.Orchard().Inputs()) {
                uint256 hash;std::copy(input.txid_wire.begin(),input.txid_wire.end(),hash.begin());
                if (!spent.emplace(TxId(hash),input.output_index).second) corrupt();
            }
            for (size_t i=0;i<parsed.Orchard().Outputs().size();++i) {
                const auto& out=parsed.Orchard().Outputs()[i];
                created.emplace(OutPoint(id,uint32_t(i)),UTXOEntry(AmountUna::Una(out.amount_una),out.script_pub_key,context.height,false));
            }
        } else {
            const auto& tx=parsed.Historical();
            if (tx.IsCoinbase()!=(index==0) || Transaction::IsShieldedVersion(tx.version) || !tx.shielded_bundle_bytes.empty()) corrupt();
            if (index!=0) for (const auto& input:tx.vin)
                if (!spent.emplace(input.prevout.txid,input.prevout.vout).second) corrupt();
            for (size_t i=0;i<tx.vout.size();++i) {
                const auto& out=tx.vout[i];
                if (out.is_confidential || !out.commitment.empty() || out.value.GetUna()>orchard::kMaxMoneyUna) corrupt();
                created.emplace(OutPoint(id,uint32_t(i)),UTXOEntry(out.value,out.scriptPubKey,context.height,index==0));
            }
        }
    }
    std::set<OutPoint> expected_created,expected_spent;
    for (const auto& [point,coin]:created) if (!spent.contains(point)) expected_created.insert(point);
    for (const auto& point:spent) if (!created.contains(point)) expected_spent.insert(point);
    std::set<OutPoint> undo_created,undo_spent;
    for (const auto& coin:undo->created) if (!undo_created.emplace(TxId(coin.txid),coin.vout).second) corrupt();
    for (const auto& coin:undo->spent) {
        if (!undo_spent.emplace(TxId(coin.prev_txid),coin.prev_vout).second ||
            coin.height>=context.height || coin.value>orchard::kMaxMoneyUna ||
            coin.is_confidential || !coin.commitment.empty()) corrupt();
    }
    if (undo_created!=expected_created || undo_spent!=expected_spent) corrupt();
    const DatabaseCoins current(db,context.height);
    for (const auto& [point,expected]:created) {
        const auto coin=current.getCoin(point);
        if (!coin.ok() && coin.status()!=Status::NotFound) throw OrchardStateLookupError(coin.status());
        if (spent.contains(point)) { if (coin.ok()) corrupt(); continue; }
        if (!coin.ok() || coin->value!=expected.value || coin->scriptPubKey!=expected.scriptPubKey ||
            coin->height!=expected.height || coin->isCoinbase!=expected.isCoinbase ||
            coin->is_confidential || !coin->commitment.empty()) corrupt();
    }
    for (const auto& point:expected_spent) {
        const auto coin=current.getCoin(point);
        if (coin.ok()) corrupt();
        if (coin.status()!=Status::NotFound) throw OrchardStateLookupError(coin.status());
    }
    StorageCheck(db.stageOrchardDisconnect(token,*state,batch));
    for (const auto& point:expected_created) StorageCheck(db.deleteCoin(token,point.txid.AsUint256(),point.vout,&batch));
    for (const auto& coin:undo->spent) {
        const UTXOEntry restored(AmountUna::Una(coin.value),coin.scriptPubKey,coin.height,coin.is_coinbase);
        StorageCheck(db.putCoin(token,coin.prev_txid,coin.prev_vout,StoredCoin(restored),&batch));
    }
    guard.Keep();
}
} // namespace dinero::consensus
