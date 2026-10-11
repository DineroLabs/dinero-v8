#pragma once

#include "daemon/services/assumeutxo_replay.h"
#include "consensus/chainparams.h"
#include "consensus/chainwork.h"
#include "consensus/orchard_legacy_accounting.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_state_transition.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/state_commitment.h"
#include "consensus/undo.h"
#include "consensus/utreexo_delta_codec.h"
#include "wallet/runtime_replay_spool_codec.h"
#include "crypto/sha256.h"
#include "consensus/utreexo_stump.h"
#include "consensus/utreexo_maturity_leaf_activation.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace dinero {

// Independently validated historical prefix strictly BELOW the activation parent.
// This type has no retirement certificate and cannot authorize canonical writes.
// The service joins it to a separately completed selected-branch replay and
// rebinds both before-images. A failed append/finish poisons all result access.
// It owns a full private replay set; work limits are not resident-memory claims.
class HistoricalCompactReplay final {
public:
    struct Snapshot {
        uint8_t network_code=0xff;
        uint256 genesis,legacy_state_root,tree_root;
        uint32_t branch_id=0,activation_height=0,legacy_epoch_height=0,leaf_activation_height=0;
        uint64_t legacy_value=0,tree_size=0,nullifier_count=0;
        std::vector<uint8_t> stump;
    };
    struct Target {
        uint32_t height;
        uint256 hash;
        arith_uint256 chainwork;
    };
    struct Limits {
        uint32_t blocks;
        size_t serialized_bytes;
    };

    // Value material from a completed range, never a canonical authorization.
    // No full coin map/forest copy is retained for intermediate heights.
    struct Checkpoint {
        Target target;
        uint256 parent,wire_hash;
        Snapshot snapshot;
        std::vector<uint8_t> frontier,anchors,undo;
        std::string delta;
        std::vector<std::pair<uint32_t,uint256>> nullifiers;
    };
    void CaptureCheckpointsFrom(uint32_t first,size_t material_limit) {
        CheckThread();
        Require(!poisoned_&&!finished_&&!next_height_&&!checkpoints_&&
            first<=target_.height&&material_limit&&material_limit<=limits_.serialized_bytes,
            "Historical checkpoint range unavailable");
        auto spool=std::make_unique<wallet::detail::RuntimeReplaySpool>();
        // Account from the epoch containing the earliest requested prefix.
        // Without range capture, retain the established final-epoch behavior.
        epoch_=Params().shielded_activation_height;reset_epoch_=false;
        for(uint32_t boundary:{Params().shielded_epoch_reset_height,Params().shielded_spend_auth_epoch_reset_height}) {
            if(boundary<=first&&boundary>=epoch_){epoch_=boundary;reset_epoch_=true;}
        }
        accounting_={epoch_,target_.height,target_.hash,0,0,0};
        checkpoint_first_=first;checkpoint_limit_=material_limit;checkpoints_=std::move(spool);
    }
    uint64_t CheckpointCount() const {CheckFinished();return checkpoint_count_;}
    Checkpoint CheckpointAt(uint32_t height) const {
        CheckFinished();Require(checkpoints_&&height>=checkpoint_first_&&height<=target_.height,
            "Historical checkpoint outside captured range");
        const auto bytes=checkpoints_->Get(CheckpointKey(height));
        Require(bool(bytes),"Historical checkpoint missing");
        auto value=DecodeCheckpoint(*bytes);
        Require(value.target.height==height&&value.snapshot.branch_id==snapshot_.branch_id&&
            value.snapshot.activation_height==snapshot_.activation_height&&
            value.snapshot.network_code==snapshot_.network_code&&value.snapshot.genesis==snapshot_.genesis&&
            value.snapshot.leaf_activation_height==snapshot_.leaf_activation_height,
            "Historical checkpoint domain mismatch");
        if(height==target_.height)Require(value.target.hash==target_.hash&&value.target.chainwork==target_.chainwork&&
            value.snapshot.stump==snapshot_.stump,"Historical checkpoint final mismatch");
        return value;
    }

