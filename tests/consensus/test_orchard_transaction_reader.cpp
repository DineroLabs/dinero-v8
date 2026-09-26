#include "primitives/transaction_reader.h"
#ifdef DINERO_READER_ROOT_BLOCK_TESTS
#include "primitives/block.h"
#endif
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <type_traits>

using namespace dinero;
using Bytes=std::vector<uint8_t>;
static constexpr auto kFull=TxSerializationMode::WithWitness;
static constexpr auto kBase=TxSerializationMode::WithoutWitness;
static constexpr auto kStaged=TransactionReadMode::StagedOrchard;
static constexpr auto kHistorical=TransactionReadMode::HistoricalOnly;
static_assert(!std::is_default_constructible_v<ParsedTransaction>);
static_assert(!std::is_convertible_v<ParsedTransaction,Transaction>);
static_assert(!std::is_convertible_v<orchard::TransactionEnvelope,Transaction>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<ParsedTransaction>().Historical())>>);
static void Require(bool ok, std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("reader check failed at line "+std::to_string(at.line()));
}
template<class F> static void Reject(F fn) {
    bool rejected=false;
    try { fn(); } catch(const std::exception&) { rejected=true; }
    Require(rejected);
}
static Bytes Load(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("missing fixture "+path);
    return {std::istreambuf_iterator<char>(file),{}};
}
static Transaction HistoricalFixture(int32_t version) {
    Transaction tx; tx.version=version; tx.lockTime=42;
    TxInput in;
    uint256 h; h.data[0]=42; in.prevout.txid=TxId(h); in.prevout.vout=1;
    in.scriptSig={0x51}; in.witness={{0x12,0x34},{}};
    tx.vin={in}; tx.vout.emplace_back(AmountUna::Una(10),Bytes{0x51});
    if(Transaction::IsShieldedVersion(version)) {
        tx.SetExplicitFee(1); tx.shielded_bundle_bytes={0x11,0x22,0x33};
    }
    return tx;
}
int main(int argc,char** argv) {
    try {
        Require(argc==2);
        const auto wire=Load(std::string(argv[1])+"/candidate-envelope.bin");
        const auto parsed=ParsedTransaction::DecodeExact(wire,kStaged);
        Require(parsed.IsOrchard() && parsed.Serialize(kFull)==wire);
        const auto txid=Load(std::string(argv[1])+"/candidate-envelope.txid");
        const auto wtxid=Load(std::string(argv[1])+"/candidate-envelope.wtxid");
        Require(std::equal(txid.begin(),txid.end(),parsed.GetTxid().AsUint256().begin()));
        Require(std::equal(wtxid.begin(),wtxid.end(),parsed.GetWtxid().AsUint256().begin()));
        Require(parsed.GetWeight()==3*parsed.GetBaseSize()+wire.size());
        const auto base_hash=TransactionSerializer::DoubleSHA256Bytes(parsed.Serialize(kBase));
        Require(base_hash==txid);
        Reject([&]{ (void)parsed.Historical(); });
        Reject([&]{ (void)ParsedTransaction::DecodeExact(wire,kHistorical); });
        Reject([&]{ (void)ParsedTransaction::DecodeExact(wire,static_cast<TransactionReadMode>(9)); });
        // The live, backend-independent parser must reject instead of returning
        // a ten-byte empty v7 prefix. A failed attempt does not publish a shell.
        auto previous=HistoricalFixture(2);
        const auto before=previous.Serialize(kFull);
        size_t consumed=999;
        Require(!TransactionSerializer::Deserialize(previous,wire,consumed));
        Require(consumed==0 && previous.Serialize(kFull)==before);
        for(size_t n=0;n<wire.size();++n)
            Reject([&]{ (void)ParsedTransaction::DecodeExact(std::span(wire).first(n),kStaged); });
        for(size_t offset=6;offset<19;++offset) {
            auto bad=wire; bad[offset]^=0xff;
            Reject([&]{ (void)ParsedTransaction::DecodePrefix(bad,kStaged); });
        }
        auto extra=wire; extra.push_back(0);
        Require(ParsedTransaction::DecodePrefix(extra,kStaged).second==wire.size());
        Reject([&]{ (void)ParsedTransaction::DecodeExact(extra,kStaged); });
        Bytes stream;
        std::vector<Transaction> historical;
        // Numeric v7 and the formerly unreserved compact number stay ordinary
        // historical transactions unless the disjoint zero-input/output marker
        // is present. v5/v6 bundles here test encoding, not proof validity.
        for(int32_t version:{1,2,5,6,7,Transaction::TX_VERSION_COMPACT_REGTEST}) {
            auto tx=HistoricalFixture(version);
            const auto bytes=tx.Serialize(kFull);
            for(auto mode:{kHistorical,kStaged}) {
                const auto old=ParsedTransaction::DecodeExact(bytes,mode);
                Require(!old.IsOrchard()); Reject([&]{ (void)old.Orchard(); });
                Require(old.Serialize(kFull)==bytes && old.Serialize(kBase)==tx.Serialize(kBase));
                Require(old.GetTxid()==tx.GetTxid() && old.GetWtxid()==tx.GetWtxid());
                Require(old.GetSize()==tx.GetSize() && old.GetWeight()==tx.GetWeight());
            }
            stream.insert(stream.end(),bytes.begin(),bytes.end());
            stream.insert(stream.end(),wire.begin(),wire.end());
            historical.push_back(std::move(tx));
        }
        size_t offset=0;
        for(const auto& expected:historical) {
            auto [old,n]=ParsedTransaction::DecodePrefix(std::span(stream).subspan(offset),kStaged);
            Require(!old.IsOrchard() && old.GetTxid()==expected.GetTxid()); offset+=n;
            auto [next,m]=ParsedTransaction::DecodePrefix(std::span(stream).subspan(offset),kStaged);
            Require(next.IsOrchard() && next.GetTxid()==parsed.GetTxid()); offset+=m;
        }
        Require(offset==stream.size());
        #ifdef DINERO_READER_ROOT_BLOCK_TESTS
        // Current Block::Deserialize still consumes only historical Transaction.
        // The staged reader must not accidentally make Orchard block-admissible.
        Block block; block.vtx={HistoricalFixture(2)};
        const auto block_wire=block.Serialize();
        Bytes block_bytes(block_wire.begin(),block_wire.end());
        Require(Block::Deserialize(block_bytes).has_value());
        // Header 128 bytes and one-byte count precede this single transaction.
        block_bytes.resize(129); block_bytes.insert(block_bytes.end(),wire.begin(),wire.end());
        Require(!Block::Deserialize(block_bytes).has_value());
        #endif
        std::cout << "Typed reader: Orchard identities, historical formats, mixed streams, bounds and closed legacy admission passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
