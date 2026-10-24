#include "consensus/orchard_snapshot.h"
#include "primitives/orchard_block_reader.h"
#include "consensus/utxo_set_digest.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/tx_validation.h"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include "util/hex.h"
#include "wallet/runtime_replay_spool.h"
#include <fstream>
#include <set>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero::consensus {
namespace {
void Require(bool ok,int line=__builtin_LINE()) {
    if(!ok)throw std::invalid_argument("invalid v6 snapshot container at check "+std::to_string(line));
}
constexpr uint64_t MaximumScript=10000;
class Input {
public:
    Input(std::istream& stream,uint64_t maximum):stream_(stream),maximum_(maximum){}
    void Read(std::span<uint8_t> bytes) {
        Require(bytes.size()<=maximum_-used_);
        stream_.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));Require(bool(stream_));
        hash_.Write(bytes.data(),bytes.size());used_+=bytes.size();
    }
    std::vector<uint8_t> Bytes(uint64_t count) {
        Require(count<=maximum_-used_ && count<=size_t(-1));
        std::vector<uint8_t> bytes(size_t(count),uint8_t{});
        Read(bytes);return bytes;
    }
    uint64_t Number(unsigned width) {const auto b=Bytes(width);uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(b[i])<<(8*i);return n;}
    uint256 Hash(){const auto b=Bytes(32);uint256 h;std::copy(b.begin(),b.end(),h.begin());return h;}
    uint256 Finish() {
        Require(32<=maximum_-used_);uint256 actual,expected;hash_.Finalize(actual.data);
        stream_.read(reinterpret_cast<char*>(expected.data),32);Require(bool(stream_)&&expected==actual);
        Require(stream_.peek()==std::char_traits<char>::eof()&&!stream_.bad());used_+=32;return actual;
    }
    uint64_t Used()const{return used_;}
private:
    std::istream& stream_;uint64_t maximum_,used_=0;crypto::CSHA256 hash_;
};
class Output {
public:
    explicit Output(std::ostream& stream):stream_(stream){}
    void Bytes(std::span<const uint8_t> bytes) {
        stream_.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));Require(bool(stream_));
        hash_.Write(bytes.data(),bytes.size());
    }
    void Number(uint64_t n,unsigned width){uint8_t b[8];for(unsigned i=0;i<width;++i){b[i]=uint8_t(n);n>>=8;}Bytes({b,width});}
    void Hash(const uint256& h){Bytes({h.data,32});}
    void Finish(){uint256 digest;hash_.Finalize(digest.data);stream_.write(reinterpret_cast<const char*>(digest.data),32);Require(bool(stream_));}
