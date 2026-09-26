// Generated storage fixtures only. Receipt roots/amounts are deliberately
// synthetic; these tests do not establish historical funding or SHR1 validity.
#include "shielded_store_fixture.h"
#include "consensus/tx_validation.h"
#include "consensus/orchard_pool_balance.h"
#include <sys/wait.h>
using namespace shielded_store_fixture;
using dinero::uint256;
using dinero::storage::LegacyRetirementState;
const auto token = ChainWriteToken::CreateForTesting();
uint256 H(uint8_t b) { uint256 h; h.data[0] = b; return h; }
LegacyRetirementState State(uint8_t block = 10, uint32_t height = 10) {
    return {{2, H(1), 1, 10, 3, H(9), H(55), 37, H(90), 2, 2}, height, H(block), H(height == 10 ? 9 : 10)};
}
void Tip(ChainDB& db, rocksdb::WriteBatch& batch, uint32_t h, const uint256& hash) {
    CHECK(db.setTip(token, hash, h, dinero::arith_uint256(h), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, hash, h, &batch) == Status::Ok);
}
void Initial(ChainDB& db) {
    rocksdb::WriteBatch b; Tip(db, b, 9, H(9));
    CHECK(db.putShieldedTipMarker(token, {9, H(9), H(90), 2, 2}, &b) == Status::Ok);
    dinero::Coin coin; coin.amount = 123; coin.script_pubkey = "51"; coin.height = 1;
    CHECK(db.putCoin(token, H(77), 0, coin, &b) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok);
}
void Check(ChainDB& db, const std::optional<LegacyRetirementState>& state) {
    const auto stored = db.getLegacyRetirementState();
    if (state) CHECK(stored.ok() && *stored == *state);
    else CHECK(stored.status() == Status::NotFound);
    const auto marker = RequiredValue(db.getShieldedTipMarker());
    CHECK(marker.height == (state ? int(state->height) : 9));
    CHECK(marker.block_hash == (state ? state->block_hash : H(9)));
    CHECK(marker.shielded_root == H(90) && marker.tree_size == 2 && marker.nullifier_count == 2);
    const auto tip = RequiredValue(db.getValidatedTip());
    CHECK(tip.height == marker.height && tip.hash == marker.block_hash);
    CHECK(RequiredValue(db.getCoin(H(77), 0)).amount == 123);
}
void Connect(ChainDB& db, const std::optional<LegacyRetirementState>& parent, const LegacyRetirementState& next) {
    rocksdb::WriteBatch b; Tip(db, b, next.height, next.block_hash);
    CHECK(db.stageLegacyRetirementConnect(token, parent, next, b) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok); Check(db, next);
}
void Disconnect(ChainDB& db, const LegacyRetirementState& tip, const std::optional<LegacyRetirementState>& parent) {
    rocksdb::WriteBatch b;
    Tip(db, b, parent ? parent->height : 9, parent ? parent->block_hash : H(9));
    CHECK(db.stageLegacyRetirementDisconnect(token, tip, b) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok); Check(db, parent);
}
void RoundTrip() {
    TempDir tmp; Seed(tmp.path); ChainDB db; CHECK(db.init(tmp.path) == Status::Ok); Initial(db);
    db.close(); auto original = Inspect(tmp.path); CHECK(db.init(tmp.path) == Status::Ok);
    { rocksdb::WriteBatch b; Tip(db, b, 10, H(10));
      CHECK(db.stageLegacyRetirementConnect(token, std::nullopt, State(), b) == Status::Ok);
      const auto bytes = b.Data();
      CHECK(db.stageLegacyRetirementConnect(token, std::nullopt, State(), b) == Status::Invalid);
      CHECK(b.Data() == bytes); Check(db, std::nullopt); }
    db.close(); CHECK(Inspect(tmp.path) == original); CHECK(db.init(tmp.path) == Status::Ok);
    Connect(db, std::nullopt, State());
    db.close(); CHECK(db.init(tmp.path) == Status::Ok); Check(db, State());
    Connect(db, State(), State(11, 11));
    db.close(); CHECK(db.init(tmp.path) == Status::Ok); Check(db, State(11, 11));
    Disconnect(db, State(11, 11), State()); Disconnect(db, State(), std::nullopt);
    db.close(); auto restored = Inspect(tmp.path);
    // setTip has a local wall-clock field; every consensus/storage byte restores.
    auto& tip = restored.at("meta").at("tip"); const auto& old = original.at("meta").at("tip");
    CHECK(tip.size() == old.size() && tip.size() >= 4); tip.replace(tip.size()-4, 4, old, old.size()-4, 4);
    CHECK(restored == original); CHECK(db.init(tmp.path) == Status::Ok);
    // A different boundary block is legal only after the old retirement is undone.
    auto replacement = State(20); Connect(db, std::nullopt, replacement);
    Disconnect(db, replacement, std::nullopt);
    // Different parent ancestry derives a new receipt, not a reused old one.
    { rocksdb::WriteBatch b; Tip(db, b, 9, H(8));
      CHECK(db.putShieldedTipMarker(token, {9,H(8),H(90),2,2}, &b)==Status::Ok);
      CHECK(db.writeBatch(token,std::move(b),true)==Status::Ok); }
    replacement = State(21); replacement.record.boundary_parent = H(8); replacement.parent_hash = H(8); replacement.record.retired_value = 25;
    rocksdb::WriteBatch b; CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Invalid);
    CHECK(b.Count()==0); CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,replacement,b)==Status::Ok);
    Tip(db,b,10,H(21)); CHECK(db.writeBatch(token,std::move(b),true)==Status::Ok);
    CHECK(RequiredValue(db.getLegacyRetirementState())==replacement);
}
void Rejects() {
    TempDir tmp; Seed(tmp.path); ChainDB db; CHECK(db.init(tmp.path) == Status::Ok); Initial(db);
    for (int f=0;f<8;++f) {
        auto bad=State();
        if(f==0)bad.record.network_code=255;
        if(f==1)bad.record.genesis={};
        if(f==2)bad.record.branch_id=0;
        if(f==3)bad.record.activation_height=0;
        if(f==4)bad.record.legacy_epoch_height=10;
        if(f==5)bad.record.retired_value=dinero::consensus::MAX_MONEY+1;
        if(f==6)bad.height=11;
        if(f==7)bad.block_hash={};
        rocksdb::WriteBatch b; CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,bad,b)==Status::Invalid); CHECK(b.Count()==0);
    }
    // Existing legacy writes/marker changes must not coexist with retirement.
    for(int f=0;f<3;++f){
        rocksdb::WriteBatch b;
        if(f==0)CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,"changed",&b)==Status::Ok);
        if(f==1)CHECK(db.putShieldedNullifier(token,9,H(44).data,&b)==Status::Ok);
        if(f==2)CHECK(db.putShieldedTipMarker(token,{9,H(9),H(90),2,2},&b)==Status::Ok);
        auto saved=b.Data(); b.SetSavePoint();
        CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Invalid);
        CHECK(b.Data()==saved); CHECK(b.RollbackToSavePoint().ok()); CHECK(b.Data()==saved);
    }
    auto wrong=State();wrong.record.tree_root=H(91);
    rocksdb::WriteBatch b;CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,wrong,b)==Status::Corruption);CHECK(b.Count()==0);
    Connect(db,std::nullopt,State());
    CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::AlreadyExists);
    for(int f=0;f<11;++f){
        auto changed=State(11,11);auto& r=changed.record;
        if(f==0)r.network_code=1;if(f==1)r.genesis=H(2);if(f==2)r.branch_id=2;
        if(f==3)r.activation_height=9;if(f==4)r.legacy_epoch_height=4;if(f==5)r.boundary_parent=H(8);
        if(f==6)r.legacy_state_root=H(56);if(f==7)r.retired_value=36;if(f==8)r.tree_root=H(91);
        if(f==9)r.tree_size=3;if(f==10)r.nullifier_count=3;
        CHECK(db.stageLegacyRetirementConnect(token,State(),changed,b)==Status::Invalid);CHECK(b.Count()==0);
    }
    CHECK(db.stageLegacyRetirementDisconnect(token,State(12),b)==Status::Invalid);CHECK(b.Count()==0);
    auto wrong_parent=State(11,11);wrong_parent.parent_hash=H(8);
    CHECK(db.stageLegacyRetirementConnect(token,State(),wrong_parent,b)==Status::Invalid);CHECK(b.Count()==0);
    Check(db,State());
}
void AtomicOrchard(bool orchard_first) {
    TempDir tmp;Seed(tmp.path);ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);Initial(db);
    dinero::storage::OrchardStoredState next{10,H(10),H(60),0,0,"opaque-frontier"};
    rocksdb::WriteBatch b;
    if(orchard_first)CHECK(db.stageOrchardConnect(token,std::nullopt,next,{}, {},b)==Status::Ok);
    CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Ok);
    if(!orchard_first)CHECK(db.stageOrchardConnect(token,std::nullopt,next,{}, {},b)==Status::Ok);
    Tip(db,b,10,H(10));CHECK(db.writeBatch(token,std::move(b),true)==Status::Ok);
    db.close();CHECK(db.init(tmp.path)==Status::Ok);Check(db,State());
    CHECK(RequiredValue(db.getOrchardState()).pool_balance==0); // No retired credit.
    rocksdb::WriteBatch undo;
    if(orchard_first)CHECK(db.stageOrchardDisconnect(token,next,undo)==Status::Ok);
    CHECK(db.stageLegacyRetirementDisconnect(token,State(),undo)==Status::Ok);
    if(!orchard_first)CHECK(db.stageOrchardDisconnect(token,next,undo)==Status::Ok);
    Tip(db,undo,9,H(9));CHECK(db.writeBatch(token,std::move(undo),true)==Status::Ok);Check(db,std::nullopt);
    CHECK(db.getOrchardState().status()==Status::NotFound);
}
void ProcessBoundary(bool disconnect, bool commit) {
    TempDir tmp;Seed(tmp.path);
    {ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);Initial(db);if(disconnect)Connect(db,std::nullopt,State());}
    const auto pid=fork();CHECK(pid>=0);
    if(pid==0){try{
        ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);rocksdb::WriteBatch b;
        if(disconnect)CHECK(db.stageLegacyRetirementDisconnect(token,State(),b)==Status::Ok);
        else CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Ok);
        Tip(db,b,disconnect?9:10,H(disconnect?9:10));
        if(commit)CHECK(db.writeBatch(token,std::move(b),true)==Status::Ok);
        _Exit(0);
    }catch(...){_Exit(2);}}
    int status=0;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);
    Check(db,disconnect!=commit?std::optional{State()}:std::nullopt);
}
void Corruption() {
    for(int mode=0;mode<7;++mode){
        TempDir tmp;Seed(tmp.path);{ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);Initial(db);Connect(db,std::nullopt,State());
            if(mode==6)Connect(db,State(),State(11,11));}
        auto rows=Inspect(tmp.path);auto names=legacy;names.push_back(shielded);
        const auto selected=mode==6?State(11,11):State();
        const auto key=mode<3?std::string("R1S"):std::string("R1U")+std::string(reinterpret_cast<const char*>(selected.block_hash.data),32);
        auto value=rows.at(shielded).at(key);
        if(mode==6){
            // Fixed DLU1 framing: alter only the before-state block hash. The
            // after-state still matches the current receipt byte for byte.
            CHECK(value.size()==479);value[178]^=0x40;
        }else{
            if(mode%3==0)value.pop_back();if(mode%3==1)value.push_back(0);if(mode%3==2)value[0]^=1;
        }
        {Raw raw(tmp.path,names);raw.put(shielded,key,value);}
        ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);rocksdb::WriteBatch b;
        if(mode<3)CHECK(db.getLegacyRetirementState().status()==Status::Corruption);
        CHECK(db.stageLegacyRetirementDisconnect(token,selected,b)==Status::Corruption);CHECK(b.Count()==0);
    }
    // Missing current receipt plus leftover retirement data is not initial state.
    TempDir tmp;Seed(tmp.path);auto names=legacy;names.push_back(shielded);
    {Raw raw(tmp.path,names);raw.put(shielded,"R1Uorphan","bad");}
    ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);Initial(db);rocksdb::WriteBatch b;
    CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Corruption);CHECK(b.Count()==0);
}
void LayoutRefusal() {
    TempDir tmp;Seed(tmp.path,false,std::nullopt);ChainDB db;CHECK(db.init(tmp.path)==Status::Ok);
    CHECK(db.getLegacyRetirementState().status()==Status::Invalid);rocksdb::WriteBatch b;
    CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,State(),b)==Status::Invalid);
    CHECK(db.stageLegacyRetirementDisconnect(token,State(),b)==Status::Invalid);CHECK(b.Count()==0);
}
int main(){try{
    RoundTrip();Rejects();AtomicOrchard(false);AtomicOrchard(true);Corruption();LayoutRefusal();
    for(bool disconnect:{false,true})for(bool commit:{false,true})ProcessBoundary(disconnect,commit);
    std::cout<<"Retirement receipt: frozen fields, atomic marker/Orchard batch, exact undo, branch replacement, corruption and four process boundaries passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
