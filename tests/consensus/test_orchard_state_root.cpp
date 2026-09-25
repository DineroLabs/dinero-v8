#include "orchard_state_test_fixture.h"
#include "../storage/shielded_store_fixture.h"
#include "consensus/orchard_state_root.h"
using namespace shielded_store_fixture;
static const auto token=ChainWriteToken::CreateForTesting();
static bool Eq(const uint256& h,const Bytes& b){return b.size()==32&&std::equal(b.begin(),b.end(),h.begin());}
template<class F>static void RootReject(F f){bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}catch(const BackendError&){rejected=true;}Require(rejected);}
static storage::LegacyRetirementRecord Record(const SigningDomain& d){
    uint256 genesis;std::copy(d.genesis_wire.begin(),d.genesis_wire.end(),genesis.begin());
    return {d.network_code,genesis,d.branch_id,20001,3,H(1),H(55),37,H(90),2,2};
}
static void Stage(ChainDB& db,const PreparedOrchardState& p){
    rocksdb::WriteBatch batch;Require(db.stageOrchardConnect(token,p.Parent(),p.Next(),p.Nullifiers(),p.Flows(),batch)==Status::Ok);
    Require(db.writeBatch(token,std::move(batch),true)==Status::Ok);
}
int main(int argc,char**argv){try{
    Require(argc==2);const std::string base=argv[1];
    const auto shield=Authorized(base,false,20000),spend=Authorized(base,true,20001);
    OrchardBlockContext c{20001,H(2),H(1),20001,Fixture(base).domain};
    OrchardStateLookups lookups{[](const uint256&)->StatusOr<bool>{return false;},[](const uint256&)->StatusOr<bool>{return false;}};
    const auto funded=PrepareOrchardStateTransition(c,{},std::vector{shield},lookups);
    const auto record=Record(c.domain);
    OrchardStateRootContext root_context{c.domain,c.activation_height,c.height,c.parent_hash};
    TempDir tmp;Seed(tmp.path);ChainDB db;Require(db.init(tmp.path)==Status::Ok);
    const auto projected=RequiredValue(db.previewOrchardCommitmentSets({},funded.Next(),funded.Nullifiers()));
    Require(Eq(projected.nullifiers,Load(base+"/state-set.nullifiers")));
    Require(Eq(projected.anchors,Load(base+"/state-funded.anchors")));
    Require(projected.nullifier_count==2&&projected.anchor_count==1&&projected.anchor_references==1);
    const auto pre=BuildOrchardStateRootPreimage(root_context,record,funded.Next(),projected);
    Require(pre==Load(base+"/state-funded.preimage"));
    const auto root=ComputeOrchardStateRoot(root_context,record,funded.Next(),projected);
    Require(Eq(root,Load(base+"/state-funded.root")));
    Require(db.getOrchardState().status()==Status::NotFound); // Projection is read-only.
    // Initial empty blocks commit a real empty set, not an unavailable set.
    const auto empty_initial=PrepareOrchardStateTransition(c,{}, {},lookups);
    const auto empty_initial_sets=RequiredValue(db.previewOrchardCommitmentSets({},empty_initial.Next(),{}));
    Require(empty_initial_sets.nullifier_count==0&&empty_initial_sets.anchor_references==1);
    Require(Eq(empty_initial_sets.nullifiers,Load(base+"/state-empty.nullifiers")));
    Require(ComputeOrchardStateRoot(root_context,record,empty_initial.Next(),empty_initial_sets)!=root);
    Stage(db,funded);db.close();Require(db.init(tmp.path)==Status::Ok);
    Require(RequiredValue(db.getOrchardCommitmentSets(funded.Next()))==projected);
    auto own_hash=funded.Next();own_hash.block_hash=H(100);
    Require(ComputeOrchardStateRoot(root_context,record,own_hash,projected)==root); // No cycle.
    Require(db.getOrchardCommitmentSets(own_hash).status()==Status::Invalid); // Storage view still exact.
    // Every independently meaningful committed field changes the digest or rejects.
    for(int f=0;f<20;++f){
        auto ctx=root_context;auto r=record;auto state=funded.Next();auto sets=projected;
        if(f==0){ctx.domain.network_code=1;r.network_code=1;}
        if(f==1){ctx.domain.genesis_wire[0]^=1;r.genesis.data[0]^=1;}
        if(f==2){++ctx.domain.branch_id;++r.branch_id;}
        if(f==3)++r.legacy_epoch_height;
        if(f==4){ctx.parent_hash=H(99);r.boundary_parent=H(99);}
        if(f==5)r.legacy_state_root=H(99);
        if(f==6)++r.retired_value;if(f==7)r.tree_root=H(99);
        if(f==8)++r.tree_size;if(f==9)++r.nullifier_count;if(f==10)++state.pool_balance;
        if(f==11)sets.nullifiers=H(99);if(f==12)sets.anchors=H(99);
        if(f==13)++sets.nullifier_count;if(f==14)++sets.anchor_count;if(f==15)++sets.anchor_references;
        if(f==16)state.anchor=H(99);if(f==17)++state.tree_size;if(f==18)state.frontier.push_back(0);
        if(f==19)ctx.activation_height=UINT32_MAX;
        bool changed=false;try{changed=ComputeOrchardStateRoot(ctx,r,state,sets)!=root;}
        catch(const std::invalid_argument&){changed=true;}catch(const BackendError&){changed=true;}
        Require(changed);
    }
    auto bad=root_context;bad.domain={};RootReject([&]{(void)ComputeOrchardStateRoot(bad,record,funded.Next(),projected);});
    auto large=record;large.retired_value=kMaxMoneyUna;
    RootReject([&]{(void)ComputeOrchardStateRoot(root_context,large,funded.Next(),projected);});
    auto child=c;++child.height;child.parent_hash=c.block_hash;child.block_hash=H(3);
    lookups.active_anchor=[&](const uint256& a)->StatusOr<bool>{return a==funded.Next().anchor;};
    const auto empty=PrepareOrchardStateTransition(child,funded.Next(),{},lookups);
    const auto empty_sets=RequiredValue(db.previewOrchardCommitmentSets(funded.Next(),empty.Next(),{}));
    OrchardStateRootContext child_ctx{child.domain,child.activation_height,child.height,child.parent_hash};
    Require(BuildOrchardStateRootPreimage(child_ctx,record,empty.Next(),empty_sets)==Load(base+"/state-empty-child.preimage"));
    Require(Eq(ComputeOrchardStateRoot(child_ctx,record,empty.Next(),empty_sets),Load(base+"/state-empty-child.root")));
    Stage(db,empty);Require(RequiredValue(db.getOrchardCommitmentSets(empty.Next()))==empty_sets);
    Require(RequiredValue(db.previewOrchardDisconnectCommitmentSets(empty.Next()))==projected);
    Require(RequiredValue(db.getOrchardCommitmentSets(empty.Next()))==empty_sets); // Read only.
    rocksdb::WriteBatch undo;Require(db.stageOrchardDisconnect(token,empty.Next(),undo)==Status::Ok);
    Require(db.writeBatch(token,std::move(undo),true)==Status::Ok);
    Require(RequiredValue(db.getOrchardCommitmentSets(funded.Next()))==projected);
    const auto paid=PrepareOrchardStateTransition(child,funded.Next(),std::vector{spend},lookups);
    const auto paid_sets=RequiredValue(db.previewOrchardCommitmentSets(funded.Next(),paid.Next(),paid.Nullifiers()));
    Require(paid_sets.nullifier_count==4&&paid_sets.anchor_count==2&&paid_sets.anchor_references==2);
    Stage(db,paid);db.close();Require(db.init(tmp.path)==Status::Ok);
    Require(RequiredValue(db.getOrchardCommitmentSets(paid.Next()))==paid_sets);
    Require(RequiredValue(db.previewOrchardDisconnectCommitmentSets(paid.Next()))==projected);
    Require(RequiredValue(db.getOrchardCommitmentSets(paid.Next()))==paid_sets);
    Require(db.previewOrchardDisconnectCommitmentSets(own_hash).status()==Status::Invalid);
    // Projection must reject duplicate new nullifiers, existing membership and wrong size.
    auto next=paid.Next();++next.height;next.block_hash=H(4);next.tree_size+=2;next.anchor=H(99);
    Require(db.previewOrchardCommitmentSets(paid.Next(),next,{H(44),H(44)}).status()==Status::Invalid);
    Require(db.previewOrchardCommitmentSets(paid.Next(),next,{funded.Nullifiers()[0],H(44)}).status()==Status::AlreadyExists);
    next.tree_size--;
    Require(db.previewOrchardCommitmentSets(paid.Next(),next,{H(43),H(44)}).status()==Status::Invalid);
    db.close();const auto pristine=Inspect(tmp.path);auto names=legacy;names.push_back(shielded_store_fixture::shielded);
    const auto rawkey=[](const char* prefix,const uint256& h){return std::string(prefix)+std::string(reinterpret_cast<const char*>(h.data),32);};
    const auto nf_key=rawkey("O1N",funded.Nullifiers()[0]),anchor_key=rawkey("O1A",paid.Next().anchor);
    // An unreadable/missing logical set is never represented by an empty digest.
    for(int kind=0;kind<7;++kind){
        const auto key=kind<3?nf_key:anchor_key;const auto old=pristine.at(shielded_store_fixture::shielded).at(key);
        {Raw raw(tmp.path,names);
         if(kind==0){rocksdb::WriteOptions o;Require(raw.db->Delete(o,raw.cf(shielded_store_fixture::shielded),key).ok());}
         else if(kind==1)raw.put(shielded_store_fixture::shielded,key,std::string(32,0));
         else if(kind==2)raw.put(shielded_store_fixture::shielded,key,"bad");
         else if(kind==3)raw.put(shielded_store_fixture::shielded,key,std::string(8,0));
         else if(kind==4)raw.put(shielded_store_fixture::shielded,key,std::string(8,char(0xff)));
         else if(kind==5)raw.put(shielded_store_fixture::shielded,key,"bad");
         else{rocksdb::WriteOptions o;Require(raw.db->Delete(o,raw.cf(shielded_store_fixture::shielded),key).ok());}}
        Require(db.init(tmp.path)==Status::Ok);
        Require(db.getOrchardCommitmentSets(paid.Next()).status()==Status::Corruption);
        Require(db.previewOrchardDisconnectCommitmentSets(paid.Next()).status()==Status::Corruption);
        db.close();{Raw raw(tmp.path,names);raw.put(shielded_store_fixture::shielded,key,old);}
    }
    // Reverse projection additionally checks exact stored undo, ownership of
    // removed nullifiers, and survival of the restored parent's anchor.
    const auto removed_nf=rawkey("O1N",paid.Nullifiers()[0]);
    const auto undo_key=rawkey("O1U",paid.Next().block_hash);
    const auto parent_anchor=rawkey("O1A",funded.Next().anchor);
    for(int kind=0;kind<4;++kind){
        const auto key=kind==0?removed_nf:(kind==3?parent_anchor:undo_key);
        const auto old=pristine.at(shielded_store_fixture::shielded).at(key);
        {Raw raw(tmp.path,names);
         if(kind==0)raw.put(shielded_store_fixture::shielded,key,std::string(32,42));
         else if(kind==2)raw.put(shielded_store_fixture::shielded,key,"truncated undo");
         else{rocksdb::WriteOptions o;Require(raw.db->Delete(o,raw.cf(shielded_store_fixture::shielded),key).ok());}}
        const auto changed=Inspect(tmp.path);
        Require(db.init(tmp.path)==Status::Ok);
        Require(db.previewOrchardDisconnectCommitmentSets(paid.Next()).status()==Status::Corruption);
        db.close();Require(Inspect(tmp.path)==changed); // No repair or partial rollback.
        {Raw raw(tmp.path,names);raw.put(shielded_store_fixture::shielded,key,old);}
    }
    Require(Inspect(tmp.path)==pristine);
    Require(db.init(tmp.path)==Status::Ok);
    rocksdb::WriteBatch undo_paid;Require(db.stageOrchardDisconnect(token,paid.Next(),undo_paid)==Status::Ok);
    Require(db.writeBatch(token,std::move(undo_paid),true)==Status::Ok);
    const auto before_boundary=RequiredValue(db.previewOrchardDisconnectCommitmentSets(funded.Next()));
    Require(before_boundary.nullifier_count==0&&before_boundary.anchor_count==0&&before_boundary.anchor_references==0);
    Require(Eq(before_boundary.nullifiers,Load(base+"/state-empty.nullifiers")));
    Require(RequiredValue(db.getOrchardCommitmentSets(funded.Next()))==projected);
    rocksdb::WriteBatch undo_funded;Require(db.stageOrchardDisconnect(token,funded.Next(),undo_funded)==Status::Ok);
    Require(db.writeBatch(token,std::move(undo_funded),true)==Status::Ok);
    Require(db.getOrchardState().status()==Status::NotFound);
    const auto replay=RequiredValue(db.previewOrchardCommitmentSets({},funded.Next(),funded.Nullifiers()));
    Require(replay==projected);
    std::cout<<"Composite state root: independent framing/set vectors, projection/commit/reopen/undo parity, field binding, no own-hash cycle and corrupt-set refusal passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