    HistoricalCompactReplay(Target target, Limits limits)
        : target_(std::move(target)), limits_(limits), profile_(Profile()),
          thread_(std::this_thread::get_id()) {
        const auto& params = Params();
        Require(consensus::OrchardProfileConfigurationValid(params) &&
            params.orchard_activation_height != UINT32_MAX &&
            params.orchard_activation_height != 0 &&
            uint64_t(target_.height) + 1 < params.orchard_activation_height &&
            params.shielded_activation_height < params.orchard_activation_height &&
            !target_.hash.IsNull() && !target_.chainwork.IsZero(), "Historical prefix replay profile unavailable");
        Require(limits_.blocks && uint64_t(target_.height) + 1 <= limits_.blocks &&
            limits_.serialized_bytes, "Historical prefix replay exceeds local work policy");
        epoch_ = params.shielded_activation_height;
        for(uint32_t boundary:{params.shielded_epoch_reset_height,params.shielded_spend_auth_epoch_reset_height}) {
            if(boundary<=target_.height&&boundary>=epoch_){epoch_=boundary;reset_epoch_=true;}
        }
        accounting_ = {epoch_, target_.height, target_.hash, 0, 0, 0};
        replay_.SetUndoTailWindow(1);
    }
    HistoricalCompactReplay(const HistoricalCompactReplay&) = delete;
    HistoricalCompactReplay& operator=(const HistoricalCompactReplay&) = delete;
    HistoricalCompactReplay(HistoricalCompactReplay&&) = delete;
    HistoricalCompactReplay& operator=(HistoricalCompactReplay&&) = delete;

    // stored_work is captured with this exact block's header/height. It is only
    // a comparison input: accumulated work is independently calculated here.
    void Append(const Block& block, uint32_t height, const arith_uint256& stored_work) {
        CheckThread();
        const bool usable = !poisoned_ && !finished_;
        poisoned_ = true;
        Require(usable && Profile() == profile_, "Historical prefix replay owner unavailable");
        Require(height == next_height_ && height <= target_.height &&
            block.header.prev_block_hash == previous_ && !block.vtx.empty(),
            "Historical prefix replay input is not contiguous");
        const auto bytes = block.Serialize().size();
        Require(bytes <= limits_.serialized_bytes - material_, "Historical prefix replay material limit");
        auto next_work = work_ + GetBlockProof(block.header.difficulty);
        Require(next_work >= work_ && next_work == stored_work, "Historical prefix replay work mismatch");
        const auto hash = block.GetHash();
        if (height == target_.height)
            Require(hash == target_.hash && next_work == target_.chainwork,
                "Historical prefix replay target mismatch");

        // Accounting is speculative until the SAME complete block passes the
        // normal stateful validator (scripts, maturity, fees, proofs, roots).
        auto next_accounting = accounting_;
        // Account each actual prefix in its own epoch; a later reset must not
        // erase accounting from an earlier retained checkpoint.
        for(uint32_t boundary:{Params().shielded_epoch_reset_height,Params().shielded_spend_auth_epoch_reset_height}) {
            if(height==boundary&&boundary>=epoch_) {
                epoch_=boundary;reset_epoch_=true;
                next_accounting={epoch_,height,hash,0,0,0};
            }
        }
        next_accounting.parent_height=height;
        next_accounting.parent_hash=hash;
        if (height >= epoch_) AccountBlock(block, height, next_accounting);
        std::string error;
        const bool valid = height == 0 ? replay_.SeedGenesis(block, error)
            : replay_.ConnectAndAdvance(block, height, hash, error);
        Require(valid, "Historical prefix replay validation failed: " + error);
        if (height == target_.height) {
            parent_coinbase_ = block.vtx.front();
        }
        accounting_ = next_accounting;
        previous_ = hash;
        work_ = next_work;
        material_ += bytes;
        ++next_height_;
        if(checkpoints_&&height>=checkpoint_first_)StoreCheckpoint(block,height,work_);
        poisoned_ = false;
    }

    void Finish() {
        CheckThread();
        const bool usable = !poisoned_ && !finished_;
        poisoned_ = true;
        Require(usable && Profile() == profile_ &&
            next_height_ == uint64_t(target_.height) + 1 && previous_ == target_.hash &&
            replay_.Height() == target_.height && work_ == target_.chainwork &&
            accounting_.blocks_read == (target_.height>=epoch_ ? uint64_t(target_.height)+1-epoch_ : 0),
            "Historical prefix replay is incomplete");
        auto result=MakeSnapshot(target_.height,target_.hash,parent_coinbase_);
        if(checkpoints_) {
            Require(checkpoint_count_==uint64_t(target_.height)-checkpoint_first_+1,
                "Historical checkpoint range incomplete");
            checkpoints_->Freeze();
        }
        snapshot_ = std::move(result);
        finished_ = true;
        poisoned_ = false;
    }

