#include "daemon/runtime_block_reader.h"
#include "daemon/services/chainstate_service.h"
#include "../consensus/orchard_block_test_fixture.h"
#include "../storage/shielded_store_fixture.h"
#include "storage/block_storage.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include <rocksdb/write_batch.h>

using namespace shielded_store_fixture;
static std::string BinaryToHexString(const std::string& bytes) {
    static constexpr char digits[]="0123456789abcdef";
    std::string result; result.reserve(bytes.size()*2);
    for(unsigned char c:bytes){result.push_back(digits[c>>4]);result.push_back(digits[c&15]);}
    return result;
}
static const auto token = ChainWriteToken::CreateForTesting();
static void Index(ChainDB& db, const BlockHeader& header, uint32_t height, const FilePosition& pos) {
    const auto hash = header.GetHash();
    CHECK(db.putHeader(token, hash, header, height, arith_uint256(height)) == Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash=header.prev_block_hash; metadata.height=height;
    metadata.status_flags=BLOCK_HAVE_DATA;
    metadata.file_number=pos.file_number; metadata.data_pos=pos.offset; metadata.data_size=pos.size;
    CHECK(db.putHeaderMetadata(token,hash,metadata)==Status::Ok);
    CHECK(db.putHeightIndex(token,height,hash)==Status::Ok);
}
static void Run(const std::string& fixtures) {
    SelectParams(Chain::REGTEST);
    MutableParams().orchard_activation_height=10; MutableParams().orchard_branch_id=1;
    MutableParams().enforce_witness_commitment=true;
    MutableParams().witness_commitment_enforcement_height=10;
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path)==Status::Ok);
    auto blocks=std::make_shared<BlockStorage>(); CHECK(blocks->init(temp.path)==Status::Ok);
    OrchardBlockContext context; context.height=10; context.parent_hash=H(3);
    const auto body=CandidateWires(context,{Load(fixtures+"/candidate-envelope.bin")});
    const auto hash=body.Header().GetHash();
    const std::string wire(body.WireBytes().begin(),body.WireBytes().end());
    const auto pos=RequiredValue(blocks->writeBlockBytes(hash,wire));
    Index(db,body.Header(),10,pos);
    CHECK(blocks->readBlock(pos).status()==Status::Serialization);
    const auto before=Inspect(temp.path);
    {
        ChainstateService service; service.setChainDB(&db); service.setBlockStorage(blocks);
        const auto read=service.getRuntimeBlockByHash(hash); CHECK(read.ok());
        CHECK((*read)->IsOrchardProfile());
        CHECK((*read)->Orchard().Transactions().size()==2);
        CHECK((*read)->Orchard().Transactions()[1].IsOrchard());
        CHECK((*read)->Serialize()==body.WireBytes());
        CHECK((*read)->Context()->domain.network_code==2 && (*read)->Context()->domain.branch_id==1);
        bool separated=false;
        try { (void)(*read)->Historical(); } catch(const std::bad_variant_access&) { separated=true; }
        CHECK(separated);
        CHECK(service.getBlock(10)==BinaryToHexString(wire));
        // Runtime reads are not admission: the old API cannot expose a fake
        // historical transaction shell for this mixed body.
        CHECK(!service.getBlockByHash(hash).ok());
        CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,9).status()==Status::Corruption);
        CHECK(ReadRuntimeBlockUnderLock(db,nullptr,hash,10).status()==Status::Internal);
    }
    db.close(); CHECK(Inspect(temp.path)==before); CHECK(db.init(temp.path)==Status::Ok);
    blocks->close(); CHECK(blocks->init(temp.path)==Status::Ok);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).ok());

    // Activation comes from selected parameters, not an envelope marker.
    MutableParams().orchard_activation_height=11;
    CHECK(!ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).ok());
    MutableParams().orchard_activation_height=10; MutableParams().orchard_branch_id=0;
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).status()==Status::Internal);
    MutableParams().orchard_branch_id=1;

    // Historical parsing/serialization before the boundary remains the actual
    // old implementation, without imposing the post-boundary envelope framing.
    Block old; old.header={}; old.header.version=1; old.header.nonce=19;
    Transaction tx; TxInput input; input.prevout.vout=UINT32_MAX; input.scriptSig={1,9}; tx.vin={input};
    tx.vout.emplace_back(AmountUna::Una(100),Bytes{0x51}); old.vtx={tx};
    old.header.merkle_root=consensus::ComputeMerkleRoot(old.vtx);
    const auto old_pos=RequiredValue(blocks->writeBlock(old.GetHash(),old)); Index(db,old.header,9,old_pos);
    const auto historical=ReadRuntimeBlockUnderLock(db,blocks.get(),old.GetHash(),9);
    CHECK(historical.ok() && !historical->IsOrchardProfile());
    CHECK(historical->Historical().vtx[0].GetTxid()==tx.GetTxid());
    const auto old_wire=old.Serialize(); CHECK(historical->Serialize()==Bytes(old_wire.begin(),old_wire.end()));
    {
        ChainstateService service;service.setChainDB(&db);service.setBlockStorage(blocks);
        CHECK(service.getBlock(9)==BinaryToHexString(old_wire));
    }

    // A valid framing checksum and matching header hash are insufficient.
    // Use a different honest body under the original header to exercise root
    // authentication without constructing a hostile transaction or proof.
    auto changed=CandidateWires(context,{}).WireBytes();
    std::copy_n(body.WireBytes().begin(),128,changed.begin());
    const auto badpos=RequiredValue(blocks->writeBlockBytes(hash,std::string(changed.begin(),changed.end())));
    Index(db,body.Header(),10,badpos);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).status()==Status::Corruption);
    Index(db,body.Header(),10,pos);
    // A database copy never masks a missing strict flatfile locator.
    auto metadata=RequiredValue(db.getHeaderMetadata(hash)); metadata.data_size=0;
    rocksdb::WriteBatch batch;
    CHECK(db.stageOrchardBlock(token,body,true,batch)==Status::Ok);
    CHECK(db.writeBatch(token,std::move(batch),true)==Status::Ok);
    CHECK(db.putHeaderMetadata(token,hash,metadata)==Status::Ok);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).status()==Status::NotFound);
    Index(db,body.Header(),10,pos);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).ok());
    metadata=RequiredValue(db.getHeaderMetadata(hash)); metadata.parent_hash=H(99);
    CHECK(db.putHeaderMetadata(token,hash,metadata)==Status::Ok);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),hash,10).status()==Status::Corruption);
    Index(db,body.Header(),10,pos);

    // The transparent witness commitment is selected by height, not an input
    // flag to the daemon query. Rebuild a consistent txid Merkle root while
    // omitting only the coinbase witness commitment.
    auto coinbase=body.Transactions()[0].Historical(); coinbase.vout.pop_back();
    const auto cb=coinbase.Serialize(TxSerializationMode::WithWitness);
    const auto txwire=body.Transactions()[1].Serialize(TxSerializationMode::WithWitness);
    auto header=body.Header();
    const std::vector<TxId> identities{coinbase.GetTxid(),body.Transactions()[1].GetTxid()};
    header.merkle_root=consensus::ComputeTransactionMerkleRoot(identities);
    const auto prefix=header.SerializeForHash(); Bytes missing(prefix.begin(),prefix.end());
    missing.push_back(2);missing.insert(missing.end(),cb.begin(),cb.end());
    missing.insert(missing.end(),txwire.begin(),txwire.end());missing.push_back(0);
    const auto missing_pos=RequiredValue(blocks->writeBlockBytes(header.GetHash(),std::string(missing.begin(),missing.end())));
    Index(db,header,10,missing_pos);
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),header.GetHash(),10).status()==Status::Corruption);
    MutableParams().enforce_witness_commitment=false;
    CHECK(ReadRuntimeBlockUnderLock(db,blocks.get(),header.GetHash(),10).ok());
}
int main(int argc,char** argv) {
    try { CHECK(argc==2); Run(argv[1]); std::cout<<"OrchardRuntimeReader PASS\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
