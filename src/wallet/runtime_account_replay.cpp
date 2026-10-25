#include "wallet/runtime_account_replay.h"
#include "wallet/runtime_replay_disk_membership.h"
#include "wallet/runtime_replay_spool_codec.h"
#include "wallet/runtime_replay_state_retention.h"
#include "wallet/runtime_replay_authorization_retention.h"
#include "wallet/runtime_replay_index.h"
#include "consensus/undo.h"
#include "primitives/block.h"
#include <set>
#include <list>
#include <mutex>

namespace dinero {
namespace {
using namespace consensus;
using Point=wallet::OrchardAccountDelivery::RestorePoint;
void Require(bool value) { if(!value) throw OrchardStateLookupError(Status::Corruption); }
uint256 Hash(const orchard::Hash& bytes) { uint256 h;std::copy(bytes.begin(),bytes.end(),h.begin());return h; }
storage::OrchardStoredState Empty(uint32_t height,const uint256& hash) {
    const auto f=orchard::OrchardFrontier::Empty();
    return {height,hash,Hash(f.Root()),0,0,std::string(f.Bytes().begin(),f.Bytes().end())};
}
bool Domain(const orchard::SigningDomain& a,const orchard::SigningDomain& b) {
    return a.network_code==b.network_code&&a.genesis_wire==b.genesis_wire&&a.branch_id==b.branch_id;
}
// Only retained consumed parent coins and earlier in-block creations exist in
// this view. It is sufficient to reverify transaction authorizations, never to
// certify global unspentness, output non-collision or a historical baseline.
class Inputs final:public ChainStateView {
public:
    uint32_t height;
    std::map<OutPoint,UTXOEntry> coins;
    explicit Inputs(uint32_t h):height(h){}
    StatusOr<UTXOEntry> getCoin(const OutPoint& p) const override {
        auto i=coins.find(p);return i==coins.end()?StatusOr<UTXOEntry>(Status::NotFound):StatusOr<UTXOEntry>(i->second);
    }
    bool hasCoin(const OutPoint& p) const override{return coins.contains(p);}
    uint32_t getHeight() const override{return height;}
};
std::vector<VerifiedOrchardAuthorizations> Authorize(const RuntimeOutboxEvent& e,const OrchardBlockCandidate& block) {
    const auto& frame=*e.orchard_replay;
    const auto undo=UndoRecord::Deserialize(frame.coin_undo);
    Require(undo.Serialize()==frame.coin_undo&&!undo.pre_block_shielded_frontier&&
        !undo.pre_block_shielded_anchors&&!undo.pre_reset_shielded_epoch);
    Inputs view(e.context.height-1);std::set<OutPoint> remaining,spent,created;
    for(const auto& coin:undo.spent) {
        OutPoint p(TxId(coin.prev_txid),coin.prev_vout);
        Require(coin.height<e.context.height&&coin.value<=orchard::kMaxMoneyUna&&
            !coin.is_confidential&&coin.commitment.empty()&&remaining.insert(p).second);
        view.coins.emplace(p,UTXOEntry(AmountUna::Una(coin.value),coin.scriptPubKey,coin.height,coin.is_coinbase));
    }
    std::vector<VerifiedOrchardAuthorizations> auths;std::set<TxId> ids;
    for(size_t ordinal=0;ordinal<block.Transactions().size();++ordinal) {
        const auto& tx=block.Transactions()[ordinal];const auto id=tx.GetTxid();Require(ids.insert(id).second);
        std::vector<OutPoint> inputs;std::vector<UTXOEntry> outputs;
        if(tx.IsOrchard()) {
            for(const auto& input:tx.Orchard().Inputs())inputs.emplace_back(TxId(Hash(input.txid_wire)),input.output_index);
            const auto snapshot=OrchardCoinSnapshot::ResolveUnderChainstateLock(tx.Orchard(),view);
            auths.push_back(VerifyOrchardAuthorizations(snapshot,e.context.domain,e.context.height,
                [&frame](uint32_t height)->std::optional<uint64_t>{
                    const auto it=frame.branch_mtp.find(height);
                    if(it==frame.branch_mtp.end())return std::nullopt;
                    return it->second;
                }));
            for(const auto& out:tx.Orchard().Outputs())
                outputs.emplace_back(AmountUna::Una(out.amount_una),out.script_pub_key,e.context.height,false);
        } else {
            const auto& old=tx.Historical();
            Require(old.IsCoinbase()==(ordinal==0)&&!Transaction::IsShieldedVersion(old.version)&&old.shielded_bundle_bytes.empty());
            if(ordinal)for(const auto& input:old.vin)inputs.emplace_back(input.prevout.txid,input.prevout.vout);
            for(const auto& out:old.vout){
                Require(!out.is_confidential&&out.commitment.empty()&&out.value.GetUna()<=orchard::kMaxMoneyUna);
                outputs.emplace_back(out.value,out.scriptPubKey,e.context.height,ordinal==0);
            }
        }
        for(const auto& p:inputs){Require(spent.insert(p).second&&view.coins.erase(p)==1);remaining.erase(p);}
        for(size_t i=0;i<outputs.size();++i){
            const OutPoint p(id,uint32_t(i));
            Require(!spent.contains(p)&&created.insert(p).second&&view.coins.emplace(p,outputs[i]).second);
        }
    }
    Require(remaining.empty());std::set<OutPoint> retained;
    for(const auto& out:undo.created)Require(retained.emplace(TxId(out.txid),out.vout).second);
    for(const auto& p:spent)created.erase(p);
    Require(retained==created);
    return auths;
}
}
struct RuntimeAccountReplay::Data {
    struct Node {
        uint32_t height=0;uint256 parent;
        // Locator of the already verified body/frame in the immutable spool.
        // Owner/content-bound authorization and prepared-state tokens remain below.
        uint64_t event_sequence=0;
        bool historical=false;
        std::optional<wallet::detail::RuntimeReplayStateRetention::Token> state;
        std::optional<wallet::detail::RuntimeReplayAuthorizationRetention::Token> auths;
        storage::OrchardStoredState checkpoint;
        wallet::detail::RuntimeReplayDiskMembership::Root nullifiers=0,anchors=0;
    };
    RuntimeOutboxCursor head;uint32_t activation=0;orchard::SigningDomain domain;
    uint256 origin;
    wallet::detail::RuntimeReplaySpool spool;
    wallet::detail::RuntimeReplayStateRetention states;
    wallet::detail::RuntimeReplayAuthorizationRetention authorizations;
    wallet::detail::RuntimeReplayDiskMembership membership{spool};
    wallet::detail::RuntimeReplayIndex index{spool};
    // A small owned cache preserves recent handles without retaining every
    // raw event. The byte charge is encoded material, not a total RSS bound.
    static constexpr size_t EventCacheEntries=4;
    static constexpr size_t EventCacheBytes=wallet::detail::RuntimeReplaySpool::MaximumRecordBytes;
    mutable std::mutex event_mutex;
    mutable std::list<std::pair<EventHandle,size_t>> event_cache;
    mutable size_t event_cache_bytes=0;
    EventHandle ReadEvent(uint64_t sequence) const {
        Require(sequence&&sequence<=head.sequence);
        // This private lock never spans source callbacks or proof work. No
        // path takes event_mutex while holding the spool's private mutex.
        std::lock_guard guard(event_mutex);
        for(auto it=event_cache.begin();it!=event_cache.end();++it) {
            if(it->first->cursor.sequence!=sequence)continue;
            const auto result=it->first;event_cache.splice(event_cache.begin(),event_cache,it);return result;
        }
        const auto bytes=spool.Get(wallet::detail::replay_spool_codec::EventKey(sequence));Require(bytes.has_value());
        auto value=wallet::detail::replay_spool_codec::Decode(*bytes);Require(value.cursor.sequence==sequence);
        auto result=std::make_shared<const RuntimeOutboxEvent>(std::move(value));
        event_cache.emplace_front(result,bytes->size());event_cache_bytes+=bytes->size();
        while(event_cache.size()>EventCacheEntries||event_cache_bytes>EventCacheBytes) {
            event_cache_bytes-=event_cache.back().second;event_cache.pop_back();
        }
        return result;
    }
    struct Body {
        BlockHandle orchard;
        std::shared_ptr<const dinero::Block> historical;
    };
    struct CachedBody {uint64_t sequence;Body value;size_t charge;};
    static constexpr size_t BodyCacheEntries=4;
    static constexpr size_t BodyCacheBytes=wallet::detail::RuntimeReplaySpool::MaximumRecordBytes;
    mutable std::mutex body_mutex;
    mutable std::list<CachedBody> body_cache;
    mutable size_t body_cache_bytes=0;
    Body ReadBody(const Node& node) const {
        Require(node.event_sequence&&(bool(node.state)!=node.historical));
        // Lock order: body cache -> event cache -> private spool. No callback,
        // proof verification, or wallet/selected-chain acquisition here.
        std::lock_guard guard(body_mutex);
        for(auto it=body_cache.begin();it!=body_cache.end();++it) {
            if(it->sequence!=node.event_sequence)continue;
            const auto value=it->value;body_cache.splice(body_cache.begin(),body_cache,it);return value;
        }
        const auto event=ReadEvent(node.event_sequence);
        Require(event->context.height==node.height&&event->context.parent_hash==node.parent&&
            event->context.block_hash==node.checkpoint.block_hash&&event->IsOrchardProfile()==bool(node.state));
        Body value;
        if(node.state) {
            value.orchard=std::make_shared<const OrchardBlockCandidate>(OrchardBlockCandidate::DecodeExact(event->body));
            Require(value.orchard->Header().GetHash()==node.checkpoint.block_hash);
        } else {
            auto decoded=dinero::Block::Deserialize(event->body);Require(decoded.has_value());
            Require(decoded->Serialize()==std::string(event->body.begin(),event->body.end())&&
                decoded->GetHash()==node.checkpoint.block_hash);
            value.historical=std::make_shared<const dinero::Block>(std::move(*decoded));
        }
        // Charge serialized bytes, not total resident memory. Returned shared
        // handles own their bodies even after this bounded cache evicts them.
        const size_t charge=event->body.size();
        body_cache.push_front({node.event_sequence,value,charge});body_cache_bytes+=charge;
        while(body_cache.size()>BodyCacheEntries||body_cache_bytes>BodyCacheBytes) {
            body_cache_bytes-=body_cache.back().charge;body_cache.pop_back();
        }
        return value;
    }
    using IndexKey=wallet::detail::RuntimeReplayIndex::Key;
    static IndexKey NodeKey(const uint256& hash){IndexKey key{'n'};std::copy(hash.begin(),hash.end(),key.begin()+1);return key;}
    static IndexKey PositionKey(uint64_t sequence){IndexKey key{'p'};for(unsigned i=0;i<8;++i)key[i+1]=uint8_t(sequence>>(8*i));return key;}
    std::optional<Node> Find(const uint256& hash) const {
        const auto bytes=index.Get(NodeKey(hash));if(!bytes)return std::nullopt;
        wallet::detail::replay_spool_codec::Reader r{*bytes};Require(r.Number(1)==1);
        Node n;n.height=r.Number(4);n.parent=r.Hash();n.event_sequence=r.Number(8);
        const auto historical=r.Number(1),has_state=r.Number(1);Require(historical<=1&&has_state<=1);n.historical=historical;
        if(has_state)n.state=states.IndexToken(r.Raw(40));
        const auto has_auths=r.Number(1);Require(has_auths<=1);if(has_auths)n.auths=authorizations.IndexToken(r.Raw(40));
        n.checkpoint=r.State();n.nullifiers=r.Number(8);n.anchors=r.Number(8);
        Require(r.bytes.empty()&&n.checkpoint.block_hash==hash&&n.height==n.checkpoint.height);return n;
    }
    Node At(const uint256& hash) const {auto n=Find(hash);Require(n.has_value());return std::move(*n);}
    void Put(const uint256& hash,const Node& n){
        Require(n.checkpoint.block_hash==hash&&n.height==n.checkpoint.height);
        wallet::detail::replay_spool_codec::Writer w;w.Number(1,1);w.Number(n.height,4);w.Hash(n.parent);w.Number(n.event_sequence,8);
        w.Number(n.historical,1);w.Number(n.state.has_value(),1);if(n.state)w.Raw(states.IndexToken(*n.state));
        w.Number(n.auths.has_value(),1);if(n.auths)w.Raw(authorizations.IndexToken(*n.auths));
        w.State(n.checkpoint);w.Number(n.nullifiers,8);w.Number(n.anchors,8);index.Set(NodeKey(hash),w.bytes);
    }
    uint256 Position(uint64_t sequence) const {
        const auto bytes=index.Get(PositionKey(sequence));Require(bytes&&bytes->size()==32);
        uint256 hash;std::copy(bytes->begin(),bytes->end(),hash.begin());return hash;
    }
    void Position(uint64_t sequence,const uint256& hash){index.Set(PositionKey(sequence),{hash.data,32});}
    Node Ancestor(uint256 tip,uint32_t height,const uint256& hash) const {
        for(;;){const auto& n=At(tip);Require(n.height>=height);if(n.height==height){Require(tip==hash);return n;}tip=n.parent;}
    }
    static wallet::detail::RuntimeReplayDiskMembership::Key MembershipKey(const uint256& hash) {
        wallet::detail::RuntimeReplayDiskMembership::Key key;
        std::copy(hash.data,hash.data+32,key.begin());return key;
    }
    bool Nullifier(uint256 tip,const uint256& nf) const {
        const auto& n=At(tip);if(n.height<activation)return false;Require(bool(n.state));
        return membership.Contains(n.nullifiers,MembershipKey(nf));
    }
    bool Anchor(uint256 tip,const uint256& anchor) const {
        const auto& n=At(tip);if(n.height<activation)return false;Require(bool(n.state));
        return membership.Contains(n.anchors,MembershipKey(anchor));
    }
};
std::shared_ptr<const RuntimeAccountReplay> RuntimeAccountReplay::Capture(const Source& source) {
    return Build(ReadSource(source));
}
std::shared_ptr<RuntimeAccountReplay::Data> RuntimeAccountReplay::ReadSource(const Source& source) {
    // Stream bounded pages into private disk storage. Total history is not a
    // material/record ceiling: disk/allocation/read failures still fail closed,
    // and EOF is accepted only at the original captured, source-checked head.
    auto data=std::make_shared<Data>();RuntimeOutboxCursor after;
    for(;;){
        auto page=source(after,128);
        if(!after.sequence){data->head=page.head;Require(data->head.sequence&&data->head.sequence<UINT64_MAX);}
        Require(page.head==data->head&&!page.events.empty()&&page.events.size()<=128);
        // The source callback returned before this private transaction starts.
        // No selected-chain access or external callback under this batch.
        wallet::detail::RuntimeReplaySpool::Batch batch(data->spool);
        for(auto& e:page.events){
            Require(e.cursor.sequence==after.sequence+1&&e.previous_digest==after.digest&&e.cursor.sequence<=data->head.sequence);
            after=e.cursor;
            data->spool.Insert(wallet::detail::replay_spool_codec::EventKey(e.cursor.sequence),
                               wallet::detail::replay_spool_codec::Encode(e));
        }
        Require(page.next==after);batch.Commit();if(after==data->head)break;
    }
    const auto last_handle=data->ReadEvent(data->head.sequence);const auto& last=*last_handle;const bool connect=last.direction==RuntimeBlockDirection::Connect;
    const auto final=source(data->head,1);Require(final.head==data->head&&final.events.empty()&&final.after_tip&&
        final.after_tip->first==(connect?last.context.block_hash:last.context.parent_hash)&&
        final.after_tip->second==(connect?last.context.height:last.context.height-1));
    return data;
}
std::shared_ptr<const RuntimeAccountReplay> RuntimeAccountReplay::Build(std::shared_ptr<Data> data) {
    // No source callbacks or selected-chain access beyond this point. Expensive
    // authorization verification uses owned immutable material after the
    // service releases its activation lock.
    const auto first_handle=data->ReadEvent(1);const auto& first=*first_handle;data->activation=first.context.activation_height;data->domain=first.context.domain;
    Require(data->activation>0&&data->activation!=UINT32_MAX&&first.direction==RuntimeBlockDirection::Connect&&first.context.height==data->activation&&first.orchard_replay&&
        !first.orchard_replay->parent);
    data->origin=first.context.parent_hash;data->Position(0,data->origin);
    Data::Node root;root.height=data->activation-1;root.checkpoint=Empty(root.height,data->origin);data->Put(data->origin,root);
    for(uint64_t sequence=1;sequence<=data->head.sequence;++sequence){
        const auto event=data->ReadEvent(sequence);const auto& e=*event;
        Require(e.context.height>0&&e.context.activation_height==data->activation&&Domain(e.context.domain,data->domain)&&
            (e.direction==RuntimeBlockDirection::Connect||e.direction==RuntimeBlockDirection::Disconnect));
        const bool connect=e.direction==RuntimeBlockDirection::Connect;const auto before=data->Position(sequence-1);
        Require(before==(connect?e.context.parent_hash:e.context.block_hash));
        Require(data->At(before).height==(connect?e.context.height-1:e.context.height));
        if(e.IsOrchardProfile()){
            Require(e.orchard_replay.has_value());const auto& frame=*e.orchard_replay;
            if(connect){
                const auto& parent=data->At(before);
                Require((e.context.height==data->activation&&!frame.parent)||
                    (frame.parent&&*frame.parent==parent.checkpoint));
                auto block=std::make_shared<const OrchardBlockCandidate>(OrchardBlockCandidate::DecodeExact(e.body));
                std::string error;Require(block->Header().GetHash()==e.context.block_hash&&block->Header().prev_block_hash==before&&
                    block->CheckSizeLimits(error)&&block->CheckIdentityCommitments(true,error));
                auto auths=Authorize(e,*block);
                OrchardStateLookups lookups{
                    [&data,before](const uint256& a)->StatusOr<bool>{return data->Anchor(before,a);},
                    [&data,before](const uint256& n)->StatusOr<bool>{return data->Nullifier(before,n);}};
                auto state=std::make_shared<const PreparedOrchardState>(PrepareOrchardStateTransition(e.context,frame.parent,auths,lookups));
                Require(state->Next()==frame.next);
                Data::Node node;node.height=e.context.height;node.parent=e.context.parent_hash;node.event_sequence=sequence;
                node.state=data->states.Capture(*state);node.auths=data->authorizations.Capture(auths);node.checkpoint=frame.next;
                // Index only the sealed, reverified state above. The parent
                // snapshot belongs to this exact branch; a sibling retains
                // its own immutable roots and never receives these inserts.
                Require(parent.height<data->activation || bool(parent.state));
                // Persist branch membership only AFTER genuine authorizations
                // and the sealed next-state comparison above. Retention
                // restores original typed content under owner-bound tokens;
                // no proof is lazily reverified under wallet locks.
                {
                    wallet::detail::RuntimeReplaySpool::Batch batch(data->spool);
                    node.nullifiers=parent.nullifiers;
                    for(const auto& nf:state->Nullifiers())node.nullifiers=data->membership.With(node.nullifiers,Data::MembershipKey(nf));
                    node.anchors=data->membership.With(parent.anchors,Data::MembershipKey(node.checkpoint.anchor));
                    batch.Commit();
                }
                // The private spool batch has released its lock before any
                // event-cache lookup (which itself reads the spool).
                const auto old=data->Find(e.context.block_hash);
                if(old) {
                    const auto previous=data->ReadEvent(old->event_sequence);
                    Require(old->state&&previous->body==e.body&&old->checkpoint==node.checkpoint&&
                        previous->orchard_replay&&previous->orchard_replay->coin_undo==frame.coin_undo&&
                        previous->orchard_replay->branch_mtp==frame.branch_mtp);
                } else data->Put(e.context.block_hash,node);
            }else{
                const auto& node=data->At(before);const auto previous=data->ReadEvent(node.event_sequence);
                Require(node.state&&previous->body==e.body&&node.checkpoint==frame.next&&data->states.Load(*node.state)->Parent()==frame.parent&&
                    previous->orchard_replay&&previous->orchard_replay->coin_undo==frame.coin_undo&&
                    previous->orchard_replay->branch_mtp==frame.branch_mtp);
            }
        }else{
            auto decoded=dinero::Block::Deserialize(e.body);Require(decoded.has_value());
            const std::string wire(e.body.begin(),e.body.end());Require(decoded->Serialize()==wire&&decoded->GetHash()==e.context.block_hash&&decoded->header.prev_block_hash==e.context.parent_hash);
            const auto old=data->Find(e.context.block_hash);auto node=old.value_or(Data::Node{});Require(!node.state);
            if(node.historical)Require(data->ReadEvent(node.event_sequence)->body==e.body);
            node.height=e.context.height;node.parent=e.context.parent_hash;node.historical=true;
            if(!node.event_sequence)node.event_sequence=sequence;
            node.checkpoint=Empty(node.height,e.context.block_hash);
            if(!data->Find(node.parent)){
                Require(!connect);Data::Node parent;parent.height=node.height-1;parent.checkpoint=Empty(parent.height,node.parent);
                data->Put(node.parent,parent);
            }
            data->Put(e.context.block_hash,node);
        }
        data->Position(sequence,connect?e.context.block_hash:e.context.parent_hash);
    }
    data->index.Freeze();
    data->spool.Freeze();data->states.Freeze();data->authorizations.Freeze();
    return std::shared_ptr<const RuntimeAccountReplay>(new RuntimeAccountReplay(std::move(data)));
}
RuntimeOutboxCursor RuntimeAccountReplay::Head() const{return data_->head;}
RuntimeAccountReplay::EventHandle RuntimeAccountReplay::Event(uint64_t sequence) const{
    return data_->ReadEvent(sequence);
}
wallet::OrchardAccountDelivery::RestorePoint RuntimeAccountReplay::Point(RuntimeOutboxCursor cursor) const {
    Require(cursor.sequence<=data_->head.sequence&&(cursor.sequence?Event(cursor.sequence)->cursor==cursor:cursor.digest.IsNull()));
    const auto tip=data_->Position(cursor.sequence);const auto data=data_;wallet::OrchardWalletRestoreLookups lookup;
    lookup.origin=[data,tip](uint32_t height,const uint256& block,const orchard::Hash& txid){
        const auto& node=data->Ancestor(tip,height,block);Require(bool(node.state)&&bool(node.auths));
        const auto retained=data->authorizations.Load(*node.auths);
        for(const auto& auth:*retained)if(auth.Orchard().Txid()==txid)return std::make_shared<const VerifiedOrchardAuthorizations>(auth);
        throw OrchardStateLookupError(Status::Corruption);
    };
    lookup.spent_nullifier=[data,tip](const uint256& nf)->StatusOr<bool>{return data->Nullifier(tip,nf);};
    lookup.selected_block=[data,tip](uint32_t height,const uint256& hash){const auto& n=data->Ancestor(tip,height,hash);Require(bool(n.state));return data->ReadBody(n).orchard;};
    lookup.selected_historical_block=[data,tip](uint32_t height,const uint256& hash){const auto& n=data->Ancestor(tip,height,hash);Require(n.historical);return data->ReadBody(n).historical;};
    return {data->At(tip).checkpoint,std::move(lookup)};
}
std::function<StatusOr<uint256>(uint32_t)> RuntimeAccountReplay::SelectedHashes(RuntimeOutboxCursor cursor) const {
    (void)Point(cursor); // Validate cursor/digest before publishing a callback.
    const auto tip=data_->Position(cursor.sequence);const auto data=data_;
    return [data,tip](uint32_t height)->StatusOr<uint256>{
        auto hash=tip;auto expected=data->At(tip).height;
        if(height>expected)return Status::NotFound;
        for(;;){
            const auto node=data->Find(hash);
            if(!node)return Status::NotFound;
            if(node->height!=expected)return Status::Corruption;
            if(expected==height)return hash;
            hash=node->parent;--expected;
        }
    };
}
uint32_t RuntimeAccountReplay::ForkHeight(RuntimeOutboxCursor cursor,const uint256& block,uint32_t height) const {
    const auto checkpoint=Point(cursor).checkpoint;
    auto left=checkpoint.block_hash,right=block;auto lh=checkpoint.height,rh=height;
    const auto parent=[&](uint256& hash,uint32_t& h){
        const auto& node=data_->At(hash);Require(h>0&&node.height==h);
        hash=node.parent;--h;Require(data_->At(hash).height==h);
    };
    Require(data_->At(right).height==rh);
    while(lh>rh)parent(left,lh);
    while(rh>lh)parent(right,rh);
    while(left!=right){parent(left,lh);parent(right,rh);}
    return lh;
}
bool RuntimeAccountReplay::IsAncestorOf(RuntimeOutboxCursor cursor,const uint256& block,uint32_t height) const {
    const auto point=Point(cursor).checkpoint;auto hash=block;
    if(height<point.height)return false;
    for(;;){
        const auto node=data_->Find(hash);
        Require(node&&node->height==height);
        if(height==point.height)return hash==point.block_hash;
        hash=node->parent;--height;
    }
}
RuntimeAccountReplay::BlockHandle RuntimeAccountReplay::Block(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence)->context.block_hash);Require(bool(n.state));return data_->ReadBody(n).orchard;
}
RuntimeAccountReplay::StateHandle RuntimeAccountReplay::State(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence)->context.block_hash);Require(bool(n.state));return data_->states.Load(*n.state);
}
RuntimeAccountReplay::AuthorizationHandle RuntimeAccountReplay::Authorizations(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence)->context.block_hash);Require(bool(n.state)&&bool(n.auths));return data_->authorizations.Load(*n.auths);
}
} // namespace dinero
