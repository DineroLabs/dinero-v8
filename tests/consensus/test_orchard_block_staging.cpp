#include "orchard_forest_test_fixture.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/utxo_publication.h"
#include "daemon/orchard_chainstate_write.h"
#include "common/annotated_mutex.h"
#include "storage/block_storage.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/genesis_canonical.h"
#include "consensus/header_chain.h"
#include <thread>
#include <functional>
#include <type_traits>
#include "consensus/orchard_block_filter.h"
#include "consensus/orchard_state_root.h"
#include "consensus/state_commitment.h"
#include "consensus/shielded/shielded_root.h"
#include "../storage/shielded_store_fixture.h"
#include <iomanip>
#include <sstream>
#include "consensus/utreexo_delta_codec.h"
#include "storage/forest_restore.h"
#include "util/hex.h"
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
static CBlockIndex DiskIndex(const ChainDB& db,const BlockHeader& header,uint32_t height) {
    const auto m=RequiredValue(db.getHeaderMetadata(header.GetHash()));
    CBlockIndex i(header,height);i.chainwork=ChainworkToHex(m.chainwork);i.status=m.status_flags;
    i.file_number=m.file_number;i.data_pos=m.data_pos;i.data_size=m.data_size;
    i.undo_file=m.undo_file;i.undo_pos=m.undo_pos;i.undo_size=m.undo_size;return i;
}
static void CheckDiskIndex(const ChainDB& db,BlockStorage& files,const CBlockIndex& i,
                           const OrchardBlockCandidate& block) {
    const auto m=RequiredValue(db.getHeaderMetadata(i.hash));
    CHECK(m.status_flags==i.status && m.file_number==i.file_number && m.data_pos==i.data_pos &&
        m.data_size==i.data_size && m.undo_file==i.undo_file && m.undo_pos==i.undo_pos && m.undo_size==i.undo_size);
    CHECK((i.status & (BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO))==(BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO));
    const auto bytes=RequiredValue(files.readBlockBytes({i.file_number,i.data_pos,i.data_size}));
    CHECK(bytes==std::string(block.WireBytes().begin(),block.WireBytes().end()));
    CHECK(RequiredValue(files.readUndo({i.undo_file,i.undo_pos,i.undo_size}))==RequiredValue(db.getUndo(i.hash)).Serialize());
}

