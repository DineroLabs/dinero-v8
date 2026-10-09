#pragma once
#include "daemon/services/historical_catalog.h"
#include <functional>

namespace dinero {
// Unreachable catalog roots for every completed replay checkpoint. Each update
// consumes that exact validated block/undo; it neither copies a full coin set
// per height nor runs another genesis replay per height. This is detached
// preparation only. A service writer must rebind the selected before-image.
class PreparedHistoricalCatalogRange final {
public:
    PreparedHistoricalCatalogRange(const PreparedHistoricalCatalogRange&)=delete;
    PreparedHistoricalCatalogRange& operator=(const PreparedHistoricalCatalogRange&)=delete;
    storage::catalog::HistoricalState At(uint32_t height) const {
        Check();Require(height>=first_&&height<=last_);
        const auto bytes=states_.Get(Key(height));Require(bool(bytes));
        const auto state=storage::catalog::HistoricalState::Decode(std::string(bytes->begin(),bytes->end()));
        Require(state.height==height);BindCheckpoint(state,proof_.CheckpointAt(height));return state;
    }
    uint32_t First()const{Check();return first_;}
    uint32_t Last()const{Check();return last_;}
private:
    friend class ChainstateService;
    friend class OrchardReindexOwner;
    friend class PreparedOrchardChainstateWrite;
    friend struct HistoricalRangeCatalogTestAccess;
    using State=storage::catalog::HistoricalState;
    using Digest=storage::catalog::Digest;
    using Bytes=wallet::detail::RuntimeReplaySpool::Bytes;
    ChainDB& db_;const HistoricalCompactReplay& proof_;
    uint32_t first_,last_;
    const std::thread::id thread_=std::this_thread::get_id();
    wallet::detail::RuntimeReplaySpool states_;
    bool finished_=false;
    PreparedHistoricalCatalogRange(ChainDB& db,const HistoricalCompactReplay& proof,uint32_t first)
        :db_(db),proof_(proof),first_(first),last_(proof.ValidatedTarget().height) {}
    static void Require(bool ok){if(!ok)throw std::runtime_error("Historical catalog range unavailable");}
    void Check() const {
        Require(finished_&&thread_==std::this_thread::get_id());(void)proof_.State();
        Require(proof_.CheckpointCount()==uint64_t(last_)-first_+1);
    }
    static Bytes Key(uint32_t height) {
        wallet::detail::replay_spool_codec::Writer w;w.Number('c',1);w.Number(height,4);return std::move(w.bytes);
    }
    static State WithCheckpoint(State state,const HistoricalCompactReplay::Checkpoint& checkpoint) {
        const auto& s=checkpoint.snapshot;state.network=s.network_code;state.genesis=s.genesis;
        state.branch=s.branch_id;state.activation=s.activation_height;state.leaf_activation=s.leaf_activation_height;
        state.height=checkpoint.target.height;state.block=checkpoint.target.hash;state.parent=checkpoint.parent;
        state.work=checkpoint.target.chainwork;state.legacy_epoch=s.legacy_epoch_height;
        state.legacy_state_root=s.legacy_state_root;state.tree_root=s.tree_root;state.legacy_value=s.legacy_value;
        state.tree_size=s.tree_size;state.nullifier_count=s.nullifier_count;state.stump=s.stump;state.Validate();return state;
    }
    static void BindCheckpoint(const State& state,const HistoricalCompactReplay::Checkpoint& checkpoint) {
        Require(WithCheckpoint(state,checkpoint).Encode()==state.Encode());
    }
    void Store(const State& state) {
        const auto bytes=state.Encode();wallet::detail::RuntimeReplaySpool::Batch batch(states_);
        states_.Insert(Key(state.height),{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});batch.Commit();
    }
    static std::unique_ptr<PreparedHistoricalCatalogRange> Create(ChainDB& db,const ChainWriteToken& token,
        const PreparedHistoricalCatalog& initial,const HistoricalCompactReplay& proof,const OrchardHistoryCapture& history,
        const std::function<Block(uint32_t,const uint256&)>& body_at) {
        initial.Check();Require(&initial.db_==&db&&history.Finished()&&bool(body_at));
        State state=initial.State();auto result=std::unique_ptr<PreparedHistoricalCatalogRange>(
            new PreparedHistoricalCatalogRange(db,proof,state.height));
        Require(result->first_<=result->last_&&proof.CheckpointCount()==uint64_t(result->last_)-result->first_+1);
        BindCheckpoint(state,proof.CheckpointAt(state.height));result->Store(state);
        constexpr size_t pending_limit=1024;std::map<Digest,std::string> pending;
        const auto flush=[&] {
            if(pending.empty())return;rocksdb::WriteBatch batch;
            for(const auto& [id,bytes]:pending)Require(db.stageOrchardCatalogNode(token,id,bytes,batch)==Status::Ok);
            Require(db.writeBatch(token,std::move(batch),true)==Status::Ok);pending.clear();
        };
        const auto read=[&](const Digest& id) {
            if(const auto it=pending.find(id);it!=pending.end())return it->second;
            const auto value=db.getOrchardCatalogNode(id);Require(value.ok());return *value;
        };
        const auto write=[&](const Digest& id,const std::string& bytes) {
            Require(storage::catalog::ValidNode(id,bytes));
            if(const auto it=pending.find(id);it!=pending.end()){Require(it->second==bytes);return;}
            if(pending.size()==pending_limit)flush();pending.emplace(id,bytes);
        };
        namespace c=storage::catalog;
        c::Tree transactions(c::Kind::Transactions,read,write),legacy(c::Kind::LegacyCoins,read,write),
            special(c::Kind::NonTransparentCoins,read,write);
        for(uint64_t next=uint64_t(result->first_)+1;next<=result->last_;++next) {
            const auto height=uint32_t(next);const auto checkpoint=proof.CheckpointAt(height);
            const auto header=history.HeaderAt(height);
            Require(checkpoint.parent==state.block&&checkpoint.target.chainwork>state.work&&
                checkpoint.target.hash==header.hash&&checkpoint.target.chainwork==header.work&&
                checkpoint.wire_hash==history.WireHashAt(height));
            const auto block=body_at(height,header.hash);const auto wire=block.Serialize();uint256 digest;
            crypto::CSHA256().Write(wire).Finalize(digest.data);
            Require(digest==checkpoint.wire_hash&&block.header.SerializeForHash()==header.header.SerializeForHash()&&!block.vtx.empty());
            const auto undo=UndoRecord::Deserialize(checkpoint.undo);
            struct Metadata{uint32_t height;bool coinbase,special;};
            std::map<OutPoint,Metadata> spent,created;std::set<OutPoint> consumed;
            for(const auto& coin:undo.spent)Require(spent.emplace(OutPoint(TxId(coin.prev_txid),coin.prev_vout),
                Metadata{coin.height,coin.is_coinbase,coin.is_confidential||!coin.commitment.empty()}).second);
            const auto remove=[&](const OutPoint& point,const Metadata& coin) {
                const auto bytes=c::OutpointBytes(point);const auto legacy_key=c::LegacyKey(bytes),special_key=c::NonTransparentKey(bytes);
                const auto old=legacy.Find(state.legacy,legacy_key);const auto old_special=special.Find(state.nontransparent,special_key);
                Require(coin.height<=height);
                if(coin.height<state.leaf_activation) {
                    auto expected=bytes;c::Number(expected,coin.height,4);expected.push_back(coin.coinbase?1:0);
                    Require(old&&*old==expected&&state.legacy_count);state.legacy=legacy.Erase(state.legacy,legacy_key);--state.legacy_count;
                } else Require(!old);
                if(coin.special){Require(old_special&&*old_special==bytes&&state.nontransparent_count);
                    state.nontransparent=special.Erase(state.nontransparent,special_key);--state.nontransparent_count;}
                else Require(!old_special);
            };
            for(size_t ordinal=0;ordinal<block.vtx.size();++ordinal) {
                const auto& tx=block.vtx[ordinal];Require(tx.IsCoinbase()==(ordinal==0));
                Require(state.transaction_count!=UINT64_MAX);
                state.transactions=transactions.Insert(state.transactions,c::TransactionKey(tx.GetTxid()),{});++state.transaction_count;
                if(ordinal)for(const auto& input:tx.vin) {
                    const OutPoint point(input.prevout.txid,input.prevout.vout);Require(consumed.insert(point).second);
                    if(const auto it=created.find(point);it!=created.end())remove(point,it->second);
                    else {const auto prior=spent.find(point);Require(prior!=spent.end());remove(point,prior->second);}
                }
                Require(tx.vout.size()<=UINT32_MAX);
                for(size_t n=0;n<tx.vout.size();++n) {
                    // Match the actual historical BlockValidator replay coin set, including OP_RETURN.
                    const auto& output=tx.vout[n];
                    const OutPoint point(tx.GetTxid(),uint32_t(n));const Metadata coin{height,ordinal==0,output.is_confidential||!output.commitment.empty()};
                    Require(created.emplace(point,coin).second);const auto bytes=c::OutpointBytes(point);
                    if(height<state.leaf_activation) {Require(state.legacy_count!=UINT64_MAX);auto value=bytes;
                        c::Number(value,height,4);value.push_back(coin.coinbase?1:0);
                        state.legacy=legacy.Insert(state.legacy,c::LegacyKey(bytes),value);++state.legacy_count;}
                    if(coin.special){Require(state.nontransparent_count!=UINT64_MAX);
                        state.nontransparent=special.Insert(state.nontransparent,c::NonTransparentKey(bytes),bytes);++state.nontransparent_count;}
                }
            }
            for(const auto& [point,coin]:spent)Require(consumed.count(point));
            Require(state.transaction_count==history.TransactionCountAt(height));
            state=WithCheckpoint(std::move(state),checkpoint);flush();result->Store(state);
        }
        result->states_.Freeze();result->finished_=true;
        // Re-read every authenticated root after the entire preparation finishes.
        for(uint64_t h=result->first_;h<=result->last_;++h)(void)result->At(uint32_t(h));
        return result;
    }
};
} // namespace dinero
