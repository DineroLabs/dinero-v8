#pragma once
#include "consensus/orchard_snapshot.h"
#include "consensus/orchard_validated_block.h"
#include "consensus/orchard_catalog_coin_view.h"
#include "consensus/orchard_catalog_transition.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/orchard_header.h"
#include "consensus/header_chain.h"
#include "consensus/orchard_state_root.h"
#include "wallet/runtime_replay_spool.h"
#include "wallet/runtime_replay_index.h"
#include "wallet/runtime_replay_spool_codec.h"
#include <thread>

namespace dinero {
class OrchardSnapshotStore;
// Streaming private validation from an authenticated activation PARENT.
// Unlike a snapshot-floor suffix, Append validates the first Orchard block.
// Only the sealed store may seed this owner after DNORST02/burial checks.
// Validated prefixes may feed uncertified scan staging; they cannot publish a
// replacement chainstate or certify a wallet before fresh target adoption.
class OrchardBoundaryReplay final {
public:
    OrchardBoundaryReplay(const OrchardBoundaryReplay&)=delete;
    OrchardBoundaryReplay& operator=(const OrchardBoundaryReplay&)=delete;
    std::unique_ptr<const consensus::ValidatedOrchardBlock> Append(const OrchardBlockCandidate& block,uint64_t now){
        using namespace consensus;Check();poisoned_=true;
        Require(!finished_&&height_<uint32_t(INT32_MAX)&&(events_||height_<target_height_)&&
            profile_==ValidatedOrchardBlock::CaptureProfile()&&TargetSelected());
        const auto height=height_+1;uint256 expected;uint32_t target=0;
        if(events_)expected=block.Header().GetHash();
        else Require(headers_->GetAncestorHashByHash(target_hash_,height,expected,target)&&target==target_height_&&expected==block.Header().GetHash());
        Require(block.Header().prev_block_hash==header_.GetHash());
        const auto indexed=headers_->GetHeaderValue(expected);Require(indexed&&indexed->height==height);
        const auto context=SelectedOrchardBlockContext(block.Header(),height);Require(bool(context));
        CheckOrchardHeaderUnderChainstateLock(block.Header(),header_,*context,*headers_,now);
        const auto view=CaptureOrchardCatalogCoins(block,*context,header_,catalog_,Read());
        std::map<uint32_t,uint64_t> times;
        const OrchardBranchMtpLookup mtp=[&](uint32_t h)->std::optional<uint64_t>{
            uint256 hash;uint32_t parent=0,found=0,time=0;
            if(h>height_||!headers_->GetAncestorHashByHash(header_.GetHash(),h,hash,parent)||parent!=height_||
                !headers_->GetMedianTimePastByHash(hash,time,found)||found!=h)return {};
            const auto [it,inserted]=times.emplace(h,time);Require(inserted||it->second==time);return time;
        };
        const bool witness=Params().enforce_witness_commitment&&height>=Params().witness_commitment_enforcement_height;
        const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(block,*context,view,mtp,witness);
        (void)CheckOrchardBlockFilter(block,coins);
        const OrchardStateLookups lookups{
            [&](const uint256& a)->StatusOr<bool>{if(events_)return ActiveContains(2,a);const auto prefix=Key(2,a);const auto row=spool_.Next(2,prefix);
                return row&&row->first.size()==37&&std::equal(prefix.begin(),prefix.end(),row->first.begin());},
            [&](const uint256& n)->StatusOr<bool>{return events_?ActiveContains(1,n):spool_.Get(Key(1,n)).has_value();}};
        const auto before=Commitments();
        if(events_&&state_)Require(LoadCheckpoint(header_.GetHash()).after_sets==before);
        const auto state=PrepareOrchardStateTransition(*context,state_,coins.Authorizations(),lookups);
        Require(state.Next().pool_balance<=orchard::kMaxMoneyUna-retirement_.retired_value);
        const auto stump=DeriveOrchardCompactBlockStump(block,coins,header_,UtreexoStump::deserialize(catalog_.stump));
        std::unique_ptr<const ValidatedOrchardBlock> verified;
        storage::catalog::State next;
        const auto old_checkpoint=events_?checkpoints_.Get(CheckpointKey(expected)):std::nullopt;
        const uint64_t checkpoint_id=old_checkpoint?DecodeCheckpoint(*old_checkpoint).id:next_checkpoint_;
        if(events_)Require(checkpoint_id&&checkpoint_id<UINT64_MAX);
        {
            wallet::detail::RuntimeReplaySpool::Batch batch(spool_);
            for(const auto& nf:state.Nullifiers()){
                if(events_)RecordOccurrence(1,nf,checkpoint_id,height,expected);
                else spool_.Insert(Key(1,nf),{});
            }
            // Append-only anchor occurrences preserve the spool's immutable
            // MAC record contract. Grouped disk iteration derives exact refs;
            // no history-sized anchor/nullifier map or mutable MAC row exists.
            if(events_)RecordOccurrence(2,state.Next().anchor,checkpoint_id,height,expected);
            else {auto anchor=Key(2,state.Next().anchor);for(unsigned i=4;i;--i)anchor.push_back(uint8_t(height>>(8*(i-1))));spool_.Insert(anchor,{});}
            const auto sets=events_?Commitments(expected,height):Commitments();
            Require(sets.nullifier_count==state.Next().tree_size&&sets.anchor_references==uint64_t(height)+1-context->activation_height);
            const auto root=ComputeOrchardSelectedStateRoot(
                {context->domain,context->activation_height,height,context->parent_hash},retirement_,state.Next(),sets,
                Params().orchard_catalog_commitment_activation_height,&catalog_);
            const auto commitment=FindStateCommitment(block.Transactions().at(0).Historical(),StateCommitmentEncoding::Orchard);
            Require(commitment.status==StateCommitmentStatus::Ok&&commitment.root==root);
            next=AdvanceOrchardCatalog(catalog_,coins,stump,indexed->chainwork,BuildOrchardCoinUndo(coins).Serialize(),Read(),
                [&](const uint256& id,const std::string& bytes){
                    Require(storage::catalog::ValidNode(id,bytes));const auto key=Key(3,id);
                    if(const auto old=spool_.Get(key))Require(std::string(old->begin(),old->end())==bytes);
                    else spool_.Insert(key,{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});
                });
            if(height==baseline_->state.height)Require(next.Encode()==baseline_->base_catalog.Encode()&&state.Next()==baseline_->state);
            verified.reset(new ValidatedOrchardBlock(profile_,block,*context,header_,coins,state,retirement_,before,
                header_.utreexo_root,std::move(times),witness));
            batch.Commit();
        }
        if(events_){
            Checkpoint captured;captured.id=checkpoint_id;captured.wire=WireHash(block);
            captured.before=state_;captured.after=state.Next();captured.parent=catalog_;captured.catalog=next;
            captured.before_sets=before;captured.after_sets=Commitments(expected,height);
            captured.undo=BuildOrchardCoinUndo(coins).Serialize();captured.mtp=verified->BranchMtp();
            const auto encoded=EncodeCheckpoint(captured);
            if(old_checkpoint)Require(*old_checkpoint==encoded);
            else {checkpoints_.Set(CheckpointKey(expected),encoded);++next_checkpoint_;}
        }
        state_=state.Next();catalog_=std::move(next);header_=block.Header();height_=height;poisoned_=false;return verified;
    }
    void Finish(){Check();poisoned_=true;Require(!finished_&&height_==target_height_&&header_.GetHash()==target_hash_&&(events_||state_.has_value())&&
        profile_==consensus::ValidatedOrchardBlock::CaptureProfile()&&TargetSelected());
        checkpoints_.Freeze();spool_.Freeze();finished_=true;poisoned_=false;}
    uint32_t Height()const{Check();return height_;}
    const storage::OrchardStoredState& State()const{Check();Require(state_.has_value());return *state_;}
    const storage::catalog::State& Catalog()const{Check();return catalog_;}
private:
    friend class OrchardSnapshotStore;
    using Bytes=std::vector<uint8_t>;
    OrchardBoundaryReplay(std::shared_ptr<const consensus::OrchardSnapshotSection> proof,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,uint32_t height,uint256 hash,bool events=false)
        :baseline_(std::move(proof)),headers_(std::move(headers)),target_height_(height),target_hash_(hash),
         profile_(consensus::ValidatedOrchardBlock::CaptureProfile()),
         spool_(baseline_->disk_nodes?baseline_->disk_nodes->WorkDirectory():std::filesystem::path{}),checkpoints_(spool_),events_(events),height_(baseline_->parent_catalog.height),
         retirement_(baseline_->retirement.record),catalog_(baseline_->parent_catalog){
        Require(headers_&&target_height_>=(events_?baseline_->parent_catalog.height:baseline_->state.height)&&height_+1==baseline_->state.height&&TargetSelected());
        const auto parent=headers_->GetHeaderValue(catalog_.block);Require(parent&&parent->height==height_);header_=parent->header;
        if(target_height_>=baseline_->state.height){uint256 base;uint32_t target=0;
            Require(headers_->GetAncestorHashByHash(target_hash_,baseline_->state.height,base,target)&&target==target_height_&&base==baseline_->state.block_hash);}
    }
    // Event mode is private DATA audit only. Checkpoint metadata is captured
    // solely from real Append validation and bound by an owned content-index
    // root, independently of the spool MAC. No raw row can mint a seal/owner.
    struct Checkpoint {
        uint64_t id=0;uint256 wire;std::optional<storage::OrchardStoredState> before;storage::OrchardStoredState after;
        storage::catalog::State parent,catalog;storage::OrchardCommitmentSets before_sets,after_sets;
        std::vector<uint8_t> undo;std::map<uint32_t,uint64_t> mtp;
    };
    static wallet::detail::RuntimeReplayIndex::Key CheckpointKey(const uint256& hash){wallet::detail::RuntimeReplayIndex::Key key{5};std::copy(hash.begin(),hash.end(),key.begin()+1);return key;}
    static uint256 WireHash(const OrchardBlockCandidate& block){return storage::catalog::Hash(std::string(block.WireBytes().begin(),block.WireBytes().end()));}
    static Bytes EncodeCheckpoint(const Checkpoint& c){
        wallet::detail::replay_spool_codec::Writer w;w.Number(c.id,8);w.Hash(c.wire);w.Number(bool(c.before),1);
        if(c.before)w.State(*c.before);w.State(c.after);
        for(const auto& state:{c.parent,c.catalog}){const auto bytes=state.Encode();w.Blob({reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});}
        for(const auto& s:{c.before_sets,c.after_sets}){w.Hash(s.nullifiers);w.Number(s.nullifier_count,8);w.Hash(s.anchors);w.Number(s.anchor_count,8);w.Number(s.anchor_references,8);}
        w.Blob(c.undo);w.Number(c.mtp.size(),4);for(const auto& [h,t]:c.mtp){w.Number(h,4);w.Number(t,8);}return std::move(w.bytes);
    }
    static Checkpoint DecodeCheckpoint(std::span<const uint8_t> bytes){
        wallet::detail::replay_spool_codec::Reader r{bytes};Checkpoint c;c.id=r.Number(8);c.wire=r.Hash();const auto has=r.Number(1);Require(has<=1);
        if(has)c.before=r.State();c.after=r.State();
        for(auto* state:{&c.parent,&c.catalog}){const auto raw=r.Blob();*state=storage::catalog::State::Decode(std::string(raw.begin(),raw.end()));}
        for(auto* s:{&c.before_sets,&c.after_sets}){s->nullifiers=r.Hash();s->nullifier_count=r.Number(8);s->anchors=r.Hash();s->anchor_count=r.Number(8);s->anchor_references=r.Number(8);}
        c.undo=r.Blob();const auto count=r.Number(4);Require(count<=r.bytes.size()/12);
        for(uint64_t i=0;i<count;++i){const auto h=uint32_t(r.Number(4));const auto t=r.Number(8);Require(c.mtp.emplace(h,t).second);}
        Require(r.bytes.empty()&&c.id&&c.id<UINT64_MAX);return c;
    }
    Checkpoint LoadCheckpoint(const uint256& hash)const{const auto row=checkpoints_.Get(CheckpointKey(hash));Require(bool(row));return DecodeCheckpoint(*row);}
    RuntimeOrchardReplay Rewind(const OrchardBlockCandidate& block){
        Check();poisoned_=true;Require(events_&&!finished_&&height_>=baseline_->state.height&&header_.GetHash()==block.Header().GetHash()&&TargetSelected());
        const auto c=LoadCheckpoint(header_.GetHash());Require(c.wire==WireHash(block)&&state_==std::optional(c.after)&&catalog_.Encode()==c.catalog.Encode()&&Commitments()==c.after_sets);
        const auto parent=headers_->GetHeaderValue(c.parent.block);Require(parent&&parent->height+1==height_&&parent->header.GetHash()==block.Header().prev_block_hash);
        state_=c.before;catalog_=c.parent;header_=parent->header;--height_;
        Require(Commitments()==c.before_sets);poisoned_=false;return {c.before,c.after,c.undo,c.mtp};
    }
    bool ActiveOwner(std::span<const uint8_t> bytes,const uint256& tip,uint32_t height)const{
        wallet::detail::replay_spool_codec::Reader r{bytes};const auto h=uint32_t(r.Number(4));const auto hash=r.Hash();Require(r.bytes.empty()&&h>=baseline_->state.height);
        if(h>height)return false;uint256 owner;uint32_t found=0;
        Require(headers_->GetAncestorHashByHash(tip,h,owner,found)&&found==height);return owner==hash;
    }
    bool ActiveContains(uint8_t space,const uint256& hash)const{
        const auto prefix=Key(space,hash);auto after=prefix;
        while(const auto row=spool_.Next(space,after)){
            Require(row->first.size()==41);
            if(!std::equal(prefix.begin(),prefix.end(),row->first.begin()))return false;
            if(ActiveOwner(row->second,header_.GetHash(),height_))return true;after=row->first;
        }return false;
    }
    void RecordOccurrence(uint8_t space,const uint256& hash,uint64_t id,uint32_t height,const uint256& owner){
        auto key=Key(space,hash);for(unsigned i=8;i;--i)key.push_back(uint8_t(id>>(8*(i-1))));
        wallet::detail::replay_spool_codec::Writer w;w.Number(height,4);w.Hash(owner);
        if(const auto old=spool_.Get(key))Require(*old==w.bytes);else spool_.Insert(key,w.bytes);
    }
    static void Require(bool ok){if(!ok)throw std::runtime_error("Orchard activation-origin replay refused");}
    void Check()const{Require(!poisoned_&&thread_==std::this_thread::get_id());}
    bool TargetSelected()const{consensus::HeaderIndexEntry target;return headers_&&
        headers_->GetHeaderAtHeightCopy(target_height_,target)&&target.hash==target_hash_;}
    static Bytes Key(uint8_t space,const uint256& hash){Bytes bytes{space};bytes.insert(bytes.end(),hash.begin(),hash.end());return bytes;}
    static void Number(Bytes& bytes,uint64_t n){for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(n>>(8*i)));}
    storage::catalog::Tree::Read Read()const{return [this](const uint256& id){
        if(const auto value=spool_.Get(Key(3,id)))return std::string(value->begin(),value->end());
        return consensus::ReadOrchardSnapshotNode(*baseline_,id);
    };}
    storage::OrchardCommitmentSets Commitments(const uint256& event_tip={},uint32_t event_height=0)const{
        storage::OrchardCommitmentSets sets;crypto::CSHA256 nf,anchor;
        nf.Write(std::string("ONF1\x01",5));anchor.Write(std::string("OAN1\x01",5));
        for(uint8_t space=1;space<=2;++space){
            std::optional<uint256> group;uint64_t refs=0;
            const auto flush=[&]{if(group){Require(refs&&refs<=UINT64_MAX-sets.anchor_references);
                anchor.Write(group->data,32);Bytes bytes;Number(bytes,refs);anchor.Write(bytes.data(),bytes.size());
                ++sets.anchor_count;sets.anchor_references+=refs;}};
            Bytes after{space};while(const auto row=spool_.Next(space,after)){
                after=row->first;
                if(events_){Require(row->first.size()==41);if(!ActiveOwner(row->second,event_tip.IsNull()?header_.GetHash():event_tip,event_tip.IsNull()?height_:event_height))continue;}
                else Require(row->second.empty());
                if(space==1){Require(row->first.size()==(events_?41u:33u));uint256 hash;std::copy_n(row->first.begin()+1,32,hash.begin());
                    Require(!group||*group!=hash);group=hash;nf.Write(hash.data,32);++sets.nullifier_count;}
                else{Require(row->first.size()==(events_?41u:37u));uint256 hash;std::copy_n(row->first.begin()+1,32,hash.begin());
                    if(!group||*group!=hash){flush();group=hash;refs=0;}Require(refs<UINT64_MAX);++refs;}
            }
            if(space==2)flush();
        }
        Bytes n;Number(n,sets.nullifier_count);nf.Write(n.data(),n.size());nf.Finalize(sets.nullifiers.data);
        n.clear();Number(n,sets.anchor_count);Number(n,sets.anchor_references);anchor.Write(n.data(),n.size());anchor.Finalize(sets.anchors.data);return sets;
    }
    const std::shared_ptr<const consensus::OrchardSnapshotSection> baseline_;
    const std::shared_ptr<const consensus::HeaderChainSelector> headers_;
    const uint32_t target_height_;const uint256 target_hash_;const std::string profile_;
    const std::thread::id thread_=std::this_thread::get_id();
    wallet::detail::RuntimeReplaySpool spool_;
    wallet::detail::RuntimeReplayIndex checkpoints_;const bool events_;uint64_t next_checkpoint_=1;
    uint32_t height_;storage::LegacyRetirementRecord retirement_;storage::catalog::State catalog_;
    BlockHeader header_;std::optional<storage::OrchardStoredState> state_;bool poisoned_=false,finished_=false;
};
} // namespace dinero