static storage::LegacyRetirementRecord FixtureRetirement(const OrchardBlockContext& c) {
    consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
    anchors.RecordRoot(c.activation_height-1,tree.Root());
    const auto root=tree.Root();uint256 tree_root,genesis;
    std::copy(root.begin(),root.end(),tree_root.begin());std::copy(c.domain.genesis_wire.begin(),c.domain.genesis_wire.end(),genesis.begin());
    const auto shr=consensus::shielded::ComputeShieldedRootFromParts({root.begin(),root.end()},tree.Size(),
        consensus::shielded::ComputeNullifierAccumulator({}),anchors.SerializeBytes());CHECK(shr);
    return {c.domain.network_code,genesis,c.domain.branch_id,c.activation_height,3,c.parent_hash,*shr,37,tree_root,0,0};
}
static void SeedFrozenLegacy(ChainDB& db,const OrchardBlockContext& c,rocksdb::WriteBatch& batch) {
    consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
    anchors.RecordRoot(c.activation_height-1,tree.Root());
    const auto frontier=tree.SerializeFrontier(),history=anchors.SerializePersistenceBytes();
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()},&batch)==Status::Ok);
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{history.begin(),history.end()},&batch)==Status::Ok);
    CHECK(db.deleteAllShieldedNullifiers(token,&batch).ok());
    CHECK(db.putShieldedTipMarker(token,{int32_t(c.height-1),c.parent_hash,FixtureRetirement(c).tree_root,0,0},&batch)==Status::Ok);
}
static StagedOrchardChainstate Connect(ChainDB& db,const ChainWriteToken& t,const OrchardBlockContext& c,
    const OrchardBlockCandidate& b,const BlockHeader& p,const UtreexoForest& f,const OrchardBranchMtpLookup& mtp,
    bool witness,bool checkpoint,rocksdb::WriteBatch& batch) {
    return StageOrchardChainstateConnectUnderLock(db,t,c,b,p,f,mtp,witness,checkpoint,batch,
        c.height==c.activation_height?std::optional(FixtureRetirement(c)):std::nullopt);
}
static OrchardBlockCandidate AppendStateScript(const OrchardBlockCandidate& block,const Bytes& script) {
    CHECK(!block.Utreexo());auto cb=block.Transactions()[0].Historical();cb.vout.emplace_back(AmountUna::Zero(),script);
    std::vector<Bytes> wires{Wire(cb)};std::vector<TxId> ids;
    for(size_t i=1;i<block.Transactions().size();++i)wires.push_back(block.Transactions()[i].Serialize(TxSerializationMode::WithWitness));
    for(const auto& wire:wires)ids.push_back(ParsedTransaction::DecodeExact(wire,TransactionReadMode::StagedOrchard).GetTxid());
    auto h=block.Header();h.merkle_root=ComputeTransactionMerkleRoot(ids);const auto prefix=h.SerializeForHash();Bytes bytes(prefix.begin(),prefix.end());bytes.push_back(wires.size());
    for(const auto& wire:wires)bytes.insert(bytes.end(),wire.begin(),wire.end());bytes.push_back(0);
    return OrchardBlockCandidate::DecodeExact(bytes);
}
static OrchardBlockCandidate WithStateRoot(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const PreparedOrchardBlockCoins& coins) {
    std::optional<storage::OrchardStoredState> parent;const auto stored=db.getOrchardState();
    if(stored.ok())parent=*stored;else CHECK(stored.status()==Status::NotFound);
    OrchardStateLookups lookup{
        [&](const uint256& a)->StatusOr<bool>{return db.getOrchardAnchorReferences(a).ok();},
        [&](const uint256& n)->StatusOr<bool>{return db.getOrchardNullifierOwner(n).ok();}};
    const auto prepared=PrepareOrchardStateTransition(c,parent,coins.Authorizations(),lookup);
    const auto record=parent?RequiredValue(db.getLegacyRetirementState()).record:FixtureRetirement(c);
    const auto sets=RequiredValue(db.previewOrchardCommitmentSets(parent,prepared.Next(),prepared.Nullifiers()));
    const auto root=ComputeOrchardStateRoot({c.domain,c.activation_height,c.height,c.parent_hash},record,prepared.Next(),sets);
    return AppendStateScript(block,BuildStateCommitmentScript(root,StateCommitmentEncoding::Orchard));
}
static void Tip(ChainDB& db, uint256 hash, uint32_t height, rocksdb::WriteBatch& batch) {
    CHECK(db.setTip(token, hash, height, arith_uint256(height), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, hash, height, &batch) == Status::Ok);
}
static void Commit(ChainDB& db, rocksdb::WriteBatch& batch) {
    CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
}
static UTXOEntry MemoryCoin(const Coin& coin) {
    Bytes script;CHECK(util::unhex(coin.script_pubkey,script));CHECK(coin.height>=0);
    return UTXOEntry(AmountUna::Una(coin.amount),script,uint32_t(coin.height),
        coin.coinbase,coin.is_confidential,coin.commitment);
}
static void CheckMemoryCoins(const ChainDB& db,const ConsensusUTXOSet& live) {
    size_t count=0;
    CHECK(db.forEachUTXO([&](const uint256& hash,uint32_t n,const Coin& stored) {
        ++count;const auto coin=live.GetCoin(OutPoint(TxId(hash),n));
        const auto expected=MemoryCoin(stored);CHECK(coin!=nullptr);
        CHECK(coin->value==expected.value && coin->scriptPubKey==expected.scriptPubKey &&
            coin->height==expected.height && coin->isCoinbase==expected.isCoinbase &&
            coin->is_confidential==expected.is_confidential && coin->commitment==expected.commitment);
        return true;
    })==Status::Ok);
    CHECK(live.GetSetSize()==count);
    const auto tip=RequiredValue(db.getTip());
    CHECK(live.GetBestBlock()==tip.hash && live.GetHeight()==uint32_t(tip.height));
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
    const OrchardBlockCandidate& block,const BlockHeader& parent,const UtreexoForest& forest,bool checkpoint,bool owned=false,bool indexed=false) {
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
    (void)Connect(reference,token,context,block,parent,restored.forest,{},true,checkpoint,reconnect);
    Commit(reference,reconnect);reference.close();
    CHECK(ConsensusRows(oracle.path)==before);
    const auto block_file=path/"synthetic-orchard-block.bin";
    {std::ofstream file(block_file,std::ios::binary);const auto& bytes=block.WireBytes();
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());CHECK(file.good());}
    const std::vector<std::pair<std::string,std::string>> steps = owned
        ? std::vector<std::pair<std::string,std::string>>{{"disconnect","failed"},{"disconnect","pre"},
            {"disconnect","published"},{"connect","failed"},{"connect","pre"},{"connect","published"}}
        : std::vector<std::pair<std::string,std::string>>{{"disconnect","pre"},{"disconnect","post"},
            {"connect","pre"},{"connect","post"},{"disconnect","published"},{"connect","published"}};
    for(const auto& step:steps) {
        std::vector<std::string> args{executable,indexed?"--indexed-child":owned?"--owner-child":"--crash-child",base,path.string(),block_file.string(),
            step.first,step.second,checkpoint?"1":"0"};
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        pid_t child=0;CHECK(posix_spawn(&child,executable.c_str(),nullptr,nullptr,argv.data(),environ)==0);
        int status=0;pid_t waited;do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
        CHECK(waited==child && WIFEXITED(status) &&
            WEXITSTATUS(status)==(step.second=="failed"?(indexed?77:76):step.second=="pre"?73:step.second=="post"?74:75));
        const bool connected=(step.first=="disconnect")== (step.second=="pre" || step.second=="failed");
        CHECK(ConsensusRows(path)==(connected?before:disconnected));
        ChainDB reopened;CHECK(reopened.init(path)==Status::Ok);UtreexoForest recovered;std::string error;
        CHECK(storage::RestoreHistoricalForest(reopened,connected?context.height:context.height-1,recovered,error)==Status::Ok);
        CHECK(recovered.dumpInternalState()==(connected?forest:restored.forest).dumpInternalState());
        if(connected)AuditOrchardChainstateTipUnderLock(reopened,token,context,parent,recovered,true);
        else CHECK(reopened.getOrchardState().status()==Status::NotFound);
        reopened.close();
        std::cout<<(owned?"Owned commit boundary passed: ":"Atomic crash boundary passed: ")<<step.first<<" "<<step.second<<" checkpoint="<<checkpoint<<'\n';
    }
}
static void IndexedInitialCrash(const std::string& executable,const std::string& base,
    const std::filesystem::path& source,const OrchardBlockContext& context,
    const OrchardBlockCandidate& block,const BlockHeader& parent,bool checkpoint) {
    const auto before=ConsensusRows(source);
    for(const std::string phase:{"pre","published"}) {
        TempDir copy;
        std::filesystem::copy(source,copy.path,std::filesystem::copy_options::recursive|
            std::filesystem::copy_options::overwrite_existing);
        const auto body=copy.path/"initial-orchard-block.bin";
        {std::ofstream file(body,std::ios::binary);const auto& bytes=block.WireBytes();
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());CHECK(file.good());}
        std::vector<std::string> args{executable,"--indexed-child",base,copy.path.string(),body.string(),
            "connect",phase,checkpoint?"1":"0"};
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        pid_t child=0;CHECK(posix_spawn(&child,executable.c_str(),nullptr,nullptr,argv.data(),environ)==0);
        int status=0;pid_t waited;do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
        CHECK(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==(phase=="pre"?73:75));
        if(phase=="pre")CHECK(ConsensusRows(copy.path)==before);
        ChainDB db;CHECK(db.init(copy.path)==Status::Ok);BlockStorage files;CHECK(files.init(copy.path)==Status::Ok);
        const auto index=DiskIndex(db,block.Header(),context.height);
        if(phase=="pre") {
            CHECK(!(index.status & (BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO)) && !index.data_size && !index.undo_size);
            CHECK(RequiredValue(db.getTip()).hash==context.parent_hash);
        } else {
            CheckDiskIndex(db,files,index,block);
            CHECK(RequiredValue(db.getTip()).hash==context.block_hash);
            UtreexoForest recovered;std::string error;
            CHECK(storage::RestoreHistoricalForest(db,context.height,recovered,error)==Status::Ok);
            AuditOrchardChainstateTipUnderLock(db,token,context,parent,recovered,true);
        }
        std::cout<<"Initial indexed crash boundary passed: "<<phase<<" checkpoint="<<checkpoint<<'\n';
    }
}
static void CrashChild(int argc,char** argv) {
    CHECK(argc==8);Fixture keys(argv[2]);ChainDB db;CHECK(db.init(argv[3])==Status::Ok);
    std::ifstream file(argv[4],std::ios::binary);CHECK(file.good());
    const Bytes bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    const auto block=OrchardBlockCandidate::DecodeExact(bytes);const auto& header=block.Header();
    const auto parent=RequiredValue(db.getHeader(header.prev_block_hash));
    OrchardBlockContext context{20001,header.GetHash(),header.prev_block_hash,20001,keys.domain};
    const bool connect=std::string(argv[5])=="connect",post=std::string(argv[6])=="post",
        published=std::string(argv[6])=="published",checkpoint=std::string(argv[7])=="1";
    CHECK(connect || std::string(argv[5])=="disconnect");CHECK(post || published || std::string(argv[6])=="pre" || std::string(argv[6])=="failed");
    UtreexoForest forest;std::string error;
    CHECK(storage::RestoreHistoricalForest(db,connect?20000:20001,forest,error)==Status::Ok);
    // No original connect result exists in this fresh process. Restore the
    // exact current memory view from persisted coins and the checked forest.
    ConsensusUTXOSet live;
    CHECK(db.forEachUTXO([&](const uint256& hash,uint32_t n,const Coin& coin) {
        CHECK(live.AddCoin(OutPoint(TxId(hash),n),MemoryCoin(coin)));return true;
    })==Status::Ok);
    live.ReplaceForestGuarded(forest);
    live.SetBestBlock(connect?context.parent_hash:context.block_hash,connect?20000:20001);
    CheckMemoryCoins(db,live);
    if (std::string(argv[1])=="--owner-child" || std::string(argv[1])=="--indexed-child") {
        AnnotatedRecursiveMutex activation;
        BlockStorage files;const bool indexed=std::string(argv[1])=="--indexed-child";
        CBlockIndex index;
        if(indexed){CHECK(files.init(argv[3])==Status::Ok);index=DiskIndex(db,header,context.height);}
        auto write = indexed ? (connect
            ? PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,files,index,live,context,
                block,parent,forest,{},true,checkpoint,FixtureRetirement(context))
            : PreparedOrchardChainstateWrite::DisconnectIndexed(activation,db,token,files,index,live,context,
                block,parent,forest,true)) : connect
            ? PreparedOrchardChainstateWrite::Connect(activation,db,token,live,context,
                block,parent,forest,{},true,checkpoint,FixtureRetirement(context))
            : PreparedOrchardChainstateWrite::Disconnect(activation,db,token,live,context,
                block,parent,forest,true);
        if (std::string(argv[6])=="failed") {
            // Temporary store only. Force writeBatch to return Internal; the
            // owner must fail stop, never return and continue on old memory.
            db.close();
            if(indexed) {
                // Indexed readiness reads metadata before entering Writing.
                // An unavailable DB must consume the owner without publication.
                bool refused=false;try{write->Commit();}catch(const OrchardStateLookupError&){refused=true;}
                CHECK(refused);
                bool consumed=false;try{write->Commit();}catch(const std::logic_error&){consumed=true;}
                CHECK(consumed);std::_Exit(77);
            }
            std::set_terminate([]{std::_Exit(76);});write->Commit();std::_Exit(2);
        }
        if (published) {write->Commit();CheckMemoryCoins(db,live);if(indexed)CheckDiskIndex(db,files,index,block);}
        std::_Exit(published?75:73);
    }
    rocksdb::WriteBatch batch;
    std::vector<UTXOPublicationChange> changes;UtreexoForest next_forest;
    if(connect) {
        auto staged=Connect(db,token,context,block,parent,forest,{},true,checkpoint,batch);
        for(const auto& change:staged.block.coins.Changes())changes.push_back({change.outpoint,change.before,change.after});
        next_forest=staged.forest.After();
    } else {
        auto staged=StageOrchardChainstateDisconnectUnderLock(db,token,context,block,parent,forest,true,batch);
        for(const auto& change:staged.coins)changes.push_back({change.outpoint,change.before,change.after});
        next_forest=std::move(staged.forest);
    }
    auto publication=PreparedUTXOPublication::PrepareUnderLock(live,connect?20000:20001,
        connect?context.parent_hash:context.block_hash,connect?parent.utreexo_root:header.utreexo_root,
        changes,std::move(next_forest),connect?20001:20000,
        connect?context.block_hash:context.parent_hash,connect?header.utreexo_root:parent.utreexo_root);
    publication.CheckReadyUnderLock();
    if(post || published)Commit(db,batch);
    if(published) {
        std::move(publication).PublishAfterCommitUnderLock();
        CheckMemoryCoins(db,live);
        const auto marker=RequiredValue(db.getForestTipMarker());
        const auto commitment=live.SnapshotForestCommitment();
        CHECK(Bytes(marker.forest_root.begin(),marker.forest_root.end())==commitment);
    }
    // Test subprocess only: no close/destructors. The parent reopens the store
    // at pre-commit, post-commit/pre-publication and post-publication boundaries.
    std::_Exit(published?75:post?74:73);
}
// Contextual regtest ancestry for actual service connection tests. This is
// generated from the canonical genesis, never injected at a claimed height.
static BlockHeader ServiceFixtureParent(const uint256& root,
    consensus::HeaderChainSelector* selector=nullptr) {
    auto parent=BuildCanonicalGenesis(Params()).header;
    if(selector) CHECK(selector->AddHeader(parent));
    const auto start=parent.timestamp;
    for(uint32_t h=1;h<=20000;++h) {
        BlockHeader next{};next.version=1;next.prev_block_hash=parent.GetHash();
        next.timestamp=start+120*h;next.difficulty=0x207fffff;
        if(h==20000)next.utreexo_root=root;
        if(selector) CHECK(selector->AddHeader(next));
        parent=next;
    }
    return parent;
}
static OrchardBlockCandidate WithParentTiming(const OrchardBlockCandidate& block,const BlockHeader& parent) {
    if(!parent.difficulty)return block;
    auto header=block.Header();header.timestamp=parent.timestamp+120;header.difficulty=parent.difficulty;
    auto bytes=block.WireBytes();const auto prefix=header.SerializeForHash();
    std::copy(prefix.begin(),prefix.end(),bytes.begin());return OrchardBlockCandidate::DecodeExact(bytes);
}
static void JournalContinuation(ChainDB& db,const OrchardBlockContext& previous,
    const OrchardBlockCandidate& parent,const UtreexoForest& parent_forest,bool checkpoint,
    const std::function<void(const OrchardBlockContext&,const OrchardBlockCandidate&,const UtreexoForest&)>& audit={},
    bool audit_disconnects=false) {
    auto next=previous;++next.height;next.parent_hash=previous.block_hash;
    View view;view.height=previous.height;
    const auto uncommitted=WithParentTiming(CandidateWires(next,{},42),parent.Header());next.block_hash=uncommitted.Header().GetHash();
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(uncommitted,next,view,{},true);
    const auto filtered=WithFilterHash(uncommitted,BuildOrchardBlockFilter(preliminary).GetHash());next.block_hash=filtered.Header().GetHash();
    const auto filter_coins=PrepareOrchardBlockCoinsUnderChainstateLock(filtered,next,view,{},true);
    const auto draft=WithStateRoot(db,next,filtered,filter_coins);next.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,next,view,{},true);
    const auto transition=PrepareOrchardForestTransition(coins,parent.Header(),parent_forest);
    auto header=draft.Header();header.utreexo_root=transition.Root();
    auto bytes=draft.WireBytes();const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto child=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,parent_forest));
    next.block_hash=child.Header().GetHash();
    CHECK(db.putHeader(token,next.block_hash,child.Header(),next.height,parent.Header().difficulty ? RequiredValue(db.getBlockWork(previous.block_hash))+GetBlockProof(child.Header().difficulty) : arith_uint256(next.height))==Status::Ok);
    const auto parent_state=RequiredValue(db.getOrchardState());
    const std::string key="orchard_consensus_journal:v1:00004e21:"+previous.block_hash.GetHex();
    std::string record;CHECK(db.getRaw(key,record)==Status::Ok);
    rocksdb::WriteBatch remove;remove.Delete(key);Commit(db,remove);
    rocksdb::WriteBatch failed;
    LookupReject(Status::Corruption,[&]{(void)Connect(db,token,next,child,parent.Header(),parent_forest,{},true,checkpoint,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getOrchardState())==parent_state);
    rocksdb::WriteBatch repair;repair.Put(key,record);Commit(db,repair);
    rocksdb::WriteBatch connect;
    auto staged=Connect(db,token,next,child,parent.Header(),parent_forest,{},true,checkpoint,connect);
    Commit(db,connect);
    CHECK(RequiredValue(db.getOrchardUndoParent(staged.block.orchard.Next()))==parent_state);
    AuditOrchardChainstateTipUnderLock(db,token,next,parent.Header(),staged.forest.After(),true);
    if(audit)audit(next,child,staged.forest.After());
    if(audit_disconnects) {
        CHECK(RequiredValue(db.getTip()).hash==previous.block_hash);
        CHECK(RequiredValue(db.getOrchardState())==parent_state);
        return;
    }
    // A valid current record cannot authorize restoring a different/missing
    // parent's state. Check the saved parent record as part of reversal too.
    rocksdb::WriteBatch erase_parent;erase_parent.Delete(key);Commit(db,erase_parent);
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateDisconnectUnderLock(db,token,next,child,parent.Header(),staged.forest.After(),true,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getTip()).hash==next.block_hash);
    rocksdb::WriteBatch restore_parent;restore_parent.Put(key,record);Commit(db,restore_parent);
    rocksdb::WriteBatch disconnect;
    const auto restored=StageOrchardChainstateDisconnectUnderLock(db,token,next,child,parent.Header(),staged.forest.After(),true,disconnect);
    Commit(db,disconnect);
    CHECK(restored.forest.dumpInternalState()==parent_forest.dumpInternalState());
    CHECK(RequiredValue(db.getOrchardState())==parent_state);
}
static void AtomicForest(const std::string& base,bool checkpoint,const std::string& crash_executable={},bool owned_write=false,bool indexed=false,
    const std::function<void(ChainDB&,const OrchardBlockContext&,const OrchardBlockCandidate&,const UtreexoForest&,const std::filesystem::path&)>& startup_check={},bool contextual_headers=false) {
    AnnotatedRecursiveMutex activation;
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    BlockStorage files;CBlockIndex disk_index;
    if(indexed)CHECK(files.init(temp.path)==Status::Ok);
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
    if(contextual_headers)parent=ServiceFixtureParent(parent.utreexo_root);
    const auto parent_work=contextual_headers ? GetBlockProof(BuildCanonicalGenesis(Params()).header.difficulty) : arith_uint256(20000);
    auto full_parent_work=parent_work;
    if(contextual_headers)for(unsigned i=0;i<20000;++i)full_parent_work+=GetBlockProof(parent.difficulty);
    const auto child_work=contextual_headers ? full_parent_work+GetBlockProof(parent.difficulty) : arith_uint256(20001);
    OrchardBlockContext c{20001,H(2),parent.GetHash(),20001,keys.domain};
    const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
    const auto child=Child(OutPoint(id,0),UTXOEntry(AmountUna::Una(tx.Outputs()[0].amount_una),tx.Outputs()[0].script_pub_key,c.height,false),keys);
    const auto uncommitted=WithParentTiming(CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)}),parent);c.block_hash=uncommitted.Header().GetHash();
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(uncommitted,c,view,{},true);
    const auto filtered=WithFilterHash(uncommitted,BuildOrchardBlockFilter(preliminary).GetHash());c.block_hash=filtered.Header().GetHash();
    const auto filter_coins=PrepareOrchardBlockCoinsUnderChainstateLock(filtered,c,view,{},true);
    const auto draft=WithStateRoot(db,c,filtered,filter_coins);c.block_hash=draft.Header().GetHash();
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
    CHECK(db.putHeader(token,parent.GetHash(),parent,20000,full_parent_work,&seed)==Status::Ok);
    CHECK(db.putHeader(token,c.block_hash,header,20001,child_work,&seed)==Status::Ok);
    CHECK(db.putHeader(token,no_filter_header.GetHash(),no_filter_header,20001,child_work,&seed)==Status::Ok);
    CHECK(db.putHeightIndex(token,20000,parent.GetHash(),&seed)==Status::Ok);
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),parent.utreexo_root},&seed)==Status::Ok);
    CHECK(db.putUtreexoCheckpointWithChecksum(token,20000,parent_forest.serialize(),&seed)==Status::Ok);
    Tip(db,parent.GetHash(),20000,seed);CHECK(db.setTip(token,parent.GetHash(),20000,full_parent_work,&seed)==Status::Ok);SeedFrozenLegacy(db,c,seed);Commit(db,seed);
    if(indexed) {
        ChainDB::PersistedHeaderMetadata m;m.height=c.height;m.parent_hash=c.parent_hash;
        m.chainwork=RequiredValue(db.getBlockWork(c.block_hash));m.status_flags=BLOCK_VALID_HEADER;
        CHECK(db.putHeaderMetadata(token,c.block_hash,m)==Status::Ok);
        disk_index=DiskIndex(db,header,c.height);
    }

    db.close();auto original=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    const auto before=parent_forest.dumpInternalState();
    ConsensusUTXOSet live;
    for (const auto& [point,coin]:view.coins) CHECK(live.AddCoin(point,coin));
    live.ReplaceForestGuarded(parent_forest);
    live.SetBestBlock(parent.GetHash(),c.height-1);

    { auto no_filter=c;no_filter.block_hash=no_filter_header.GetHash();
      rocksdb::WriteBatch rejected;bool failed=false;
      try{(void)Connect(db,token,no_filter,no_filter_block,parent,parent_forest,{},true,checkpoint,rejected);}
      catch(const OrchardBlockCoinError& e){CHECK(e.Code()==OrchardBlockCoinErrorCode::Filter);failed=true;}
      CHECK(failed && rejected.Count()==0 && db.getOrchardState().status()==Status::NotFound);
    }

    // All other body/forest/proof obligations remain valid for each rejected
    // commitment candidate. Recompute the changed coinbase's forest identity.
    for(int kind=0;kind<4;++kind) {
        const auto draft_bad=[&] {
            if(kind==1)return AppendStateScript(filtered,BuildStateCommitmentScript(H(99),StateCommitmentEncoding::Orchard));
            if(kind==2)return AppendStateScript(filtered,BuildStateCommitmentScript(H(99),StateCommitmentEncoding::Legacy));
            if(kind==3)return AppendStateScript(draft,BuildStateCommitmentScript(H(99),StateCommitmentEncoding::Orchard));
            return filtered;
        }();
        auto bad_context=c;bad_context.block_hash=draft_bad.Header().GetHash();
        const auto bad_coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft_bad,bad_context,view,{},true);
        auto bad_header=draft_bad.Header();bad_header.utreexo_root=PrepareOrchardForestTransition(bad_coins,parent,parent_forest).Root();
        auto wire_bad=draft_bad.WireBytes();const auto header_bytes=bad_header.SerializeForHash();
        std::copy(header_bytes.begin(),header_bytes.end(),wire_bad.begin());
        const auto candidate=WithProof(OrchardBlockCandidate::DecodeExact(wire_bad),MixedProof(bad_coins,parent_forest));
        bad_context.block_hash=bad_header.GetHash();
        CHECK(db.putHeader(token,bad_context.block_hash,bad_header,c.height,child_work)==Status::Ok);
        rocksdb::WriteBatch rejected;
        StateReject(StateError::StateCommitment,[&]{(void)Connect(db,token,bad_context,candidate,parent,parent_forest,{},true,checkpoint,rejected);});
        CHECK(rejected.Count()==0 && db.getLegacyRetirementState().status()==Status::NotFound && db.getOrchardState().status()==Status::NotFound);
    }
    db.close();original=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    {rocksdb::WriteBatch rejected;auto wrong_amount=FixtureRetirement(c);++wrong_amount.retired_value;
     StateReject(StateError::StateCommitment,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,rejected,wrong_amount);});
     CHECK(rejected.Count()==0);}
    // The boundary value/epoch source cannot be silently omitted.
    {rocksdb::WriteBatch rejected;
     LookupReject(Status::Invalid,[&]{(void)StageOrchardChainstateConnectUnderLock(db,token,c,block,parent,parent_forest,{},true,checkpoint,rejected);});
     CHECK(rejected.Count()==0);}
    {
        rocksdb::WriteBatch abandoned;
        const auto staged=Connect(db,token,c,block,parent,parent_forest,{},true,checkpoint,abandoned);
        CHECK(staged.forest.Root()==header.utreexo_root);
        CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
    }
    db.close();CHECK(Inspect(temp.path)==original);CHECK(db.init(temp.path)==Status::Ok);
    // Refuse an incomplete local state generation before staging the child.
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),H(98)})==Status::Ok);
    rocksdb::WriteBatch stale;
    LookupReject(Status::Corruption,[&]{(void)Connect(db,token,c,block,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),parent.utreexo_root})==Status::Ok);
    UtreexoForest missing;missing.setCanonicalEmptyRoots(true);
    LookupReject(Status::Corruption,[&]{(void)Connect(db,token,c,block,parent,missing,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    const auto absent_proof=OrchardBlockCandidate::DecodeExact(bytes);
    StateReject(StateError::BlockBody,[&]{(void)Connect(db,token,c,absent_proof,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.putTxIndex(token,id.AsUint256(),H(99),1)==Status::Ok);
    LookupReject(Status::AlreadyExists,[&]{(void)Connect(db,token,c,block,parent,parent_forest,{},true,checkpoint,stale);});
    CHECK(stale.Count()==0);
    CHECK(db.deleteTxIndex(token,id.AsUint256())==Status::Ok);
    // A mismatching retained delta must roll back even coins/state already staged.
    const auto key=MakeUtreexoDeltaUndoKey(c.block_hash);
    rocksdb::WriteBatch poison;poison.Put(key,"invalid-delta");Commit(db,poison);
    rocksdb::WriteBatch failed;
    LookupReject(Status::Corruption,[&]{(void)Connect(db,token,c,block,parent,parent_forest,{},true,checkpoint,failed);});
    CHECK(failed.Count()==0 && db.getOrchardState().status()==Status::NotFound);
    rocksdb::WriteBatch clear;clear.Delete(key);Commit(db,clear);
    rocksdb::WriteBatch connect;
    auto staged=Connect(db,token,c,block,parent,parent_forest,{},true,checkpoint,connect);
    CHECK(parent_forest.dumpInternalState()==before);
    std::vector<UTXOPublicationChange> memory_changes;
    for (const auto& change:staged.block.coins.Changes())
        memory_changes.push_back({change.outpoint,change.before,change.after});
    auto publication=PreparedUTXOPublication::PrepareUnderLock(live,c.height-1,parent.GetHash(),
        parent.utreexo_root,memory_changes,staged.forest.After(),c.height,c.block_hash,header.utreexo_root);
    publication.CheckReadyUnderLock();
    CHECK(live.GetBestBlock()==parent.GetHash());
    if (owned_write) {
        static_assert(!std::is_move_constructible_v<PreparedOrchardChainstateWrite>);
        static_assert(!std::is_copy_constructible_v<PreparedOrchardChainstateWrite>);
        const auto prepare = [&] {
            if(indexed)return PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,files,disk_index,live,c,
                block,parent,parent_forest,{},true,checkpoint,FixtureRetirement(c));
            return PreparedOrchardChainstateWrite::Connect(activation,db,token,live,c,
                block,parent,parent_forest,{},true,checkpoint,FixtureRetirement(c));
        };
        // Abandonment releases the lock and changes no durable/logical state.
        { auto abandoned=prepare();CHECK(activation.HeldByCurrentThread()); }
        CHECK(!activation.HeldByCurrentThread());CheckMemoryCoins(db,live);
        CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
        if(indexed) {
            CHECK(disk_index.data_size==0 && disk_index.undo_size==0);
            CHECK(RequiredValue(db.getHeaderMetadata(c.block_hash)).data_size==0);
            // Mismatched in-memory locator is rejected without publishing it.
            disk_index.data_size=1;bool rejected=false;
            try{(void)prepare();}catch(const OrchardStateLookupError&){rejected=true;}
            CHECK(rejected);disk_index.data_size=0;CHECK(!activation.HeldByCurrentThread());
            {
                auto stale=prepare();const auto old_status=disk_index.status;
                disk_index.status|=BLOCK_IN_FLIGHT;
                bool refused=false;try{stale->Commit();}catch(const OrchardStateLookupError&){refused=true;}
                CHECK(refused && RequiredValue(db.getTip()).hash==parent.GetHash());
                disk_index.status=old_status;
            }
            db.close();files.close();
            IndexedInitialCrash(crash_executable,base,temp.path,c,block,parent,checkpoint);
            CHECK(db.init(temp.path)==Status::Ok && files.init(temp.path)==Status::Ok);
            disk_index=DiskIndex(db,header,c.height);
        }
        {
            auto aborted=prepare();
            live.SetBestBlock(H(111),c.height-1); // simulate a violated host contract
            bool refused=false;try{aborted->Commit();}catch(const std::exception&){refused=true;}
            CHECK(refused && RequiredValue(db.getTip()).hash==parent.GetHash());
            live.SetBestBlock(parent.GetHash(),c.height-1);
            refused=false;try{aborted->Commit();}catch(const std::logic_error&){refused=true;}
            CHECK(refused); // an aborted transaction can never be retried
        }
        auto write=prepare();
        bool excluded=false,wrong_thread=false;
        std::thread contender([&] {
            excluded=!activation.try_lock();if(!excluded)activation.unlock();
            try{write->Commit();}catch(const std::logic_error&){wrong_thread=true;}
        });contender.join();CHECK(excluded && wrong_thread);
        CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
        write->Commit();CHECK(activation.HeldByCurrentThread());
        if(indexed)CheckDiskIndex(db,files,disk_index,block);
        bool duplicate=false;try{write->Commit();}catch(const std::logic_error&){duplicate=true;}
        CHECK(duplicate);write.reset();CHECK(!activation.HeldByCurrentThread());
        CheckMemoryCoins(db,live);
    } else {
        Commit(db,connect);
        std::move(publication).PublishAfterCommitUnderLock();
    }
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
    const auto retired=RequiredValue(db.getLegacyRetirementState());
    CHECK(retired.record==FixtureRetirement(c) && retired.height==c.height && retired.block_hash==c.block_hash);
    CHECK(RequiredValue(db.getShieldedTipMarker()).block_hash==c.block_hash);
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
    if(startup_check) { startup_check(db,c,block,reopened,temp.path); return; }
    // Frozen legacy anchor contents are checked on startup/disconnect.
    const auto legacy_anchors=RequiredValue(db.getShieldedState(ChainDB::ShieldedStateRecord::AnchorHistory));
    consensus::shielded::AnchorHistory changed_anchors;
    CHECK(changed_anchors.DeserializePersistenceBytes({legacy_anchors.begin(),legacy_anchors.end()})==consensus::shielded::AnchorHistory::IoResult::Ok);
    consensus::shielded::CommitmentTree empty_tree;changed_anchors.RecordRoot(c.height,empty_tree.Root());
    const auto changed_bytes=changed_anchors.SerializePersistenceBytes();
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{changed_bytes.begin(),changed_bytes.end()})==Status::Ok);
    LookupReject(Status::Corruption,[&]{AuditOrchardChainstateTipUnderLock(db,token,c,parent,reopened,true);});
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,legacy_anchors)==Status::Ok);
    // Reopening must not trust a structurally valid coin undo whose value no
    // longer agrees with the authenticated parent forest's restored leaf.
    const auto saved_undo=RequiredValue(db.getUndo(c.block_hash));
    CHECK(!saved_undo.spent.empty());
    auto inconsistent_undo=saved_undo;++inconsistent_undo.spent[0].value;
    CHECK(db.putUndo(token,c.block_hash,inconsistent_undo)==Status::Ok);
    rocksdb::WriteBatch inconsistent;
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateDisconnectUnderLock(
        db,token,c,block,parent,reopened,true,inconsistent);});
    CHECK(inconsistent.Count()==0 && RequiredValue(db.getTip()).hash==c.block_hash);
    CHECK(db.putUndo(token,c.block_hash,saved_undo)==Status::Ok);
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
        db.close();if(indexed)files.close();
        CrashLifecycle(crash_executable,base,temp.path,c,block,parent,reopened,checkpoint,owned_write,indexed);
        CHECK(db.init(temp.path)==Status::Ok);
        if(indexed){CHECK(files.init(temp.path)==Status::Ok);disk_index=DiskIndex(db,header,c.height);}
    }
    std::string delta;CHECK(db.getRaw(key,delta)==Status::Ok);
    rocksdb::WriteBatch corrupt;corrupt.Put(key,"truncated");Commit(db,corrupt);
    LookupReject(Status::Corruption,[&]{(void)StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,failed);});
    CHECK(failed.Count()==0 && RequiredValue(db.getTip()).hash==c.block_hash);
    rocksdb::WriteBatch repair;repair.Put(key,delta);Commit(db,repair);
    {
        rocksdb::WriteBatch abandoned;
        const auto undone=StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,abandoned);
        CHECK(undone.forest.dumpInternalState()==before);
        CHECK(RequiredValue(db.getTip()).hash==c.block_hash);
    }
    rocksdb::WriteBatch disconnect;
    const auto restored=StageOrchardChainstateDisconnectUnderLock(db,token,c,block,parent,reopened,true,disconnect);
    CHECK(restored.forest.dumpInternalState()==before && reopened.dumpInternalState()==staged.forest.After().dumpInternalState());
    std::vector<UTXOPublicationChange> reverse_changes;
    for(const auto& change:restored.coins)
        reverse_changes.push_back({change.outpoint,change.before,change.after});
    auto rollback=PreparedUTXOPublication::PrepareUnderLock(live,c.height,c.block_hash,
        header.utreexo_root,reverse_changes,restored.forest,c.height-1,parent.GetHash(),parent.utreexo_root);
    rollback.CheckReadyUnderLock();
    CHECK(live.GetBestBlock()==c.block_hash);
    if (owned_write) {
        const auto prepare_disconnect=[&] {
            if(indexed)return PreparedOrchardChainstateWrite::DisconnectIndexed(activation,db,token,files,disk_index,
                live,c,block,parent,reopened,true);
            return PreparedOrchardChainstateWrite::Disconnect(activation,db,token,
                live,c,block,parent,reopened,true);
        };
        if(indexed) {
            const auto wrong=RequiredValue(files.writeUndo(c.block_hash,Bytes{0,0,0,0}));
            auto m=RequiredValue(db.getHeaderMetadata(c.block_hash));const auto good=m;
            m.undo_file=wrong.file_number;m.undo_pos=wrong.offset;m.undo_size=wrong.size;
            CHECK(db.putHeaderMetadata(token,c.block_hash,m)==Status::Ok);disk_index=DiskIndex(db,header,c.height);
            bool refused=false;try{(void)prepare_disconnect();}catch(const OrchardStateLookupError&){refused=true;}
            CHECK(refused && RequiredValue(db.getTip()).hash==c.block_hash);CheckMemoryCoins(db,live);
            CHECK(db.putHeaderMetadata(token,c.block_hash,good)==Status::Ok);disk_index=DiskIndex(db,header,c.height);
            CheckDiskIndex(db,files,disk_index,block);
            // A checksum-valid flatfile body with the same header still must
            // equal the fully authenticated candidate used for rollback.
            auto changed=block.WireBytes();changed.back()^=1;
            const auto other=RequiredValue(files.writeBlockBytes(c.block_hash,
                std::string(changed.begin(),changed.end())));
            m=good;m.file_number=other.file_number;m.data_pos=other.offset;m.data_size=other.size;
            CHECK(db.putHeaderMetadata(token,c.block_hash,m)==Status::Ok);disk_index=DiskIndex(db,header,c.height);
            refused=false;try{(void)prepare_disconnect();}catch(const OrchardStateLookupError&){refused=true;}
            CHECK(refused && RequiredValue(db.getTip()).hash==c.block_hash);CheckMemoryCoins(db,live);
            CHECK(db.putHeaderMetadata(token,c.block_hash,good)==Status::Ok);disk_index=DiskIndex(db,header,c.height);
            CheckDiskIndex(db,files,disk_index,block);
        }
        { auto abandoned=prepare_disconnect(); }
        CHECK(!activation.HeldByCurrentThread());CheckMemoryCoins(db,live);
        CHECK(RequiredValue(db.getTip()).hash==c.block_hash);
        auto write=prepare_disconnect();
        write->Commit();CheckMemoryCoins(db,live);
        if(indexed)CheckDiskIndex(db,files,disk_index,block);
        write.reset();CHECK(!activation.HeldByCurrentThread());
    } else {
        Commit(db,disconnect);
        std::move(rollback).PublishAfterCommitUnderLock();
    }
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
    CHECK(db.getLegacyRetirementState().status()==Status::NotFound);
    CHECK(RequiredValue(db.getShieldedTipMarker()).block_hash==parent.GetHash());
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
    const auto again=Connect(db,token,c,block,parent,restarted,{},true,checkpoint,reconnect);
    Commit(db,reconnect);
    CHECK(again.forest.After().dumpInternalState()==staged.forest.After().dumpInternalState());
    CHECK(ReadStoredOrchardBlock(db,c.block_hash,true).WireBytes()==block.WireBytes());
    CHECK(RequiredValue(db.getOrchardState())==staged.block.orchard.Next());
}
#ifdef DINERO_TEST_ORCHARD_SERVICE_STARTUP
#include "../daemon/orchard_service_startup_checks.h"
#endif
int main(int argc,char**argv) {
    try { SelectParams(Chain::REGTEST);
#ifdef DINERO_TEST_ORCHARD_SERVICE_STARTUP
        if(argc==3 && std::string(argv[1])=="--service-startup") {
            AtomicForest(argv[2],false,{},false,false,ServiceStartupChecks);
            AtomicForest(argv[2],true,{},false,false,ServiceStartupChecks);
            std::cout<<"OrchardServiceStartup PASS\n";return 0;
        }
        if(argc==3 && std::string(argv[1])=="--service-reorg-plan") {
            AtomicForest(argv[2],false,{},false,false,ServiceReorgPlanChecks,true);
            AtomicForest(argv[2],true,{},false,false,ServiceReorgPlanChecks,true);
            std::cout<<"OrchardServiceReorgPlan PASS\n";return 0;
        }
        if(argc==3 && std::string(argv[1])=="--service-fork-point") {
            AtomicForest(argv[2],false,{},false,false,ServiceForkPointChecks,true);
            AtomicForest(argv[2],true,{},false,false,ServiceForkPointChecks,true);
            std::cout<<"OrchardServiceForkPoint PASS\n";return 0;
        }
        if(argc==3 && std::string(argv[1])=="--service-connect") {
            AtomicForest(argv[2],false,{},false,false,ServiceConnectChecks,true);
            AtomicForest(argv[2],true,{},false,false,ServiceConnectChecks,true);
            std::cout<<"OrchardServiceConnect PASS\n";return 0;
        }
        if(argc==3 && std::string(argv[1])=="--service-disconnect") {
        AtomicForest(argv[2],false,{},false,false,ServiceDisconnectChecks);
        AtomicForest(argv[2],true,{},false,false,ServiceDisconnectChecks);
        std::cout<<"OrchardServiceDisconnect PASS\n";return 0;
    }
    if(argc==3 && std::string(argv[1])=="--service-undo-coverage") {
            AtomicForest(argv[2],false,{},false,false,ServiceUndoCoverageChecks);
            AtomicForest(argv[2],true,{},false,false,ServiceUndoCoverageChecks);
            std::cout<<"OrchardServiceUndoCoverage PASS\n";return 0;
        }
#endif
        if(argc>1 && (std::string(argv[1])=="--crash-child" || std::string(argv[1])=="--owner-child" || std::string(argv[1])=="--indexed-child")) {CrashChild(argc,argv);return 2;}
        if(argc==3 && std::string(argv[1])=="--crash-lifecycle") {
            const auto executable=std::filesystem::absolute(argv[0]).string();
            AtomicForest(argv[2],false,executable);AtomicForest(argv[2],true,executable);return 0;
        }
        if(argc==3 && std::string(argv[1])=="--indexed-commit") {
            const auto executable=std::filesystem::absolute(argv[0]).string();
            AtomicForest(argv[2],false,executable,true,true);AtomicForest(argv[2],true,executable,true,true);
            std::cout<<"Indexed atomic writes: durable body/undo, locator publication and restart passed\n";return 0;
        }
        if(argc==3 && std::string(argv[1])=="--commit-owner") {
            const auto executable=std::filesystem::absolute(argv[0]).string();
            AtomicForest(argv[2],false,executable,true);AtomicForest(argv[2],true,executable,true);
            std::cout<<"Owned atomic writes: connect/disconnect, abandonment, stale memory, thread exclusion and single-use passed\n";
            return 0;
        }
        CHECK(argc==2);RoundTrip(argv[1]);CorruptParent(argv[1]);Coverage(argv[1]);AtomicCoins(argv[1]);AtomicForest(argv[1],false);AtomicForest(argv[1],true);
        std::cout<<"Orchard ChainDB staging: real frontier, anchors/nullifiers, reopen, branch replacement, abandonment and error separation passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
