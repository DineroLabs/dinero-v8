#include "orchard_block_test_fixture.h"
#include "wallet/orchard_chain_restore.h"
#include "daemon/runtime_block_outbox.h"
#include "consensus/orchard_block_staging.h"
#include "../storage/shielded_store_fixture.h"
#include "consensus/chainparams.h"
#include "storage/block_storage.h"
using namespace shielded_store_fixture;
using dinero::wallet::OrchardAccountState;
using dinero::wallet::RestoreOrchardAccountFromChainUnderLock;
static const auto token = ChainWriteToken::CreateForTesting();
static uint256 Hash256(const Hash& value) {
    uint256 out; std::copy(value.begin(),value.end(),out.begin()); return out;
}
static void Header(ChainDB& db, const BlockHeader& header, uint32_t height) {
    CHECK(db.putHeader(token,header.GetHash(),header,height,arith_uint256(height))==Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata; metadata.parent_hash=header.prev_block_hash; metadata.height=height;
    CHECK(db.putHeaderMetadata(token,header.GetHash(),metadata)==Status::Ok);
    CHECK(db.putHeightIndex(token,height,header.GetHash())==Status::Ok);
}
template<class F> static void RestoreReject(F fn) {
    bool failed=false;try{fn();}catch(const std::exception&){failed=true;}CHECK(failed);
}
static void HistoricalRestore(const OrchardAccountState& ready, SigningDomain domain,
    const FullViewingKeyBytes& fvk,const OutPoint& pending_input) {
    TempDir temp;Seed(temp.path);ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    Transaction coinbase;coinbase.version=2;TxInput cb;cb.prevout.vout=UINT32_MAX;cb.scriptSig={2,31,78};
    coinbase.vin={cb};coinbase.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});
    Block grand;grand.header={};grand.header.version=1;grand.header.prev_block_hash=H(88);grand.header.timestamp=19999;
    grand.vtx={coinbase};grand.header.merkle_root=ComputeMerkleRoot(grand.vtx);
    Block parent=grand;parent.header.prev_block_hash=grand.GetHash();parent.header.timestamp=20000;
    parent.vtx[0].vin[0].scriptSig={2,32,78};parent.header.merkle_root=ComputeMerkleRoot(parent.vtx);
    CHECK(db.putHeightIndex(token,19998,H(88))==Status::Ok);Header(db,grand.header,19999);Header(db,parent.header,20000);
    CHECK(db.putBlock(token,grand.GetHash(),grand)==Status::Ok);CHECK(db.putBlock(token,parent.GetHash(),parent)==Status::Ok);
    auto account=OrchardAccountState::RestoreForRescan(ready.Encode(),domain,fvk,20001,parent.GetHash());
    const auto event=[&](const Block& b,RuntimeBlockDirection direction,uint64_t sequence,uint8_t digest,uint8_t previous) {
        OrchardBlockContext c{20000,b.GetHash(),b.header.prev_block_hash,20001,domain};const auto wire=b.Serialize();
        return RuntimeOutboxEvent{{sequence,H(digest)},previous?H(previous):uint256{},direction,c,Bytes(wire.begin(),wire.end())};
    };
    auto below=account.ApplyHistoricalDelivery(event(parent,RuntimeBlockDirection::Disconnect,1,111,0));
    CHECK(db.setTip(token,grand.GetHash(),19999,arith_uint256(19999))==Status::Ok);
    CHECK(db.setValidatedTip(token,grand.GetHash(),19999)==Status::Ok);
    auto restored=RestoreOrchardAccountFromChainUnderLock(db,nullptr,below.Encode(),domain,fvk,20001);
    CHECK(restored.Scan().Checkpoint()==below.Scan().Checkpoint() && restored.Delivery()==below.Delivery());
    auto alternate=parent;alternate.header.nonce=17;
    Transaction conflict;conflict.version=2;TxInput in;in.prevout.txid=pending_input.txid;in.prevout.vout=pending_input.vout;conflict.vin={in};
    conflict.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});alternate.vtx.push_back(conflict);
    alternate.header.merkle_root=ComputeMerkleRoot(alternate.vtx);
    auto conflicted=below.ApplyHistoricalDelivery(event(alternate,RuntimeBlockDirection::Connect,2,112,111));
    CHECK(!conflicted.Observations().empty());
    Header(db,alternate.header,20000);CHECK(db.putBlock(token,alternate.GetHash(),alternate)==Status::Ok);
    CHECK(db.setTip(token,alternate.GetHash(),20000,arith_uint256(20000))==Status::Ok);
    CHECK(db.setValidatedTip(token,alternate.GetHash(),20000)==Status::Ok);
    auto encoded=conflicted.Encode();
    const auto restore=[&]{return RestoreOrchardAccountFromChainUnderLock(db,nullptr,encoded,domain,fvk,20001);};
    db.close();const auto rows=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    CHECK(restore().Observations()==conflicted.Observations());
    CHECK(restore().Delivery()==conflicted.Delivery());
    db.close();CHECK(Inspect(temp.path)==rows);CHECK(db.init(temp.path)==Status::Ok);
    RestoreReject([&]{(void)RestoreOrchardAccountFromChainUnderLock(db,nullptr,below.Encode(),domain,fvk,20001);});
    CHECK(db.putHeightIndex(token,20000,parent.GetHash())==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putHeightIndex(token,20000,alternate.GetHash())==Status::Ok);
    CHECK(db.deleteBlock(token,alternate.GetHash())==Status::Ok);
    LookupReject(Status::NotFound,[&]{(void)restore();});
    auto bad=alternate;bad.vtx.back().vout[0].value=AmountUna::Una(2);
    CHECK(db.putBlock(token,alternate.GetHash(),bad)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putBlock(token,alternate.GetHash(),alternate)==Status::Ok);
    CHECK(restore().Observations()==conflicted.Observations());
}

