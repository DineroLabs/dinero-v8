#include "primitives/orchard_block_reader.h"
#include "consensus/merkle_root.h"
#include "consensus/witness_commitment.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <type_traits>
using namespace dinero;
using Bytes=std::vector<uint8_t>;
static void Require(bool ok, std::source_location loc=std::source_location::current()) {
    if(!ok) throw std::runtime_error("block reader check failed at "+std::to_string(loc.line()));
}
template<class F> static void Reject(F fn) {
    bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}Require(rejected);
}
static Bytes Load(const std::string& file) {
    std::ifstream f(file,std::ios::binary);Require(f.good());return {std::istreambuf_iterator<char>(f),{}};
}
static Transaction Ordinary(unsigned n) {
    Transaction t;t.version=2;t.lockTime=n;
    TxInput i;i.prevout.txid=TxId(uint256());i.prevout.vout=UINT32_MAX;i.scriptSig={1,static_cast<uint8_t>(n)};
    t.vin={i};t.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});return t;
}
static Bytes Framing(const std::vector<Bytes>& txs,const Bytes& suffix={0}) {
    Require(txs.size()<253);BlockHeader h{};
    std::vector<TxId> ids;for(const auto& t:txs) ids.push_back(ParsedTransaction::DecodeExact(t,TransactionReadMode::StagedOrchard).GetTxid());
    h.merkle_root=consensus::ComputeTransactionMerkleRoot(ids);
    const auto header=h.SerializeForHash();Bytes b(header.begin(),header.end());b.push_back(txs.size());
    for(const auto& t:txs)b.insert(b.end(),t.begin(),t.end());b.insert(b.end(),suffix.begin(),suffix.end());return b;
}
static void Sizes(const Bytes& orchard) {
    std::string error;
    const auto cb=Ordinary(1).Serialize(TxSerializationMode::WithWitness);
    const auto one=OrchardBlockCandidate::DecodeExact(Framing({cb,orchard}));
    const auto tx=ParsedTransaction::DecodeExact(orchard,TransactionReadMode::StagedOrchard);
    Require(one.BaseSize()==130+Ordinary(1).Serialize(TxSerializationMode::WithoutWitness).size()+tx.GetBaseSize());
    Require(one.Weight()==3*one.BaseSize()+one.WireBytes().size() && one.CheckSizeLimits(error));
    // The full Utreexo suffix is base data, with no witness discount.
    consensus::BlockUtreexoData proof;proof.accumulator_root_before.assign(32,0);
    const auto encoded=proof.serialize();Bytes suffix{1};suffix.insert(suffix.end(),encoded.begin(),encoded.end());
    const auto extended=OrchardBlockCandidate::DecodeExact(Framing({cb,orchard},suffix));
    Require(extended.BaseSize()==one.BaseSize()+encoded.size());
    Require(extended.Weight()==one.Weight()+4*encoded.size());
    std::vector<Bytes> wires{cb};
    for(unsigned i=0;i<12;++i) {
        auto ordinary=Ordinary(10+i);ordinary.vin[0].prevout.vout=0;
        ordinary.vout.clear();
        for(unsigned j=0;j<10;++j)ordinary.vout.emplace_back(AmountUna::Una(1),Bytes(9000,0x61));
        wires.push_back(ordinary.Serialize(TxSerializationMode::WithWitness));
        const auto block=OrchardBlockCandidate::DecodeExact(Framing(wires));
        Require(block.CheckSizeLimits(error)==(block.BaseSize()<=1000000));
    }
    Require(!OrchardBlockCandidate::DecodeExact(Framing(wires)).CheckSizeLimits(error) && error=="block-base-size");
    // Witness-heavy transactions remain individually below 100 KB while their
    // aggregate weight crosses four million before the base crosses one MB.
    wires={cb};
    for(unsigned i=0;i<20;++i) {
        auto ordinary=Ordinary(30+i);ordinary.vin[0].prevout.vout=0;ordinary.witness_version=1;
        ordinary.vin[0].witness={Bytes(59000,0x61)};ordinary.vout.clear();
        for(unsigned j=0;j<5;++j)ordinary.vout.emplace_back(AmountUna::Una(1),Bytes(7900,0x61));
        const auto wire=ordinary.Serialize(TxSerializationMode::WithWitness);Require(wire.size()<100000);
        wires.push_back(wire);
        const auto block=OrchardBlockCandidate::DecodeExact(Framing(wires));
        Require(block.BaseSize()<1000000);
        Require(block.CheckSizeLimits(error)==(block.Weight()<=4000000));
    }
    Require(!OrchardBlockCandidate::DecodeExact(Framing(wires)).CheckSizeLimits(error) && error=="block-weight");
}
static_assert(!std::is_default_constructible_v<OrchardBlockCandidate>);
static_assert(!std::is_convertible_v<OrchardBlockCandidate,Block>);
int main(int argc,char**argv) {
 try {
    Require(argc==2);const auto orchard=Load(std::string(argv[1])+"/candidate-envelope.bin");
    Sizes(orchard);
    const auto old=Ordinary(1).Serialize(TxSerializationMode::WithWitness);
    const auto mixed=Framing({old,orchard});const auto parsed=OrchardBlockCandidate::DecodeExact(mixed);
    Require(parsed.Transactions().size()==2 && !parsed.Transactions()[0].IsOrchard() && parsed.Transactions()[1].IsOrchard());
    Require(parsed.WireBytes()==mixed && parsed.MatchesTransactionRoot() && !parsed.Utreexo());
    Require(!Block::Deserialize(mixed)); // No live admission route has opened.
    // Independent two-leaf SHA256d oracle, without the common Merkle routine.
    Bytes pair=TransactionSerializer::DoubleSHA256Bytes(Ordinary(1).Serialize(TxSerializationMode::WithoutWitness));
    const auto txid=Load(std::string(argv[1])+"/candidate-envelope.txid");pair.insert(pair.end(),txid.begin(),txid.end());
    const auto expected=TransactionSerializer::DoubleSHA256Bytes(pair);
    Require(std::equal(expected.begin(),expected.end(),parsed.Header().merkle_root.begin()));
    pair.assign(32,0);const auto wtxid=Load(std::string(argv[1])+"/candidate-envelope.wtxid");pair.insert(pair.end(),wtxid.begin(),wtxid.end());
    const auto witness=TransactionSerializer::DoubleSHA256Bytes(pair);const auto got=parsed.WitnessRoot();
    Require(std::equal(witness.begin(),witness.end(),got.begin()));
    std::string error;
    Require(parsed.CheckIdentityCommitments(false,error));
    Require(!parsed.CheckIdentityCommitments(true,error) && error=="missing-witness-commitment");
    pair=witness;pair.resize(64,0);const auto dinw_hash=TransactionSerializer::DoubleSHA256Bytes(pair);
    Bytes dinw{0x6a,37,0x44,0x4e,0x52,0x57,1};dinw.insert(dinw.end(),dinw_hash.begin(),dinw_hash.end());
    Require(consensus::BuildWitnessCommitmentFromRoot(got)==dinw);
    auto coinbase=Ordinary(1);coinbase.vout.emplace_back(AmountUna::Zero(),dinw);
    const auto committed=OrchardBlockCandidate::DecodeExact(Framing({coinbase.Serialize(TxSerializationMode::WithWitness),orchard}));
    Require(committed.CheckIdentityCommitments(true,error));
    coinbase.vout.back().scriptPubKey.back()^=1;
    const auto bad_commitment=OrchardBlockCandidate::DecodeExact(Framing({coinbase.Serialize(TxSerializationMode::WithWitness),orchard}));
    Require(!bad_commitment.CheckIdentityCommitments(false,error)); // Recognized DINW always checked.
    Require(!OrchardBlockCandidate::DecodeExact(Framing({orchard,old})).CheckIdentityCommitments(false,error));
    for(size_t n=0;n<mixed.size();++n) Reject([&]{(void)OrchardBlockCandidate::DecodeExact(std::span(mixed).first(n));});
    auto bad=mixed;bad.push_back(0);Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});
    bad=mixed;bad[36]^=1;Require(!OrchardBlockCandidate::DecodeExact(bad).MatchesTransactionRoot());
    bad=mixed;bad[128]=0;Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});
    bad=mixed;bad[128]=253;bad.insert(bad.begin()+129,{2,0});Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});
    bad=mixed;bad[128]=255;bad.insert(bad.begin()+129,8,255);Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});
    bad.assign(4000001,0);Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});
    for(unsigned flag:{1,2,255}) {bad=mixed;bad.back()=flag;Reject([&]{(void)OrchardBlockCandidate::DecodeExact(bad);});}
    const auto duplicated=OrchardBlockCandidate::DecodeExact(Framing({old,orchard,orchard,orchard}));
    Require(!duplicated.MatchesTransactionRoot());
    // New canonical proof suffix uses the actual shared Utreexo codec.
    consensus::BlockUtreexoData proof;proof.accumulator_root_before.assign(32,0);
    Bytes suffix{1};const auto proof_bytes=proof.serialize();suffix.insert(suffix.end(),proof_bytes.begin(),proof_bytes.end());
    const auto with_proof=OrchardBlockCandidate::DecodeExact(Framing({old,orchard},suffix));
    Require(with_proof.Utreexo() && with_proof.Utreexo()->serialize()==proof_bytes);
    suffix.push_back(0);Reject([&]{(void)OrchardBlockCandidate::DecodeExact(Framing({old,orchard},suffix));});
    // Historical tree roots/mutation flags remain identical for every prefix.
    std::vector<Transaction> historical;std::vector<TxId> ids;std::vector<WTxId> wids;
    for(unsigned i=0;i<33;++i){
      historical.push_back(Ordinary(i));ids.push_back(historical.back().GetTxid());wids.push_back(historical.back().GetWtxid());
      bool a=false,b=false;
      Require(consensus::ComputeMerkleRoot(historical,&a)==consensus::ComputeTransactionMerkleRoot(ids,&b) && a==b);
      Require(consensus::ComputeWitnessMerkleRoot(historical,&a)==consensus::ComputeWitnessMerkleRootFromIds(wids,&b) && a==b);
    }
    std::cout<<"Orchard candidate block: bounded ordered mixed decoding, exact suffix, identities and historical Merkle parity passed\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
