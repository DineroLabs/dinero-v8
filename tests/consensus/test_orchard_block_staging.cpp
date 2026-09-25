#include "orchard_forest_test_fixture.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/utxo_publication.h"
#include "consensus/orchard_block_filter.h"
#include "../storage/shielded_store_fixture.h"
#include <iomanip>
#include <sstream>
#include "consensus/utreexo_delta_codec.h"
#include "storage/forest_restore.h"
#include "consensus/chainparams.h"
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdlib>
#include <ctime>
extern char** environ;

using namespace shielded_store_fixture;
static const auto token = ChainWriteToken::CreateForTesting();
static void Tip(ChainDB& db, uint256 hash, uint32_t height, rocksdb::WriteBatch& batch) {
    CHECK(db.setTip(token, hash, height, arith_uint256(height), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, hash, height, &batch) == Status::Ok);
}
static void Commit(ChainDB& db, rocksdb::WriteBatch& batch) {
    CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
}
static PreparedOrchardState StageFixture(ChainDB& db, const ChainWriteToken& token,
    OrchardBlockContext& context, std::span<const VerifiedOrchardAuthorizations> auth,
    rocksdb::WriteBatch& batch) {
    const auto block=Candidate(context,auth);context.block_hash=block.Header().GetHash();
    return StageOrchardBlockUnderChainstateLock(db,token,context,block,true,auth,batch);
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
        const auto prepared=StageFixture(db,token,c,shields,abandoned);
        Tip(db,c.block_hash,c.height,abandoned);
        CHECK(prepared.Next().pool_balance==5000);
        CHECK(db.getOrchardState().status()==Status::NotFound);
    }
    db.close(); CHECK(Inspect(temp.path)==original); CHECK(db.init(temp.path)==Status::Ok);
    rocksdb::WriteBatch funding;
    const auto funded=StageFixture(db,token,c,shields,funding);
    Tip(db,c.block_hash,c.height,funding); Commit(db,funding);
    db.close(); CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getOrchardState())==funded.Next());
    CHECK(RequiredValue(db.getOrchardAnchorReferences(funded.Next().anchor))==1);
    for(const auto& nf:funded.Nullifiers()) CHECK(RequiredValue(db.getOrchardNullifierOwner(nf))==c.block_hash);
    auto next=c;next.height++;next.parent_hash=c.block_hash;next.block_hash=H(3);
    rocksdb::WriteBatch spending;
    const auto paid=StageFixture(db,token,next,spends,spending);
    CHECK(paid.Next().pool_balance==4500 && paid.Next().tree_size==4);
    const auto saved_batch=spending.Data();
    // Refuse a second transition without damaging caller-staged data.
    LookupReject(Status::Invalid,[&]{(void)StageFixture(db,token,next,spends,spending);});
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
    const auto replacement_block=Candidate(next,spends,{},1);
    next.block_hash=replacement_block.Header().GetHash();rocksdb::WriteBatch replacement;
    CHECK(next.block_hash!=paid.Next().block_hash);
    const auto replaced=StageOrchardBlockUnderChainstateLock(db,token,next,replacement_block,true,spends,replacement);
    CHECK(replaced.Next().frontier==paid.Next().frontier);
    Tip(db,next.block_hash,next.height,replacement);Commit(db,replacement);
    auto duplicate=next;duplicate.height++;duplicate.parent_hash=next.block_hash;duplicate.block_hash=H(5);
    // Fresh height authorization of the same transaction still hits the database nullifier gate.
    const std::vector<VerifiedOrchardAuthorizations> repeated{Authorized(base,true,20002)};
    rocksdb::WriteBatch failed; Tip(db,H(90),29999,failed); const auto untouched=failed.Data();
    StateReject(StateError::SpentNullifier,[&]{(void)StageFixture(db,token,duplicate,repeated,failed);});
    CHECK(failed.Data()==untouched);
    auto wrong=duplicate;wrong.parent_hash=H(99);
    StateReject(StateError::Context,[&]{(void)StageFixture(db,token,wrong,{},failed);});
    CHECK(failed.Data()==untouched);
    db.close();
    LookupReject(Status::Internal,[&]{(void)StageFixture(db,token,duplicate,{},failed);});
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
    LookupReject(Status::Corruption,[&]{(void)StageFixture(db,token,c,{},rejected);});
    CHECK(rejected.Data()==before);
}
static void Coverage(const std::string& base) {
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    rocksdb::WriteBatch initial;Tip(db,H(1),20000,initial);Commit(db,initial);
    const auto shield=Authorized(base,false,20000),spend=Authorized(base,true,20001);
    const std::vector<VerifiedOrchardAuthorizations> auth{shield}, extra{shield,spend}, wrong{spend};
    OrchardBlockContext c{20001,H(2),H(1),20001,Fixture(base).domain};
    const auto block=Candidate(c,auth);c.block_hash=block.Header().GetHash();
    rocksdb::WriteBatch batch;Tip(db,H(99),20001,batch);const auto before=batch.Data();
    for(const auto& list:std::vector<std::vector<VerifiedOrchardAuthorizations>>{{},extra,wrong}) {
        StateReject(StateError::AuthorizationCoverage,[&]{
            (void)StageOrchardBlockUnderChainstateLock(db,token,c,block,true,list,batch);
        });
        CHECK(batch.Data()==before);
    }
    auto mismatched=c;mismatched.block_hash=H(90);
    StateReject(StateError::Context,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,mismatched,block,true,auth,batch);});
    auto disabled=c;disabled.activation_height=UINT32_MAX;
    StateReject(StateError::Inactive,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,disabled,block,true,auth,batch);});
    auto body=block.WireBytes();body[36]^=1;
    const auto bad=OrchardBlockCandidate::DecodeExact(body);auto bad_context=c;bad_context.block_hash=bad.Header().GetHash();
    StateReject(StateError::BlockBody,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,bad_context,bad,true,auth,batch);});
    Transaction retired;retired.version=6;retired.SetExplicitFee(0);retired.shielded_bundle_bytes={1};
    const auto old=Candidate(c,auth,{retired});auto old_context=c;old_context.block_hash=old.Header().GetHash();
    StateReject(StateError::RetiredLegacyPool,[&]{(void)StageOrchardBlockUnderChainstateLock(db,token,old_context,old,true,auth,batch);});
    CHECK(batch.Data()==before && db.getOrchardState().status()==Status::NotFound);
}
static std::string StorageScriptHex(const Bytes& script) {
    std::ostringstream out;
    for(const auto byte:script)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
    return out.str();
}
static void AtomicCoins(const std::string& base) {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path)==Status::Ok);
    const auto auth=Authorized(base,false,20000);Fixture keys(base);
    OrchardBlockContext c{20001,H(2),H(1),20001,keys.domain};
    const auto& tx=auth.Transaction();
    const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
    UTXOEntry intermediate(AmountUna::Una(tx.Outputs()[0].amount_una),tx.Outputs()[0].script_pub_key,c.height,false);
    const auto child=Child(OutPoint(id,0),intermediate,keys);
    const auto block=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)});c.block_hash=block.Header().GetHash();
    rocksdb::WriteBatch initial;Tip(db,c.parent_hash,c.height-1,initial);
    for(size_t i=0;i<tx.Inputs().size();++i) {
        const auto point=Point(tx.Inputs()[i]);const auto& in=auth.Transparent().Snapshot().Coins()[i];
        Coin coin;coin.amount=in.value.GetUna();coin.script_pubkey=StorageScriptHex(in.scriptPubKey);
        coin.height=in.height;coin.coinbase=in.isCoinbase;
        CHECK(db.putCoin(token,point.txid.AsUint256(),point.vout,coin,&initial)==Status::Ok);
    }
    Commit(db,initial);
    const auto first_point=Point(tx.Inputs()[0]);
    const auto stored_first=RequiredValue(db.getCoin(first_point.txid.AsUint256(),first_point.vout));
    for(const std::string invalid:{std::string("0"),std::string("gg"),std::string(1,char(0xff))}) {
        auto malformed=stored_first;malformed.script_pubkey=invalid;
        CHECK(db.putCoin(token,first_point.txid.AsUint256(),first_point.vout,malformed)==Status::Ok);
        rocksdb::WriteBatch rejected;bool corruption=false;
        try{(void)StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,block,{},true,rejected);}
        catch(const OrchardCoinLookupError&e){corruption=e.SourceStatus()==Status::Corruption;}
        CHECK(corruption && rejected.Count()==0);
    }
    CHECK(db.putCoin(token,first_point.txid.AsUint256(),first_point.vout,stored_first)==Status::Ok);
    db.close();const auto original=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    {
        rocksdb::WriteBatch abandoned;
        const auto staged=StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,block,{},true,abandoned);
        CHECK(staged.coins.TotalFees()==789 && staged.orchard.Next().pool_balance==5000);
        CHECK(db.getOrchardState().status()==Status::NotFound);
        CHECK(db.getCoin(Point(tx.Inputs()[0]).txid.AsUint256(),tx.Inputs()[0].output_index).ok());
    }
    db.close();CHECK(Inspect(temp.path)==original);CHECK(db.init(temp.path)==Status::Ok);
    // An incompatible pre-existing conventional undo is a local failure AFTER
    // Orchard and coin writes were staged. All of those staged writes roll back.
    UndoRecord wrong;wrong.created.emplace_back(H(77),1);
    CHECK(db.putUndo(token,c.block_hash,wrong)==Status::Ok);
    rocksdb::WriteBatch rejected;
    LookupReject(Status::Corruption,[&]{(void)StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,block,{},true,rejected);});
    CHECK(rejected.Count()==0 && db.getOrchardState().status()==Status::NotFound);
    // Use a distinct honest candidate header so its conventional undo is absent.
    const auto accepted=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)},1);c.block_hash=accepted.Header().GetHash();
    rocksdb::WriteBatch batch;
    const auto staged=StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,accepted,{},true,batch);
    Tip(db,c.block_hash,c.height,batch);Commit(db,batch);
    db.close();CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getOrchardState())==staged.orchard.Next());
    CHECK(db.getCoin(Point(tx.Inputs()[0]).txid.AsUint256(),tx.Inputs()[0].output_index).status()==Status::NotFound);
    CHECK(db.getCoin(id.AsUint256(),0).status()==Status::NotFound); // Same-block parent output never persists.
    CHECK(RequiredValue(db.getCoin(id.AsUint256(),1)).script_pubkey==StorageScriptHex(tx.Outputs()[1].script_pub_key));
    CHECK(RequiredValue(db.getCoin(child.GetTxid().AsUint256(),0)).amount==intermediate.value.GetUna()-123);
    const auto undo=RequiredValue(db.getUndo(c.block_hash));
    CHECK(undo.spent.size()==2 && undo.created.size()==4);
    CHECK(std::none_of(undo.spent.begin(),undo.spent.end(),[&](const auto& coin){return coin.prev_txid==id.AsUint256();}));
    // Refuse composing over already-staged changes, preserving the caller batch.
    rocksdb::WriteBatch occupied;Tip(db,c.block_hash,c.height,occupied);
    const auto saved=occupied.Data();
    LookupReject(Status::Invalid,[&]{(void)StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,accepted,{},true,occupied);});
    CHECK(occupied.Data()==saved);
    // Validate current output data and exact conventional undo before rollback.
    const auto output=RequiredValue(db.getCoin(child.GetTxid().AsUint256(),0));
    auto damaged=output;damaged.amount++;
    CHECK(db.putCoin(token,child.GetTxid().AsUint256(),0,damaged)==Status::Ok);
    rocksdb::WriteBatch mismatch;
    LookupReject(Status::Corruption,[&]{StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(db,token,c,accepted,true,mismatch);});
    CHECK(mismatch.Count()==0);
    CHECK(db.putCoin(token,child.GetTxid().AsUint256(),0,output)==Status::Ok);
    auto extra_undo=undo;extra_undo.created.emplace_back(H(88),0);
    CHECK(db.putUndo(token,c.block_hash,extra_undo)==Status::Ok);
    LookupReject(Status::Corruption,[&]{StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(db,token,c,accepted,true,mismatch);});
    CHECK(mismatch.Count()==0);
    CHECK(db.putUndo(token,c.block_hash,undo)==Status::Ok);
    {
        rocksdb::WriteBatch abandoned;
        StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(db,token,c,accepted,true,abandoned);
        CHECK(RequiredValue(db.getOrchardState())==staged.orchard.Next());
        CHECK(db.getCoin(child.GetTxid().AsUint256(),0).ok());
    }
    rocksdb::WriteBatch disconnect;
    StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(db,token,c,accepted,true,disconnect);
    Tip(db,c.parent_hash,c.height-1,disconnect);Commit(db,disconnect);
    db.close();CHECK(db.init(temp.path)==Status::Ok);
    CHECK(db.getOrchardState().status()==Status::NotFound);
    CHECK(db.getCoin(child.GetTxid().AsUint256(),0).status()==Status::NotFound);
    CHECK(db.getCoin(id.AsUint256(),0).status()==Status::NotFound);
    CHECK(db.getCoin(id.AsUint256(),1).status()==Status::NotFound);
    for(size_t i=0;i<tx.Inputs().size();++i) {
        const auto point=Point(tx.Inputs()[i]);
        CHECK(RequiredValue(db.getCoin(point.txid.AsUint256(),point.vout)).amount==auth.Transparent().Snapshot().Coins()[i].value.GetUna());
    }
    rocksdb::WriteBatch reconnect;
    const auto again=StageOrchardBlockCoinsAndStateUnderChainstateLock(db,token,c,accepted,{},true,reconnect);
    Tip(db,c.block_hash,c.height,reconnect);Commit(db,reconnect);
    CHECK(again.orchard.Next()==staged.orchard.Next());
    CHECK(RequiredValue(db.getCoin(child.GetTxid().AsUint256(),0)).amount==output.amount);
}

