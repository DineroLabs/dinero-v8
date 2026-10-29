// Sequential durable-prefix coverage. Generated storage and honest proof
// fixtures exercise the real indexed commit owners; they do not establish
// historical PoW, global coin provenance, service locking or memory capacity.
#include "../consensus/orchard_forest_test_fixture.h"
#include "../storage/shielded_store_fixture.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/utxo_publication.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/orchard_state_root.h"
#include "consensus/state_commitment.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "daemon/orchard_chainstate_write.h"
#include "daemon/runtime_block_outbox.h"
#include "wallet/runtime_account_replay.h"
#include "storage/block_storage.h"
#include "common/annotated_mutex.h"
#include <iomanip>
#include <sstream>

using namespace shielded_store_fixture;
namespace dinero {
struct RuntimeAccountReplayTestAccess {
    static auto CaptureSource(const std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>& source) {
        return RuntimeAccountReplay::Capture(source);
    }
};
}
static const auto token=ChainWriteToken::CreateForTesting();
static CBlockIndex DiskIndex(const ChainDB& db,const BlockHeader& header,uint32_t height) {
    const auto m=RequiredValue(db.getHeaderMetadata(header.GetHash()));
    CBlockIndex i(header,height);i.chainwork=ChainworkToHex(m.chainwork);i.status=m.status_flags;
    i.file_number=m.file_number;i.data_pos=m.data_pos;i.data_size=m.data_size;
    i.undo_file=m.undo_file;i.undo_pos=m.undo_pos;i.undo_size=m.undo_size;return i;
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
static std::string StorageScriptHex(const Bytes& script) {
    std::ostringstream out;
    for(const auto byte:script)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
    return out.str();
}
static OrchardBlockCandidate WithParentTiming(const OrchardBlockCandidate& block,const BlockHeader& parent) {
    if(!parent.difficulty)return block;
    auto header=block.Header();header.timestamp=parent.timestamp+120;header.difficulty=parent.difficulty;
    auto bytes=block.WireBytes();const auto prefix=header.SerializeForHash();
    std::copy(prefix.begin(),prefix.end(),bytes.begin());return OrchardBlockCandidate::DecodeExact(bytes);
}
static Block HistoricalDeliveryParent() {
    // A real historical encoding for source continuity, not a claim that this
    // generated parent's historical scripts/PoW/UTXO provenance were replayed.
    Block block{};Transaction coinbase;coinbase.vin.resize(1);
    coinbase.vin[0].prevout=TxOutPoint(TxId(uint256{}),UINT32_MAX);
    coinbase.vin[0].scriptSig={1,1};coinbase.vout.emplace_back(AmountUna::Zero(),std::vector<uint8_t>{0x51});
    block.vtx.push_back(coinbase);block.header.version=1;
    block.header.prev_block_hash=H(93);block.header.timestamp=20000;
    block.header.merkle_root=ComputeMerkleRoot(block.vtx);return block;
}

static void Run(const std::string& base) {
    AnnotatedRecursiveMutex activation;
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    BlockStorage files;CHECK(files.init(temp.path)==Status::Ok);
    Fixture keys(base);const auto auth=Authorized(base,false,20000);
    const auto& tx=auth.Transaction();View view;view.height=20000;
    UtreexoForest parent_forest;parent_forest.setCanonicalEmptyRoots(true);
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
    BlockHeader parent=HistoricalDeliveryParent().header;
    const auto parent_root=parent_forest.getCommitment();
    std::copy(parent_root.begin(),parent_root.end(),parent.utreexo_root.begin());
    OrchardBlockContext c{20001,H(2),parent.GetHash(),20001,keys.domain};
    const auto uncommitted=WithParentTiming(CandidateWires(c,{auth.Orchard().CanonicalBytes()}),parent);
    c.block_hash=uncommitted.Header().GetHash();
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(uncommitted,c,view,{},true);
    const auto filtered=WithFilterHash(uncommitted,BuildOrchardBlockFilter(preliminary).GetHash());
    c.block_hash=filtered.Header().GetHash();
    const auto filter_coins=PrepareOrchardBlockCoinsUnderChainstateLock(filtered,c,view,{},true);
    const auto draft=WithStateRoot(db,c,filtered,filter_coins);c.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,c,view,{},true);
    const auto computed=PrepareOrchardForestTransition(coins,parent,parent_forest);
    auto header=draft.Header();header.utreexo_root=computed.Root();
    auto bytes=draft.WireBytes();const auto wire=header.SerializeForHash();
    std::copy(wire.begin(),wire.end(),bytes.begin());
    const auto block=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,parent_forest));
    c.block_hash=header.GetHash();
    CHECK(db.putHeader(token,parent.GetHash(),parent,20000,arith_uint256(20000),&seed)==Status::Ok);
    CHECK(db.putHeader(token,c.block_hash,header,20001,arith_uint256(20001),&seed)==Status::Ok);
    CHECK(db.putHeightIndex(token,20000,parent.GetHash(),&seed)==Status::Ok);
    CHECK(db.putForestTipMarker(token,{20000,parent.GetHash(),parent.utreexo_root},&seed)==Status::Ok);
    CHECK(db.putUtreexoCheckpointWithChecksum(token,20000,parent_forest.serialize(),&seed)==Status::Ok);
    Tip(db,parent.GetHash(),20000,seed);SeedFrozenLegacy(db,c,seed);Commit(db,seed);
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=c.height;metadata.parent_hash=c.parent_hash;
    metadata.chainwork=RequiredValue(db.getBlockWork(c.block_hash));metadata.status_flags=BLOCK_VALID_HEADER;
    CHECK(db.putHeaderMetadata(token,c.block_hash,metadata)==Status::Ok);
    auto index=DiskIndex(db,header,c.height);
    ConsensusUTXOSet live;
    for(const auto& [point,coin]:view.coins)CHECK(live.AddCoin(point,coin));
    live.ReplaceForestGuarded(parent_forest);live.SetBestBlock(parent.GetHash(),c.height-1);
    const auto connect=[&] {
        auto owner=PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,files,index,live,c,
            block,parent,parent_forest,{},true,true,FixtureRetirement(c));
        owner->Commit();
    };
    connect();CHECK(!activation.HeldByCurrentThread());
    const auto first=ReadRuntimeOutboxUnderLock(db,c);
    CHECK(first.head.sequence==1&&first.events.size()==1&&first.events[0].orchard_replay);
    CHECK(first.events[0].body==block.WireBytes());
    const auto first_state=RequiredValue(db.getOrchardState());
    {
        auto owner=PreparedOrchardChainstateWrite::DisconnectIndexed(activation,db,token,files,index,live,c,
            block,parent,computed.After(),true);
        owner->Commit();
    }
    CHECK(!activation.HeldByCurrentThread());
    const auto current=ReadRuntimeOutboxUnderLock(db,c);
    CHECK(current.head.sequence==2&&current.events.size()==2);
    CHECK(current.events[1].direction==RuntimeBlockDirection::Disconnect);
    CHECK(live.GetBestBlock()==parent.GetHash()&&RequiredValue(db.getTip()).hash==parent.GetHash());
    files.close();db.close();const auto before_reads=Inspect(temp.path);
    CHECK(db.init(temp.path)==Status::Ok&&files.init(temp.path)==Status::Ok);
    index=DiskIndex(db,header,c.height);
    const auto page=ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,{},1);
    CHECK(page.head==first.head&&page.next==first.head&&page.events.size()==1);
    CHECK(page.events.front().body==block.WireBytes());
    const auto eof=ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,first.head,1);
    CHECK(eof.head==first.head&&eof.next==first.head&&eof.events.empty()&&eof.after_tip);
    CHECK(eof.after_tip->first==c.block_hash&&eof.after_tip->second==c.height);
    CHECK(RequiredValue(db.getTip()).hash==parent.GetHash());
    std::cout<<"RuntimePrefixStorage ReopenedPrefixHasOwnTip PASS\n";

    auto bad=first.head;bad.digest=H(248);
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,bad,{});});
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,current.head);});
    auto future=current.head;++future.sequence;
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,future,{});});
    auto wrong=c;++wrong.domain.branch_id;
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,wrong,first.head,{});});
    LookupReject(Status::Invalid,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,{},{});});
    LookupReject(Status::Invalid,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,{},0);});
    LookupReject(Status::Invalid,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,{},129);});
    LookupReject(Status::Invalid,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,{},1,1);});
    LookupReject(Status::Invalid,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,{},1,16*1024*1024+1);});
    files.close();db.close();CHECK(Inspect(temp.path)==before_reads);
    CHECK(db.init(temp.path)==Status::Ok&&files.init(temp.path)==Status::Ok);
    index=DiskIndex(db,header,c.height);
    std::cout<<"RuntimePrefixStorage InvalidCursorsPreserveStorage PASS\n";

    // One ordinary sequential reconnect between page calls. No concurrent
    // writer, contender, synchronization removal or repeated restart control.
    std::optional<RuntimeOutboxCursor> captured_head;size_t calls=0;
    const auto replay=RuntimeAccountReplayTestAccess::CaptureSource([&](RuntimeOutboxCursor after,size_t count) {
        const auto bounded=std::min<size_t>(count,1);
        auto result=captured_head?ReadRuntimeOutboxPrefixUnderLock(db,c,*captured_head,after,bounded):
            ReadRuntimeOutboxUnderLock(db,c,after,bounded);
        if(!captured_head)captured_head=result.head;
        if(++calls==1) {
            connect();files.close();db.close();
            CHECK(db.init(temp.path)==Status::Ok&&files.init(temp.path)==Status::Ok);
            index=DiskIndex(db,header,c.height);
        }
        return result;
    });
    CHECK(calls==3&&replay->Head()==current.head);
    CHECK(replay->Point(first.head).checkpoint==first_state);
    CHECK(replay->Point(current.head).checkpoint.block_hash==parent.GetHash());
    CHECK(replay->Event(1)->body==block.WireBytes()&&replay->Event(2)->body==block.WireBytes());
    const auto appended=ReadRuntimeOutboxUnderLock(db,c);
    CHECK(appended.head.sequence==3&&appended.events.size()==3);
    CHECK(appended.events.back().previous_digest==current.head.digest);
    CHECK(RequiredValue(db.getTip()).hash==c.block_hash&&live.GetBestBlock()==c.block_hash);
    std::cout<<"RuntimePrefixStorage ReplayStopsAtCapturedHead PASS\n";

    // Prefix EOF still checks the current durable tip, even though the prefix
    // itself remains byte-valid. Repair only this generated test database.
    rocksdb::WriteBatch mismatched_tip;Tip(db,H(249),c.height,mismatched_tip);Commit(db,mismatched_tip);
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,first.head);});
    rocksdb::WriteBatch restore_tip;Tip(db,c.block_hash,c.height,restore_tip);Commit(db,restore_tip);
    CHECK(ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,first.head).events.empty());
    std::string saved_head;CHECK(db.getRaw("runtime_orchard_outbox:v1:head",saved_head)==Status::Ok);
    rocksdb::WriteBatch missing;missing.Delete("runtime_orchard_outbox:v1:head");Commit(db,missing);
    LookupReject(Status::Corruption,[&]{(void)ReadRuntimeOutboxPrefixUnderLock(db,c,first.head,first.head);});
    rocksdb::WriteBatch restore_head;restore_head.Put("runtime_orchard_outbox:v1:head",saved_head);Commit(db,restore_head);
    CHECK(ReadRuntimeOutboxUnderLock(db,c).head==appended.head);
    std::cout<<"RuntimePrefixStorage CurrentHeadRequiredAtPrefixEof PASS\n";
}
int main(int argc,char** argv) {
    try { CHECK(argc==2);SelectParams(Chain::REGTEST);Run(argv[1]);return 0; }
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
