#include "daemon/services/snapshot_wallet_source.h"
#include "consensus/utxo_snapshot.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/outpoint.h"
#include <bit>
#include <limits>
#include <optional>
#include <stdexcept>

namespace dinero {
namespace {
constexpr uint64_t MaxCoins=1000000,MaxLeaves=1000000,MaxBytes=256ULL*1024*1024;
class Reader {
    std::istream& input_;uint64_t used_=0;
public:
    explicit Reader(std::istream& input):input_(input){}
    void Bytes(uint8_t* out,size_t size){
        if(size>MaxBytes-used_)throw std::runtime_error("Snapshot wallet source exceeds byte limit");
        input_.read(reinterpret_cast<char*>(out),std::streamsize(size));
        if(!input_)throw std::runtime_error("Snapshot wallet source truncated");
        used_+=size;
    }
    uint64_t Number(unsigned bytes){uint8_t buffer[8]{};Bytes(buffer,bytes);uint64_t n=0;
        for(unsigned i=0;i<bytes;++i)n|=uint64_t(buffer[i])<<(8*i);return n;}
    uint256 Hash(){uint256 h;Bytes(h.data,32);return h;}
};
uint64_t NumberAt(const std::vector<uint8_t>& bytes,size_t offset,unsigned count){
    if(offset>bytes.size()||count>bytes.size()-offset)throw std::runtime_error("Snapshot wallet forest header truncated");
    uint64_t n=0;for(unsigned i=0;i<count;++i)n|=uint64_t(bytes[offset+i])<<(8*i);return n;
}
}
SnapshotWalletSourceData ReadSnapshotWalletSource(std::istream& input,
        const BlockHeader& base,uint32_t height,const Block& genesis){
    Reader read(input);SnapshotWalletSourceData result;
    if(read.Number(4)!=consensus::SNAPSHOT_MAGIC)throw std::runtime_error("Snapshot wallet magic mismatch");
    const auto version=read.Number(4);
    if(version!=consensus::SNAPSHOT_VERSION_V3&&version!=consensus::SNAPSHOT_VERSION_V4&&
       version!=consensus::SNAPSHOT_VERSION_V5)throw std::runtime_error("Snapshot wallet container unsupported");
    result.base=read.Hash();result.height=uint32_t(read.Number(4));
    if(!height||result.height!=height||result.base!=base.GetHash())throw std::runtime_error("Snapshot wallet base mismatch");
    const auto count=read.Number(8);(void)read.Number(8);(void)read.Number(8);
    if(count>MaxCoins)throw std::runtime_error("Snapshot wallet coin count exceeds limit");
    result.coins.reserve(size_t(count));std::optional<OutPoint> previous;
    for(uint64_t i=0;i<count;++i){
        SnapshotWalletCoin coin;coin.txid=read.Hash();coin.output=uint32_t(read.Number(4));coin.amount=read.Number(8);
        const auto size=read.Number(4);
        if(size>100000||coin.amount>uint64_t(INT64_MAX))throw std::runtime_error("Snapshot wallet coin range invalid");
        coin.script.resize(size_t(size));read.Bytes(coin.script.data(),coin.script.size());
        coin.height=uint32_t(read.Number(4));const auto flags=read.Number(1);
        if(coin.height>height||flags>1)throw std::runtime_error("Snapshot wallet creation metadata invalid");
        coin.coinbase=flags!=0;coin.maturity_bound=consensus::IsUtreexoMaturityLeafActive(coin.height);
        const OutPoint point(TxId(coin.txid),coin.output);
        if(previous&&!(previous.value()<point))throw std::runtime_error("Snapshot wallet records not unique and ordered");
        previous=point;result.coins.push_back(std::move(coin));
    }
    if(read.Number(4)!=consensus::SNAPSHOT_V3_UTREEXO_MAGIC||
       read.Number(4)!=consensus::SNAPSHOT_V3_UTREEXO_SECTION_VERSION)
        throw std::runtime_error("Snapshot wallet forest section invalid");
    const auto size=read.Number(8),leaves=read.Number(8);(void)read.Number(8);const auto root=read.Hash();
    if(size>128ULL*1024*1024||leaves>MaxLeaves||root!=base.utreexo_root)
        throw std::runtime_error("Snapshot wallet forest identity or capacity invalid");
    std::vector<uint8_t> bytes(size_t(size),uint8_t(0));read.Bytes(bytes.data(),bytes.size());
    // Bound work BEFORE the deserializer rebuilds roots/indexes from untrusted
    // counts. Accept current v2/v3 full or leaf-only checkpoints, not v1 guesses.
    if(bytes.empty()||(bytes[0]!=2&&bytes[0]!=3))throw std::runtime_error("Snapshot wallet forest encoding unsupported");
    const size_t offset=bytes[0]==3?2:1;
    const auto declared=NumberAt(bytes,offset,8),roots=NumberAt(bytes,offset+8,4);
    if(declared!=leaves||roots>64||NumberAt(bytes,offset+12+size_t(roots)*32,4)>4*MaxLeaves+64)
        throw std::runtime_error("Snapshot wallet forest counts invalid");
    const auto forest=consensus::UtreexoForest::deserialize(bytes);
    const auto commitment=forest.getCommitment();
    if(forest.getNumLeaves()!=leaves||commitment.size()!=32||
       !std::equal(commitment.begin(),commitment.end(),root.begin()))
        throw std::runtime_error("Snapshot wallet forest not authenticated by base header");
    std::vector<uint8_t> seen(size_t((leaves+7)/8),0);uint64_t covered=0,compiled=0;
    if(genesis.vtx.empty())throw std::runtime_error("Snapshot wallet genesis unavailable");
    const auto& generation=genesis.vtx.front();
    for(const auto& coin:result.coins){
        const bool is_genesis=coin.txid==generation.GetTxid().AsUint256();
        if(is_genesis){
            if(coin.output>=generation.vout.size()||coin.height||!coin.coinbase||
               coin.amount!=generation.vout[coin.output].value.GetUna()||coin.script!=generation.vout[coin.output].scriptPubKey)
                throw std::runtime_error("Snapshot wallet compiled genesis coin mismatch");
            ++compiled;
        }
        const auto leaf=consensus::HashUTXOForCreationHeight(coin.txid,coin.output,coin.amount,coin.script,coin.height,coin.coinbase);
        const auto position=forest.findLeafPosition(leaf);
        if(!position){if(!is_genesis)throw std::runtime_error("Snapshot wallet coin absent from authenticated forest");continue;}
        if(*position>=leaves||forest.isDeleted(*position))throw std::runtime_error("Snapshot wallet coin is not live");
        auto& marker=seen[size_t(*position/8)];const auto bit=uint8_t(1u<<(*position%8));
        if(marker&bit)throw std::runtime_error("Snapshot wallet leaf covered twice");marker|=bit;++covered;
    }
    for(uint64_t at=0;at<leaves;++at){
        const bool present=(seen[size_t(at/8)]&uint8_t(1u<<(at%8)))!=0;
        if(present==forest.isDeleted(at))throw std::runtime_error("Snapshot wallet source omits or invents a live leaf");
    }
    if(covered!=forest.getActiveLeaves()||compiled!=generation.vout.size())
        throw std::runtime_error("Snapshot wallet source coverage incomplete");
    return result;
}
}
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <array>
#endif
namespace dinero {
SnapshotWalletSourceData ReadSnapshotWalletSourceFile(const std::string& path,
        const BlockHeader& base,uint32_t height,const Block& genesis){
#ifdef _WIN32
    (void)path;(void)base;(void)height;(void)genesis;
    throw std::runtime_error("Snapshot wallet file recovery unavailable on this platform");
#else
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw std::runtime_error("Snapshot wallet source cannot be opened safely");
    struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat info{};
    if(::fstat(fd,&info)||!S_ISREG(info.st_mode)||info.st_size<64||uint64_t(info.st_size)>256ULL*1024*1024)
        throw std::runtime_error("Snapshot wallet source is not a bounded regular file");
    class Buffer final:public std::streambuf{
        int fd_;std::array<char,65536> bytes_{};
        int_type underflow()override{
            ssize_t n;do{n=::read(fd_,bytes_.data(),bytes_.size());}while(n<0&&errno==EINTR);
            if(n<=0)return traits_type::eof();setg(bytes_.data(),bytes_.data(),bytes_.data()+n);
            return traits_type::to_int_type(*gptr());
        }
    public:explicit Buffer(int fd):fd_(fd){}
    } buffer(fd);
    std::istream input(&buffer);return ReadSnapshotWalletSource(input,base,height,genesis);
#endif
}
}
