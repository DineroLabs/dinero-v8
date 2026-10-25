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
        Require(!finished_&&height_<target_height_&&profile_==ValidatedOrchardBlock::CaptureProfile()&&TargetSelected());
        const auto height=height_+1;uint256 expected;uint32_t target=0;
        Require(headers_->GetAncestorHashByHash(target_hash_,height,expected,target)&&target==target_height_&&
            expected==block.Header().GetHash()&&block.Header().prev_block_hash==header_.GetHash());
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
            [&](const uint256& a)->StatusOr<bool>{const auto prefix=Key(2,a);const auto row=spool_.Next(2,prefix);
                return row&&row->first.size()==37&&std::equal(prefix.begin(),prefix.end(),row->first.begin());},
            [&](const uint256& n)->StatusOr<bool>{return spool_.Get(Key(1,n)).has_value();}};
        const auto before=Commitments();
        const auto state=PrepareOrchardStateTransition(*context,state_,coins.Authorizations(),lookups);
        Require(state.Next().pool_balance<=orchard::kMaxMoneyUna-retirement_.retired_value);
        const auto stump=DeriveOrchardCompactBlockStump(block,coins,header_,UtreexoStump::deserialize(catalog_.stump));
        std::unique_ptr<const ValidatedOrchardBlock> verified;
        storage::catalog::State next;
        {
            wallet::detail::RuntimeReplaySpool::Batch batch(spool_);
            for(const auto& nf:state.Nullifiers())spool_.Insert(Key(1,nf),{});
            // Append-only anchor occurrences preserve the spool's immutable
            // MAC record contract. Grouped disk iteration derives exact refs;
            // no history-sized anchor/nullifier map or mutable MAC row exists.
            auto anchor=Key(2,state.Next().anchor);for(unsigned i=4;i;--i)anchor.push_back(uint8_t(height>>(8*(i-1))));
            spool_.Insert(anchor,{});
            const auto sets=Commitments();
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
        state_=state.Next();catalog_=std::move(next);header_=block.Header();height_=height;poisoned_=false;return verified;
    }
    void Finish(){Check();poisoned_=true;Require(!finished_&&height_==target_height_&&header_.GetHash()==target_hash_&&state_.has_value()&&
        profile_==consensus::ValidatedOrchardBlock::CaptureProfile()&&TargetSelected());
        spool_.Freeze();finished_=true;poisoned_=false;}
    uint32_t Height()const{Check();return height_;}
    const storage::OrchardStoredState& State()const{Check();Require(state_.has_value());return *state_;}
    const storage::catalog::State& Catalog()const{Check();return catalog_;}
private:
    friend class OrchardSnapshotStore;
    using Bytes=std::vector<uint8_t>;
    OrchardBoundaryReplay(std::shared_ptr<const consensus::OrchardSnapshotSection> proof,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,uint32_t height,uint256 hash)
        :baseline_(std::move(proof)),headers_(std::move(headers)),target_height_(height),target_hash_(hash),
         profile_(consensus::ValidatedOrchardBlock::CaptureProfile()),
         spool_(baseline_->disk_nodes?baseline_->disk_nodes->WorkDirectory():std::filesystem::path{}),height_(baseline_->parent_catalog.height),
         retirement_(baseline_->retirement.record),catalog_(baseline_->parent_catalog){
        Require(headers_&&target_height_>=baseline_->state.height&&height_+1==baseline_->state.height&&TargetSelected());
        const auto parent=headers_->GetHeaderValue(catalog_.block);Require(parent&&parent->height==height_);header_=parent->header;
        uint256 base;uint32_t target=0;
        Require(headers_->GetAncestorHashByHash(target_hash_,baseline_->state.height,base,target)&&target==target_height_&&base==baseline_->state.block_hash);
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
    storage::OrchardCommitmentSets Commitments()const{
        storage::OrchardCommitmentSets sets;crypto::CSHA256 nf,anchor;
        nf.Write(std::string("ONF1\x01",5));anchor.Write(std::string("OAN1\x01",5));
        for(uint8_t space=1;space<=2;++space){
            std::optional<uint256> group;uint64_t refs=0;
            const auto flush=[&]{if(group){Require(refs&&refs<=UINT64_MAX-sets.anchor_references);
                anchor.Write(group->data,32);Bytes bytes;Number(bytes,refs);anchor.Write(bytes.data(),bytes.size());
                ++sets.anchor_count;sets.anchor_references+=refs;}};
            Bytes after{space};while(const auto row=spool_.Next(space,after)){
                after=row->first;Require(row->second.empty());
                if(space==1){Require(row->first.size()==33);nf.Write(row->first.data()+1,32);++sets.nullifier_count;}
                else{Require(row->first.size()==37);uint256 hash;std::copy_n(row->first.begin()+1,32,hash.begin());
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
    uint32_t height_;storage::LegacyRetirementRecord retirement_;storage::catalog::State catalog_;
    BlockHeader header_;std::optional<storage::OrchardStoredState> state_;bool poisoned_=false,finished_=false;
};
} // namespace dinero