private:
    std::ostream& stream_;crypto::CSHA256 hash_;
};
void CheckMetadata(const SnapshotMetadata& m,const OrchardSnapshotSection& s,uint64_t count) {
    Require(m.magic==SNAPSHOT_MAGIC&&m.version==SNAPSHOT_VERSION_V6&&m.reserved==0&&
        m.block_hash==s.state.block_hash&&m.block_height==s.state.height&&m.utxo_count==count&&
        m.timestamp==OrchardBlockCandidate::DecodeExact(s.base_body).Header().timestamp);
    Require(s.profile!=OrchardSnapshotProfile::Compact||count==0);
}
SnapshotMetadata Metadata(Input& input) {
    SnapshotMetadata m;m.magic=uint32_t(input.Number(4));m.version=uint32_t(input.Number(4));m.block_hash=input.Hash();
    m.block_height=uint32_t(input.Number(4));m.utxo_count=input.Number(8);m.timestamp=input.Number(8);m.reserved=input.Number(8);
    Require(m.magic==SNAPSHOT_MAGIC&&m.version==SNAPSHOT_VERSION_V6&&m.reserved==0&&
        m.block_height>0&&m.block_height<=uint32_t(INT32_MAX));return m;
}
// A coin-coverage audit is not a live full-forest owner. Canonical roots can
// be rebuilt with the existing stump addition algorithm, substituting the
// canonical ZERO_HASH at each tombstone. Keep only a disk leaf-position index
// instead of constructing the full forest's node and leaf-index allocations.
class CoinForestAudit {
    using Spool=wallet::detail::RuntimeReplaySpool;
    Spool index_;
    std::optional<UtreexoForest> legacy_;
    uint64_t leaves_=0,active_=0;
    static Spool::Bytes Key(const UtreexoHash& hash) {
        Require(hash.size()==32);Spool::Bytes key{2};key.insert(key.end(),hash.begin(),hash.end());return key;
    }
public:
    explicit CoinForestAudit(const OrchardSnapshotSection& s)
        :index_(s.disk_nodes?s.disk_nodes->WorkDirectory():std::filesystem::path{},true) {
        const auto& bytes=s.accumulator;
        if(bytes.size()<2||bytes[0]!=3||bytes[1]!=1) {
            // Preserve the existing strict behavior for pre-canonical flags;
            // do not reinterpret historical deleted-subtree semantics.
            legacy_=DecodeOrchardSnapshotForest(bytes);leaves_=legacy_->getNumLeaves();active_=legacy_->getActiveLeaves();
            Require(UtreexoStump::fromForest(*legacy_).serialize()==s.base_catalog.stump);return;
        }
        Require(bytes.size()<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);size_t at=2;
        const auto raw=[&](size_t count) {
            Require(at<=bytes.size()&&count<=bytes.size()-at);const auto result=std::span(bytes).subspan(at,count);at+=count;return result;
        };
        const auto number=[&](unsigned width) {const auto b=raw(width);uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(b[i])<<(8*i);return n;};
        leaves_=number(8);Require(leaves_<=MAX_UTREEXO_LEAVES&&leaves_<=bytes.size());
        const auto root_count=number(4);Require(root_count==std::popcount(leaves_));
        const auto encoded_roots=raw(size_t(root_count)*32);Require(number(4)==leaves_);
        std::vector<uint8_t> absent(size_t((leaves_+7)/8),0);uint64_t tombstones=0;
        UtreexoStump rebuilt;std::unique_ptr<Spool::Batch> batch;unsigned pending=0;
        for(uint64_t i=0;i<leaves_;++i) {
            const auto present=number(1);Require(present<=1);UtreexoHash hash(32,0);
            if(present) {
                const auto wire=raw(32);std::copy(wire.begin(),wire.end(),hash.begin());
                Spool::Bytes position;for(unsigned j=0;j<8;++j)position.push_back(uint8_t(i>>(8*j)));
                if(!batch)batch=std::make_unique<Spool::Batch>(index_);
                index_.Insert(Key(hash),position);++active_;
                if(++pending==1024){batch->Commit();batch.reset();pending=0;}
            } else {absent[size_t(i/8)]|=uint8_t(1u<<(i%8));++tombstones;}
            rebuilt.addSingle(hash);
        }
        if(batch){batch->Commit();batch.reset();}index_.Freeze();
        Require(number(4)==tombstones);std::optional<uint64_t> previous;
        for(uint64_t i=0;i<tombstones;++i) {
            const auto position=number(8);Require(position<leaves_&&(!previous||position>*previous)&&
                (absent[size_t(position/8)]&uint8_t(1u<<(position%8))));previous=position;
        }
        Require(at==bytes.size()&&rebuilt.serialize()==s.base_catalog.stump);
        size_t offset=0;for(const auto& root:rebuilt.getRoots()) {
            Require(root.size()==32&&std::equal(root.begin(),root.end(),encoded_roots.begin()+offset));offset+=32;
        }
        Require(offset==encoded_roots.size());
    }
    uint64_t Leaves()const{return leaves_;}
    uint64_t Active()const{return active_;}
    std::optional<uint64_t> Find(const UtreexoHash& hash)const {
        if(legacy_)return legacy_->findLeafPosition(hash);
        const auto bytes=index_.Get(Key(hash));if(!bytes)return {};
        Require(bytes->size()==8);uint64_t position=0;for(unsigned i=0;i<8;++i)position|=uint64_t((*bytes)[i])<<(8*i);
        Require(position<leaves_);return position;
    }
};
}
SnapshotMetadata ReadOrchardSnapshotMetadata(std::istream& stream,uint64_t maximum) {
    Input input(stream,maximum);return Metadata(input);
}
void VerifyOrchardSnapshotCoins(const OrchardSnapshotSection& s,const OrchardSnapshotCoins& coins) {
    if(s.profile==OrchardSnapshotProfile::Compact){Require(coins.empty());return;}
    const CoinForestAudit forest(s);
    // These fields are not in Utreexo leaves. A transparent full snapshot must
    // not erase authenticated nontransparent membership or invent its contents.
    Require(s.base_catalog.nontransparent_count==0);
    const auto read=[&](const uint256& id){return ReadOrchardSnapshotNode(s,id);};
    storage::catalog::Tree legacy(storage::catalog::Kind::LegacyCoins,read);
    Transaction genesis;std::vector<uint8_t> genesis_wire;size_t consumed=0;
    Require(util::unhex(Params().genesis.genesisCoinbaseHex,genesis_wire)&&
        TransactionSerializer::Deserialize(genesis,genesis_wire,consumed)&&consumed==genesis_wire.size());
    const auto genesis_id=genesis.GetTxid();uint64_t genesis_count=0;
    // A bit per forest position, not a separately allocated ordered-set node
    // per coin. Duplicate live-leaf claims still refuse exactly as before.
    std::vector<uint8_t> positions(size_t((forest.Leaves()+7)/8),0);
    uint64_t positions_count=0,amount=0,legacy_count=0;
    for(const auto& [point,coin]:coins) {
        Require(UtxoRecordIsSnapshotSafe(coin)&&coin.height<=s.state.height&&coin.value.GetUna()<=MAX_MONEY&&
            coin.scriptPubKey.size()<=MaximumScript&&coin.value.GetUna()<=MAX_MONEY-amount);
        amount+=coin.value.GetUna();
        if(coin.height<s.base_catalog.leaf_activation) {
            ++legacy_count;
        }
        if(point.txid==genesis_id) {
            // Genesis seeding keeps these exact compiled coin rows but does
            // not add them to Utreexo. They are not publisher-chosen leaves.
            Require(point.vout<genesis.vout.size()&&coin.height==0&&coin.isCoinbase&&
                coin.value==genesis.vout[point.vout].value&&coin.scriptPubKey==genesis.vout[point.vout].scriptPubKey);
            ++genesis_count;continue;
        }
        const auto position=forest.Find(HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,
            coin.value.GetUna(),coin.scriptPubKey,coin.height,coin.isCoinbase));
        Require(position&&*position<forest.Leaves());
        const auto bit=uint8_t(1u<<(*position%8));auto& covered=positions[size_t(*position/8)];
        Require(!(covered&bit));covered|=bit;++positions_count;
    }
    // Walk the authenticated metadata once instead of doing O(log N) disk
    // reads per coin. Every leaf must name an existing old-height coin with
    // exactly matching metadata; equal counts then prove complete coverage.
    uint64_t matched=0;
    legacy.ForEachNode(s.base_catalog.legacy,s.base_catalog.legacy_count,[&](const auto&,const auto& bytes) {
        const auto node=storage::catalog::Node::Decode(bytes);if(node.bit!=256)return true;
        uint256 id;std::copy_n(reinterpret_cast<const uint8_t*>(node.value.data()),32,id.begin());
        const OutPoint point(TxId(id),uint32_t(storage::catalog::Number(node.value,32,4)));
        const auto it=coins.find(point);Require(it!=coins.end());const auto& coin=it->second;
        Require(coin.height<s.base_catalog.leaf_activation&&coin.height==storage::catalog::Number(node.value,36,4)&&
            coin.isCoinbase==(uint8_t(node.value[40])!=0));++matched;return true;
    });
    Require(legacy_count==matched&&matched==s.base_catalog.legacy_count&&positions_count==forest.Active()&&
        coins.size()==positions_count+genesis_count);
    storage::catalog::Tree transactions(storage::catalog::Kind::Transactions,read);
    if(transactions.Find(s.parent_catalog.transactions,storage::catalog::TransactionKey(genesis_id)))
        Require(genesis_count==genesis.vout.size());
}
OrchardSnapshotFile ReadOrchardSnapshotFile(std::istream& stream,uint64_t maximum,OrchardSnapshotProfile profile,
    const std::filesystem::path& workspace) {
    Require(maximum>=108);Input input(stream,maximum);OrchardSnapshotFile result;result.metadata=Metadata(input);const auto& m=result.metadata;
    Require(m.utxo_count<=(maximum-108)/53&&
        (profile!=OrchardSnapshotProfile::Compact||m.utxo_count==0));
    std::optional<OutPoint> previous;
    for(uint64_t i=0;i<m.utxo_count;++i) {
        const auto txid=input.Hash();const auto index=uint32_t(input.Number(4));const OutPoint point(TxId(txid),index);
        Require(!previous||*previous<point);previous=point;
        const auto value=input.Number(8);Require(value<=MAX_MONEY);UTXOEntry coin;coin.value=AmountUna::Una(value);
        const auto length=input.Number(4);Require(length<=MaximumScript);coin.scriptPubKey=input.Bytes(length);
        coin.height=uint32_t(input.Number(4));const auto flag=input.Number(1);Require(flag<=1&&coin.height<=m.block_height);
        coin.isCoinbase=flag!=0;Require(result.coins.emplace(point,std::move(coin)).second);
    }
    const auto length=input.Number(8);Require(length<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
    // The byte budget is checked before allocation, not after a short read.
    Require(length<=maximum-input.Used()&&maximum-input.Used()-length>=32);
    result.orchard=ReadOrchardSnapshotSection(length,[&](auto bytes){input.Read(bytes);},workspace);
    result.checksum=input.Finish();result.bytes=input.Used();Require(result.orchard.profile==profile);
    CheckMetadata(m,result.orchard,result.coins.size());VerifyOrchardSnapshotCoins(result.orchard,result.coins);return result;
}
void WriteOrchardSnapshotFile(std::ostream& stream,const SnapshotMetadata& m,
    const OrchardSnapshotSection& s,const OrchardSnapshotCoins& coins) {
    CheckMetadata(m,s,coins.size());VerifyOrchardSnapshotCoins(s,coins);
    const auto section_size=OrchardSnapshotSectionEncodedSize(s);Output output(stream);
    output.Number(m.magic,4);output.Number(m.version,4);output.Hash(m.block_hash);output.Number(m.block_height,4);
    output.Number(m.utxo_count,8);output.Number(m.timestamp,8);output.Number(m.reserved,8);
    std::vector<OutPoint> sorted;sorted.reserve(coins.size());for(const auto& [point,coin]:coins)sorted.push_back(point);
    std::sort(sorted.begin(),sorted.end());
    for(const auto& point:sorted) {
        const auto& coin=coins.at(point);const auto& id=point.txid.AsUint256();output.Hash(id);
        output.Number(point.vout,4);output.Number(coin.value.GetUna(),8);output.Number(coin.scriptPubKey.size(),4);
        output.Bytes(coin.scriptPubKey);output.Number(coin.height,4);output.Number(coin.isCoinbase?1:0,1);
    }
    output.Number(section_size,8);
    Require(StreamOrchardSnapshotSection(s,[&](auto bytes){output.Bytes(bytes);})==section_size);output.Finish();
}
SnapshotExportResult WriteOrchardSnapshotFileAtomically(const std::filesystem::path& path,const SnapshotMetadata& header,
    const OrchardSnapshotSection& section,const OrchardSnapshotCoins& coins) {
    SnapshotExportResult result{};std::filesystem::path directory,temporary;bool owned_directory=false,published=false;
    try {
        const auto random=secure_random_bytes(16);std::string suffix;
        for(const auto byte:random){suffix.push_back("0123456789abcdef"[byte>>4]);suffix.push_back("0123456789abcdef"[byte&15]);}
        directory=path;directory+=".staging-"+suffix;
        Require(std::filesystem::create_directory(directory));owned_directory=true;temporary=directory/"snapshot";
        {std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);Require(bool(stream));
         WriteOrchardSnapshotFile(stream,header,section,coins);stream.flush();Require(bool(stream));stream.close();Require(!stream.fail());}
        // Read the actual bytes back before publication. A failed export must
        // leave any existing destination intact, including out-of-space errors.
        const auto size=std::filesystem::file_size(temporary);
        {std::ifstream stream(temporary,std::ios::binary);const auto checked=ReadOrchardSnapshotFile(stream,size,section.profile,directory);
         result.checksum=checked.checksum;result.bytes_written=checked.bytes;}
#ifdef _WIN32
        HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(handle!=INVALID_HANDLE_VALUE);const auto flushed=FlushFileBuffers(handle);CloseHandle(handle);Require(flushed);
        Require(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
        published=true;
#else
        const int descriptor=open(temporary.c_str(),O_RDONLY);Require(descriptor>=0);
        const int flushed=fsync(descriptor);const int closed=close(descriptor);Require(flushed==0&&closed==0);
        std::filesystem::rename(temporary,path);
        published=true;
        const auto parent=path.has_parent_path()?path.parent_path():std::filesystem::path(".");
        const int parent_descriptor=open(parent.c_str(),O_RDONLY);Require(parent_descriptor>=0);
        const int parent_flushed=fsync(parent_descriptor);const int parent_closed=close(parent_descriptor);
        Require(parent_flushed==0&&parent_closed==0);
#endif
        std::filesystem::remove(directory);directory.clear();result.success=true;result.utxos_exported=coins.size();
        result.block_hash=header.block_hash;result.block_height=header.block_height;
    } catch(const std::exception& error) {
        result.success=false;result.error_message=error.what();
        if(published)result.error_message="v6 snapshot was published but durability confirmation failed: "+result.error_message;
        // Only this operation's exclusively created file/directory is removed.
        // Never recursively remove a caller path or an existing destination.
        std::error_code ignored;if(!temporary.empty())std::filesystem::remove(temporary,ignored);
        if(owned_directory&&!directory.empty())std::filesystem::remove(directory,ignored);
    }
    return result;
}
} // namespace dinero::consensus
