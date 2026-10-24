#pragma once
#include "daemon/services/orchard_parent_replay.h"
#include "daemon/services/orchard_history_capture.h"
#include "daemon/services/orchard_replay_header_view.h"
#include "consensus/orchard_block_coins.h"
#include "consensus/orchard_candidate_coin_view.h"
#include "consensus/orchard_validated_block.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/orchard_forest_transition.h"
#include "consensus/orchard_header.h"
#include "consensus/orchard_state_root.h"
#include "consensus/orchard_catalog_transition.h"
#include "consensus/orchard_catalog_coin_view.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/orchard_snapshot.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "crypto/sha256.h"
#include <cstring>

namespace dinero {
// Private, single-threaded replay from an independently proved historical
// parent, or (only through the sealed snapshot store) an authenticated v6 base.
// The latter validates the suffix only and never claims pre-base execution.
// No selected database, global block
// index, wallet or external callback supplies coins or membership. The service
// keeps the parent alive and runs this outside selected/wallet ownership.
// A failed append poisons the owner; only Finish exposes a completed result.
class OrchardBranchReplay final {
public:
    using Target=OrchardParentReplay::Target;
    // Bounds the retained Orchard suffix, independently of the activation
    // height. The service also charges serialized parent and suffix material
    // together. This count is a work policy, not a resident-memory guarantee.
    static constexpr uint32_t MaximumBranchBlocks = 100000;
    static constexpr bool TargetWithinWorkPolicy(uint32_t parent_height,
                                                 uint32_t target_height) noexcept {
        return target_height != UINT32_MAX && target_height > parent_height &&
            uint64_t(target_height) - parent_height <= MaximumBranchBlocks;
    }
    OrchardBranchReplay(const OrchardParentReplay& parent,
        const std::vector<BlockHeader>& history, std::set<TxId> transaction_ids, Target target)
        : parent_(&parent), target_(target), profile_(consensus::ValidatedOrchardBlock::CaptureProfile()), coins_(parent.ProvenState().ProvenUtxos()),
          forest_(*parent.ProvenState().Forest()),
          height_(parent.ValidatedTarget().height), work_(parent.ValidatedTarget().chainwork) {
        Require(history.size()==uint64_t(height_)+1 && TargetWithinWorkPolicy(height_,target.height),
            "Branch replay local work policy");
        headers_=std::make_unique<consensus::HeaderChainSelector>();
        uint256 previous;
        for(const auto& header:history) {
            Require(header.prev_block_hash==previous && headers_->AddHeader(header),
                "Branch replay historical header mismatch");
            previous=header.GetHash();
        }
        header_=history.back();
        Require(previous==parent.ValidatedTarget().hash,"Branch replay parent mismatch");
        std::vector<TxId> ids;ids.reserve(256);
        for(const auto& id:transaction_ids) {
            ids.push_back(id);if(ids.size()==256){RecordTransactions(ids);ids.clear();}
        }
        if(!ids.empty())RecordTransactions(ids);
        InitializeCatalog([&](const auto& visitor){for(const auto& id:transaction_ids)visitor(id);});
    }
    // The service owns this immutable capture for longer than the branch. No
    // historical header vector or all-history transaction set is reconstructed.
    OrchardBranchReplay(const OrchardParentReplay& parent,
        const OrchardHistoryCapture& history,Target target)
        : parent_(&parent),target_(target),profile_(consensus::ValidatedOrchardBlock::CaptureProfile()),
          coins_(parent.ProvenState().ProvenUtxos()),forest_(*parent.ProvenState().Forest()),
          history_(&history),height_(parent.ValidatedTarget().height),work_(parent.ValidatedTarget().chainwork) {
        Require(history.Finished() && history.Count()==uint64_t(height_)+1 &&
            TargetWithinWorkPolicy(height_,target.height),"Branch replay local work policy");
        const auto captured=history.HeaderAt(height_);
        Require(captured.hash==parent.ValidatedTarget().hash && captured.work==work_,
            "Branch replay parent mismatch");
        header_=captured.header;
        captured_headers_=std::make_unique<OrchardReplayHeaderView>(history);
        InitializeCatalog([&](const auto& visitor){history.ForEachTransaction(visitor);});
    }
    OrchardBranchReplay(const OrchardBranchReplay&)=delete;
    OrchardBranchReplay& operator=(const OrchardBranchReplay&)=delete;
    void Append(const OrchardBlockCandidate& block,uint32_t height,const arith_uint256& work,uint64_t now) {
        CheckThread(); const bool usable=!poisoned_ && !finished_;poisoned_=true;
        if(parent_)(void)parent_->ValidatedTarget();
        Require(usable && profile_==consensus::ValidatedOrchardBlock::CaptureProfile() &&
            uint64_t(height_)+1==height && height<=target_.height,
            "Branch replay discontinuity");
        const auto context=consensus::SelectedOrchardBlockContext(block.Header(),height);
        Require(bool(context),"Branch replay inactive profile");
        if(captured_headers_)
            consensus::CheckOrchardHeaderUnderChainstateLock(block.Header(),header_,*context,*captured_headers_,now);
        else
            consensus::CheckOrchardHeaderUnderChainstateLock(block.Header(),header_,*context,*headers_,now);
        const auto next_work=work_+GetBlockProof(block.Header().difficulty);
        Require(next_work>work_ && next_work==work,"Branch replay work mismatch");
        std::vector<TxId> ids;ids.reserve(block.Transactions().size());
        for(const auto& tx:block.Transactions())ids.push_back(tx.GetTxid());
        CoinView authenticated_parent(coins_,height_);
        // Retain only candidate inputs and proved output absences for detached
        // validation. The replayed branch supplies exact legacy metadata;
        // membership is queried BEFORE recording this candidate's transaction IDs.
        const auto view=compact_replay_?consensus::CaptureOrchardCatalogCoins(block,*context,header_,*catalog_,CatalogRead()):
            consensus::OrchardCandidateCoinView::Capture(block,*context,header_,
            consensus::UtreexoStump::fromForest(forest_),authenticated_parent,
            [&](const TxId& id)->StatusOr<bool> { return ContainsTransaction(id); });
        RecordTransactions(ids);
        const auto parent_hash=header_.GetHash();const auto parent_height=height_;
        std::map<uint32_t,uint64_t> recorded_mtp;
        consensus::OrchardBranchMtpLookup mtp=[&](uint32_t h)->std::optional<uint64_t> {
            uint256 ancestor;uint32_t selected=0,found=0,time=0;
            if(h>parent_height)return std::nullopt;
            if(captured_headers_) {
                if(!captured_headers_->GetAncestorHashByHash(parent_hash,h,ancestor,selected) ||
                    selected!=parent_height)return std::nullopt;
                time=captured_headers_->MedianTimePast(h);
            } else if(!headers_->GetAncestorHashByHash(parent_hash,h,ancestor,selected) ||
                selected!=parent_height || !headers_->GetMedianTimePastByHash(ancestor,time,found) || found!=h)
                return std::nullopt;
            const auto [it,inserted]=recorded_mtp.emplace(h,time);
            Require(inserted || it->second==time,"Branch replay inconsistent MTP");
            return time;
        };
        const bool witness=Params().enforce_witness_commitment &&
            height>=Params().witness_commitment_enforcement_height;
        auto coins=consensus::PrepareOrchardBlockCoinsUnderChainstateLock(block,*context,view,mtp,witness);
        (void)consensus::CheckOrchardBlockFilter(block,coins);
        consensus::OrchardStateLookups lookups{
            [&](const uint256& a)->StatusOr<bool>{return anchors_.count(a)!=0;},
            [&](const uint256& n)->StatusOr<bool>{return nullifiers_.count(n)!=0;}};
        const auto parent_membership=Commitments();
        uint256 parent_forest_root=header_.utreexo_root;
        if(!compact_replay_) {
            const auto commitment=forest_.getCommitment();
            std::copy(commitment.begin(),commitment.end(),parent_forest_root.begin());
        }
        auto state=consensus::PrepareOrchardStateTransition(*context,state_,coins.Authorizations(),lookups);
        Require(state.Next().pool_balance<=orchard::kMaxMoneyUna-Retirement().retired_value,
            "Branch replay combined pool overflow");
        for(const auto& nf:state.Nullifiers())
            Require(nullifiers_.insert(nf).second,"Branch replay duplicate nullifier");
        auto& references=anchors_[state.Next().anchor];
        Require(references<UINT64_MAX,"Branch replay anchor count overflow");++references;
        const auto sets=Commitments();
        Require(sets.nullifier_count==state.Next().tree_size &&
            sets.anchor_references==uint64_t(height)+1-context->activation_height,
            "Branch replay membership count mismatch");
        const auto root=consensus::ComputeOrchardSelectedStateRoot(
            {context->domain,context->activation_height,height,context->parent_hash},
            Retirement(),state.Next(),sets,Params().orchard_catalog_commitment_activation_height,
            catalog_?&*catalog_:nullptr);
        const auto commitment=consensus::FindStateCommitment(block.Transactions().at(0).Historical(),
            consensus::StateCommitmentEncoding::Orchard);
        Require(commitment.status==consensus::StateCommitmentStatus::Ok && commitment.root==root,
            "Branch replay state commitment mismatch");
        std::optional<consensus::PreparedOrchardForest> forest;
        auto stump=compact_stump_;
        if(compact_replay_)stump=consensus::DeriveOrchardCompactBlockStump(block,coins,header_,compact_stump_);
        else {
            forest.emplace(consensus::PrepareOrchardForestTransition(coins,header_,forest_));
            Require(forest->MatchesHeader(block.Header()),"Branch replay forest commitment mismatch");
            consensus::CheckOrchardBlockUtreexoProof(block,coins,header_,forest_);
            stump=consensus::UtreexoStump::fromForest(forest->After());
        }
        if(catalog_) {
            wallet::detail::RuntimeReplaySpool::Batch batch(*catalog_spool_);
            auto next=consensus::AdvanceOrchardCatalog(*catalog_,coins,
                stump,work,
                consensus::BuildOrchardCoinUndo(coins).Serialize(),CatalogRead(),CatalogWrite());
            batch.Commit();catalog_=std::move(next);
        }
        if(!compact_replay_)for(const auto& change:coins.Changes()) {
            if(change.after) coins_.insert_or_assign(change.outpoint,*change.after);
            else coins_.erase(change.outpoint);
        }
        if(captured_headers_)captured_headers_->AppendHeader(block.Header(),height);
        else Require(headers_->AddHeader(block.Header()),"Branch replay header publication failed");
        // Keep only the per-block transition and its parent bindings, never a
        // full forest snapshot per block. Results remain hidden until Finish.
        auto validated=std::unique_ptr<const consensus::ValidatedOrchardBlock>(
            new consensus::ValidatedOrchardBlock(profile_,block,*context,header_,coins,state,
                Retirement(),parent_membership,parent_forest_root,std::move(recorded_mtp),witness));
        validated_.push_back(std::move(validated));
        if(forest)forest_=forest->After();compact_stump_=std::move(stump);
        state_=state.Next();header_=block.Header();height_=height;work_=work;
        poisoned_=false;
    }
    void Finish() {
        CheckThread();const bool usable=!poisoned_ && !finished_;poisoned_=true;
        if(parent_)(void)parent_->ValidatedTarget();
        Require(usable && profile_==consensus::ValidatedOrchardBlock::CaptureProfile() &&
            validated_.size()==uint64_t(target_.height)-BaseHeight() &&
            height_==target_.height && header_.GetHash()==target_.hash &&
            work_==target_.chainwork,"Branch replay incomplete");
        transactions_spool_.Freeze();
        if(catalog_spool_)catalog_spool_->Freeze();
        if(captured_headers_)captured_headers_->Freeze();
        finished_=true;poisoned_=false;
    }
    const storage::OrchardStoredState& ProvenState()const {
        CheckThread();if(parent_)(void)parent_->ValidatedTarget();
        Require(finished_ && !poisoned_ && state_.has_value() &&
            profile_==consensus::ValidatedOrchardBlock::CaptureProfile(),"Branch replay unavailable");return *state_;
    }
    const consensus::ValidatedOrchardBlock& ProvenBlock(uint32_t height,const uint256& hash)const {
        (void)ProvenState();
        const auto base=BaseHeight();
        Require(height>base && height<=target_.height,"Branch replay block unavailable");
        const auto& result=*validated_.at(size_t(height-base-1));
        Require(result.Context().height==height && result.Context().block_hash==hash,
            "Branch replay block identity mismatch");
        return result;
    }
    storage::OrchardCommitmentSets ProvenCommitments()const { (void)ProvenState();return Commitments(); }
    const storage::catalog::State& ProvenCatalog()const {
        (void)ProvenState();Require(catalog_.has_value(),"Branch replay catalog unavailable");return *catalog_;
    }
    const auto& ProvenUtxos()const {(void)ProvenState();Require(!compact_replay_,"Compact replay has no full coins");return coins_;}
    const consensus::UtreexoForest& ProvenForest()const {(void)ProvenState();Require(!compact_replay_,"Compact replay has no full forest");return forest_;}
private:
    friend class OrchardSnapshotStore;
    // Only the sealed store can seed this owner, after authenticating the
    // durable baseline against real selected headers. This does not declare
    // pre-base history executed and owns no full coin table or forest.
    OrchardBranchReplay(const consensus::OrchardSnapshotSection& baseline,
        const std::vector<BlockHeader>& history,Target target)
        :parent_(nullptr),target_(target),profile_(consensus::ValidatedOrchardBlock::CaptureProfile()),
         height_(baseline.state.height),work_(baseline.base_catalog.work),compact_replay_(true),
         snapshot_height_(baseline.state.height),snapshot_retirement_(baseline.retirement.record) {
        Require(history.size()==uint64_t(height_)+1&&TargetWithinWorkPolicy(height_,target.height),"Snapshot branch replay work policy");
        headers_=std::make_unique<consensus::HeaderChainSelector>();uint256 previous;
        for(const auto& header:history) {
            Require(header.prev_block_hash==previous&&headers_->AddHeader(header),"Snapshot branch replay header ancestry");
            previous=header.GetHash();
        }
        Require(previous==baseline.state.block_hash,"Snapshot branch replay base identity");
        header_=history.back();state_=baseline.state;catalog_=baseline.base_catalog;
        compact_stump_=consensus::UtreexoStump::deserialize(catalog_->stump);
        nullifiers_.insert(baseline.nullifiers.begin(),baseline.nullifiers.end());
        for(const auto& anchor:baseline.anchors)anchors_.emplace(anchor.anchor,anchor.references);
        catalog_spool_=std::make_unique<wallet::detail::RuntimeReplaySpool>();size_t count=0;
        std::unique_ptr<wallet::detail::RuntimeReplaySpool::Batch> batch;
        consensus::ForEachOrchardSnapshotNode(baseline,[&](const auto& id,const auto& bytes) {
            if(!batch)batch=std::make_unique<wallet::detail::RuntimeReplaySpool::Batch>(*catalog_spool_);
            CatalogWrite()(id,bytes);
            if(++count==256){batch->Commit();batch.reset();count=0;}
        });
        if(batch)batch->Commit();
    }
    uint32_t BaseHeight()const {return parent_?parent_->ValidatedTarget().height:snapshot_height_;}
    const storage::LegacyRetirementRecord& Retirement()const {return parent_?parent_->Record():snapshot_retirement_;}
    static std::array<uint8_t,32> CatalogKey(const uint256& id) {
        std::array<uint8_t,32> key;std::copy(id.begin(),id.end(),key.begin());return key;
    }
    storage::catalog::Tree::Read CatalogRead() const {
        return [this](const uint256& id) {
            const auto value=catalog_spool_->Get(CatalogKey(id));
            Require(value.has_value(),"Branch replay missing catalog node");
            return std::string(value->begin(),value->end());
        };
    }
    storage::catalog::Tree::Write CatalogWrite() {
        return [this](const uint256& id,const std::string& bytes) {
            const auto key=CatalogKey(id);const auto prior=catalog_spool_->Get(key);
            if(prior)Require(std::string(prior->begin(),prior->end())==bytes,"Branch replay catalog collision");
            else catalog_spool_->Insert(key,{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});
        };
    }
    template<class Enumerate> void InitializeCatalog(const Enumerate& enumerate) {
        if(Params().orchard_catalog_commitment_activation_height==UINT32_MAX)return;
        namespace c=storage::catalog;
        catalog_spool_=std::make_unique<wallet::detail::RuntimeReplaySpool>();
        c::State s;const auto& r=Retirement();
        s.network=r.network_code;s.genesis=r.genesis;s.branch=r.branch_id;s.activation=r.activation_height;
        s.leaf_activation=consensus::GetUtreexoMaturityLeafActivationHeight();s.height=height_;
        s.block=header_.GetHash();s.parent=header_.prev_block_hash;s.work=work_;
        s.stump=consensus::UtreexoStump::fromForest(forest_).serialize();
        c::Tree transactions(c::Kind::Transactions,CatalogRead(),CatalogWrite());
        // Enumeration performs external historical reads, so each callback
        // opens/closes only its own node batch; no spool locks are nested.
        enumerate([&](const TxId& id) {
            wallet::detail::RuntimeReplaySpool::Batch batch(*catalog_spool_);
            s.transactions=transactions.Insert(s.transactions,c::TransactionKey(id),{});
            Require(s.transaction_count!=UINT64_MAX,"Branch replay catalog count overflow");
            ++s.transaction_count;batch.Commit();return true;
        });
        c::Tree legacy(c::Kind::LegacyCoins,CatalogRead(),CatalogWrite());
        c::Tree special(c::Kind::NonTransparentCoins,CatalogRead(),CatalogWrite());
        for(const auto& [point,coin]:coins_) {
            wallet::detail::RuntimeReplaySpool::Batch batch(*catalog_spool_);
            const auto bytes=c::OutpointBytes(point);
            if(coin.height<s.leaf_activation) {
                auto value=bytes;c::Number(value,coin.height,4);value.push_back(coin.isCoinbase?1:0);
                s.legacy=legacy.Insert(s.legacy,c::LegacyKey(bytes),value);++s.legacy_count;
            }
            if(coin.is_confidential||!coin.commitment.empty()) {
                s.nontransparent=special.Insert(s.nontransparent,c::NonTransparentKey(bytes),bytes);++s.nontransparent_count;
            }
            batch.Commit();
        }
        s.Validate();catalog_=std::move(s);
    }
    friend struct OrchardBranchCaptureTestAccess;
    friend struct OrchardBranchAncestryTestAccess;
    bool ContainsTransaction(const TxId& id) const {
        if(history_ && history_->ContainsTransaction(id)) return true;
        wallet::detail::RuntimeReplayDiskMembership::Key key;
        const auto& hash=id.AsUint256();std::copy_n(hash.data,32,key.begin());
        return transactions_.Contains(transactions_root_,key);
    }
    void RecordTransactions(std::span<const TxId> ids) {
        // The historical owner uses another spool. Finish its checked reads
        // before taking this branch's private transaction; no nested database
        // mutex or external proof/source work is held through the batch.
        if(history_)for(const auto& id:ids)
            Require(!history_->ContainsTransaction(id),"Branch replay historical duplicate transaction");
        auto root=transactions_root_;
        wallet::detail::RuntimeReplaySpool::Batch batch(transactions_spool_);
        for(const auto& id:ids) {
            wallet::detail::RuntimeReplayDiskMembership::Key key;
            const auto& hash=id.AsUint256();std::copy_n(hash.data,32,key.begin());
            Require(!transactions_.Contains(root,key),"Branch replay duplicate transaction");
            root=transactions_.With(root,key);
        }
        batch.Commit();transactions_root_=root;
    }
    static void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
    void CheckThread()const {Require(thread_==std::this_thread::get_id(),"Branch replay wrong thread");}
    struct RawLess {bool operator()(const uint256& a,const uint256& b)const noexcept {
        return std::memcmp(a.data,b.data,32)<0;}};
    using Coins=std::unordered_map<OutPoint,consensus::UTXOEntry>;
    struct CoinView final:consensus::ChainStateView {
        const Coins& coins;uint32_t height;
        CoinView(const Coins& c,uint32_t h):coins(c),height(h){}
        StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& p)const override {
            const auto i=coins.find(p);if(i==coins.end())return Status::NotFound;return i->second;}
        bool hasCoin(const OutPoint& p)const override{return coins.count(p)!=0;}
        uint32_t getHeight()const override{return height;}
    };
    // Canonical logical encoding used by ChainDB::readOrchardCommitmentSets:
    // raw 32-byte lexical order, domain tags, little-endian 64-bit counts.
    // These sets originate only from the fully validated activation prefix.
    storage::OrchardCommitmentSets Commitments()const {
        storage::OrchardCommitmentSets result;crypto::CSHA256 nf,anchors;
        nf.Write(std::string("ONF1\x01",5));anchors.Write(std::string("OAN1\x01",5));
        const auto number=[](crypto::CSHA256& hash,uint64_t n) {
            uint8_t bytes[8];for(unsigned i=0;i<8;++i)bytes[i]=uint8_t(n>>(8*i));hash.Write(bytes,8);};
        for(const auto& n:nullifiers_){nf.Write(n.data,32);++result.nullifier_count;}
        number(nf,result.nullifier_count);nf.Finalize(result.nullifiers.data);
        for(const auto& [a,count]:anchors_) {
            Require(count && count<=UINT64_MAX-result.anchor_references,"Branch replay anchor overflow");
            anchors.Write(a.data,32);number(anchors,count);++result.anchor_count;result.anchor_references+=count;
        }
        number(anchors,result.anchor_count);number(anchors,result.anchor_references);anchors.Finalize(result.anchors.data);
        return result;
    }
    const OrchardParentReplay* parent_;const Target target_;
    const std::string profile_;
    std::vector<std::unique_ptr<const consensus::ValidatedOrchardBlock>> validated_;
    const std::thread::id thread_=std::this_thread::get_id();
    Coins coins_;consensus::UtreexoForest forest_;
    bool compact_replay_=false;uint32_t snapshot_height_=0;
    storage::LegacyRetirementRecord snapshot_retirement_;
    consensus::UtreexoStump compact_stump_;
    // Only the legacy vector fixture path constructs a header selector. The
    // service path borrows its immutable historical capture and spills append
    // headers without recreating the parent replay's complete header index.
    std::unique_ptr<consensus::HeaderChainSelector> headers_;
    std::unique_ptr<OrchardReplayHeaderView> captured_headers_;
    wallet::detail::RuntimeReplaySpool transactions_spool_;
    wallet::detail::RuntimeReplayDiskMembership transactions_{transactions_spool_};
    wallet::detail::RuntimeReplayDiskMembership::Root transactions_root_=0;
    const OrchardHistoryCapture* history_=nullptr;
    std::set<uint256,RawLess> nullifiers_;
    std::map<uint256,uint64_t,RawLess> anchors_;
    std::optional<storage::OrchardStoredState> state_;
    std::unique_ptr<wallet::detail::RuntimeReplaySpool> catalog_spool_;
    std::optional<storage::catalog::State> catalog_;
    BlockHeader header_;uint32_t height_;arith_uint256 work_;bool poisoned_=false,finished_=false;
};
}