static Rows ConsensusRows(const std::filesystem::path& path) {
    auto rows=Inspect(path);auto& tip=rows.at("meta").at("tip");
    // setTip appends a wall-clock diagnostic timestamp. Compare ALL other
    // bytes/keys across independent commits, and bound this field separately.
    CHECK(tip.size()==105);uint32_t timestamp=0;
    for(size_t i=0;i<4;++i)timestamp|=uint32_t(uint8_t(tip[101+i]))<<(8*i);
    const auto now=std::time(nullptr);CHECK(timestamp<=now && now-timestamp<600);
    std::fill(tip.begin()+101,tip.end(),0);return rows;
}
static void CrashLifecycle(const std::string& executable,const std::string& base,
    const std::filesystem::path& path,const OrchardBlockContext& context,
    const OrchardBlockCandidate& block,const BlockHeader& parent,const UtreexoForest& forest,bool checkpoint) {
    // Both stores are CLOSED before copying or spawning. The child execs a new
    // process: never call RocksDB/Rayon on inherited post-fork worker state.
    const auto before=ConsensusRows(path);TempDir oracle;
    std::filesystem::copy(path,oracle.path,std::filesystem::copy_options::recursive|
        std::filesystem::copy_options::overwrite_existing);
    ChainDB reference;CHECK(reference.init(oracle.path)==Status::Ok);
    rocksdb::WriteBatch batch;
    const auto restored=StageOrchardChainstateDisconnectUnderLock(reference,token,context,block,parent,forest,true,batch);
    Commit(reference,batch);reference.close();const auto disconnected=ConsensusRows(oracle.path);
    CHECK(reference.init(oracle.path)==Status::Ok);rocksdb::WriteBatch reconnect;
    (void)StageOrchardChainstateConnectUnderLock(reference,token,context,block,parent,restored,{},true,checkpoint,reconnect);
    Commit(reference,reconnect);reference.close();
    CHECK(ConsensusRows(oracle.path)==before);
    const auto block_file=path/"synthetic-orchard-block.bin";
    {std::ofstream file(block_file,std::ios::binary);const auto& bytes=block.WireBytes();
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());CHECK(file.good());}
    for(const auto& step:std::vector<std::pair<std::string,std::string>>{
        {"disconnect","pre"},{"disconnect","post"},{"connect","pre"},{"connect","post"}}) {
        std::vector<std::string> args{executable,"--crash-child",base,path.string(),block_file.string(),
            step.first,step.second,checkpoint?"1":"0"};
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        pid_t child=0;CHECK(posix_spawn(&child,executable.c_str(),nullptr,nullptr,argv.data(),environ)==0);
        int status=0;pid_t waited;do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
        CHECK(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==(step.second=="pre"?73:74));
        const bool connected=(step.first=="disconnect")== (step.second=="pre");
        CHECK(ConsensusRows(path)==(connected?before:disconnected));
        ChainDB reopened;CHECK(reopened.init(path)==Status::Ok);UtreexoForest recovered;std::string error;
        CHECK(storage::RestoreHistoricalForest(reopened,connected?context.height:context.height-1,recovered,error)==Status::Ok);
        CHECK(recovered.dumpInternalState()==(connected?forest:restored).dumpInternalState());
        if(connected)AuditOrchardChainstateTipUnderLock(reopened,token,context,parent,recovered,true);
        else CHECK(reopened.getOrchardState().status()==Status::NotFound);
        reopened.close();
        std::cout<<"Atomic crash boundary passed: "<<step.first<<" "<<step.second<<" checkpoint="<<checkpoint<<'\n';
    }
}
static void CrashChild(int argc,char** argv) {
    CHECK(argc==8);Fixture keys(argv[2]);ChainDB db;CHECK(db.init(argv[3])==Status::Ok);
    std::ifstream file(argv[4],std::ios::binary);CHECK(file.good());
    const Bytes bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    const auto block=OrchardBlockCandidate::DecodeExact(bytes);const auto& header=block.Header();
    const auto parent=RequiredValue(db.getHeader(header.prev_block_hash));
    OrchardBlockContext context{20001,header.GetHash(),header.prev_block_hash,20001,keys.domain};
    const bool connect=std::string(argv[5])=="connect",post=std::string(argv[6])=="post",checkpoint=std::string(argv[7])=="1";
    CHECK(connect || std::string(argv[5])=="disconnect");CHECK(post || std::string(argv[6])=="pre");
    UtreexoForest forest;std::string error;
    CHECK(storage::RestoreHistoricalForest(db,connect?20000:20001,forest,error)==Status::Ok);
    rocksdb::WriteBatch batch;
    if(connect)(void)StageOrchardChainstateConnectUnderLock(db,token,context,block,parent,forest,{},true,checkpoint,batch);
    else (void)StageOrchardChainstateDisconnectUnderLock(db,token,context,block,parent,forest,true,batch);
    if(post)Commit(db,batch);
    // Test subprocess only: no close, destructors, memory publication, or second
    // commit. A separate process must reopen and find exactly old or new rows.
    std::_Exit(post?74:73);
}
static void JournalContinuation(ChainDB& db,const OrchardBlockContext& previous,
    const OrchardBlockCandidate& parent,const UtreexoForest& parent_forest,bool checkpoint) {
    auto next=previous;++next.height;next.parent_hash=previous.block_hash;
    View view;view.height=previous.height;
    const auto uncommitted=CandidateWires(next,{},42);next.block_hash=uncommitted.Header().GetHash();
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(uncommitted,next,view,{},true);
    const auto draft=WithFilterHash(uncommitted,BuildOrchardBlockFilter(preliminary).GetHash());next.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,next,view,{},true);
    const auto transition=PrepareOrchardForestTransition(coins,parent.Header(),parent_forest);
    auto header=draft.Header();header.utreexo_root=transition.Root();
    auto bytes=draft.WireBytes();const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto child=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,parent_forest));
    next.block_hash=child.Header().GetHash();
    CHECK(db.putHeader(token,next.block_hash,child.Header(),next.height,arith_uint256(next.height))==Status::Ok);
    const auto parent_state=RequiredValue(db.getOrchardState());
    const std::string key="orchard_consensus_journal:v1:00004e21:"+previous.block_hash.GetHex();
    std::string record;CHECK(db.getRaw(key,record)==Status::Ok);
    rocksdb::WriteBatch remove;remove.Delete(key);Commit(db,remove);
    rocksdb::WriteBatch failed;
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,next,child,parent.Header(),parent_forest,{},true,checkpoint,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getOrchardState())==parent_state);
    rocksdb::WriteBatch repair;repair.Put(key,record);Commit(db,repair);
    rocksdb::WriteBatch connect;
    auto staged=StageOrchardChainstateConnectUnderLock(db,token,next,child,parent.Header(),parent_forest,{},true,checkpoint,connect);
    Commit(db,connect);
    CHECK(RequiredValue(db.getOrchardUndoParent(staged.block.orchard.Next()))==parent_state);
    AuditOrchardChainstateTipUnderLock(db,token,next,parent.Header(),staged.forest.After(),true);
    // A valid current record cannot authorize restoring a different/missing
    // parent's state. Check the saved parent record as part of reversal too.
    rocksdb::WriteBatch erase_parent;erase_parent.Delete(key);Commit(db,erase_parent);
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateDisconnectUnderLock(db,token,next,child,parent.Header(),staged.forest.After(),true,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getTip()).hash==next.block_hash);
    rocksdb::WriteBatch restore_parent;restore_parent.Put(key,record);Commit(db,restore_parent);
    rocksdb::WriteBatch disconnect;
    const auto restored=StageOrchardChainstateDisconnectUnderLock(db,token,next,child,parent.Header(),staged.forest.After(),true,disconnect);
    Commit(db,disconnect);
    CHECK(restored.dumpInternalState()==parent_forest.dumpInternalState());
    CHECK(RequiredValue(db.getOrchardState())==parent_state);
}
static void AtomicForest(const std::string& base,bool checkpoint,const std::string& crash_executable={}) {
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    Fixture keys(base);const auto auth=Authorized(base,false,20000);const auto& tx=auth.Transaction();
    View view;view.height=20000;UtreexoForest parent_forest;parent_forest.setCanonicalEmptyRoots(true);
    rocksdb::WriteBatch seed;
    for(size_t i=0;i<tx.Inputs().size();++i) {
        const auto point=Point(tx.Inputs()[i]);const auto coin=auth.Transparent().Snapshot().Coins()[i];
        view.coins.emplace(point,coin);
        CHECK(parent_forest.add(HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,
            coin.value.GetUna(),coin.scriptPubKey,coin.height,coin.isCoinbase))!=UINT64_MAX);
        Coin stored;stored.amount=coin.value.GetUna();stored.script_pubkey=StorageScriptHex(coin.scriptPubKey);
        stored.height=coin.height;stored.coinbase=coin.isCoinbase;
        CHECK(db.putCoin(token,point.txid.AsUint256(),point.vout,stored,&seed)==Status::Ok);
    }
    BlockHeader parent{};parent.version=1;parent.timestamp=20000;
    const auto parent_root=parent_forest.getCommitment();std::copy(parent_root.begin(),parent_root.end(),parent.utreexo_root.begin());
    OrchardBlockContext c{20001,H(2),parent.GetHash(),20001,keys.domain};
    const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
    const auto child=Child(OutPoint(id,0),UTXOEntry(AmountUna::Una(tx.Outputs()[0].amount_una),tx.Outputs()[0].script_pub_key,c.height,false),keys);
    const auto uncommitted=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)});c.block_hash=uncommitted.Header().GetHash();
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(uncommitted,c,view,{},true);
    const auto draft=WithFilterHash(uncommitted,BuildOrchardBlockFilter(preliminary).GetHash());c.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,c,view,{},true);
    const auto computed=PrepareOrchardForestTransition(coins,parent,parent_forest);
    auto header=draft.Header();header.utreexo_root=computed.Root();
    auto bytes=draft.WireBytes();const auto wire=header.SerializeForHash();std::copy(wire.begin(),wire.end(),bytes.begin());
    const auto block=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,parent_forest));c.block_hash=header.GetHash();
    auto no_filter_header=uncommitted.Header();
    no_filter_header.utreexo_root=PrepareOrchardForestTransition(preliminary,parent,parent_forest).Root();
    auto no_filter_bytes=uncommitted.WireBytes();const auto no_filter_prefix=no_filter_header.SerializeForHash();
    std::copy(no_filter_prefix.begin(),no_filter_prefix.end(),no_filter_bytes.begin());
    const auto no_filter_block=WithProof(OrchardBlockCandidate::DecodeExact(no_filter_bytes),MixedProof(preliminary,parent_forest));
    CHECK(db.putHeader(token,parent.GetHash(),parent,20000,arith_uint256(20000),&seed)==Status::Ok);
    CHECK(db.putHeader(token,c.block_hash,header,20001,arith_uint256(20001),&seed)==Status::Ok);
    CHECK(db.putHeader(token,no_filter_header.GetHash(),no_filter_header,20001,arith_uint256(20001),&seed)==Status::Ok);
    CHECK(db.putHeightIndex(token,20000,parent.GetHash(),&seed)==Status::Ok);
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),parent.utreexo_root},&seed)==Status::Ok);
    CHECK(db.putUtreexoCheckpointWithChecksum(token,20000,parent_forest.serialize(),&seed)==Status::Ok);
    Tip(db,parent.GetHash(),20000,seed);Commit(db,seed);
    db.close();const auto original=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    const auto before=parent_forest.dumpInternalState();
    ConsensusUTXOSet live;
    for (const auto& [point,coin]:view.coins) CHECK(live.AddCoin(point,coin));
    live.ReplaceForestGuarded(parent_forest);
    live.SetBestBlock(parent.GetHash(),c.height-1);

    { auto no_filter=c;no_filter.block_hash=no_filter_header.GetHash();
      rocksdb::WriteBatch rejected;bool failed=false;
      try{(void)StageOrchardChainstateConnectUnderLock(db,token,no_filter,no_filter_block,parent,parent_forest,{},true,checkpoint,rejected);}
      catch(const OrchardBlockCoinError& e){CHECK(e.Code()==OrchardBlockCoinErrorCode::Filter);failed=true;}
      CHECK(failed && rejected.Count()==0 && db.getOrchardState().status()==Status::NotFound);
    }

    {
        rocksdb::WriteBatch abandoned;
        const auto staged=StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,abandoned);
        CHECK(staged.forest.Root()==header.utreexo_root);
        CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
    }
    db.close();CHECK(Inspect(temp.path)==original);CHECK(db.init(temp.path)==Status::Ok);
    // Refuse an incomplete local state generation before staging the child.
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),H(98)})==Status::Ok);
    rocksdb::WriteBatch stale;
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),parent.utreexo_root})==Status::Ok);
    UtreexoForest missing;missing.setCanonicalEmptyRoots(true);
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,missing,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    const auto absent_proof=OrchardBlockCandidate::DecodeExact(bytes);
    StateReject(StateError::BlockBody,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,absent_proof,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.putTxIndex(token,id.AsUint256(),H(99),1)==Status::Ok);
    LookupReject(Status::AlreadyExists,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.deleteTxIndex(token,id.AsUint256())==Status::Ok);
    // A mismatching retained delta must roll back even coins/state already staged.
    const auto key=MakeUtreexoDeltaUndoKey(c.block_hash);
    rocksdb::WriteBatch poison;poison.Put(key,"invalid-delta");Commit(db,poison);
    rocksdb::WriteBatch failed;
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,failed);});
    CHECK(failed.Count()==0 && db.getOrchardState().status()==Status::NotFound);
    rocksdb::WriteBatch clear;clear.Delete(key);Commit(db,clear);
    rocksdb::WriteBatch connect;
    auto staged=StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,connect);
    CHECK(parent_forest.dumpInternalState()==before);
    std::vector<UTXOPublicationChange> memory_changes;
    for (const auto& change:staged.block.coins.Changes())
        memory_changes.push_back({change.outpoint,change.before,change.after});
    auto publication=PreparedUTXOPublication::PrepareUnderLock(live,c.height-1,parent.GetHash(),
        parent.utreexo_root,memory_changes,staged.forest.After(),c.height,c.block_hash,header.utreexo_root);
    publication.CheckReadyUnderLock();
    CHECK(live.GetBestBlock()==parent.GetHash());
    Commit(db,connect);
    std::move(publication).PublishAfterCommitUnderLock();
    CHECK(live.GetBestBlock()==c.block_hash && live.GetHeight()==c.height);
    CHECK(live.SnapshotForestCommitment()==staged.forest.After().getCommitment());
    for(const auto& change:memory_changes) {
        CHECK(live.HaveCoin(change.outpoint)==bool(change.after));
        if(change.after) CHECK(live.GetCoin(change.outpoint)->value==change.after->value);
    }