    const Snapshot& State() const { CheckFinished(); return snapshot_; }
    const std::deque<assumeutxo::AssumeUtxoReplayEngine::CapturedUndo>& ProvenUndo() const {
        CheckFinished();return replay_.UndoTail();
    }
    const consensus::SelectedLegacyPoolAccounting& Accounting() const { CheckFinished(); return accounting_; }
    const assumeutxo::AssumeUtxoReplayEngine& ProvenState() const { CheckFinished(); return replay_; }
    const Target& ValidatedTarget() const { CheckFinished(); return target_; }

private:
    Snapshot MakeSnapshot(uint32_t height,const uint256& hash,const Transaction& coinbase) const {
        const auto& params = Params();
        Snapshot result;
        result.network_code = params.name == "mainnet" ? 0 : params.name == "testnet" ? 1 :
            params.name == "regtest" ? 2 : 0xff;
        Require(result.network_code != 0xff && uint256::FromHex(params.genesis_hash, result.genesis)
            && !result.genesis.IsNull(), "Historical prefix replay domain unavailable");
        result.branch_id = params.orchard_branch_id;
        result.activation_height = params.orchard_activation_height;
        result.legacy_epoch_height = epoch_;
        result.legacy_value = accounting_.value_una;
        result.leaf_activation_height = consensus::GetUtreexoMaturityLeafActivationHeight();
        const auto root = consensus::shielded::ComputeShieldedRoot(*replay_.ShieldedTree(),
            *replay_.ShieldedNullifiers(), *replay_.ShieldedAnchors());
        Require(bool(root), "Historical prefix replay shielded root unavailable");
        result.legacy_state_root = *root;
        const auto tree_root = replay_.ShieldedTree()->Root();
        std::copy(tree_root.begin(), tree_root.end(), result.tree_root.begin());
        result.tree_size = replay_.ShieldedTree()->Size();
        const auto count = replay_.ShieldedNullifiers()->TryCount();
        Require(bool(count), "Historical prefix replay nullifier count unavailable");
        uint64_t enumerated = 0;
        Require(replay_.ShieldedNullifiers()->ForEach([&](uint32_t row_height, const uint8_t*) {
            if (row_height < epoch_ || row_height > height || enumerated >= *count) return false;
            ++enumerated;
            return true;
        }) && enumerated == *count, "Historical prefix replay nullifier inventory mismatch");
        result.nullifier_count = enumerated;
        if (consensus::IsStateCommitmentActive(height, params.state_commitment_activation_height)) {
            const auto commitment = consensus::FindStateCommitment(coinbase);
            Require(commitment.status == consensus::StateCommitmentStatus::Ok &&
                commitment.root == result.legacy_state_root, "Historical prefix replay frozen commitment mismatch");
        }
        const auto* forest=replay_.Forest();Require(forest!=nullptr,"Historical prefix forest unavailable");
        const auto stump=consensus::UtreexoStump::fromForest(*forest);
        result.stump=stump.serialize();
        Require(consensus::UtreexoStump::deserialize(result.stump).serialize()==result.stump &&
            stump.getNumLeaves()==forest->getNumLeaves() && stump.getCommitment()==forest->getCommitment(),
            "Historical prefix stump mismatch");
        const auto& undo=replay_.UndoTail();
        Require(height==0 ? undo.empty() :
            undo.size()==1 && undo.front().height==height && undo.front().block_hash==hash,
            "Historical prefix undo unavailable");
        return result;
    }
    // Parameters are immutable after network selection. Check the owning
    // release/epoch domain as well so sequential test/network changes refuse.
    // This is not permission for concurrent mutation of process ChainParams.
    using ProfileIdentity = std::tuple<std::string, std::string, std::string,
        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static ProfileIdentity Profile() {
        const auto& p = Params();
        return std::make_tuple(p.name, p.network_id, p.genesis_hash,
            p.orchard_activation_height, p.orchard_branch_id, p.release_v8113_activation_height,
            p.shielded_activation_height, p.shielded_epoch_reset_height,
            p.shielded_spend_auth_epoch_reset_height, p.state_commitment_activation_height,
            p.shielded_compact_activation_height,p.sixty_second_activation_height,
            consensus::GetUtreexoMaturityLeafActivationHeight());
    }
    static void Require(bool condition, const std::string& reason) {
        if (!condition) throw std::runtime_error(reason);
    }
    void CheckThread() const {
        Require(thread_ == std::this_thread::get_id(), "Historical prefix replay belongs to another thread");
    }
    void CheckFinished() const {
        CheckThread();
        Require(finished_ && !poisoned_ && Profile() == profile_, "Historical prefix replay has no completed result");
    }
    static uint64_t Add(uint64_t left, uint64_t right) {
        Require(left <= orchard::kMaxMoneyUna && right <= orchard::kMaxMoneyUna - left,
            "Historical prefix replay monetary overflow");
        return left + right;
    }
    template<class Coin> static uint64_t Amount(const Coin& coin) {
        Require(!coin.is_confidential && coin.commitment.empty(), "Historical prefix replay amount is confidential");
        return Add(0, coin.value.GetUna());
    }
    void AccountBlock(const Block& block, uint32_t height,
                      consensus::SelectedLegacyPoolAccounting& result) const {
        // Include only earlier outputs from this same immutable block. Prefix
        // inputs are copied from the independently validated private UTXO set,
        // never located through the current canonical transaction index.
        std::map<OutPoint, TxOutput> earlier_outputs;
        std::set<OutPoint> spent;
        for (size_t index = 0; index < block.vtx.size(); ++index) {
            const auto& tx = block.vtx[index];
            Require(tx.IsCoinbase() == (index == 0), "Historical prefix replay coinbase position mismatch");
            if (Transaction::IsShieldedVersion(tx.version)) {
                Require(index && !(reset_epoch_ && height == epoch_), "Historical prefix replay invalid epoch transaction");
                Require(tx.ShieldedBundleCommitsToTxid() && tx.HasExplicitFee(),
                    "Historical prefix replay unauthenticated historical amount");
                uint64_t inputs = 0, outputs_and_fee = Add(0, tx.GetExplicitFee());
                for (const auto& input : tx.vin) {
                    const OutPoint point(input.prevout.txid, input.prevout.vout);
                    Require(!spent.count(point), "Historical prefix replay duplicate spend");
                    const auto earlier = earlier_outputs.find(point);
                    if (earlier != earlier_outputs.end()) inputs = Add(inputs, Amount(earlier->second));
                    else {
                        const auto coin = replay_.CopyPrefixCoin(point);
                        Require(bool(coin), "Historical prefix replay funding coin unavailable");
                        inputs = Add(inputs, Amount(*coin));
                    }
                    spent.insert(point);
                }
                for (const auto& output : tx.vout) outputs_and_fee = Add(outputs_and_fee, Amount(output));
                if (inputs >= outputs_and_fee) result.value_una = Add(result.value_una, inputs - outputs_and_fee);
                else {
                    Require(outputs_and_fee - inputs <= result.value_una, "Historical prefix replay negative legacy pool");
                    result.value_una -= outputs_and_fee - inputs;
                }
                ++result.shielded_transactions;
            } else if (index) {
                for (const auto& input : tx.vin)
                    Require(spent.emplace(input.prevout.txid, input.prevout.vout).second,
                        "Historical prefix replay duplicate spend");
            }
            const auto txid = tx.GetTxid();
            Require(tx.vout.size() <= UINT32_MAX, "Historical prefix replay output index overflow");
            for (size_t n = 0; n < tx.vout.size(); ++n)
                Require(earlier_outputs.emplace(OutPoint(txid, uint32_t(n)), tx.vout[n]).second,
                    "Historical prefix replay duplicate output");
        }
        ++result.blocks_read;
    }

