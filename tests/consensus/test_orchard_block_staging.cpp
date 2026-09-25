#include "orchard_state_test_fixture.h"
#include "consensus/orchard_block_staging.h"
#include "../storage/shielded_store_fixture.h"

using namespace shielded_store_fixture;
static const auto token = ChainWriteToken::CreateForTesting();
static void Tip(ChainDB& db, uint256 hash, uint32_t height, rocksdb::WriteBatch& batch) {
    CHECK(db.setTip(token, hash, height, arith_uint256(height), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, hash, height, &batch) == Status::Ok);
}
static void Commit(ChainDB& db, rocksdb::WriteBatch& batch) {
    CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
}
static void RoundTrip(const std::string& base) {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch initial; Tip(db,H(1),20000,initial); Commit(db,initial);
    const std::vector<VerifiedOrchardAuthorizations> shields{Authorized(base,false,20000)};
    const std::vector<VerifiedOrchardAuthorizations> spends{Authorized(base,true,20001)};
    OrchardBlockContext c{20001,H(2),H(1),20001,Fixture(base).domain};
    db.close(); const auto original=Inspect(temp.path); CHECK(db.init(temp.path)==Status::Ok);
    {
        rocksdb::WriteBatch abandoned;
        const auto prepared=StageOrchardBlockUnderChainstateLock(db,token,c,shields,abandoned);
        Tip(db,c.block_hash,c.height,abandoned);
        CHECK(prepared.Next().pool_balance==5000);
        CHECK(db.getOrchardState().status()==Status::NotFound);
    }
    db.close(); CHECK(Inspect(temp.path)==original); CHECK(db.init(temp.path)==Status::Ok);
    rocksdb::WriteBatch funding;
    const auto funded=StageOrchardBlockUnderChainstateLock(db,token,c,shields,funding);
    Tip(db,c.block_hash,c.height,funding); Commit(db,funding);
    db.close(); CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getOrchardState())==funded.Next());
    CHECK(RequiredValue(db.getOrchardAnchorReferences(funded.Next().anchor))==1);
    for(const auto& nf:funded.Nullifiers()) CHECK(RequiredValue(db.getOrchardNullifierOwner(nf))==c.block_hash);
    auto next=c;next.height++;next.parent_hash=c.block_hash;next.block_hash=H(3);
    rocksdb::WriteBatch spending;
    const auto paid=StageOrchardBlockUnderChainstateLock(db,token,next,spends,spending);
    CHECK(paid.Next().pool_balance==4500 && paid.Next().tree_size==4);
    const auto saved_batch=spending.Data();
    // Refuse a second transition without damaging caller-staged data.
    LookupReject(Status::Invalid,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,next,spends,spending);});
    CHECK(spending.Data()==saved_batch);
    Tip(db,next.block_hash,next.height,spending); Commit(db,spending);
    db.close(); CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getOrchardState())==paid.Next());
    rocksdb::WriteBatch undo;
    CHECK(db.stageOrchardDisconnect(token,paid.Next(),undo)==Status::Ok);
    Tip(db,c.block_hash,c.height,undo); Commit(db,undo);
    db.close(); CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getOrchardState())==funded.Next());
    for(const auto& nf:paid.Nullifiers()) CHECK(db.getOrchardNullifierOwner(nf).status()==Status::NotFound);
    // The honest spend is again eligible on a replacement branch after undo.
    next.block_hash=H(4);rocksdb::WriteBatch replacement;
    const auto replaced=StageOrchardBlockUnderChainstateLock(db,token,next,spends,replacement);
    CHECK(replaced.Next().frontier==paid.Next().frontier);
    Tip(db,next.block_hash,next.height,replacement);Commit(db,replacement);
    auto duplicate=next;duplicate.height++;duplicate.parent_hash=next.block_hash;duplicate.block_hash=H(5);
    // Fresh height authorization of the same transaction still hits the database nullifier gate.
    const std::vector<VerifiedOrchardAuthorizations> repeated{Authorized(base,true,20002)};
    rocksdb::WriteBatch failed; Tip(db,H(90),29999,failed); const auto untouched=failed.Data();
    StateReject(StateError::SpentNullifier,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,duplicate,repeated,failed);});
    CHECK(failed.Data()==untouched);
    auto wrong=duplicate;wrong.parent_hash=H(99);
    StateReject(StateError::Context,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,wrong,{},failed);});
    CHECK(failed.Data()==untouched);
    db.close();
    LookupReject(Status::Internal,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,duplicate,{},failed);});
    CHECK(failed.Data()==untouched);
}
static void CorruptParent(const std::string& base) {
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    OrchardBlockContext c{20002,H(3),H(2),20001,Fixture(base).domain};
    // Storage can serialize opaque bytes; the adapter must authenticate the tree
    // and report local corruption, not a consensus failure of the next block.
    storage::OrchardStoredState malformed{20001,H(2),H(90),0,0,"not-a-frontier"};
    rocksdb::WriteBatch seed;
    CHECK(db.stageOrchardConnect(token,{},malformed,{}, {},seed)==Status::Ok);
    Tip(db,H(2),20001,seed);Commit(db,seed);
    rocksdb::WriteBatch rejected;const auto before=rejected.Data();
    LookupReject(Status::Corruption,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,c,{},rejected);});
    CHECK(rejected.Data()==before);
}
int main(int argc,char**argv) {
    try { CHECK(argc==2);RoundTrip(argv[1]);CorruptParent(argv[1]);
        std::cout<<"Orchard ChainDB staging: real frontier, anchors/nullifiers, reopen, branch replacement, abandonment and error separation passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