db.close();CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getTip()).hash==c.block_hash);
    CHECK(RequiredValue(db.getValidatedTip()).hash==c.block_hash);
    CHECK(RequiredValue(db.getForestTipMarker()).forest_root==header.utreexo_root);
    CHECK(RequiredValue(db.getOrchardState())==staged.block.orchard.Next());
    CHECK(ReadStoredOrchardBlock(db,c.block_hash,true).WireBytes()==block.WireBytes());
    CHECK(!db.getBlock(c.block_hash).ok()); // No implicit fallback to the legacy family.
    for(size_t i=0;i<block.Transactions().size();++i) {
        const auto location=RequiredValue(db.getTxLocation(block.Transactions()[i].GetTxid().AsUint256()));
        CHECK(location.first==c.block_hash && location.second==i);
    }
    UtreexoForest reopened;std::string error;
    CHECK(storage::RestoreHistoricalForest(db,c.height,reopened,error)==Status::Ok);
    CHECK(reopened.dumpInternalState()==staged.forest.After().dumpInternalState());
    AuditOrchardChainstateTipUnderLock(db,token,c,parent,reopened,true);
    const auto filter=BuildOrchardBlockFilter(staged.block.coins);
    CHECK(RequiredValue(db.getBlockFilter(c.block_hash)).data==filter.encoded_data);
    CHECK(RequiredValue(db.getBlockFilter(c.block_hash)).element_count==filter.element_count);
    // Count metadata is not part of GCSFilter::GetHash; audit must recompute it.
    CHECK(db.putBlockFilter(token,c.block_hash,filter.encoded_data,filter.element_count+1)==Status::Ok);
    LookupReject(Status::Corruption,[&]{AuditOrchardChainstateTipUnderLock(db,token,c,parent,reopened,true);});
    CHECK(db.putBlockFilter(token,c.block_hash,filter.encoded_data,filter.element_count)==Status::Ok);
    JournalContinuation(db,c,block,reopened,checkpoint);
    const std::string journal_key="orchard_consensus_journal:v1:00004e21:"+c.block_hash.GetHex();
    std::string journal;CHECK(db.getRaw(journal_key,journal)==Status::Ok && journal.size()==36);
    for(const std::string corrupt_record:{std::string{},std::string("DOC1")+std::string(32,'\0')}) {
        rocksdb::WriteBatch damage;
        if(corrupt_record.empty())damage.Delete(journal_key);else damage.Put(journal_key,corrupt_record);
        Commit(db,damage);
        LookupReject(Status::Corruption,[&]{AuditOrchardChainstateTipUnderLock(db,token,c,parent,reopened,true);});
    }
    rocksdb::WriteBatch restore_journal;restore_journal.Put(journal_key,journal);Commit(db,restore_journal);
    auto wrong_domain=c;wrong_domain.domain.branch_id++;
    LookupReject(Status::Corruption,[&]{AuditOrchardChainstateTipUnderLock(db,token,wrong_domain,parent,reopened,true);});
    auto wrong_activation=c;--wrong_activation.activation_height;
    LookupReject(Status::Corruption,[&]{AuditOrchardChainstateTipUnderLock(db,token,wrong_activation,parent,reopened,true);});
    db.close();const auto audit_before=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    AuditOrchardChainstateTipUnderLock(db,token,c,parent,reopened,true);
    db.close();CHECK(Inspect(temp.path)==audit_before);CHECK(db.init(temp.path)==Status::Ok);
    if(!crash_executable.empty()) {
        db.close();CrashLifecycle(crash_executable,base,temp.path,c,block,parent,reopened,checkpoint);
        CHECK(db.init(temp.path)==Status::Ok);
    }
    std::string delta;CHECK(db.getRaw(key,delta)==Status::Ok);
    rocksdb::WriteBatch corrupt;corrupt.Put(key,"truncated");Commit(db,corrupt);
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getTip()).hash==c.block_hash);
    rocksdb::WriteBatch repair;repair.Put(key,delta);Commit(db,repair);
    {
        rocksdb::WriteBatch abandoned;
        const auto undone=StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,abandoned);
        CHECK(undone.dumpInternalState()==before);
        CHECK(RequiredValue(db.getTip()).hash==c.block_hash);
    }
    rocksdb::WriteBatch disconnect;
    const auto restored=StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,disconnect);
    CHECK(restored.dumpInternalState()==before && reopened.dumpInternalState()==staged.forest.After().dumpInternalState());
    std::vector<UTXOPublicationChange> reverse_changes;
    for(const auto& change:memory_changes)
        reverse_changes.push_back({change.outpoint,change.after,change.before});
    auto rollback=PreparedUTXOPublication::PrepareUnderLock(live,c.height,c.block_hash,
        header.utreexo_root,reverse_changes,restored,c.height-1,parent.GetHash(),parent.utreexo_root);
    rollback.CheckReadyUnderLock();
    CHECK(live.GetBestBlock()==c.block_hash);
    Commit(db,disconnect);
    std::move(rollback).PublishAfterCommitUnderLock();
    CHECK(live.GetBestBlock()==parent.GetHash() && live.GetHeight()==c.height-1);
    CHECK(live.SnapshotForestCommitment()==parent_forest.getCommitment());
    for(const auto& change:reverse_changes) CHECK(live.HaveCoin(change.outpoint)==bool(change.after));
