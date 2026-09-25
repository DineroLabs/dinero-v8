#include "consensus/orchard_block_staging.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/filter_commitment.h"
#include <set>
#include <exception>
#include <map>
#include "consensus/undo.h"
#include "consensus/utreexo_delta_codec.h"
#include "util/hex.h"
#include "crypto/sha256.h"
#include <cstdio>

namespace dinero {
Status ChainDB::stageOrchardBlock(const ChainWriteToken& token,const OrchardBlockCandidate& block,
    bool require_witness_commitment,rocksdb::WriteBatch& batch) {
    (void)token;
    if(!db_)return Status::Internal;
    std::string error;
    if(!block.Header().IsReservedValid() || !block.CheckSizeLimits(error) ||
        !block.CheckIdentityCommitments(require_witness_commitment,error))return Status::Invalid;
    const auto& bytes=block.WireBytes();
    return convertRocksDBStatus(batch.Put(cf_[idx_blocks_].get(),makeBlockKey(block.Header().GetHash()),
        rocksdb::Slice(reinterpret_cast<const char*>(bytes.data()),bytes.size())));
}
}

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
template<class T> T RequiredLocal(StatusOr<T> result) {
    if(!result.ok())throw OrchardStateLookupError(result.status()==Status::NotFound?Status::Corruption:result.status());
    return std::move(result.value());
}
arith_uint256 StoredHeaderWork(const ChainDB& db,const BlockHeader& header,uint32_t height) {
    const auto hash=header.GetHash();
    const auto stored=RequiredLocal(db.getHeader(hash));
    if(height>INT32_MAX || RequiredLocal(db.getBlockHeight(hash))!=int(height) ||
        stored.SerializeForHash()!=header.SerializeForHash())throw OrchardStateLookupError(Status::Corruption);
    return RequiredLocal(db.getBlockWork(hash));
}
void CheckStateMarkers(const ChainDB& db,uint32_t height,const uint256& hash,const uint256& root) {
    const auto tip=RequiredLocal(db.getTip());
    const auto validated=RequiredLocal(db.getValidatedTip());
    const auto marker=RequiredLocal(db.getForestTipMarker());
    if(height>INT32_MAX || tip.height!=int(height) || tip.hash!=hash ||
        validated.height!=int(height) || validated.hash!=hash ||
        marker.height!=int(height) || marker.block_hash!=hash || marker.forest_root!=root ||
        RequiredLocal(db.getBlockHashByHeight(int(height)))!=hash)
        throw OrchardStateLookupError(Status::Corruption);
}
void StageMarkers(ChainDB& db,const ChainWriteToken& token,const BlockHeader& header,
    uint32_t height,const arith_uint256& work,rocksdb::WriteBatch& batch) {
    const auto hash=header.GetHash();
    StorageCheck(db.putForestTipMarker(token,{int32_t(height),hash,header.utreexo_root},&batch));
    StorageCheck(db.putHeightIndex(token,int(height),hash,&batch));
    StorageCheck(db.setTip(token,hash,int(height),work,&batch));
    StorageCheck(db.setValidatedTip(token,hash,int(height),&batch));
}
std::string CommitKey(uint32_t height,const uint256& hash) {
    char height_hex[9];std::snprintf(height_hex,sizeof(height_hex),"%08x",height);
    // Versioned separately from the legacy shielded journal, whose fingerprint
    // covers different containers. A retained row is NOT an active-tip pointer.
    return std::string("orchard_consensus_journal:v1:")+height_hex+":"+hash.GetHex();
}
std::string CommitRecord(const OrchardBlockContext& context,const BlockHeader& header,
    const arith_uint256& work,const UtreexoForest& forest,const storage::OrchardStoredState& state) {
    (void)orchard::SigningContext::Create(context.domain,0,{}, {},0);
    const auto root=forest.getCommitment();
    if(context.activation_height==UINT32_MAX || !context.activation_height ||
        state.height<context.activation_height || state.height!=context.height ||
        state.block_hash!=context.block_hash || header.GetHash()!=context.block_hash ||
        header.prev_block_hash!=context.parent_hash ||
        !std::equal(root.begin(),root.end(),header.utreexo_root.begin()))
        throw OrchardStateLookupError(Status::Corruption);
    const auto frontier=[&] {
        try{return orchard::OrchardFrontier::Decode({
            reinterpret_cast<const uint8_t*>(state.frontier.data()),state.frontier.size()});}
        catch(const orchard::BackendError& e){throw OrchardStateLookupError(
            e.Status()==DINERO_ORCHARD_PANIC?Status::Internal:Status::Corruption);}
    }();
    if(frontier.Size()!=state.tree_size || state.pool_balance>orchard::kMaxMoneyUna ||
        !std::equal(frontier.Root().begin(),frontier.Root().end(),state.anchor.begin()))
        throw OrchardStateLookupError(Status::Corruption);
    crypto::CSHA256 hash;hash.Write("DIN/orchard/chainstate-commit/v1");
    const auto number=[&](uint64_t value,size_t width) {
        std::array<uint8_t,8> bytes{};for(size_t i=0;i<width;++i)bytes[i]=uint8_t(value>>(8*i));
        hash.Write(bytes.data(),width);
    };
    number(context.domain.network_code,1);hash.Write(context.domain.genesis_wire.data(),32);
    number(context.domain.branch_id,4);number(context.activation_height,4);number(context.height,4);
    const auto bytes=header.SerializeForHash();hash.Write(bytes.data(),bytes.size());
    hash.Write(work.GetHex());number(forest.getNumLeaves(),8);
    number(state.pool_balance,8);number(state.tree_size,8);hash.Write(state.anchor.begin(),32);
    number(state.frontier.size(),4);hash.Write(state.frontier);
    const auto digest=hash.Finalize();
    return std::string("DOC1")+std::string(digest.begin(),digest.end());
}
void CheckCommitRecord(const ChainDB& db,const OrchardBlockContext& context,const BlockHeader& header,
    const arith_uint256& work,const UtreexoForest& forest,const storage::OrchardStoredState& state) {
    std::string stored;const auto status=db.getRaw(CommitKey(context.height,context.block_hash),stored);
    if(status!=Status::Ok)throw OrchardStateLookupError(status==Status::NotFound?Status::Corruption:status);
    if(stored!=CommitRecord(context,header,work,forest,state))throw OrchardStateLookupError(Status::Corruption);
}
OrchardBlockContext ParentContext(const OrchardBlockContext& current,const BlockHeader& parent) {
    auto result=current;--result.height;result.block_hash=parent.GetHash();result.parent_hash=parent.prev_block_hash;
    return result;
}
}
OrchardBlockCandidate ReadStoredOrchardBlock(const ChainDB& db,const uint256& hash,
    bool require_witness_commitment,const BlockStorage* archival_blocks) {
    auto bytes=db.getBlockEncoding(hash);
    // Missing embedded bytes may use the indexed flatfile. Never hide an I/O
    // error or corrupt embedded body by silently selecting another source.
    if(bytes.status()==Status::NotFound && archival_blocks) {
        const auto metadata=db.getHeaderMetadata(hash);
        if(!metadata.ok())throw OrchardStateLookupError(metadata.status());
        if(metadata->data_size==0)throw OrchardStateLookupError(Status::NotFound);
        const auto raw=archival_blocks->readBlockBytes(
            FilePosition(metadata->file_number,metadata->data_pos,metadata->data_size));
        if(!raw.ok())throw OrchardStateLookupError(raw.status());
        bytes=std::vector<uint8_t>(raw->begin(),raw->end());
    }
    if(!bytes.ok())throw OrchardStateLookupError(bytes.status());
    const auto block=[&] {
        try{return OrchardBlockCandidate::DecodeExact(*bytes);}
        catch(const std::invalid_argument&){throw OrchardStateLookupError(Status::Corruption);}
        catch(const orchard::BackendError& e){throw OrchardStateLookupError(
            e.Status()==DINERO_ORCHARD_PANIC?Status::Internal:Status::Corruption);}
    }();
    std::string error;
    if(block.Header().GetHash()!=hash || !block.Header().IsReservedValid() ||
        !block.CheckSizeLimits(error) || !block.CheckIdentityCommitments(require_witness_commitment,error))
        throw OrchardStateLookupError(Status::Corruption);
    return block;
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
    if (!block.Header().IsReservedValid() || !block.CheckSizeLimits(body_error) ||
        !block.CheckIdentityCommitments(require_witness_commitment, body_error))
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

StagedOrchardChainstate StageOrchardChainstateConnectUnderLock(ChainDB& db,
    const ChainWriteToken& token,const OrchardBlockContext& context,const OrchardBlockCandidate& block,
    const BlockHeader& parent,const UtreexoForest& forest,const OrchardBranchMtpLookup& mtp,
    bool require_witness_commitment,bool checkpoint,rocksdb::WriteBatch& batch) {
    EmptyBatchGuard guard(batch);
    if(context.height==0 || context.height>INT32_MAX || parent.GetHash()!=context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const auto parent_work=StoredHeaderWork(db,parent,context.height-1);
    const auto work=StoredHeaderWork(db,block.Header(),context.height);
    CheckStateMarkers(db,context.height-1,context.parent_hash,parent.utreexo_root);
    if(RequiredLocal(db.getTip()).work!=parent_work || work<=parent_work)
        throw OrchardStateLookupError(Status::Corruption);
    // This path owns active transaction indexing. A prior row would need an
    // explicit undo policy; never silently overwrite an unrelated location.
    for(const auto& tx:block.Transactions()) {
        const auto location=db.getTxLocation(tx.GetTxid().AsUint256());
        if(location.ok())throw OrchardStateLookupError(Status::AlreadyExists);
        if(location.status()!=Status::NotFound)throw OrchardStateLookupError(location.status());
    }
    auto prepared=StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,context,block,mtp,
        require_witness_commitment,batch);
    const auto filter=CheckOrchardBlockFilter(block,prepared.coins);
    if(prepared.orchard.Parent())CheckCommitRecord(db,ParentContext(context,parent),parent,parent_work,
        forest,*prepared.orchard.Parent());
    auto transition=[&] {
        try{return PrepareOrchardForestTransition(prepared.coins,parent,forest);}
        catch(const OrchardForestError&){throw OrchardStateLookupError(Status::Corruption);}
    }();
    if(!transition.MatchesHeader(block.Header()))throw OrchardStateError(OrchardStateErrorCode::BlockBody);
    try{CheckOrchardBlockUtreexoProof(block,prepared.coins,parent,forest);}
    catch(const OrchardForestError& e) {
        if(e.Code()!=OrchardForestErrorCode::Proof)throw OrchardStateLookupError(Status::Corruption);
        throw OrchardStateError(OrchardStateErrorCode::BlockBody);
    }
    std::string delta,error;
    if(!SerializeUtreexoDelta(transition.Delta(),delta,error))throw OrchardStateLookupError(Status::Corruption);
    const auto key=MakeUtreexoDeltaUndoKey(context.block_hash);
    std::string existing;const auto status=db.getRaw(key,existing);
    if(status==Status::Ok) {if(existing!=delta)throw OrchardStateLookupError(Status::Corruption);}
    else if(status!=Status::NotFound)throw OrchardStateLookupError(status);
    batch.Put(key,delta);
    if(checkpoint)StorageCheck(db.putUtreexoCheckpointWithChecksum(token,int(context.height),transition.After().serialize(),&batch));
    const auto retained_filter=db.getBlockFilter(context.block_hash);
    if(retained_filter.ok()) {
        if(retained_filter->data!=filter.encoded_data || retained_filter->element_count!=filter.element_count)
            throw OrchardStateLookupError(Status::Corruption);
    } else if(retained_filter.status()!=Status::NotFound)throw OrchardStateLookupError(retained_filter.status());
    StorageCheck(db.putBlockFilter(token,context.block_hash,filter.encoded_data,filter.element_count,&batch));
    StorageCheck(db.stageOrchardBlock(token,block,require_witness_commitment,batch));
    for(size_t i=0;i<block.Transactions().size();++i)
        StorageCheck(db.putTxIndex(token,block.Transactions()[i].GetTxid().AsUint256(),context.block_hash,uint32_t(i),&batch));
    const auto journal=CommitRecord(context,block.Header(),work,transition.After(),prepared.orchard.Next());
    std::string retained;const auto journal_status=db.getRaw(CommitKey(context.height,context.block_hash),retained);
    if(journal_status==Status::Ok && retained!=journal)throw OrchardStateLookupError(Status::Corruption);
    if(journal_status!=Status::Ok && journal_status!=Status::NotFound)throw OrchardStateLookupError(journal_status);
    StorageCheck(batch.Put(CommitKey(context.height,context.block_hash),journal).ok()?Status::Ok:Status::Internal);
    StageMarkers(db,token,block.Header(),context.height,work,batch);
    guard.Keep();
    return {std::move(prepared),std::move(transition)};
}

UtreexoForest StageOrchardChainstateDisconnectUnderLock(ChainDB& db,
    const ChainWriteToken& token,const OrchardBlockContext& context,const OrchardBlockCandidate& block,
    const BlockHeader& parent,const UtreexoForest& forest,bool require_witness_commitment,
    rocksdb::WriteBatch& batch) {
    EmptyBatchGuard guard(batch);
    if(context.height==0 || context.height>INT32_MAX || parent.GetHash()!=context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const auto parent_work=StoredHeaderWork(db,parent,context.height-1);
    const auto work=StoredHeaderWork(db,block.Header(),context.height);
    CheckStateMarkers(db,context.height,context.block_hash,block.Header().utreexo_root);
    if(RequiredLocal(db.getTip()).work!=work || work<=parent_work)
        throw OrchardStateLookupError(Status::Corruption);
    const auto current_state=RequiredLocal(db.getOrchardState());
    CheckCommitRecord(db,context,block.Header(),work,forest,current_state);
    const auto undo_parent=RequiredLocal(db.getOrchardUndoParent(current_state));
    if((context.height==context.activation_height)!=!undo_parent)
        throw OrchardStateLookupError(Status::Corruption);
    for(size_t i=0;i<block.Transactions().size();++i) {
        const auto location=RequiredLocal(db.getTxLocation(block.Transactions()[i].GetTxid().AsUint256()));
        if(location.first!=context.block_hash || location.second!=i)throw OrchardStateLookupError(Status::Corruption);
    }
    std::string encoded,error;UtreexoDelta delta;
    const auto status=db.getRaw(MakeUtreexoDeltaUndoKey(context.block_hash),encoded);
    if(status!=Status::Ok)throw OrchardStateLookupError(status==Status::NotFound?Status::Corruption:status);
    if(!DeserializeUtreexoDelta(encoded,delta,error))throw OrchardStateLookupError(Status::Corruption);
    auto restored=[&] {
        try{return UndoOrchardForestDelta(forest,delta,parent,block.Header(),context.height);}
        catch(const OrchardForestError&){throw OrchardStateLookupError(Status::Corruption);}
    }();
    if(undo_parent)CheckCommitRecord(db,ParentContext(context,parent),parent,parent_work,restored,*undo_parent);
    StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(db,token,context,block,
        require_witness_commitment,batch);
    // Coins/undo have now been checked against the exact body. Reconstruct
    // the filter independently, including inputs spent within this same block.
    // The encoded-data hash alone does not authenticate the stored element count.
    const auto undo=RequiredLocal(db.getUndo(context.block_hash));
    std::map<OutPoint,std::vector<uint8_t>> scripts_by_coin;
    std::vector<std::vector<uint8_t>> scripts;
    for(const auto& coin:undo.spent)
        scripts_by_coin.emplace(OutPoint(TxId(coin.prev_txid),coin.prev_vout),coin.scriptPubKey);
    for(const auto& tx:block.Transactions()) {
        const auto add=[&](uint32_t n,const std::vector<uint8_t>& script) {
            scripts_by_coin.emplace(OutPoint(tx.GetTxid(),n),script);
            if(!script.empty() && script.front()!=0x6a)scripts.push_back(script);
        };
        if(tx.IsOrchard()) {
            const auto& outputs=tx.Orchard().Outputs();
            for(size_t i=0;i<outputs.size();++i)add(uint32_t(i),outputs[i].script_pub_key);
        } else {
            const auto& outputs=tx.Historical().vout;
            for(size_t i=0;i<outputs.size();++i)add(uint32_t(i),outputs[i].scriptPubKey);
        }
    }
    const auto spent_script=[&](const OutPoint& point) {
        const auto it=scripts_by_coin.find(point);
        if(it==scripts_by_coin.end())throw OrchardStateLookupError(Status::Corruption);
        if(!it->second.empty())scripts.push_back(it->second);
    };
    for(size_t i=1;i<block.Transactions().size();++i) {
        const auto& tx=block.Transactions()[i];
        if(tx.IsOrchard())for(const auto& in:tx.Orchard().Inputs()) {
            uint256 hash;std::copy(in.txid_wire.begin(),in.txid_wire.end(),hash.begin());
            spent_script(OutPoint(TxId(hash),in.output_index));
        } else for(const auto& in:tx.Historical().vin)spent_script(OutPoint(in.prevout.txid,in.prevout.vout));
    }
    const auto filter=GCSFilter::Build(scripts,context.parent_hash);
    const auto stored_filter=RequiredLocal(db.getBlockFilter(context.block_hash));
    std::string filter_error;
    if(stored_filter.data!=filter.encoded_data || stored_filter.element_count!=filter.element_count ||
        !ValidateFilterCommitment(block.Transactions()[0].Historical(),filter.GetHash(),context.height,filter_error))
        throw OrchardStateLookupError(Status::Corruption);
    for(const auto& tx:block.Transactions())StorageCheck(db.deleteTxIndex(token,tx.GetTxid().AsUint256(),&batch));
    StorageCheck(db.deleteHeightIndex(token,int(context.height),&batch));
    StorageCheck(db.deleteUtreexoCheckpointWithChecksum(token,int(context.height),&batch));
    StageMarkers(db,token,parent,context.height-1,parent_work,batch);
    guard.Keep();
    return restored;
}
void AuditOrchardChainstateTipUnderLock(ChainDB& db,const ChainWriteToken& token,
    const OrchardBlockContext& context,const BlockHeader& parent,const UtreexoForest& forest,
    bool require_witness_commitment) {
    const auto block=ReadStoredOrchardBlock(db,context.block_hash,require_witness_commitment);
    rocksdb::WriteBatch abandoned;
    // Check reversibility without applying it. This also checks tip-local coins,
    // nullifier owners, anchor references, indexes and persistent undo/delta.
    (void)StageOrchardChainstateDisconnectUnderLock(db,token,context,block,parent,forest,
        require_witness_commitment,abandoned);
}
} // namespace dinero::consensus
