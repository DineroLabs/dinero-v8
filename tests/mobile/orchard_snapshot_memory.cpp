#include "consensus/orchard_snapshot.h"
#include "consensus/chainparams.h"
#include "consensus/utxo_set_digest.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "primitives/orchard_block_reader.h"
#include "wallet/runtime_replay_spool.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>

// Capacity benchmark, NOT an authenticated bootstrap fixture. The catalog and
// outer coin/forest binding are canonical; the synthetic frontier/body do not
// assert a valid chain transition. Real authority/install/restart tests remain
// OrchardSnapshotRuntime. Never enroll this fixture in a running node.
namespace {
using namespace dinero;
using namespace dinero::consensus;
namespace c=storage::catalog;
uint256 Id(uint64_t n) {
    uint256 h;
    for(unsigned i=0;i<8;++i)h.data[i]=uint8_t(n>>(56-8*i));
    h.data[31]=1;return h;
}
using Spool=wallet::detail::RuntimeReplaySpool;
uint256 Tree(const Spool& leaves,c::Kind kind,OrchardSnapshotCatalog& inventory) {
    struct Frame{uint16_t bit;uint256 left;};std::vector<Frame> stack;stack.reserve(256);
    const auto space=uint8_t(kind);Spool::Bytes after{space};c::Key previous{};uint256 current;bool have=false;
    const auto put=[&](const c::Node& node){const auto bytes=node.Encode();const auto id=c::Hash(bytes);inventory.InsertUnique(id,bytes);return id;};
    const auto combine=[&]{const auto top=stack.back();stack.pop_back();c::Node node{kind};node.bit=top.bit;node.zero=top.left;node.one=current;current=put(node);};
    // Sorted-leaf Patricia construction needs at most one frame per key bit,
    // not a vector of all leaves or abandoned persistent insertion paths.
    while(const auto row=leaves.Next(space,after)) {
        const auto node=c::Node::Decode(std::string(row->second.begin(),row->second.end()));
        if(have) {
            unsigned bit=0;
            while(bit<256&&((previous[bit/8]^node.key[bit/8])&(1u<<(7-bit%8)))==0)++bit;
            if(bit==256)throw std::runtime_error("duplicate benchmark key");
            while(!stack.empty()&&stack.back().bit>=bit)combine();
            stack.push_back({uint16_t(bit),current});if(stack.size()>256)throw std::runtime_error("benchmark depth");
        }
        current=put(node);previous=node.key;have=true;after=row->first;
    }
    while(!stack.empty())combine();return current;
}
OrchardSnapshotFile Fixture(uint64_t count,OrchardSnapshotProfile profile,const std::filesystem::path& workspace) {
    if(!count||count>10000000)throw std::runtime_error("count must be 1..10000000");
    OrchardSnapshotFile f;auto& s=f.orchard;s.profile=profile;
    const uint32_t height=150000+uint32_t(count)-1;
    s.state={height,{},Id(1),0,count,std::string(4096,'F')};
    s.retirement={{2,Id(90),17,150000,3,Id(91),Id(55),0,Id(60),0,0},height,{},Id(92)};
    s.nullifiers.reserve(count);s.anchors.reserve(count);
    for(uint64_t i=1;i<=count;++i){s.nullifiers.push_back(Id(i));s.anchors.push_back({Id(i),1});}
    auto& parent=s.parent_catalog;parent.network=2;parent.genesis=Id(90);parent.branch=17;
    parent.activation=150000;parent.leaf_activation=GetUtreexoMaturityLeafActivationHeight();
    parent.height=height-1;parent.block=Id(92);parent.parent=Id(93);parent.work=arith_uint256{10};
    if(parent.height+1!=parent.activation){parent.previous_record=Id(94);parent.undo=Id(95);}
    Spool leaves(workspace,true);std::unique_ptr<Spool::Batch> batch;unsigned pending=0;
    const auto leaf=[&](const c::Node& node) {
        if(!batch)batch=std::make_unique<Spool::Batch>(leaves);
        Spool::Bytes key{uint8_t(node.kind)};key.insert(key.end(),node.key.begin(),node.key.end());
        const auto bytes=node.Encode();leaves.Insert(key,{reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});
        if(++pending==1024){batch->Commit();batch.reset();pending=0;}
    };
    UtreexoForest forest;forest.setCanonicalEmptyRoots(true);
    for(uint64_t i=1;i<=count;++i) {
        const OutPoint point(TxId(Id(i)),0);const auto bytes=c::OutpointBytes(point);
        c::Node tx{c::Kind::Transactions};tx.key=c::TransactionKey(point.txid);leaf(tx);
        c::Node old{c::Kind::LegacyCoins};old.key=c::LegacyKey(bytes);old.value=bytes;
        c::Number(old.value,1,4);old.value.push_back(0);leaf(old);
        if(profile==OrchardSnapshotProfile::Full) {
            UTXOEntry coin;coin.value=AmountUna::Una(1);coin.scriptPubKey={0x51};coin.height=1;
            forest.add(HashUTXOForCreationHeight(point.txid.AsUint256(),0,1,coin.scriptPubKey,1,false));
            f.coins.emplace(point,std::move(coin));
        } else {
            c::Node other{c::Kind::NonTransparentCoins};other.key=c::NonTransparentKey(bytes);other.value=bytes;
            leaf(other);
        }
    }
    if(batch){batch->Commit();batch.reset();}leaves.Freeze();
    auto inventory=std::make_shared<OrchardSnapshotCatalog>(workspace);
    parent.transactions=Tree(leaves,c::Kind::Transactions,*inventory);parent.transaction_count=count;
    parent.legacy=Tree(leaves,c::Kind::LegacyCoins,*inventory);parent.legacy_count=count;
    parent.nontransparent=Tree(leaves,c::Kind::NonTransparentCoins,*inventory);
    inventory->Seal();s.disk_nodes=std::move(inventory);
    parent.nontransparent_count=profile==OrchardSnapshotProfile::Compact?count:0;
    parent.stump=UtreexoStump::fromForest(forest).serialize();
    BlockHeader header;header.version=1;header.prev_block_hash=parent.block;
    header.timestamp=1700000000;header.difficulty=0x207fffff;
    const auto commitment=forest.getCommitment();std::copy(commitment.begin(),commitment.end(),header.utreexo_root.begin());
    Transaction cb;cb.version=2;TxInput input;input.prevout.vout=UINT32_MAX;input.scriptSig={1,1};cb.vin={input};
    cb.vout.emplace_back(AmountUna::Una(1),std::vector<uint8_t>{0x51});header.merkle_root=cb.GetTxid().AsUint256();
    const auto prefix=header.SerializeForHash();s.base_body.assign(prefix.begin(),prefix.end());s.base_body.push_back(1);
    const auto tx=cb.Serialize(TxSerializationMode::WithWitness);s.base_body.insert(s.base_body.end(),tx.begin(),tx.end());
    s.base_body.push_back(0);(void)OrchardBlockCandidate::DecodeExact(s.base_body);
    s.state.block_hash=header.GetHash();s.retirement.block_hash=s.state.block_hash;
    s.base_catalog=parent;s.base_catalog.height=height;s.base_catalog.block=s.state.block_hash;
    s.base_catalog.parent=parent.block;s.base_catalog.work=arith_uint256{11};s.base_catalog.undo=Id(96);
    s.base_catalog.previous_record=c::Hash(parent.Encode());
    if(profile==OrchardSnapshotProfile::Full) {
        s.accumulator=forest.serializeCanonicalLeaves(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);forest=UtreexoForest{};
    } else s.accumulator=parent.stump;
    f.metadata.version=SNAPSHOT_VERSION_V6;f.metadata.block_height=height;f.metadata.block_hash=s.state.block_hash;
    f.metadata.timestamp=header.timestamp;f.metadata.utxo_count=f.coins.size();return f;
}
uint64_t PeakBytes() {
    rusage usage{};if(getrusage(RUSAGE_SELF,&usage))throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return uint64_t(usage.ru_maxrss);
#else
    return uint64_t(usage.ru_maxrss)*1024;
#endif
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5)throw std::runtime_error("usage: orchard_snapshot_memory generate|read|proof-roundtrip full|compact COUNT FILE");
        const std::string phase=argv[1],name=argv[2];
        if(name!="full"&&name!="compact")throw std::runtime_error("unknown profile");
        const auto profile=name=="full"?OrchardSnapshotProfile::Full:OrchardSnapshotProfile::Compact;
        size_t end=0;const auto count=std::stoull(argv[3],&end);
        if(end!=std::string(argv[3]).size())throw std::runtime_error("invalid count");
        SelectParams(Chain::REGTEST);
        const auto start=std::chrono::steady_clock::now();uint64_t nodes=0;
        const auto workspace=std::filesystem::path(argv[4]).parent_path();
        if(phase=="generate") {
            if(std::filesystem::exists(argv[4]))throw std::runtime_error("benchmark output already exists");
            auto f=Fixture(count,profile,workspace);nodes=OrchardSnapshotNodeCount(f.orchard);
            std::ofstream output(argv[4],std::ios::binary);WriteOrchardSnapshotFile(output,f.metadata,f.orchard,f.coins);
            output.close();if(!output)throw std::runtime_error("benchmark write failed");
        } else if(phase=="read"||phase=="proof-roundtrip") {
            std::ifstream input(argv[4],std::ios::binary);
            const auto f=ReadOrchardSnapshotFile(input,std::filesystem::file_size(argv[4]),profile,workspace);
            if(f.orchard.nullifiers.size()!=count||f.orchard.anchors.size()!=count)throw std::runtime_error("fixture count mismatch");
            nodes=OrchardSnapshotNodeCount(f.orchard);
            if(!f.orchard.nodes.empty()||!f.orchard.disk_nodes)throw std::runtime_error("catalog was materialized in memory");
            if(phase=="proof-roundtrip") {
                // Exercise immutable proof encode/decode with the imported
                // inventory retained, as restart/recovery does. No DB authority.
                const auto proof=std::filesystem::path(std::string(argv[4])+".proof");
                if(std::filesystem::exists(proof))throw std::runtime_error("proof output already exists");
                std::ofstream out(proof,std::ios::binary);
                const auto size=StreamOrchardSnapshotSection(f.orchard,[&](auto bytes){
                    out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
                    if(!out)throw std::runtime_error("proof write failed");
                });
                out.close();if(!out)throw std::runtime_error("proof close failed");
                std::ifstream in(proof,std::ios::binary);
                const auto decoded=ReadOrchardSnapshotSection(size,[&](auto bytes){
                    in.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
                    if(!in)throw std::runtime_error("proof read failed");
                },workspace);
                if(!decoded.disk_nodes||!decoded.nodes.empty()||
                    HashOrchardSnapshotSection(decoded)!=HashOrchardSnapshotSection(f.orchard))throw std::runtime_error("proof changed");
                in.close();std::filesystem::remove(proof);
            }
        } else throw std::runtime_error("unknown phase");
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"{\"phase\":\""<<phase<<"\",\"profile\":\""<<name<<"\",\"count\":"<<count
            <<",\"catalog_nodes\":"<<nodes<<",\"file_bytes\":"<<std::filesystem::file_size(argv[4])
            <<",\"peak_rss_bytes\":"<<PeakBytes()<<",\"elapsed_ms\":"<<ms<<",\"authority\":\"synthetic-capacity-only\"}\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