static void Run(const char* fixtures, bool same_block) {
    Fixture f(fixtures);
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path)==Status::Ok);
    const auto first=f.view.coins.at(Point(f.inputs[0])),second=f.view.coins.at(Point(f.inputs[1]));
    // Authenticated-content fixture, not a full replay/PoW qualification.
    Block funding; funding.header={};funding.header.version=1;funding.header.timestamp=100;
    funding.header.prev_block_hash=H(98);
    Transaction coinbase;coinbase.version=2;TxInput cb;cb.prevout.vout=UINT32_MAX;cb.scriptSig={1,100};coinbase.vin={cb};
    coinbase.vout.emplace_back(first.value,first.scriptPubKey);coinbase.vout.emplace_back(second.value,second.scriptPubKey);
    funding.vtx={coinbase};funding.header.merkle_root=ComputeMerkleRoot(funding.vtx);
    const auto funding_id=coinbase.GetTxid();
    Transaction intermediate=coinbase;intermediate.vin[0].prevout.txid=funding_id;
    intermediate.vin[0].prevout.vout=0;
    intermediate.vin[0].scriptSig.clear();
    intermediate.vin.push_back(intermediate.vin[0]);intermediate.vin[1].prevout.vout=1;
    const auto source_id=same_block?intermediate.GetTxid():funding_id;
    CHECK(db.putHeightIndex(token,99,H(98))==Status::Ok);Header(db,funding.header,100);
    CHECK(db.putBlock(token,funding.GetHash(),funding)==Status::Ok);
    CHECK(db.putTxIndex(token,funding_id.AsUint256(),funding.GetHash(),0)==Status::Ok);
    f.view.coins.clear();
    for(size_t i=0;i<2;++i){
        std::copy(source_id.AsUint256().begin(),source_id.AsUint256().end(),f.inputs[i].txid_wire.begin());
        f.inputs[i].output_index=i;
        f.view.coins.emplace(Point(f.inputs[i]),UTXOEntry(i?second.value:first.value,i?second.scriptPubKey:first.scriptPubKey,same_block?20001:100,!same_block));
    }
    f.outputs[0].script_pub_key=first.scriptPubKey;f.outputs[1].script_pub_key=second.scriptPubKey;f.outputs[1].amount_una=51000;
    auto keys=WalletKeys::FromSeed(std::array<uint8_t,64>{7},0);const auto fvk=keys.ExportFullViewingKey();
    std::vector<WalletPayment> payments{{5000,keys.Receiver(WalletScope::External,{})}};
    auto plan=WalletBundlePlan::PrepareShield(keys,payments);
    std::vector<ResolvedInput> inputs;
    for(const auto& in:f.inputs){const auto& c=f.view.coins.at(Point(in));inputs.push_back({in.txid_wire,in.output_index,in.sequence,c.value.GetUna(),c.scriptPubKey});}
    const auto signing=SigningContext::Create(f.domain,f.lock,inputs,f.outputs,f.fee);
    BlockHeader parent{};parent.version=1;parent.timestamp=20000;parent.prev_block_hash=H(42);
    CHECK(db.putHeightIndex(token,19999,H(42))==Status::Ok);Header(db,parent,20000);
    auto initial=OrchardAccountState::Begin(f.domain,fvk,20001,parent.GetHash());
    auto [issued,receiver]=initial.IssueReceiver(WalletScope::External);
    auto reserved=issued.Reserve(Hash{7},plan.Intent(signing));
    auto proved=std::move(plan).Prove(signing);f.bundle=proved.Bytes();f.Sign();
    const std::vector<VerifiedOrchardAuthorizations> auth{VerifyOrchardAuthorizations(f.Snapshot(),f.domain,20001,{})};
    auto ready=reserved.SetReady(Hash{7},auth[0]);
    if(!same_block) HistoricalRestore(ready,f.domain,fvk,Point(f.inputs[0]));
    OrchardBlockContext context{20001,H(2),parent.GetHash(),20001,f.domain};
    const auto body=same_block?CandidateWires(context,{intermediate.Serialize(TxSerializationMode::WithWitness),auth[0].Orchard().CanonicalBytes()}):Candidate(context,auth);
    const uint32_t origin_index=same_block?2:1;
    context.block_hash=body.Header().GetHash();
    OrchardStateLookups state_view{[](const uint256&)->StatusOr<bool>{return false;},[](const uint256&)->StatusOr<bool>{return false;}};
    const auto prepared=PrepareOrchardStateTransition(context,std::nullopt,auth,state_view);
    auto funded=ready.Advance(context,body,prepared,auth);CHECK(funded.Scan().BalanceUna()==5000);
    Header(db,body.Header(),20001);
    rocksdb::WriteBatch batch;
    CHECK(db.stageOrchardConnect(token,{},prepared.Next(),prepared.Nullifiers(),prepared.Flows(),batch)==Status::Ok);
    CHECK(db.stageOrchardBlock(token,body,true,batch)==Status::Ok);
    CHECK(db.putTxIndex(token,Hash256(auth[0].Orchard().Txid()),context.block_hash,origin_index,&batch)==Status::Ok);
    if(same_block) CHECK(db.putTxIndex(token,source_id.AsUint256(),context.block_hash,1,&batch)==Status::Ok);
    CHECK(db.setTip(token,context.block_hash,20001,arith_uint256(20001),&batch)==Status::Ok);
    CHECK(db.setValidatedTip(token,context.block_hash,20001,&batch)==Status::Ok);
    CHECK(db.writeBatch(token,std::move(batch),true)==Status::Ok);
    auto encoded=funded.Encode();
    db.close();const auto before=Inspect(temp.path);CHECK(db.init(temp.path)==Status::Ok);
    const auto restore=[&]{return RestoreOrchardAccountFromChainUnderLock(db,nullptr,encoded,f.domain,fvk,20001);};
    const auto restored=restore();CHECK(restored.Scan().BalanceUna()==5000);
    CHECK(restored.Operations().Entries().at(Hash{7}).transaction==ready.Operations().Entries().at(Hash{7}).transaction);
    CHECK(restored.IssueReceiver(WalletScope::External).second==funded.IssueReceiver(WalletScope::External).second);
    // Neither input exists in today's UTXO set: origin recovery reads original
    // outputs, not current unspentness. It must not invent or insert coin rows.
    CHECK(db.getCoin(funding_id.AsUint256(),0).status()==Status::NotFound);
    db.close();CHECK(Inspect(temp.path)==before);CHECK(db.init(temp.path)==Status::Ok);
    const auto origin_id=Hash256(auth[0].Orchard().Txid());
    CHECK(db.putTxIndex(token,origin_id,context.block_hash,0)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putTxIndex(token,origin_id,context.block_hash,origin_index)==Status::Ok);
    if(same_block) {
        CHECK(db.putTxIndex(token,source_id.AsUint256(),context.block_hash,origin_index)==Status::Ok);
        LookupReject(Status::Corruption,[&]{(void)restore();});
        CHECK(db.putTxIndex(token,source_id.AsUint256(),context.block_hash,1)==Status::Ok);
    } else {
    CHECK(db.putTxIndex(token,funding_id.AsUint256(),context.block_hash,1)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putTxIndex(token,funding_id.AsUint256(),funding.GetHash(),0)==Status::Ok);
    CHECK(db.putHeightIndex(token,100,H(44))==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putHeightIndex(token,100,funding.GetHash())==Status::Ok);
    auto bad=funding;bad.vtx[0].vout[0].value=AmountUna::Una(12346);
    CHECK(db.putBlock(token,funding.GetHash(),bad)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.putBlock(token,funding.GetHash(),funding)==Status::Ok);
    CHECK(db.deleteBlock(token,funding.GetHash())==Status::Ok);
    LookupReject(Status::NotFound,[&]{(void)restore();});
    CHECK(db.putBlock(token,funding.GetHash(),funding)==Status::Ok);
    CHECK(restore().Scan().BalanceUna()==5000);
    }
    CHECK(db.setValidatedTip(token,parent.GetHash(),20000)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)restore();});
    CHECK(db.setValidatedTip(token,context.block_hash,20001)==Status::Ok);
    RestoreReject([&]{(void)RestoreOrchardAccountFromChainUnderLock(db,nullptr,initial.Encode(),f.domain,fvk,20001);});
    // A present nullifier owner must be an authenticated selected block that
    // actually contains this nullifier. A malformed owner is never absence.
    const auto& nf=funded.Scan().Notes()[0].note->Facts().nullifier;
    const std::string nf_key="O1N"+std::string(reinterpret_cast<const char*>(nf),32);
    std::vector<std::string> families;for(const auto& [name,rows]:before)families.push_back(name);
    const auto owner_row=[&](const std::optional<std::string>& value){
        db.close();{
            Raw raw(temp.path,families);
            if(value)raw.put(shielded_store_fixture::shielded,nf_key,*value);
            else CHECK(raw.db->Delete(rocksdb::WriteOptions(),raw.cf(shielded_store_fixture::shielded),nf_key).ok());
        }CHECK(db.init(temp.path)==Status::Ok);
    };
    owner_row(std::string(reinterpret_cast<const char*>(context.block_hash.begin()),32));
    LookupReject(Status::Corruption,[&]{(void)restore();});
    owner_row(std::string("bad"));LookupReject(Status::Corruption,[&]{(void)restore();});
    owner_row(std::nullopt);CHECK(restore().Scan().BalanceUna()==5000);
    // Move the typed body to the actual durable flatfile path. Reopen and
    // recover both ordinary historical and same-block prevout variants.
    BlockStorage blocks;CHECK(blocks.init(temp.path)==Status::Ok);
    const std::string wire(body.WireBytes().begin(),body.WireBytes().end());
    const auto position=RequiredValue(blocks.writeBlockBytes(context.block_hash,wire));
    auto metadata=RequiredValue(db.getHeaderMetadata(context.block_hash));
    metadata.file_number=position.file_number;metadata.data_pos=position.offset;metadata.data_size=position.size;
    CHECK(db.putHeaderMetadata(token,context.block_hash,metadata)==Status::Ok);
    CHECK(db.deleteBlock(token,context.block_hash)==Status::Ok);
    LookupReject(Status::NotFound,[&]{(void)restore();});
    blocks.close();CHECK(blocks.init(temp.path)==Status::Ok);
    const auto flatRestore=[&]{return RestoreOrchardAccountFromChainUnderLock(db,&blocks,encoded,f.domain,fvk,20001);};
    CHECK(flatRestore().Scan().BalanceUna()==5000);
    CHECK(blocks.readBlock(position).status()==Status::Serialization);
    // A valid storage checksum/header is not a transaction Merkle check.
    auto changed_outputs=f.outputs;changed_outputs[0].amount_una++;
    const auto changed=TransactionEnvelope::Create(f.lock,f.inputs,changed_outputs,f.fee,f.bundle).CanonicalBytes();
    auto damaged=body.WireBytes();const auto& original=auth[0].Orchard().CanonicalBytes();
    CHECK(changed.size()==original.size());
    const auto at=std::search(damaged.begin(),damaged.end(),original.begin(),original.end());CHECK(at!=damaged.end());
    std::copy(changed.begin(),changed.end(),at);
    const auto bad_pos=RequiredValue(blocks.writeBlockBytes(context.block_hash,std::string(damaged.begin(),damaged.end())));
    metadata.data_pos=bad_pos.offset;metadata.data_size=bad_pos.size;metadata.file_number=bad_pos.file_number;
    CHECK(db.putHeaderMetadata(token,context.block_hash,metadata)==Status::Ok);
    LookupReject(Status::Corruption,[&]{(void)flatRestore();});
    metadata.data_pos=position.offset;metadata.data_size=position.size;metadata.file_number=position.file_number;
    CHECK(db.putHeaderMetadata(token,context.block_hash,metadata)==Status::Ok);
    CHECK(flatRestore().Scan().BalanceUna()==5000);
    blocks.close();
    db.close();LookupReject(Status::Internal,[&]{(void)restore();});
}
int main(int argc,char** argv) {try{
    CHECK(argc==2);SelectParams(Chain::REGTEST);Run(argv[1],false);Run(argv[1],true);
    std::cout<<"PASS: selected ChainDB recovery, historical and same-block prevouts, stale indexes and read-only reopen\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