    using CheckpointBytes=wallet::detail::RuntimeReplaySpool::Bytes;
    static CheckpointBytes CheckpointKey(uint32_t height) {
        wallet::detail::replay_spool_codec::Writer w;w.Number('p',1);w.Number(height,4);return std::move(w.bytes);
    }
    static CheckpointBytes EncodeCheckpoint(const Checkpoint& c) {
        wallet::detail::replay_spool_codec::Writer w;w.Number(1,1);
        w.Number(c.target.height,4);w.Hash(c.target.hash);w.Hash(c.parent);w.Hash(c.wire_hash);
        for(unsigned n=0;n<4;++n)w.Number(c.target.chainwork.GetWord(n),8);
        const auto& v=c.snapshot;w.Number(v.network_code,1);w.Hash(v.genesis);
        w.Number(v.branch_id,4);w.Number(v.activation_height,4);w.Number(v.legacy_epoch_height,4);w.Number(v.leaf_activation_height,4);
        w.Hash(v.legacy_state_root);w.Hash(v.tree_root);w.Number(v.legacy_value,8);w.Number(v.tree_size,8);w.Number(v.nullifier_count,8);
        w.Blob(v.stump);w.Blob(c.frontier);w.Blob(c.anchors);w.Blob(c.undo);
        w.Blob({reinterpret_cast<const uint8_t*>(c.delta.data()),c.delta.size()});
        Require(c.nullifiers.size()==v.nullifier_count,"Historical checkpoint nullifier count");w.Number(c.nullifiers.size(),4);
        for(const auto& [height,hash]:c.nullifiers){w.Number(height,4);w.Hash(hash);}
        return std::move(w.bytes);
    }
    static Checkpoint DecodeCheckpoint(std::span<const uint8_t> bytes) {
        namespace codec=wallet::detail::replay_spool_codec;
        Require(bytes.size()<=wallet::detail::RuntimeReplaySpool::MaximumRecordBytes,"Historical checkpoint record limit");
        codec::Reader r{bytes};Require(r.Number(1)==1,"Historical checkpoint version");Checkpoint c;
        c.target.height=uint32_t(r.Number(4));c.target.hash=r.Hash();c.parent=r.Hash();c.wire_hash=r.Hash();
        for(unsigned n=0;n<4;++n)c.target.chainwork.SetWord(n,r.Number(8));
        auto& v=c.snapshot;v.network_code=uint8_t(r.Number(1));v.genesis=r.Hash();
        v.branch_id=uint32_t(r.Number(4));v.activation_height=uint32_t(r.Number(4));v.legacy_epoch_height=uint32_t(r.Number(4));v.leaf_activation_height=uint32_t(r.Number(4));
        v.legacy_state_root=r.Hash();v.tree_root=r.Hash();v.legacy_value=r.Number(8);v.tree_size=r.Number(8);v.nullifier_count=r.Number(8);
        v.stump=r.Blob(9+64*33);c.frontier=r.Blob();c.anchors=r.Blob();c.undo=r.Blob();
        const auto delta=r.Blob();c.delta.assign(delta.begin(),delta.end());const auto count=r.Number(4);
        Require(count==v.nullifier_count&&count<=r.bytes.size()/36,"Historical checkpoint nullifier framing");
        std::set<uint256> unique;
        for(uint64_t n=0;n<count;++n){const auto height=uint32_t(r.Number(4));const auto hash=r.Hash();
            Require(height>=v.legacy_epoch_height&&height<=c.target.height&&unique.insert(hash).second,
                "Historical checkpoint nullifier identity");c.nullifiers.emplace_back(height,hash);}
        Require(r.bytes.empty()&&std::is_sorted(c.nullifiers.begin(),c.nullifiers.end()),"Historical checkpoint terminal bytes");
        Require(v.network_code<=2&&!v.genesis.IsNull()&&v.branch_id&&v.activation_height!=UINT32_MAX&&
            uint64_t(c.target.height)+1<v.activation_height&&!c.target.hash.IsNull()&&!c.target.chainwork.IsZero()&&
            !c.wire_hash.IsNull()&&v.legacy_epoch_height<v.activation_height&&v.legacy_value<=orchard::kMaxMoneyUna&&
            !v.legacy_state_root.IsNull()&&!v.tree_root.IsNull(),"Historical checkpoint domain");
        Require(c.target.height? !c.parent.IsNull() : c.target.hash==v.genesis&&c.parent.IsNull(),"Historical checkpoint ancestry");
        Require(consensus::UtreexoStump::deserialize(v.stump).serialize()==v.stump,"Historical checkpoint stump");
        if(c.target.height){
            Require(!c.undo.empty()&&UndoRecord::Deserialize(c.undo).Serialize()==c.undo,"Historical checkpoint undo");
            consensus::UtreexoDelta parsed;std::string error;
            Require(DeserializeUtreexoDelta(c.delta,parsed,error),"Historical checkpoint delta");
        } else Require(c.undo.empty()&&c.delta.empty(),"Historical genesis checkpoint undo");
        Require(EncodeCheckpoint(c)==CheckpointBytes(bytes.begin(),bytes.end()),"Historical checkpoint canonical bytes");return c;
    }
    void StoreCheckpoint(const Block& block,uint32_t height,const arith_uint256& work) {
        Checkpoint c;c.target={height,block.GetHash(),work};c.parent=block.header.prev_block_hash;
        const auto wire=block.Serialize();crypto::CSHA256().Write(wire).Finalize(c.wire_hash.data);
        c.snapshot=MakeSnapshot(height,c.target.hash,block.vtx.front());
        c.frontier=replay_.ShieldedTree()->SerializeFrontier();c.anchors=replay_.ShieldedAnchors()->SerializePersistenceBytes();
        Require(c.snapshot.nullifier_count<=wallet::detail::RuntimeReplaySpool::MaximumRecordBytes/36&&
            c.snapshot.nullifier_count<=(checkpoint_limit_-checkpoint_material_)/36,
            "Historical checkpoint nullifier material limit");
        Require(replay_.ShieldedNullifiers()->ForEach([&](uint32_t h,const uint8_t* bytes){
            Require(c.nullifiers.size()<c.snapshot.nullifier_count,"Historical checkpoint nullifier excess");
            uint256 id;std::copy_n(bytes,32,id.begin());c.nullifiers.emplace_back(h,id);return true;
        }),"Historical checkpoint nullifier read");std::sort(c.nullifiers.begin(),c.nullifiers.end());
        if(height) {
            const auto& undo=replay_.UndoTail().front().undo;UndoRecord stored;
            for(const auto& e:undo.spent_coins)stored.spent.emplace_back(e.txid,e.vout,e.coin.value.GetUna(),e.coin.scriptPubKey,
                e.coin.isCoinbase,e.coin.height,e.coin.is_confidential,e.coin.commitment);
            for(const auto& tx:block.vtx){Require(tx.vout.size()<=UINT32_MAX,"Historical checkpoint output count");
                for(size_t n=0;n<tx.vout.size();++n)stored.created.emplace_back(tx.GetTxid().AsUint256(),uint32_t(n));}
            stored.pre_block_shielded_frontier=undo.pre_block_shielded_frontier;
            stored.pre_block_shielded_anchors=undo.pre_block_shielded_anchors;stored.pre_reset_shielded_epoch=undo.pre_reset_shielded_epoch;
            c.undo=stored.Serialize();std::string error;
            Require(undo.utreexo_delta&&SerializeUtreexoDelta(*undo.utreexo_delta,c.delta,error),"Historical checkpoint delta missing");
        }
        const auto bytes=EncodeCheckpoint(c);(void)DecodeCheckpoint(bytes);
        Require(bytes.size()<=checkpoint_limit_-checkpoint_material_,"Historical checkpoint material limit");
        // All validator/state reads finished before taking the private spool mutex.
        wallet::detail::RuntimeReplaySpool::Batch batch(*checkpoints_);
        checkpoints_->Insert(CheckpointKey(height),bytes);batch.Commit();
        checkpoint_material_+=bytes.size();++checkpoint_count_;
    }

    const Target target_;
    const Limits limits_;
    const ProfileIdentity profile_;
    const std::thread::id thread_;
    assumeutxo::AssumeUtxoReplayEngine replay_;
    uint32_t epoch_ = 0;
    bool reset_epoch_ = false;
    uint64_t next_height_ = 0;
    size_t material_ = 0;
    uint256 previous_;
    arith_uint256 work_{0};
    consensus::SelectedLegacyPoolAccounting accounting_{};
    Transaction parent_coinbase_;
    Snapshot snapshot_;
    std::unique_ptr<wallet::detail::RuntimeReplaySpool> checkpoints_;
    uint32_t checkpoint_first_=0;
    size_t checkpoint_limit_=0,checkpoint_material_=0;
    uint64_t checkpoint_count_=0;
    bool poisoned_ = false;
    bool finished_ = false;
};

} // namespace dinero
