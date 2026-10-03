#include "wallet/runtime_account_replay.h"
#include "consensus/undo.h"
#include "primitives/block.h"
#include <set>

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
        std::shared_ptr<const OrchardBlockCandidate> block;
        std::shared_ptr<const dinero::Block> historical;
        std::shared_ptr<const PreparedOrchardState> state;
        std::vector<VerifiedOrchardAuthorizations> auths;
        std::optional<RuntimeOrchardReplay> replay;
        storage::OrchardStoredState checkpoint;
    };
    RuntimeOutboxCursor head;uint32_t activation=0;orchard::SigningDomain domain;
    uint256 origin;std::vector<RuntimeOutboxEvent> events;
    std::vector<uint256> positions;std::map<uint256,Node> nodes;
    const Node& At(const uint256& hash) const {auto i=nodes.find(hash);Require(i!=nodes.end());return i->second;}
    const Node& Ancestor(uint256 tip,uint32_t height,const uint256& hash) const {
        for(;;){const auto& n=At(tip);Require(n.height>=height);if(n.height==height){Require(tip==hash);return n;}tip=n.parent;}
    }
    bool Nullifier(uint256 tip,const uint256& nf) const {
        for(;;){const auto& n=At(tip);if(n.height<activation)return false;Require(bool(n.state));
            if(std::find(n.state->Nullifiers().begin(),n.state->Nullifiers().end(),nf)!=n.state->Nullifiers().end())return true;
            tip=n.parent;}
    }
    bool Anchor(uint256 tip,const uint256& anchor) const {
        for(;;){const auto& n=At(tip);if(n.height<activation)return false;Require(bool(n.state));
            if(n.checkpoint.anchor==anchor)return true;tip=n.parent;}
    }
};
std::shared_ptr<const RuntimeAccountReplay> RuntimeAccountReplay::Capture(const Source& source) {
    return Build(ReadSource(source));
}
std::shared_ptr<RuntimeAccountReplay::Data> RuntimeAccountReplay::ReadSource(const Source& source) {
    // Explicit operational ceiling: fail without wallet writes, never truncate
    // a history and report EOF/readiness. This is not a resident-memory limit.
    constexpr size_t max_records=2048,max_material=64*1024*1024;
    auto data=std::make_shared<Data>();RuntimeOutboxCursor after;size_t material=0;
    for(;;){
        auto page=source(after,128);
        if(!after.sequence){data->head=page.head;Require(data->head.sequence&&data->head.sequence<=max_records);}
        Require(page.head==data->head&&!page.events.empty());
        for(auto& e:page.events){
            Require(e.cursor.sequence==after.sequence+1&&e.previous_digest==after.digest&&e.cursor.sequence<=data->head.sequence);
            size_t charge=e.body.size()+224;
            if(e.orchard_replay){const auto& r=*e.orchard_replay;charge+=128+r.coin_undo.size()+r.next.frontier.size()+r.branch_mtp.size()*12;
                if(r.parent)charge+=88+r.parent->frontier.size();}
            Require(charge<=max_material-material);material+=charge;after=e.cursor;data->events.push_back(std::move(e));
        }
        Require(page.next==after);if(after==data->head)break;
    }
    const auto& last=data->events.back();const bool connect=last.direction==RuntimeBlockDirection::Connect;
    const auto final=source(data->head,1);Require(final.head==data->head&&final.events.empty()&&final.after_tip&&
        final.after_tip->first==(connect?last.context.block_hash:last.context.parent_hash)&&
        final.after_tip->second==(connect?last.context.height:last.context.height-1));
    return data;
}
std::shared_ptr<const RuntimeAccountReplay> RuntimeAccountReplay::Build(std::shared_ptr<Data> data) {
    // No source callbacks or selected-chain access beyond this point. Expensive
    // authorization verification uses owned immutable material after the
    // service releases its activation lock.
    const auto& first=data->events.front();data->activation=first.context.activation_height;data->domain=first.context.domain;
    Require(data->activation>0&&data->activation!=UINT32_MAX&&first.direction==RuntimeBlockDirection::Connect&&first.context.height==data->activation&&first.orchard_replay&&
        !first.orchard_replay->parent);
    data->origin=first.context.parent_hash;data->positions.push_back(data->origin);
    Data::Node root;root.height=data->activation-1;root.checkpoint=Empty(root.height,data->origin);data->nodes.emplace(data->origin,std::move(root));
    for(const auto& e:data->events){
        Require(e.context.height>0&&e.context.activation_height==data->activation&&Domain(e.context.domain,data->domain)&&
            (e.direction==RuntimeBlockDirection::Connect||e.direction==RuntimeBlockDirection::Disconnect));
        const bool connect=e.direction==RuntimeBlockDirection::Connect;const auto before=data->positions.back();
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
                Data::Node node;node.height=e.context.height;node.parent=e.context.parent_hash;node.block=std::move(block);
                node.state=std::move(state);node.auths=std::move(auths);node.checkpoint=frame.next;node.replay=frame;
                auto old=data->nodes.find(e.context.block_hash);
                if(old!=data->nodes.end())Require(old->second.block&&old->second.block->WireBytes()==e.body&&old->second.checkpoint==node.checkpoint&&
                    old->second.replay->coin_undo==frame.coin_undo&&old->second.replay->branch_mtp==frame.branch_mtp);
                else data->nodes.emplace(e.context.block_hash,std::move(node));
            }else{
                const auto& node=data->At(before);Require(node.block&&node.block->WireBytes()==e.body&&node.checkpoint==frame.next&&node.state->Parent()==frame.parent&&
                    node.replay->coin_undo==frame.coin_undo&&node.replay->branch_mtp==frame.branch_mtp);
            }
        }else{
            auto decoded=dinero::Block::Deserialize(e.body);Require(decoded.has_value());
            const std::string wire(e.body.begin(),e.body.end());Require(decoded->Serialize()==wire&&decoded->GetHash()==e.context.block_hash&&decoded->header.prev_block_hash==e.context.parent_hash);
            auto block=std::make_shared<const dinero::Block>(std::move(*decoded));
            auto& node=data->nodes[e.context.block_hash];
            if(node.historical)Require(node.historical->Serialize()==wire);
            node.height=e.context.height;node.parent=e.context.parent_hash;node.historical=std::move(block);
            node.checkpoint=Empty(node.height,e.context.block_hash);
            if(!data->nodes.contains(node.parent)){
                Require(!connect);Data::Node parent;parent.height=node.height-1;parent.checkpoint=Empty(parent.height,node.parent);
                data->nodes.emplace(node.parent,std::move(parent));
            }
        }
        data->positions.push_back(connect?e.context.block_hash:e.context.parent_hash);
    }
    return std::shared_ptr<const RuntimeAccountReplay>(new RuntimeAccountReplay(std::move(data)));
}
RuntimeOutboxCursor RuntimeAccountReplay::Head() const{return data_->head;}
const RuntimeOutboxEvent& RuntimeAccountReplay::Event(uint64_t sequence) const{
    Require(sequence&&sequence<=data_->events.size());return data_->events[sequence-1];
}
wallet::OrchardAccountDelivery::RestorePoint RuntimeAccountReplay::Point(RuntimeOutboxCursor cursor) const {
    Require(cursor.sequence<=data_->events.size()&&(cursor.sequence?Event(cursor.sequence).cursor==cursor:cursor.digest.IsNull()));
    const auto tip=data_->positions[cursor.sequence];const auto data=data_;wallet::OrchardWalletRestoreLookups lookup;
    lookup.origin=[data,tip](uint32_t height,const uint256& block,const orchard::Hash& txid){
        const auto& node=data->Ancestor(tip,height,block);Require(bool(node.block));
        for(const auto& auth:node.auths)if(auth.Orchard().Txid()==txid)return std::make_shared<const VerifiedOrchardAuthorizations>(auth);
        throw OrchardStateLookupError(Status::Corruption);
    };
    lookup.spent_nullifier=[data,tip](const uint256& nf)->StatusOr<bool>{return data->Nullifier(tip,nf);};
    lookup.selected_block=[data,tip](uint32_t height,const uint256& hash){const auto& n=data->Ancestor(tip,height,hash);Require(bool(n.block));return n.block;};
    lookup.selected_historical_block=[data,tip](uint32_t height,const uint256& hash){const auto& n=data->Ancestor(tip,height,hash);Require(bool(n.historical));return n.historical;};
    return {data->At(tip).checkpoint,std::move(lookup)};
}
std::function<StatusOr<uint256>(uint32_t)> RuntimeAccountReplay::SelectedHashes(RuntimeOutboxCursor cursor) const {
    (void)Point(cursor); // Validate cursor/digest before publishing a callback.
    const auto tip=data_->positions[cursor.sequence];const auto data=data_;
    return [data,tip](uint32_t height)->StatusOr<uint256>{
        auto hash=tip;auto expected=data->At(tip).height;
        if(height>expected)return Status::NotFound;
        for(;;){
            const auto it=data->nodes.find(hash);
            if(it==data->nodes.end())return Status::NotFound;
            if(it->second.height!=expected)return Status::Corruption;
            if(expected==height)return hash;
            hash=it->second.parent;--expected;
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
        const auto it=data_->nodes.find(hash);
        Require(it!=data_->nodes.end()&&it->second.height==height);
        if(height==point.height)return hash==point.block_hash;
        hash=it->second.parent;--height;
    }
}
const OrchardBlockCandidate& RuntimeAccountReplay::Block(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence).context.block_hash);Require(bool(n.block));return *n.block;
}
const PreparedOrchardState& RuntimeAccountReplay::State(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence).context.block_hash);Require(bool(n.state));return *n.state;
}
const std::vector<VerifiedOrchardAuthorizations>& RuntimeAccountReplay::Authorizations(uint64_t sequence) const{
    const auto& n=data_->At(Event(sequence).context.block_hash);Require(bool(n.state));return n.auths;
}
} // namespace dinero