db.close();CHECK(db.init(temp.path)==Status::Ok);
    CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
    CHECK(RequiredValue(db.getValidatedTip()).hash==parent.GetHash());
    CHECK(RequiredValue(db.getForestTipMarker()).forest_root==parent.utreexo_root);
    CHECK(db.getBlockHashByHeight(c.height).status()==Status::NotFound);
    CHECK(db.getUtreexoCheckpoint(c.height).status()==Status::NotFound);
    CHECK(db.getOrchardState().status()==Status::NotFound);
    CHECK(db.getCoin(id.AsUint256(),0).status()==Status::NotFound);
    CHECK(db.getCoin(child.GetTxid().AsUint256(),0).status()==Status::NotFound);
    for(const auto& input:tx.Inputs())CHECK(db.getCoin(Point(input).txid.AsUint256(),input.output_index).ok());
    UtreexoForest restarted;
    CHECK(storage::RestoreHistoricalForest(db,20000,restarted,error)==Status::Ok);
    CHECK(restarted.dumpInternalState()==before);
    for(const auto& tx:block.Transactions())CHECK(db.getTxLocation(tx.GetTxid().AsUint256()).status()==Status::NotFound);
    CHECK(ReadStoredOrchardBlock(db,c.block_hash,true).WireBytes()==block.WireBytes());
    // Corrupt temporary stored bodies retain the header hash but must not be
    // accepted merely because the key/header exists. No live store is opened.
    auto altered=block.WireBytes();auto coinbase=block.Transactions()[0].Historical();
    coinbase.vout[0].value=AmountUna::Una(2);
    const auto changed_coinbase=Wire(coinbase);
    CHECK(changed_coinbase.size()==Wire(block.Transactions()[0].Historical()).size());
    std::copy(changed_coinbase.begin(),changed_coinbase.end(),altered.begin()+129);
    CHECK(OrchardBlockCandidate::DecodeExact(altered).Header().GetHash()==c.block_hash);
    CHECK(!OrchardBlockCandidate::DecodeExact(altered).MatchesTransactionRoot());
    for(const auto& bad:std::vector<Bytes>{Bytes{1,2,3},altered}) {
        db.close();
        {auto names=legacy;names.push_back(shielded_store_fixture::shielded);Raw raw(temp.path,names);
            raw.put("blocks","b"+c.block_hash.GetHex(),std::string(bad.begin(),bad.end()));}
        CHECK(db.init(temp.path)==Status::Ok);
        LookupReject(Status::Corruption,[&]{(void)ReadStoredOrchardBlock(db,c.block_hash,true);});
    }
    // Reconnect replaces the bad local body only with the fully prepared
    // candidate, in the same batch as its restored indexes and chainstate.
    // Reconnect uses the retained exact conventional and forest undo records.
    rocksdb::WriteBatch reconnect;
    const auto again=StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,restarted,{},true,checkpoint,reconnect);
    Commit(db,reconnect);
    CHECK(again.forest.After().dumpInternalState()==staged.forest.After().dumpInternalState());
    CHECK(ReadStoredOrchardBlock(db,c.block_hash,true).WireBytes()==block.WireBytes());
    CHECK(RequiredValue(db.getOrchardState())==staged.block.orchard.Next());
}
int main(int argc,char**argv) {
    try { SelectParams(Chain::REGTEST);
        if(argc>1 && std::string(argv[1])=="--crash-child") {CrashChild(argc,argv);return 2;}
        if(argc==3 && std::string(argv[1])=="--crash-lifecycle") {
            const auto executable=std::filesystem::absolute(argv[0]).string();
            AtomicForest(argv[2],false,executable);AtomicForest(argv[2],true,executable);return 0;
        }
        CHECK(argc==2);RoundTrip(argv[1]);CorruptParent(argv[1]);Coverage(argv[1]);AtomicCoins(argv[1]);AtomicForest(argv[1],false);AtomicForest(argv[1],true);
        std::cout<<"Orchard ChainDB staging: real frontier, anchors/nullifiers, reopen, branch replacement, abandonment and error separation passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
