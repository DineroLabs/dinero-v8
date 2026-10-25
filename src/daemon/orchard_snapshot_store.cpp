#include "daemon/orchard_snapshot_store.h"
#include "daemon/datadir_guard.h"
#include "daemon/core_reopen.h"
#include "daemon/chainstate_recovery_marker.h"
#include "daemon/services/orchard_branch_replay.h"
#include "daemon/services/orchard_boundary_replay.h"
#include "daemon/services/orchard_history_capture.h"
#include "daemon/runtime_block_outbox.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_header.h"
#include "consensus/orchard_catalog_validation.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainwork.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "common/annotated_mutex.h"
#include "storage/block_storage.h"
#include "storage/forest_restore.h"
#include "storage/orchard_storage_mode.h"
#include "storage/shielded_cf_comparator.h"
#include "storage/orchard_activation_origin.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/anchor_history.h"
#include "crypto/secure_random.h"
#include "util/hex.h"
#include "common/serialization.h"
#include <rocksdb/iterator.h>
#include <map>
#include <set>
#include <exception>
#include <thread>
#include <utility>
#include <ctime>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#endif

namespace dinero {
namespace {
using namespace consensus;
void Require(bool ok,int line=__builtin_LINE()) {
    if(!ok)throw std::runtime_error("Orchard snapshot selected storage refused at check "+std::to_string(line));
}
template<class T>T Need(StatusOr<T> value){Require(value.ok());return std::move(*value);}
void Need(Status value){Require(value==Status::Ok);}
Transaction CompiledGenesis() {
    Transaction genesis;std::vector<uint8_t> wire;size_t consumed=0;
    Require(util::unhex(Params().genesis.genesisCoinbaseHex,wire)&&
        TransactionSerializer::Deserialize(genesis,wire,consumed)&&consumed==wire.size());return genesis;
}
OrchardSnapshotCoins ReadCoins(const ChainDB& db) {
    OrchardSnapshotCoins coins;
    Need(db.forEachUTXO([&](const uint256& id,uint32_t index,const Coin& stored){
        Require(stored.height>=0);UTXOEntry coin;coin.value=AmountUna::Una(stored.amount);
        Require(util::unhex(stored.script_pubkey,coin.scriptPubKey));coin.height=uint32_t(stored.height);
        coin.isCoinbase=stored.coinbase;coin.is_confidential=stored.is_confidential;coin.commitment=stored.commitment;
        Require(coins.emplace(OutPoint(TxId(id),index),std::move(coin)).second);return true;
    }));return coins;
}
std::unique_ptr<ConsensusUTXOSet> FullPublication(const OrchardSnapshotSection& s,const OrchardSnapshotCoins& coins) {
    VerifyOrchardSnapshotCoins(s,coins);auto live=std::make_unique<ConsensusUTXOSet>();
    Require(live->BulkLoad(coins,s.state.height,s.state.block_hash));
    live->ReplaceForestGuarded(UtreexoForest::deserialize(s.accumulator));return live;
}
// Separate namespace from v6 snapshot-base/floor metadata. The descriptor is
// only a durable reference to untrusted proof bytes; every use reauthenticates.
std::string ActivationProofKey(const uint256& first) {
    Require(!first.IsNull());return "orchard_activation_proof_v1:record:"+first.GetHex();
}
std::string ActivationProofChunkKey(const uint256& digest,uint64_t part) {
    return "orchard_activation_proof_v1:chunk:"+digest.GetHex()+":"+std::to_string(part);
}
std::string ActivationHeadersKey(const uint256& first){
    Require(!first.IsNull());return "orchard_activation_headers_v1:"+first.GetHex();
}
struct ActivationHeadersDescriptor {
    uint256 first,target;uint32_t height=0;
    std::string Encode()const{
        const auto& p=Params();const auto end=uint64_t(p.orchard_activation_height)+p.state_commitment_burial_depth;
        Require(p.orchard_activation_height&&p.orchard_activation_height!=UINT32_MAX&&end<=INT32_MAX&&
            height==end&&!first.IsNull()&&!target.IsNull());
        std::string out="DNOHC01";storage::catalog::Number(out,p.magic,4);out+=ConsensusChecksum(p);
        storage::catalog::Number(out,height,4);
        for(const auto& hash:{first,target})out.append(reinterpret_cast<const char*>(hash.data),32);
        const auto digest=storage::catalog::Hash(out);out.append(reinterpret_cast<const char*>(digest.data),32);return out;
    }
    static ActivationHeadersDescriptor Decode(const std::string& bytes){
        Require(bytes.size()==175&&bytes.starts_with("DNOHC01"));ActivationHeadersDescriptor d;
        d.height=uint32_t(storage::catalog::Number(bytes,75,4));
        std::copy_n(reinterpret_cast<const uint8_t*>(bytes.data()+79),32,d.first.begin());
        std::copy_n(reinterpret_cast<const uint8_t*>(bytes.data()+111),32,d.target.begin());
        Require(d.Encode()==bytes);return d;
    }
};
std::string ActivationHeaderRow(const BlockHeader& header,uint32_t height,const arith_uint256& work){
    Require(height<=INT32_MAX);VectorWriter encoded;Serialize(encoded,header);encoded.write(height);encoded.write(work);
    return encoded.release_string();
}
void ActivationHeaderSpace(const std::filesystem::path& workspace){
    std::error_code error;const auto space=std::filesystem::space(workspace,error);
    Require(!error&&space.available>=(uint64_t(512)<<20)+uint64_t(1<<20));
}
wallet::detail::RuntimeReplaySpool::Bytes ActivationHeaderCacheKey(const uint256& hash){
    wallet::detail::RuntimeReplaySpool::Bytes key{'h'};key.insert(key.end(),hash.begin(),hash.end());return key;
}
std::unique_ptr<wallet::detail::RuntimeReplaySpool> ActivationHeaderCache(rocksdb::DB& db,
    rocksdb::ColumnFamilyHandle* family,const std::filesystem::path& workspace,const std::function<bool()>& cancelled){
    using Spool=wallet::detail::RuntimeReplaySpool;ActivationHeaderSpace(workspace);
    auto cache=std::make_unique<Spool>(workspace,true);
    std::vector<std::pair<Spool::Bytes,Spool::Bytes>> rows;rows.reserve(256);
    const auto flush=[&]{if(!rows.empty()){
        Require(!cancelled||!cancelled());ActivationHeaderSpace(workspace);
        {Spool::Batch batch(*cache);for(const auto& [key,value]:rows)cache->Insert(key,value);batch.Commit();}rows.clear();
    }};
    // One checksummed CF stream, not one giant-SST materialization/checksum
    // per ancestry lookup. The temporary cache authenticates values, NEVER
    // absence; retain falls back to the actual DB before any missing-row Put.
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;
    std::unique_ptr<rocksdb::Iterator> it(db.NewIterator(reads,family));
    for(it->Seek("h");it->Valid()&&it->key().starts_with("h");it->Next()){
        Require(!cancelled||!cancelled());const auto key=it->key();
        if(key.size()!=65||it->value().size()!=128+sizeof(uint32_t)+sizeof(arith_uint256))continue;
        uint256 hash;const auto name=key.ToString();
        if(!uint256::FromHex(name.substr(1),hash)||name!="h"+hash.GetHex())continue;
        const auto bytes=it->value();rows.emplace_back(ActivationHeaderCacheKey(hash),
            Spool::Bytes(reinterpret_cast<const uint8_t*>(bytes.data()),reinterpret_cast<const uint8_t*>(bytes.data()+bytes.size())));
        if(rows.size()==256)flush();
    }Require(it->status().ok());flush();cache->Freeze();return cache;
}
std::unique_ptr<wallet::detail::RuntimeReplaySpool> ActivationNodeCache(rocksdb::DB& db,
    rocksdb::ColumnFamilyHandle* family,const std::filesystem::path& workspace,const std::function<bool()>& cancelled){
    using Spool=wallet::detail::RuntimeReplaySpool;ActivationHeaderSpace(workspace);
    auto cache=std::make_unique<Spool>(workspace,true);std::vector<std::pair<Spool::Bytes,Spool::Bytes>> rows;rows.reserve(256);
    const auto flush=[&]{if(!rows.empty()){
        Require(!cancelled||!cancelled());ActivationHeaderSpace(workspace);
        {Spool::Batch batch(*cache);for(const auto& [key,value]:rows)cache->Insert(key,value);batch.Commit();}rows.clear();
    }};
    const std::string prefix="Morchard_catalog_v1/node/";rocksdb::ReadOptions options;
    options.verify_checksums=true;options.fill_cache=false;
    std::unique_ptr<rocksdb::Iterator> it(db.NewIterator(options,family));
    for(it->Seek(prefix);it->Valid()&&it->key().starts_with(prefix);it->Next()){
        Require(!cancelled||!cancelled());Require(it->key().size()==prefix.size()+32);
        uint256 id;std::memcpy(id.data,it->key().data()+prefix.size(),32);const auto bytes=it->value().ToString();
        Require(storage::catalog::ValidNode(id,bytes));
        rows.emplace_back(Spool::Bytes(id.begin(),id.end()),Spool::Bytes(bytes.begin(),bytes.end()));
        if(rows.size()==256)flush();
    }Require(it->status().ok());flush();cache->Freeze();return cache;
}
struct ActivationProofDescriptor {
    uint256 first,parent,digest;uint64_t bytes=0;
    std::string Encode()const {
        Require(!first.IsNull()&&!parent.IsNull()&&!digest.IsNull()&&bytes&&bytes<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
        std::string out="DNOAP01";storage::catalog::Number(out,Params().magic,4);
        // The consensus checksum intentionally does not cover wire magic.
        const auto profile=ConsensusChecksum(Params());Require(profile.size()==64);out+=profile;
        for(const auto& h:{first,parent,digest})out.append(reinterpret_cast<const char*>(h.data),32);
        storage::catalog::Number(out,bytes,8);return out;
    }
    static ActivationProofDescriptor Decode(const std::string& in) {
        Require(in.size()==179&&in.compare(0,7,"DNOAP01")==0);
        ActivationProofDescriptor d;size_t at=75;
        for(auto* h:{&d.first,&d.parent,&d.digest}) {
            std::copy_n(reinterpret_cast<const uint8_t*>(in.data()+at),32,h->begin());at+=32;
        }
        d.bytes=storage::catalog::Number(in,at,8);Require(d.Encode()==in);return d;
    }
};
OrchardSnapshotSection ReadActivationProofChunks(const ChainDB& db,const ActivationProofDescriptor& d,
    const std::filesystem::path& work,const std::function<bool()>& cancelled) {
    crypto::CSHA256 hash;uint64_t loaded=0,part=0;size_t position=0;std::string chunk;
    auto proof=ReadOrchardSnapshotSection(d.bytes,[&](std::span<uint8_t> out){
        Require(!cancelled||!cancelled());
        while(!out.empty()) {
            if(position==chunk.size()) {
                Require(loaded<d.bytes);Need(db.getRaw(ActivationProofChunkKey(d.digest,part++),chunk));
                Require(chunk.size()==std::min<uint64_t>(storage::OrchardSnapshotProofChunkBytes,d.bytes-loaded));
                hash.Write(chunk);loaded+=chunk.size();position=0;
            }
            const auto n=std::min(out.size(),chunk.size()-position);
            std::copy_n(reinterpret_cast<const uint8_t*>(chunk.data()+position),n,out.data());
            position+=n;out=out.subspan(n);
        }
    },work);
    uint256 digest;hash.Finalize(digest.data);
    Require(loaded==d.bytes&&position==chunk.size()&&digest==d.digest&&
        proof.state.block_hash==d.first&&proof.parent_catalog.block==d.parent);
    Require(!cancelled||!cancelled());return proof;
}
#ifndef _WIN32
struct HandoffFd {
    int value=-1;HandoffFd()=default;explicit HandoffFd(int fd):value(fd){Require(fd>=0);}
    HandoffFd(HandoffFd&& other)noexcept:value(std::exchange(other.value,-1)){}
    HandoffFd& operator=(HandoffFd&& other)noexcept{
        if(value>=0)::close(value);value=std::exchange(other.value,-1);return *this;
    }
    HandoffFd(const HandoffFd&)=delete;~HandoffFd(){if(value>=0)::close(value);}
};
struct stat HandoffStat(int fd){struct stat s{};Require(::fstat(fd,&s)==0);return s;}
struct stat HandoffNamed(int parent,const std::string& name){
    struct stat s{};Require(::fstatat(parent,name.c_str(),&s,AT_SYMLINK_NOFOLLOW)==0);return s;
}
bool HandoffSame(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool HandoffSameFile(const struct stat& a,const struct stat& b){
#ifdef __APPLE__
    const auto am=a.st_mtimespec,ac=a.st_ctimespec,bm=b.st_mtimespec,bc=b.st_ctimespec;
#else
    const auto am=a.st_mtim,ac=a.st_ctim,bm=b.st_mtim,bc=b.st_ctim;
#endif
    return HandoffSame(a,b)&&a.st_size==b.st_size&&a.st_mode==b.st_mode&&a.st_uid==b.st_uid&&a.st_nlink==b.st_nlink&&
        am.tv_sec==bm.tv_sec&&am.tv_nsec==bm.tv_nsec&&ac.tv_sec==bc.tv_sec&&ac.tv_nsec==bc.tv_nsec;
}
struct HandoffFile {
    uint64_t device,inode,links,size,mode,uid;int64_t modified,changed,modified_ns,changed_ns;
    bool operator==(const HandoffFile&)const=default;
};
using HandoffFiles=std::map<std::string,HandoffFile>;
HandoffFiles InspectHandoffFiles(const std::filesystem::path& path,
    const std::filesystem::path& paired_sst={},const std::function<bool()>& cancelled={}){
    namespace fs=std::filesystem;HandoffFiles result;
    Require(fs::canonical(path)==path.lexically_normal());
    for(const auto& item:fs::recursive_directory_iterator(path)){
        Require(!cancelled||!cancelled());struct stat s{};Require(::lstat(item.path().c_str(),&s)==0&&
            s.st_uid==::geteuid()&&(S_ISREG(s.st_mode)||S_ISDIR(s.st_mode)));
        Require(result.size()<100000);
        if(S_ISREG(s.st_mode)&&s.st_nlink!=1){
            // Existing separated stores can share only their immutable SST
            // with the exact retained layout-upgrade rollback counterpart.
            Require(s.st_nlink==2&&!paired_sst.empty()&&item.path().extension()==".sst");
            const auto pair=paired_sst/item.path().lexically_relative(path);struct stat p{};
            Require(fs::canonical(pair)==pair.lexically_normal()&&::lstat(pair.c_str(),&p)==0&&
                S_ISREG(p.st_mode)&&p.st_nlink==2&&p.st_uid==s.st_uid&&HandoffSame(p,s));
        }
#ifdef __APPLE__
        const auto m=s.st_mtimespec,c=s.st_ctimespec;
#else
        const auto m=s.st_mtim,c=s.st_ctim;
#endif
        result.emplace(item.path().lexically_relative(path).generic_string(),HandoffFile{
            uint64_t(s.st_dev),uint64_t(s.st_ino),uint64_t(s.st_nlink),uint64_t(s.st_size),
            uint64_t(s.st_mode),uint64_t(s.st_uid),m.tv_sec,c.tv_sec,m.tv_nsec,c.tv_nsec});
    }
    return result;
}
void HandoffSync(int fd){Require(::fsync(fd)==0);}
constexpr const char* HandoffIntent=storage::OrchardHandoffSelected;
constexpr const char* HandoffDone=storage::OrchardHandoffCompleted;
bool HandoffAbsent(int parent,const std::string& name){
    struct stat s{};if(::fstatat(parent,name.c_str(),&s,AT_SYMLINK_NOFOLLOW)==0)return false;
    Require(errno==ENOENT);return true;
}
void HandoffRename(int parent,const std::string& from,const std::string& to){
    // The retained datadir guard is not a license to overwrite another name.
    // Require a kernel no-replace primitive; no exists()/rename() fallback.
#ifdef __APPLE__
    Require(::renameatx_np(parent,from.c_str(),parent,to.c_str(),RENAME_EXCL)==0);
#elif defined(__linux__) && defined(SYS_renameat2)
    Require(::syscall(SYS_renameat2,parent,from.c_str(),parent,to.c_str(),RENAME_NOREPLACE)==0);
#else
    (void)parent;(void)from;(void)to;Require(false);
#endif
    HandoffSync(parent);
}
uint256 HandoffFilesDigest(const HandoffFiles& files){
    crypto::CSHA256 hash;hash.Write("DIN/orchard/handoff/files/v1");
    for(const auto& [name,f]:files){
        std::string row;storage::catalog::Number(row,uint64_t(name.size()),8);row+=name;
        for(const auto value:{f.device,f.inode,f.links,f.size,f.mode,f.uid,uint64_t(f.modified),
            uint64_t(f.changed),uint64_t(f.modified_ns),uint64_t(f.changed_ns)})storage::catalog::Number(row,value,8);
        hash.Write(row);
    }uint256 result;hash.Finalize(result.data);return result;
}
std::string HandoffControl(int parent,const std::string& name){
    Require(HandoffAbsent(parent,name+".partial"));if(HandoffAbsent(parent,name))return {};
    HandoffFd file(::openat(parent,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    const auto before=HandoffStat(file.value);
    Require(S_ISREG(before.st_mode)&&before.st_uid==::geteuid()&&before.st_nlink==1&&
        (before.st_mode&0777)==0400&&before.st_size>32&&before.st_size<=4096&&
        HandoffSame(before,HandoffNamed(parent,name)));
    std::string bytes(size_t(before.st_size),'\0');size_t at=0;
    while(at<bytes.size()){
        const auto n=::read(file.value,bytes.data()+at,bytes.size()-at);
        if(n<0&&errno==EINTR)continue;Require(n>0);at+=size_t(n);
    }
    const auto after=HandoffStat(file.value),named=HandoffNamed(parent,name);
    Require(HandoffSameFile(before,after)&&HandoffSameFile(before,named));
    const auto digest=storage::catalog::Hash(bytes.substr(0,bytes.size()-32));
    Require(std::equal(digest.begin(),digest.end(),reinterpret_cast<const uint8_t*>(bytes.data()+bytes.size()-32)));
    // A previous publisher may have seen an uncertain final sync. Re-establish
    // persistence of the audited explicit intent before any subsequent rename.
    HandoffSync(file.value);HandoffSync(parent);
    return bytes;
}
std::string HandoffSeal(std::string bytes){
    const auto digest=storage::catalog::Hash(bytes);bytes.append(reinterpret_cast<const char*>(digest.data),32);return bytes;
}
void HandoffPublish(int parent,const std::string& name,const std::string& bytes){
    Require(bytes.size()>32&&bytes.size()<=4096&&HandoffAbsent(parent,name)&&HandoffAbsent(parent,name+".partial"));
    HandoffFd file(::openat(parent,(name+".partial").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    const auto st=HandoffStat(file.value);Require(S_ISREG(st.st_mode)&&st.st_nlink==1&&st.st_uid==::geteuid());
    size_t at=0;while(at<bytes.size()){
        const auto n=::write(file.value,bytes.data()+at,bytes.size()-at);
        if(n<0&&errno==EINTR)continue;Require(n>0);at+=size_t(n);
    }
    Require(::fchmod(file.value,0400)==0);HandoffSync(file.value);HandoffSync(parent);
    Require(HandoffSame(st,HandoffNamed(parent,name+".partial")));HandoffRename(parent,name+".partial",name);
    Require(HandoffControl(parent,name)==bytes);
}
struct HandoffSelection {
    storage::OrchardActivationOrigin origin;
    std::string nonce,journal,wallet;
    uint64_t root_dev=0,root_ino=0,chain_dev=0,chain_ino=0,source_dev=0,source_ino=0,copy_dev=0,copy_ino=0;
    uint256 source_files,copy_files,copy_rows;
    std::string Candidate()const{return "chaindb.orchard-handoff-"+nonce;}
    std::string Original()const{return "chaindb.orchard-original-"+nonce;}
    std::string Encode()const{
        const auto hex=[](std::string_view v){return std::all_of(v.begin(),v.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});};
        const std::string prefix=".core-reopen-candidates-v1-";
        Require(nonce.size()==32&&hex(nonce)&&journal.size()==prefix.size()+32&&journal.starts_with(prefix)&&
            hex(std::string_view(journal).substr(prefix.size()))&&wallet.size()<=255&&wallet.find('\0')==std::string::npos&&
            wallet.find('/')==std::string::npos&&wallet.find('\\')==std::string::npos&&
            !source_files.IsNull()&&!copy_files.IsNull()&&!copy_rows.IsNull());
        std::string bytes="DNOSEL01";bytes+=origin.Encode();bytes+=nonce;
        for(auto n:{root_dev,root_ino,chain_dev,chain_ino,source_dev,source_ino,copy_dev,copy_ino})storage::catalog::Number(bytes,n,8);
        for(const auto& h:{source_files,copy_files,copy_rows})bytes.append(reinterpret_cast<const char*>(h.data),32);
        storage::catalog::Number(bytes,uint64_t(journal.size()),2);bytes+=journal;
        storage::catalog::Number(bytes,uint64_t(wallet.size()),2);bytes+=wallet;return HandoffSeal(bytes);
    }
    static HandoffSelection Decode(const std::string& bytes){
        Require(bytes.size()>=629&&bytes.size()<=1024&&bytes.starts_with("DNOSEL01"));HandoffSelection r;
        r.origin=storage::OrchardActivationOrigin::Decode(bytes.substr(8,339));size_t at=347;
        r.nonce=bytes.substr(at,32);at+=32;
        for(auto* n:{&r.root_dev,&r.root_ino,&r.chain_dev,&r.chain_ino,&r.source_dev,&r.source_ino,&r.copy_dev,&r.copy_ino}){
            *n=storage::catalog::Number(bytes,at,8);at+=8;
        }
        for(auto* h:{&r.source_files,&r.copy_files,&r.copy_rows}){
            std::copy_n(reinterpret_cast<const uint8_t*>(bytes.data()+at),32,h->begin());at+=32;
        }
        const auto field=[&]{Require(at+2<=bytes.size()-32);const auto n=storage::catalog::Number(bytes,at,2);at+=2;
            Require(n<=bytes.size()-32-at);auto value=bytes.substr(at,size_t(n));at+=size_t(n);return value;};
        r.journal=field();r.wallet=field();Require(at==bytes.size()-32&&r.Encode()==bytes);return r;
    }
};
void HandoffMaintenanceClear(const std::filesystem::path& root,const std::filesystem::path& source){
    for(const auto& path:{daemon::ChainstateRecoveryMarkerPath(root),root/"blockchain"/"reindex_promotion.marker",
        std::filesystem::path(source.string()+".upgrade-state"),std::filesystem::path(source.string()+".upgrade-state.tmp"),
        std::filesystem::path(source.string()+".upgrade-candidate"),std::filesystem::path(source.string()+".reindex.tmp")}){
        std::error_code error;const auto status=std::filesystem::symlink_status(path,error);
        Require(status.type()==std::filesystem::file_type::not_found&&
            (!error||error==std::errc::no_such_file_or_directory));
    }
}
#endif
}

struct PreparedOrchardActivationHandoff::Data {
    std::filesystem::path root_path,source_path,candidate_path,paired_sst;
    std::shared_ptr<const consensus::HeaderChainSelector> headers;
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof;
    std::function<bool()> cancelled;
    std::unique_ptr<ChainDB> source,candidate;
    AnnotatedRecursiveMutex candidate_mutex;
    storage::OrchardActivationOrigin origin;uint256 candidate_digest;
    const std::thread::id thread=std::this_thread::get_id();
#ifndef _WIN32
    HandoffFd root,blockchain,source_dir,candidate_dir;HandoffFiles source_files,candidate_files;
    std::string selection;
#endif
};
PreparedOrchardActivationHandoff::PreparedOrchardActivationHandoff(std::unique_ptr<Data> d):data_(std::move(d)){}
PreparedOrchardActivationHandoff::~PreparedOrchardActivationHandoff()=default;
const std::filesystem::path& PreparedOrchardActivationHandoff::CandidatePath()const{return data_->candidate_path;}
const std::filesystem::path& PreparedOrchardActivationHandoff::OriginalPath()const{return data_->source_path;}
const ChainDB& PreparedOrchardActivationHandoff::OriginalForInspection()const{return *data_->source;}
const ChainDB& PreparedOrchardActivationHandoff::CandidateForInspection()const{return *data_->candidate;}

int OrchardSnapshotStore::OpenHandoffRoot(const daemon::DatadirGuard& guard){
#ifdef _WIN32
    (void)guard;throw std::runtime_error("authenticated store handoff unavailable on this platform");
#else
    Require(guard.held_&&guard.lock_fd_>=0);
    HandoffFd root(::open(guard.datadir_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto held=HandoffStat(root.value);struct stat named{};
    Require(::lstat(guard.datadir_.c_str(),&named)==0&&HandoffSame(held,named)&&S_ISDIR(held.st_mode)&&
        held.st_uid==::geteuid()&&!(held.st_mode&0022)&&
        uint64_t(held.st_dev)==guard.root_device_&&uint64_t(held.st_ino)==guard.root_inode_);
    const auto lock=HandoffStat(guard.lock_fd_),current=HandoffNamed(root.value,"dinerod.lock");
    Require(HandoffSame(lock,current)&&S_ISREG(lock.st_mode)&&lock.st_nlink==1&&lock.st_uid==::geteuid());
    return std::exchange(root.value,-1);
#endif
}

uint256 OrchardSnapshotStore::HandoffStoreDigest(const ChainDB& db,const std::function<bool()>& cancelled){
    Require(db.hasSeparatedShieldedState());crypto::CSHA256 hash;hash.Write("DIN/orchard/handoff/store/v1");
    const auto field=[&](const rocksdb::Slice& value){
        std::string length;storage::catalog::Number(length,uint64_t(value.size()),8);hash.Write(length);
        hash.Write(reinterpret_cast<const uint8_t*>(value.data()),value.size());
    };
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;
    for(const auto& family:db.cf_){
        hash.Write(std::string(1,'\x01'));field(family->GetName());
        std::unique_ptr<rocksdb::Iterator> it(db.db_->NewIterator(reads,family.get()));
        for(it->SeekToFirst();it->Valid();it->Next()){
            Require(!cancelled||!cancelled());Require(it->key().size()<=32*1024*1024&&it->value().size()<=32*1024*1024);
            hash.Write(std::string(1,'\x02'));field(it->key());field(it->value());
        }
        Require(it->status().ok());hash.Write(std::string(1,'\x03'));
    }
    hash.Write(std::string(1,'\x04'));uint256 digest;hash.Finalize(digest.data);return digest;
}
storage::catalog::State OrchardSnapshotStore::AuditActivationSuffix(AnnotatedRecursiveMutex& mutex,const ChainDB& db,
    const ChainWriteToken& token,const BlockStorage& files,
    std::shared_ptr<const consensus::HeaderChainSelector> headers,
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancelled){
#ifdef _WIN32
    (void)mutex;(void)db;(void)token;(void)files;(void)headers;(void)proof;(void)cancelled;Require(false);return {};
#else
    using namespace consensus;using Spool=wallet::detail::RuntimeReplaySpool;
    mutex.AssertHeld("audit authenticated activation suffix");const auto check=[&]{Require(!cancelled||!cancelled());};
    check();Require(headers&&proof&&db.hasSeparatedShieldedState());VerifyCatalogTransferProof(*proof,*headers);
    const auto origin=storage::ReadOrchardActivationOrigin(db);Require(origin.has_value());
    const auto tip=Need(db.getTip()),anchor=Need(db.getValidatedTip());
    HeaderIndexEntry selected,original_anchor;
    Require(origin->magic==Params().magic&&origin->profile==ConsensusChecksum(Params())&&
        origin->first==proof->state.block_hash&&origin->parent==proof->parent_catalog.block&&
        origin->parent_height==proof->parent_catalog.height&&origin->proof==HashOrchardSnapshotSection(*proof)&&
        origin->parent_catalog==storage::catalog::Hash(proof->parent_catalog.Encode())&&
        origin->retirement==storage::OrchardSnapshotRetirementFingerprint(proof->retirement.record)&&
        tip.height>=int(origin->parent_height)&&tip.height<=INT32_MAX&&
        anchor.height==int(origin->validated_height)&&anchor.hash==origin->validated&&
        headers->GetHeaderAtHeightCopy(uint32_t(anchor.height),original_anchor)&&original_anchor.hash==anchor.hash&&
        headers->GetHeaderAtHeightCopy(uint32_t(tip.height),selected)&&selected.hash==tip.hash&&selected.chainwork==tip.work&&
        !storage::ReadOrchardSnapshotBase(db));
    const auto root_marker=Need(db.getForestTipMarker());
    Require(root_marker.height==tip.height&&root_marker.block_hash==tip.hash&&root_marker.forest_root==selected.header.utreexo_root&&
        Need(db.getHeader(tip.hash)).SerializeForHash()==selected.header.SerializeForHash()&&
        Need(db.getBlockHeight(tip.hash))==tip.height&&Need(db.getBlockWork(tip.hash))==tip.work&&
        Need(db.getBlockHashByHeight(tip.height))==tip.hash);
    bool coins=false;Need(db.forEachUTXO([&](const auto&,uint32_t,const auto&){coins=true;return false;}));Require(!coins);
    // Scratch is a namespace byte oracle ONLY. It has NO tip/validated marker,
    // forest, replay certificate, outbox origin or canonical publication owner.
    const auto workspace=std::filesystem::path(db.db_->GetName()).parent_path();ActivationHeaderSpace(workspace);
    const auto work=workspace/(".orchard-suffix-audit-"+util::hex(secure_random_bytes(16)));
    Require(::mkdir(work.c_str(),0700)==0);
    struct Workspace {
        std::filesystem::path path;HandoffFd fd;int exceptions=std::uncaught_exceptions();
        explicit Workspace(std::filesystem::path p):path(std::move(p)),fd(::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)){Require(fd.value>=0);}
        ~Workspace(){if(std::uncaught_exceptions()!=exceptions)return;try{
            struct stat named{};if(::lstat(path.c_str(),&named)||!HandoffSame(HandoffStat(fd.value),named))return;
            std::error_code error;std::filesystem::remove_all(path,error); // ONLY this newly created, pinned scratch directory.
        }catch(...){}}
    } owned(work);
    CreateFreshStore(work/"oracle");ChainDB oracle;Need(oracle.init(work/"oracle"));
    const auto nodes=ActivationNodeCache(*db.db_,db.cf_[db.idx_utreexo_].get(),work,cancelled);
    const auto node=[&](const uint256& id){check();const auto bytes=nodes->Get(Spool::Bytes(id.begin(),id.end()));
        Require(bool(bytes));return std::string(bytes->begin(),bytes->end());};
    Spool seen(work,true);std::set<Spool::Bytes> pending_seen;
    const auto flush_seen=[&]{if(!pending_seen.empty()){
        check();ActivationHeaderSpace(work);{Spool::Batch batch(seen);for(const auto& key:pending_seen)seen.Insert(key,{});batch.Commit();}pending_seen.clear();
    }};
    const auto catalog=[&](const storage::catalog::State& state,const storage::catalog::Tree::Read& expected){
        Require(Need(db.getOrchardCatalogState(state.block))==state.Encode());
        using Kind=storage::catalog::Kind;
        for(const auto& [root,kind]:{std::pair{state.transactions,Kind::Transactions},
            std::pair{state.legacy,Kind::LegacyCoins},std::pair{state.nontransparent,Kind::NonTransparentCoins}}){
            std::vector<uint256> stack;if(!root.IsNull())stack.push_back(root);
            while(!stack.empty()){
                check();Require(stack.size()<=257);const auto id=stack.back();stack.pop_back();
                Spool::Bytes key{uint8_t(kind)};key.insert(key.end(),id.begin(),id.end());
                if(pending_seen.contains(key)||seen.Get(key))continue;
                const auto bytes=node(id);Require(bytes==expected(id));const auto decoded=storage::catalog::Node::Decode(bytes);
                Require(decoded.kind==kind);if(decoded.bit!=256){stack.push_back(decoded.zero);stack.push_back(decoded.one);}
                pending_seen.insert(std::move(key));if(pending_seen.size()==256)flush_seen();
            }
        }
    };
    auto current=proof->parent_catalog;
    catalog(current,[&](const auto& id){return ReadOrchardSnapshotNode(*proof,id);});
    std::optional<storage::LegacyRetirementState> retirement;
    if(uint32_t(tip.height)>=proof->state.height){
        auto replay=ReplayActivation(proof,headers,uint32_t(tip.height),tip.hash);
        for(uint64_t h=proof->state.height;h<=uint32_t(tip.height);++h){
            check();ActivationHeaderSpace(work);HeaderIndexEntry entry;
            Require(headers->GetHeaderAtHeightCopy(uint32_t(h),entry));
            const bool witness=Params().enforce_witness_commitment&&h>=Params().witness_commitment_enforcement_height;
            const auto block=ReadStoredOrchardBlock(db,entry.hash,witness,&files);
            const auto now=std::time(nullptr);Require(now>=0);
            const auto verified=replay->Append(block,uint64_t(now));
            AuditOrchardCompactStoredStepUnderLock(db,files,block,*verified,current,replay->Catalog());
            const auto& state=verified->State();rocksdb::WriteBatch batch;
            Need(oracle.stageOrchardConnect(token,state.Parent(),state.Next(),state.Nullifiers(),state.Flows(),batch));
            const storage::LegacyRetirementState next{proof->retirement.record,uint32_t(h),entry.hash,entry.prev_hash};
            const auto undo=storage::EncodeLegacyRetirementUndo(retirement,next);
            batch.Put(oracle.shieldedStateHandle(),"R1S",storage::EncodeLegacyRetirementState(next));
            batch.Put(oracle.shieldedStateHandle(),"R1U"+std::string(reinterpret_cast<const char*>(entry.hash.data),32),undo);
            Need(oracle.writeBatch(token,std::move(batch),true));retirement=next;
            current=replay->Catalog();catalog(current,replay->Read());
        }
        replay->Finish();Require(Need(db.getOrchardState())==replay->State()&&
            Need(db.getLegacyRetirementState())==*retirement);
    }
    flush_seen();seen.Freeze();
    Require(oracle.getTip().status()==Status::NotFound&&oracle.getValidatedTip().status()==Status::NotFound);
    // Compare BOTH ENTIRE state namespaces, not just committed set digests or
    // R1S; wrong nullifier owners, anchor references and orphan undo rows cannot
    // hide behind a matching selected coinbase root.
    rocksdb::ReadOptions options;options.verify_checksums=true;options.fill_cache=false;
    std::unique_ptr<rocksdb::Iterator> actual(db.db_->NewIterator(options,db.shieldedStateHandle()));
    std::unique_ptr<rocksdb::Iterator> expected(oracle.db_->NewIterator(options,oracle.shieldedStateHandle()));
    for(const auto* prefix:{"O1","R1"}){
        actual->Seek(prefix);expected->Seek(prefix);
        for(;;){
            check();Require(actual->status().ok()&&expected->status().ok());
            const bool a=actual->Valid()&&actual->key().starts_with(prefix),e=expected->Valid()&&expected->key().starts_with(prefix);
            Require(a==e);if(!a)break;Require(actual->key()==expected->key()&&actual->value()==expected->value());
            actual->Next();expected->Next();
        }
    }
    const auto context=Authenticate(*proof,*headers,Params().state_commitment_burial_depth);
    CheckOrchardFrozenLegacyUnderLock(db,proof->retirement.record,context,uint32_t(tip.height),tip.hash);
    Require(storage::ReadOrchardCompactStorageBinding(db)==storage::EncodeOrchardCompactStorageBinding(current));
    std::string head;const auto outbox=db.getRaw("runtime_orchard_outbox:v1:head",head);
    if(outbox==Status::Ok){
        auto page=ReadRuntimeOutboxUnderLock(db,context,{},32,16*1024*1024);
        const auto captured=page.head;
        while(page.next!=captured){check();const auto before=page.next;
            page=ReadRuntimeOutboxPrefixUnderLock(db,context,captured,before,32,16*1024*1024);Require(page.next!=before);}
    }else{
        Require(outbox==Status::NotFound&&uint32_t(tip.height)==proof->parent_catalog.height);
        std::unique_ptr<rocksdb::Iterator> records(db.db_->NewIterator(options));records->Seek("runtime_orchard_outbox:v1:");
        Require(records->status().ok()&&(!records->Valid()||!records->key().starts_with("runtime_orchard_outbox:v1:")));
    }
    VerifyCatalogTransferProof(*proof,*headers);check();HeaderIndexEntry final;
    Require(headers->GetHeaderAtHeightCopy(uint32_t(tip.height),final)&&final.hash==tip.hash&&final.chainwork==tip.work&&
        Need(db.getTip()).hash==tip.hash&&Need(db.getValidatedTip()).hash==anchor.hash);
    return current; // DATA only; no root/lifecycle/DB writer or compact owner.
#endif
}

storage::catalog::State OrchardSnapshotStore::AuditActivationEvents(AnnotatedRecursiveMutex& mutex,
    const ChainDB& original,const ChainDB& db,const BlockStorage& files,
    std::shared_ptr<const consensus::HeaderChainSelector> headers,
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancelled,
    const std::function<void(const RuntimeOutboxEvent&,const OrchardBlockCandidate&,
        const consensus::ValidatedOrchardBlock*)>& observed){
    using namespace consensus;mutex.AssertHeld("audit authenticated activation delivery history");
    const auto check=[&]{Require(!cancelled||!cancelled());};check();Require(headers&&proof);
    VerifyCatalogTransferProof(*proof,*headers);const auto origin=storage::ReadOrchardActivationOrigin(db);
    Require(origin&&origin->proof==HashOrchardSnapshotSection(*proof)&&
        origin->parent_catalog==storage::catalog::Hash(proof->parent_catalog.Encode())&&
        origin->source==HandoffStoreDigest(original,cancelled));
    const auto base=Need(original.getTip()),anchor=Need(original.getValidatedTip()),tip=Need(db.getTip());
    Require(base.height==int(proof->parent_catalog.height)&&base.hash==proof->parent_catalog.block&&base.work==proof->parent_catalog.work&&
        anchor.height==int(origin->validated_height)&&anchor.hash==origin->validated&&
        Need(db.getValidatedTip()).hash==anchor.hash&&Need(db.getValidatedTip()).height==anchor.height);
    CheckLegacyRetirementBeforeImageUnderLock(original,proof->retirement.record);
    const auto profile=Authenticate(*proof,*headers,Params().state_commitment_burial_depth);
    const auto old=ReadRuntimeOutboxUnderLock(original,profile,{},1).head;
    auto page=ReadRuntimeOutboxUnderLock(db,profile,old,32,16*1024*1024);const auto head=page.head;
    auto replay=std::unique_ptr<OrchardBoundaryReplay>(new OrchardBoundaryReplay(proof,headers,uint32_t(tip.height),tip.hash,true));
    const auto equal=[](const RuntimeOrchardReplay& a,const RuntimeOrchardReplay& b){
        return a.parent==b.parent&&a.next==b.next&&a.coin_undo==b.coin_undo&&a.branch_mtp==b.branch_mtp;};
    for(;;){
        for(const auto& event:page.events){
            check();Require(event.IsOrchardProfile()&&event.orchard_replay.has_value());
            const auto block=OrchardBlockCandidate::DecodeExact(event.body);
            const bool witness=Params().enforce_witness_commitment&&event.context.height>=Params().witness_commitment_enforcement_height;
            Require(ReadStoredOrchardBlock(db,event.context.block_hash,witness,&files).WireBytes()==block.WireBytes());
            if(event.direction==RuntimeBlockDirection::Connect){
                const auto now=std::time(nullptr);Require(now>=0);const auto verified=replay->Append(block,uint64_t(now));
                const RuntimeOrchardReplay actual{verified->State().Parent(),verified->State().Next(),
                    BuildOrchardCoinUndo(verified->Coins()).Serialize(),verified->BranchMtp()};
                Require(equal(actual,*event.orchard_replay));
                if(observed)observed(event,block,verified.get());
            }else{
                Require(event.direction==RuntimeBlockDirection::Disconnect&&equal(replay->Rewind(block),*event.orchard_replay));
                if(observed)observed(event,block,nullptr);
            }
        }
        if(page.next==head)break;check();const auto before=page.next;
        page=ReadRuntimeOutboxPrefixUnderLock(db,profile,head,before,32,16*1024*1024);Require(page.next!=before);
    }
    replay->Finish();Require(replay->state_==([&]()->std::optional<storage::OrchardStoredState>{
        const auto state=db.getOrchardState();if(state.status()==Status::NotFound)return {};return Need(state);}())&&
        Need(db.getOrchardCatalogState(tip.hash))==replay->Catalog().Encode());
    VerifyCatalogTransferProof(*proof,*headers);check();return replay->Catalog();
}

storage::catalog::State OrchardSnapshotStore::AuditActivationStore(AnnotatedRecursiveMutex& mutex,
    const ChainDB& original,const ChainDB& selected,const ChainWriteToken& token,const BlockStorage& files,
    const uint256& parent_rows,std::shared_ptr<const consensus::HeaderChainSelector> headers,
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancelled){
#ifdef _WIN32
    (void)mutex;(void)original;(void)selected;(void)token;(void)files;(void)parent_rows;
    (void)headers;(void)proof;(void)cancelled;Require(false);return {};
#else
    using namespace consensus;mutex.AssertHeld("audit complete activation store before-image");
    const auto check=[&]{Require(!cancelled||!cancelled());};check();Require(headers&&proof&&!parent_rows.IsNull());
    VerifyCatalogTransferProof(*proof,*headers);
    const auto workspace=std::filesystem::path(selected.db_->GetName()).parent_path();ActivationHeaderSpace(workspace);
    const auto work=workspace/(".orchard-store-audit-"+util::hex(secure_random_bytes(16)));
    Require(::mkdir(work.c_str(),0700)==0);
    struct Workspace {
        std::filesystem::path path;HandoffFd fd;int exceptions=std::uncaught_exceptions();
        explicit Workspace(std::filesystem::path p):path(std::move(p)),fd(::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)){Require(fd.value>=0);}
        ~Workspace(){if(std::uncaught_exceptions()!=exceptions)return;try{
            struct stat named{};if(::lstat(path.c_str(),&named)||!HandoffSame(HandoffStat(fd.value),named))return;
            std::error_code error;std::filesystem::remove_all(path,error); // ONLY newly created, pinned scratch.
        }catch(...){}}
    } owned(work);
    const auto source=HandoffStoreDigest(original,cancelled);
    // The original selection used the retained as-of-intent closure, NOT the
    // current selector with later header arrivals. Rebuild that exact baseline.
    const auto cold=ReadActivationHeaders(selected,proof->state.block_hash,cancelled);
    auto copy=CopyActivationParent(original,token,mutex,*cold,*proof,source,work/"expected",[]{},check,cancelled);
    auto& expected=*copy.database;
    const auto selected_origin=storage::ReadOrchardActivationOrigin(selected);
    Require(selected_origin&&HandoffStoreDigest(expected,cancelled)==parent_rows&&
        copy.origin.Encode()==selected_origin->Encode());
    auto consistency=std::shared_ptr<const OrchardSelectedConsistency>(new OrchardSelectedConsistency(expected,mutex,copy.origin));
    OrchardCompactChainstate owner;
    owner.selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
        &expected,&mutex,proof->parent_catalog,proof->retirement.record,std::move(consistency));
    const auto catalog=AuditActivationEvents(mutex,original,selected,files,headers,proof,cancelled,
        [&](const RuntimeOutboxEvent& event,const OrchardBlockCandidate& block,const ValidatedOrchardBlock* verified){
            check();ActivationHeaderSpace(work);
            const auto& context=event.context;const auto& header=block.Header();
            const auto checked=headers->GetHeaderValue(context.block_hash);
            Require(checked&&checked->header.SerializeForHash()==header.SerializeForHash()&&checked->height==context.height&&
                checked->prev_hash==context.parent_hash);
            const auto actual=Need(selected.getHeaderMetadata(context.block_hash));
            constexpr uint32_t required=BLOCK_VALID_MASK|BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO;
            Require(actual.height==int(context.height)&&actual.parent_hash==context.parent_hash&&actual.chainwork==checked->chainwork&&
                (actual.status_flags&required)==required&&(actual.status_flags&~required)==0);
            // Only exact retained archive locators enter scratch; never append
            // or infer missing body/undo bytes. Existing before-images are fixed.
            const auto old=expected.getHeaderMetadata(context.block_hash);
            if(old.ok()){
                Require(old->height==actual.height&&old->parent_hash==actual.parent_hash&&old->chainwork==actual.chainwork&&
                    (old->status_flags&~required)==0);
                if(old->status_flags&BLOCK_HAVE_DATA)Require(old->file_number==actual.file_number&&old->data_pos==actual.data_pos&&old->data_size==actual.data_size);
                else Require(!old->file_number&&!old->data_pos&&!old->data_size);
                if(old->status_flags&BLOCK_HAVE_UNDO)Require(old->undo_file==actual.undo_file&&old->undo_pos==actual.undo_pos&&old->undo_size==actual.undo_size);
                else Require(!old->undo_file&&!old->undo_pos&&!old->undo_size);
            }else Require(old.status()==Status::NotFound);
            rocksdb::WriteBatch batch;
            Need(expected.putHeader(token,context.block_hash,header,int(context.height),checked->chainwork,&batch));
            Need(expected.putHeaderMetadata(token,context.block_hash,actual,&batch));Need(expected.writeBatch(token,std::move(batch),true));
            CBlockIndex index(header,context.height);index.chainwork=ChainworkToHex(actual.chainwork);index.status=actual.status_flags;
            index.file_number=actual.file_number;index.data_pos=actual.data_pos;index.data_size=actual.data_size;
            index.undo_file=actual.undo_file;index.undo_pos=actual.undo_pos;index.undo_size=actual.undo_size;
            const auto parent=Need(expected.getHeader(context.parent_hash));
            const OrchardBranchMtpLookup mtp=[&](uint32_t h)->std::optional<uint64_t>{
                const auto at=event.orchard_replay->branch_mtp.find(h);
                return at==event.orchard_replay->branch_mtp.end()?std::nullopt:std::optional<uint64_t>(at->second);};
            const bool witness=Params().enforce_witness_commitment&&context.height>=Params().witness_commitment_enforcement_height;
            auto write=verified?PreparedOrchardChainstateWrite::ConnectCompactPrepared(mutex,expected,token,files,nullptr,
                index,owner,context,block,parent,mtp,witness,std::nullopt,true,nullptr,verified):
                PreparedOrchardChainstateWrite::DisconnectCompactPrepared(mutex,expected,token,files,nullptr,
                    index,owner,context,block,parent,mtp,witness,nullptr);
            write->Commit();
        });
    const auto tip=Need(selected.getTip()),replayed=Need(expected.getTip());
    Require(tip.hash==replayed.hash&&tip.height==replayed.height&&tip.work==replayed.work&&
        owner.SelectedUnderLock(mutex)->Encode()==catalog.Encode());
    // The sole non-deterministic canonical write is KEY_TIP's wall-clock
    // receipt timestamp. Re-encode its exact parsed fields before accepting it;
    // no other meta key, prefix, row or schema is exempted from comparison.
    VectorWriter encoded;encoded.writeString(tip.hash.GetHex());encoded.write(uint32_t(tip.height));encoded.write(tip.work);encoded.write(tip.timestamp);
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;std::string actual_tip;
    Require(selected.db_->Get(reads,selected.cf_[selected.idx_meta_].get(),ChainDB::KEY_TIP,&actual_tip).ok()&&actual_tip==encoded.release_string());
    rocksdb::WriteBatch time;time.Put(expected.cf_[expected.idx_meta_].get(),ChainDB::KEY_TIP,actual_tip);Need(expected.writeBatch(token,std::move(time),true));
    Require(HandoffStoreDigest(expected,cancelled)==HandoffStoreDigest(selected,cancelled));
    check();VerifyCatalogTransferProof(*proof,*headers);return catalog;
#endif
}

void OrchardSnapshotStore::RetainActivationHeaders(ChainDB& db,const ChainWriteToken& token,
    const consensus::HeaderChainSelector& source,const uint256& first,const std::function<bool()>& cancelled){
    using namespace consensus;Require(db.hasSeparatedShieldedState());
    const auto check=[&]{Require(!cancelled||!cancelled());};check();
    const auto end=uint64_t(Params().orchard_activation_height)+Params().state_commitment_burial_depth;
    Require(end<=INT32_MAX);HeaderIndexEntry target,activation;
    Require(source.GetHeaderAtHeightCopy(uint32_t(end),target)&&
        source.GetHeaderAtHeightCopy(Params().orchard_activation_height,activation)&&activation.hash==first);
    ActivationHeadersDescriptor descriptor{first,target.hash,uint32_t(end)};const auto encoded=descriptor.Encode();
    const auto workspace=std::filesystem::path(db.db_->GetName()).parent_path();ActivationHeaderSpace(workspace);
    const auto existing_headers=ActivationHeaderCache(*db.db_,db.cf_[db.idx_headers_].get(),workspace,cancelled);
    OrchardHistoryCapture captured(target.height,target.hash,workspace);
    std::vector<OrchardHistoryCapture::Header> rows;rows.reserve(OrchardHistoryCapture::HeaderBatchLimit);
    auto hash=target.hash;
    for(uint64_t remaining=end+1;remaining;--remaining){
        check();const auto row=source.GetHeaderValue(hash);Require(row&&row->height==remaining-1);
        rows.push_back({row->hash,row->header,row->chainwork});hash=row->prev_hash;
        if(rows.size()==OrchardHistoryCapture::HeaderBatchLimit||remaining==1){ActivationHeaderSpace(workspace);captured.CaptureReverse(rows);rows.clear();}
    }Require(hash.IsNull());HeaderChainSelector verified;rocksdb::WriteBatch batch;unsigned pending=0;
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;
    const auto flush=[&]{if(batch.Count()){check();ActivationHeaderSpace(workspace);Need(db.writeBatch(token,std::move(batch),true));batch=rocksdb::WriteBatch();pending=0;}};
    for(uint64_t height=0;height<=end;++height){
        check();const auto row=captured.HeaderAt(uint32_t(height));Require(verified.AddHeader(row.header));
        const auto checked=verified.GetHeaderValue(row.hash);Require(checked&&checked->height==height&&checked->chainwork==row.work);
        const auto expected=ActivationHeaderRow(row.header,uint32_t(height),row.work);
        if(const auto existing=existing_headers->Get(ActivationHeaderCacheKey(row.hash))){
            Require(std::string(existing->begin(),existing->end())==expected);
        }else{
            std::string stored;const auto status=db.db_->Get(reads,db.cf_[db.idx_headers_].get(),db.makeHeaderKey(row.hash),&stored);
            Require(status.IsNotFound()||(status.ok()&&stored==expected));
            if(status.IsNotFound()){Need(db.putHeader(token,row.hash,row.header,int(height),row.work,&batch));if(++pending==256)flush();}
        }
    }flush();std::string old;const auto status=db.getRaw(ActivationHeadersKey(first),old);
    Require(status==Status::NotFound||(status==Status::Ok&&old==encoded));check();
    {
        std::unique_lock<std::mutex> selected(source.mutex_);const auto* best=source.best_header_;
        const auto* current=best?best->GetAncestor(target.height):nullptr;
        Require(current&&current->hash==target.hash);
        if(status==Status::NotFound){batch.Put(ActivationHeadersKey(first),encoded);Need(db.writeBatch(token,std::move(batch),true));}
    }
    const auto cold=ReadActivationHeaders(db,first,cancelled);Require(cold->GetBestHeaderValue()->hash==target.hash);
}

std::shared_ptr<consensus::HeaderChainSelector> OrchardSnapshotStore::ReadActivationHeaders(
    const ChainDB& db,const uint256& first,const std::function<bool()>& cancelled){
    const auto check=[&]{Require(!cancelled||!cancelled());};check();
    std::string encoded;Need(db.getRaw(ActivationHeadersKey(first),encoded));
    const auto descriptor=ActivationHeadersDescriptor::Decode(encoded);
    Require(descriptor.first==first&&db.hasSeparatedShieldedState());
    const auto workspace=std::filesystem::path(db.db_->GetName()).parent_path();ActivationHeaderSpace(workspace);
    const auto stored_headers=ActivationHeaderCache(*db.db_,db.cf_[db.idx_headers_].get(),workspace,cancelled);
    OrchardHistoryCapture captured(descriptor.height,descriptor.target,workspace);
    std::vector<OrchardHistoryCapture::Header> rows;rows.reserve(OrchardHistoryCapture::HeaderBatchLimit);
    auto hash=descriptor.target;
    for(uint64_t remaining=uint64_t(descriptor.height)+1;remaining;--remaining){
        check();const auto stored=stored_headers->Get(ActivationHeaderCacheKey(hash));Require(bool(stored));
        const std::string wire(stored->begin(),stored->end());
        Reader in(wire);BlockHeader header;Deserialize(in,header);const auto height=in.read<uint32_t>();const auto work=in.read<arith_uint256>();
        Require(in.eof()&&height==remaining-1&&header.GetHash()==hash&&ActivationHeaderRow(header,height,work)==wire);
        rows.push_back({hash,header,work});hash=header.prev_block_hash;
        if(rows.size()==OrchardHistoryCapture::HeaderBatchLimit||remaining==1){ActivationHeaderSpace(workspace);captured.CaptureReverse(rows);rows.clear();}
    }Require(hash.IsNull());auto verified=std::make_shared<consensus::HeaderChainSelector>();
    for(uint64_t height=0;height<=descriptor.height;++height){
        check();const auto row=captured.HeaderAt(uint32_t(height));Require(verified->AddHeader(row.header));
        const auto checked=verified->GetHeaderValue(row.hash);Require(checked&&checked->height==height&&checked->chainwork==row.work);
    }
    consensus::HeaderIndexEntry activation;Require(verified->GetHeaderAtHeightCopy(Params().orchard_activation_height,activation)&&
        activation.hash==first&&verified->GetBestHeaderValue()->hash==descriptor.target);check();return verified;
}

OrchardSnapshotStore::ActivationParentCopy OrchardSnapshotStore::CopyActivationParent(
    const ChainDB& source,const ChainWriteToken& token,AnnotatedRecursiveMutex& mutex,
    const consensus::HeaderChainSelector& headers,const consensus::OrchardSnapshotSection& proof,
    const uint256& before,const std::filesystem::path& destination,
    const std::function<void()>& on_created,const std::function<void()>& before_choice,
    const std::function<bool()>& cancelled){
    using namespace consensus;Require(on_created&&before_choice);
    const auto check_cancel=[&]{Require(!cancelled||!cancelled());};check_cancel();
    VerifyCatalogTransferProof(proof,headers);
    const auto& parent=proof.parent_catalog;const auto validated=Need(source.getValidatedTip());
    const auto tip=Need(source.getTip());
    Require(source.hasSeparatedShieldedState()&&validated.height>=0&&validated.height<=tip.height&&
        tip.height==int(parent.height)&&tip.hash==parent.block&&tip.work==parent.work&&
        HandoffStoreDigest(source,cancelled)==before);
    // This helper cannot clear or overwrite any caller-supplied live engine.
    // Its ONLY destination is created exclusively here; the caller pins its
    // actual inode before the first DB open or row write.
    CreateFreshStore(destination);on_created();
    auto database=std::make_unique<ChainDB>();Need(database->init(destination));auto& candidate=*database;
    Require(candidate.hasSeparatedShieldedState()&&candidate.cf_.size()==source.cf_.size());
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;
    rocksdb::WriteBatch batch;unsigned rows=0;
    const auto flush=[&]{if(batch.Count()){
        check_cancel();Need(candidate.writeBatch(token,std::move(batch),true));batch=rocksdb::WriteBatch();rows=0;
    }};
    // Remove ONLY this exclusive new store's initialization rows. Every source
    // CF then streams into the copy, with checksums and bounded synced batches.
    for(const auto& family:candidate.cf_){
        std::unique_ptr<rocksdb::Iterator> it(candidate.db_->NewIterator(reads,family.get()));
        for(it->SeekToFirst();it->Valid();it->Next()){
            check_cancel();Require(batch.Delete(family.get(),it->key()).ok());if(++rows==1024)flush();
        }Require(it->status().ok());
    }flush();
    for(size_t index=0;index<source.cf_.size();++index){
        Require(candidate.cf_[index]->GetName()==source.cf_[index]->GetName());
        std::unique_ptr<rocksdb::Iterator> it(source.db_->NewIterator(reads,source.cf_[index].get()));
        for(it->SeekToFirst();it->Valid();it->Next()){
            check_cancel();Require(it->key().size()<=32*1024*1024&&it->value().size()<=32*1024*1024);
            Require(batch.Put(candidate.cf_[index].get(),it->key(),it->value()).ok());
            if(++rows==1024||batch.GetDataSize()>=4*1024*1024)flush();
        }Require(it->status().ok());
    }flush();Require(HandoffStoreDigest(candidate,cancelled)==before);
    // Header-first peers can persist their ahead-of-body ancestry only in
    // HeaderStore. Retain the exact minimal genesis-to-burial closure here too,
    // so a cold interrupted swap can revalidate it without an old RAM selector.
    RetainActivationHeaders(candidate,token,headers,proof.state.block_hash,cancelled);
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        RetainActivationProof(mutex,candidate,token,headers,proof,cancelled);
    }
    std::map<uint256,std::string> node_batch;
    const auto flush_nodes=[&]{if(!node_batch.empty()){
        check_cancel();Need(candidate.stageOrchardCatalogNodes(token,node_batch,batch));flush();node_batch.clear();
    }};
    ForEachOrchardSnapshotNode(proof,[&](const auto& id,const auto& value){
        check_cancel();const auto [at,inserted]=node_batch.emplace(id,value);Require(inserted||at->second==value);
        if(node_batch.size()==1024)flush_nodes();
    });flush_nodes();
    // Coins leave only the unselected candidate. Parse every original key and
    // value first; an unknown row is corruption, never silently dropped.
    std::unique_ptr<rocksdb::Iterator> coins(source.db_->NewIterator(reads,source.cf_[source.idx_utxo_].get()));
    for(coins->SeekToFirst();coins->Valid();coins->Next()){
        check_cancel();const auto key=coins->key();Require(key.size()==37&&key[0]==ChainDB::PREFIX_UTXO);
        uint256 id;std::reverse_copy(reinterpret_cast<const uint8_t*>(key.data()+1),reinterpret_cast<const uint8_t*>(key.data()+33),id.begin());
        uint32_t output=0;for(unsigned i=0;i<4;++i)output=(output<<8)|uint8_t(key[33+i]);
        Require(source.makeUtxoKey(id,output)==key.ToString());
        const auto value=coins->value().ToString();Reader reader(value);
        (void)reader.read<uint64_t>();(void)reader.readString();
        Require(reader.read<uint32_t>()<=uint32_t(INT32_MAX)&&reader.read<uint8_t>()<=1);
        if(!reader.eof()){
            Require(reader.read<uint8_t>()<=1);if(!reader.eof())(void)reader.readBytes();
        }
        Require(reader.eof());Need(candidate.deleteCoin(token,id,output,&batch));
        if(++rows==1024)flush();
    }Require(coins->status().ok());coins.reset();flush();
    storage::OrchardActivationOrigin origin;origin.magic=Params().magic;origin.profile=ConsensusChecksum(Params());
    origin.parent_height=parent.height;origin.validated_height=uint32_t(validated.height);
    origin.first=proof.state.block_hash;origin.parent=parent.block;origin.parent_catalog=storage::catalog::Hash(parent.Encode());
    origin.proof=HashOrchardSnapshotSection(proof);origin.retirement=storage::OrchardSnapshotRetirementFingerprint(proof.retirement.record);
    origin.source=before;origin.validated=validated.hash;
    Need(candidate.stageOrchardCatalogState(token,parent.block,parent.Encode(),batch));
    Require(batch.Put(storage::OrchardCompactStorageKey,storage::EncodeOrchardCompactStorageBinding(parent)).ok());
    Require(batch.Put(storage::OrchardActivationOriginKey,origin.Encode()).ok());
    Require(batch.Put(storage::OrchardActivationSelectionKey,origin.Selection()).ok());
    VerifyCatalogTransferProof(proof,headers);check_cancel();
    Require(HandoffStoreDigest(source,cancelled)==before);
    before_choice();flush();return {std::move(database),std::move(origin)};
}

std::unique_ptr<PreparedOrchardActivationHandoff> OrchardSnapshotStore::PrepareActivationHandoff(
    const daemon::DatadirGuard& guard,const ChainWriteToken& token,
    std::shared_ptr<const consensus::HeaderChainSelector> headers,
    std::shared_ptr<const consensus::OrchardSnapshotSection> input,const std::function<bool()>& cancelled){
#ifdef _WIN32
    (void)guard;(void)token;(void)headers;(void)input;(void)cancelled;
    throw std::runtime_error("authenticated store handoff unavailable on this platform");
#else
    using namespace consensus;namespace fs=std::filesystem;
    Require(headers&&input);auto d=std::make_unique<PreparedOrchardActivationHandoff::Data>();
    d->root_path=guard.DataDir();d->headers=std::move(headers);d->cancelled=cancelled;
    d->proof=std::make_shared<const OrchardSnapshotSection>(*input);
    const auto check_cancel=[&]{Require(!cancelled||!cancelled());};check_cancel();
    d->root=HandoffFd(OpenHandoffRoot(guard));
    RequireNoActivationHandoff(d->root_path);
    d->blockchain=HandoffFd(::openat(d->root.value,"blockchain",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto chain_id=HandoffStat(d->blockchain.value);
    Require(chain_id.st_uid==::geteuid()&&!(chain_id.st_mode&0022));
    d->source_path=d->root_path/"blockchain"/"chaindb";d->paired_sst=d->source_path.string()+".legacy-v1";
    HandoffMaintenanceClear(d->root_path,d->source_path);
    d->source_dir=HandoffFd(::openat(d->blockchain.value,"chaindb",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto source_id=HandoffStat(d->source_dir.value);
    Require(source_id.st_uid==::geteuid()&&!(source_id.st_mode&0022)&&
        HandoffSame(source_id,HandoffNamed(d->blockchain.value,"chaindb")));
    d->source_files=InspectHandoffFiles(d->source_path,d->paired_sst,cancelled);
    d->source=std::make_unique<ChainDB>();Need(d->source->initReadOnlyForHandoff(d->source_path));
    const auto& source=*d->source;const auto& proof=*d->proof;const auto& parent=proof.parent_catalog;
    VerifyCatalogTransferProof(proof,*d->headers);check_cancel();
    Require(source.hasSeparatedShieldedState()&&!storage::ReadOrchardSnapshotBase(source)&&
        !storage::ReadOrchardCompactStorageBinding(source));
    for(const char* key:{storage::OrchardActivationOriginKey,storage::OrchardActivationSelectionKey,storage::OrchardSnapshotFreshKey}){
        std::string value;Require(source.getRaw(key,value)==Status::NotFound);
    }
    CheckLegacyRetirementBeforeImageUnderLock(source,proof.retirement.record);
    const auto tip=Need(source.getTip()),validated=Need(source.getValidatedTip());
    const auto marker=Need(source.getForestTipMarker());const auto stored_header=Need(source.getHeader(parent.block));
    Require(tip.height==int32_t(parent.height)&&tip.hash==parent.block&&tip.work==parent.work&&
        Need(source.getBlockHashByHeight(int(parent.height)))==parent.block&&
        Need(source.getBlockHeight(parent.block))==int(parent.height)&&Need(source.getBlockWork(parent.block))==parent.work&&
        marker.height==tip.height&&marker.block_hash==tip.hash&&marker.forest_root==stored_header.utreexo_root);
    const auto selected=d->headers->GetHeaderValue(parent.block);HeaderIndexEntry anchor;
    Require(selected&&selected->height==parent.height&&selected->header.SerializeForHash()==stored_header.SerializeForHash()&&
        selected->chainwork==parent.work&&validated.height>=0&&validated.height<=tip.height&&
        d->headers->GetHeaderAtHeightCopy(uint32_t(validated.height),anchor)&&anchor.hash==validated.hash);
    const auto stump=UtreexoStump::deserialize(parent.stump);const auto root=stump.getCommitment();
    Require(root.size()==32&&std::equal(root.begin(),root.end(),stored_header.utreexo_root.begin()));
    Require(Need(source.previewOrchardCommitmentSets(std::nullopt,proof.state,proof.nullifiers))==ComputeOrchardSnapshotSets(proof));
    const auto before=HandoffStoreDigest(source,cancelled);
    uint64_t bytes=0;for(const auto& [name,file]:d->source_files){
        (void)name;Require(file.size<=UINT64_MAX-bytes);bytes+=file.size;
    }
    const auto proof_bytes=OrchardSnapshotSectionEncodedSize(proof);
    Require(bytes<=(UINT64_MAX-(uint64_t(1)<<30)-3*proof_bytes)/2);
    struct statvfs space{};Require(::fstatvfs(d->blockchain.value,&space)==0&&space.f_frsize);
    const uint64_t required=2*bytes+3*proof_bytes+(uint64_t(1)<<30);
    Require(required<=UINT64_MAX-(space.f_frsize-1));
    Require(uint64_t(space.f_bavail)>=(required+space.f_frsize-1)/space.f_frsize);
    const auto name="chaindb.orchard-handoff-"+util::hex(secure_random_bytes(16));
    d->candidate_path=d->source_path.parent_path()/name;
    auto copy=CopyActivationParent(source,token,d->candidate_mutex,*d->headers,proof,before,d->candidate_path,
        [&]{
            d->candidate_dir=HandoffFd(::openat(d->blockchain.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
            Require(HandoffSame(HandoffStat(d->candidate_dir.value),HandoffNamed(d->blockchain.value,name)));
        },[&]{
            // Preserve the real-root/source checks BEFORE final synced choice.
            VerifyCatalogTransferProof(proof,*d->headers);check_cancel();
            Require(HandoffStoreDigest(source,cancelled)==before&&InspectHandoffFiles(d->source_path,d->paired_sst,cancelled)==d->source_files);
            HandoffFd root_check(OpenHandoffRoot(guard));Require(HandoffSame(HandoffStat(root_check.value),HandoffStat(d->root.value)));
        },cancelled);
    d->candidate=std::move(copy.database);d->origin=std::move(copy.origin);auto& candidate=*d->candidate;
    candidate.close();Need(candidate.initReadOnlyForHandoff(d->candidate_path));
    d->candidate_digest=HandoffStoreDigest(candidate,cancelled);
    d->candidate_files=InspectHandoffFiles(d->candidate_path,{},cancelled);
    HandoffSync(d->candidate_dir.value);HandoffSync(d->blockchain.value);
    auto result=std::unique_ptr<PreparedOrchardActivationHandoff>(new PreparedOrchardActivationHandoff(std::move(d)));
    CheckActivationHandoff(guard,*result);return result;
#endif
}

void OrchardSnapshotStore::CheckActivationHandoff(const daemon::DatadirGuard& guard,
    const PreparedOrchardActivationHandoff& prepared){
#ifdef _WIN32
    (void)guard;(void)prepared;throw std::runtime_error("authenticated store handoff unavailable on this platform");
#else
    const auto& d=*prepared.data_;Require(d.thread==std::this_thread::get_id()&&d.root_path==guard.DataDir()&&
        d.headers&&d.proof&&d.source&&d.candidate&&(!d.cancelled||!d.cancelled()));
    HandoffFd current(OpenHandoffRoot(guard));Require(HandoffSame(HandoffStat(current.value),HandoffStat(d.root.value))&&
        HandoffSame(HandoffNamed(current.value,"blockchain"),HandoffStat(d.blockchain.value))&&
        HandoffSame(HandoffNamed(d.blockchain.value,d.source_path.filename().string()),HandoffStat(d.source_dir.value))&&
        HandoffSame(HandoffNamed(d.blockchain.value,d.candidate_path.filename().string()),HandoffStat(d.candidate_dir.value)));
    HandoffMaintenanceClear(d.root_path,d.root_path/"blockchain"/"chaindb");
    if(!d.selection.empty())Require(HandoffControl(d.blockchain.value,HandoffIntent)==d.selection);
    Require(InspectHandoffFiles(d.source_path,d.paired_sst,d.cancelled)==d.source_files&&
        InspectHandoffFiles(d.candidate_path,{},d.cancelled)==d.candidate_files&&
        HandoffStoreDigest(*d.source,d.cancelled)==d.origin.source&&
        HandoffStoreDigest(*d.candidate,d.cancelled)==d.candidate_digest);
    VerifyCatalogTransferProof(*d.proof,*d.headers);
    const auto cold_headers=ReadActivationHeaders(*d.candidate,d.origin.first,d.cancelled);
    VerifyCatalogTransferProof(*d.proof,*cold_headers);
    CheckLegacyRetirementBeforeImageUnderLock(*d.source,d.proof->retirement.record);
    CheckLegacyRetirementBeforeImageUnderLock(*d.candidate,d.proof->retirement.record);
    const auto tip=Need(d.candidate->getTip()),validated=Need(d.candidate->getValidatedTip());
    Require(tip.height==int32_t(d.origin.parent_height)&&tip.hash==d.origin.parent&&
        validated.height==int32_t(d.origin.validated_height)&&validated.hash==d.origin.validated&&
        d.origin.magic==Params().magic&&d.origin.profile==ConsensusChecksum(Params())&&
        !storage::ReadOrchardSnapshotBase(*d.candidate)&&
        storage::ReadOrchardCompactStorageBinding(*d.candidate)==storage::EncodeOrchardCompactStorageBinding(d.proof->parent_catalog)&&
        Need(d.candidate->getOrchardCatalogState(tip.hash))==d.proof->parent_catalog.Encode());
    std::string origin,selection;Need(d.candidate->getRaw(storage::OrchardActivationOriginKey,origin));
    Need(d.candidate->getRaw(storage::OrchardActivationSelectionKey,selection));
    Require(storage::OrchardActivationOrigin::Decode(origin).Encode()==d.origin.Encode()&&selection==d.origin.Selection());
    std::lock_guard<AnnotatedRecursiveMutex> lock(prepared.data_->candidate_mutex);
    const auto retained=ReadActivationProof(prepared.data_->candidate_mutex,*d.candidate,*d.headers,d.origin.first,d.cancelled);
    Require(retained&&HashOrchardSnapshotSection(*retained)==d.origin.proof);
    rocksdb::ReadOptions reads;reads.verify_checksums=true;reads.fill_cache=false;
    // Compare every original non-coin row byte-for-byte. Only explicitly
    // checked new proof/catalog/origin rows may appear; prefix alone is not
    // permission to hide arbitrary extra data or a deleted before-image.
    const auto proof_size=OrchardSnapshotSectionEncodedSize(*d.proof);
    const auto allowed_extra=[&](size_t family,const rocksdb::Slice& key,const rocksdb::Slice& value){
        const auto k=key.ToString(),v=value.ToString();
        if(family==0){
            if(k==ActivationHeadersKey(d.origin.first)){
                const auto descriptor=ActivationHeadersDescriptor::Decode(v);
                return descriptor.first==d.origin.first&&descriptor.target==cold_headers->GetBestHeaderValue()->hash;
            }
            if(k==storage::OrchardActivationOriginKey)return v==d.origin.Encode();
            if(k==storage::OrchardActivationSelectionKey)return v==d.origin.Selection();
            if(k==storage::OrchardCompactStorageKey)return v==storage::EncodeOrchardCompactStorageBinding(d.proof->parent_catalog);
            if(k==ActivationProofKey(d.origin.first))return ActivationProofDescriptor::Decode(v).digest==d.origin.proof;
            const auto prefix="orchard_activation_proof_v1:chunk:"+d.origin.proof.GetHex()+":";
            if(k.starts_with(prefix)){
                const auto part=k.substr(prefix.size());if(part.empty()||part.size()>20)return false;
                uint64_t index=0;for(const auto digit:part){
                    if(digit<'0'||digit>'9'||index>(UINT64_MAX-9)/10)return false;index=index*10+uint8_t(digit-'0');
                }
                return std::to_string(index)==part&&index<(proof_size+
                    storage::OrchardSnapshotProofChunkBytes-1)/storage::OrchardSnapshotProofChunkBytes;
            }
            return false;
        }
        if(family==size_t(d.source->idx_headers_)){
            if(k.size()!=65||k.front()!=ChainDB::PREFIX_HEADER)return false;uint256 id;
            if(!uint256::FromHex(k.substr(1),id))return false;const auto header=cold_headers->GetHeaderValue(id);
            return header&&k==d.source->makeHeaderKey(id)&&v==ActivationHeaderRow(header->header,header->height,header->chainwork);
        }
        if(family==size_t(d.source->idx_utreexo_)){
            const std::string nodes="Morchard_catalog_v1/node/",states="Morchard_catalog_v1/state/";
            if(k.starts_with(nodes)&&k.size()==nodes.size()+32){
                uint256 id;std::copy_n(reinterpret_cast<const uint8_t*>(k.data()+nodes.size()),32,id.begin());
                return storage::catalog::ValidNode(id,v)&&v==ReadOrchardSnapshotNode(*d.proof,id);
            }
            return k==states+std::string(reinterpret_cast<const char*>(d.origin.parent.data),32)&&v==d.proof->parent_catalog.Encode();
        }
        return false;
    };
    Require(d.source->cf_.size()==d.candidate->cf_.size());
    for(size_t family=0;family<d.source->cf_.size();++family){
        if(family==size_t(d.source->idx_utxo_))continue;
        std::unique_ptr<rocksdb::Iterator> original(d.source->db_->NewIterator(reads,d.source->cf_[family].get()));
        std::unique_ptr<rocksdb::Iterator> copy(d.candidate->db_->NewIterator(reads,d.candidate->cf_[family].get()));
        original->SeekToFirst();copy->SeekToFirst();
        while(original->Valid()||copy->Valid()){
            Require(!d.cancelled||!d.cancelled());
            if(copy->Valid()&&(!original->Valid()||copy->key().compare(original->key())<0)){
                Require(allowed_extra(family,copy->key(),copy->value()));copy->Next();continue;
            }
            Require(original->Valid()&&copy->Valid()&&original->key()==copy->key()&&original->value()==copy->value());
            original->Next();copy->Next();
        }
        Require(original->status().ok()&&copy->status().ok());
    }
    std::unique_ptr<rocksdb::Iterator> coins(d.candidate->db_->NewIterator(reads,d.candidate->cf_[d.candidate->idx_utxo_].get()));
    coins->SeekToFirst();Require(!coins->Valid()&&coins->status().ok());
#endif
}
std::unique_ptr<PreparedOrchardActivationHandoff> OrchardSnapshotStore::SelectActivationHandoff(
    const daemon::DatadirGuard& guard,const ClosedCoreReopenIntent& receipt,
    std::unique_ptr<PreparedOrchardActivationHandoff> prepared,const std::function<bool()>& cancelled){
#ifdef _WIN32
    (void)guard;(void)receipt;(void)prepared;(void)cancelled;Require(false);return {};
#else
    Require(prepared&&prepared->data_);receipt.Check(guard);
    CheckActivationHandoff(guard,*prepared);auto& d=*prepared->data_;
    Require(d.selection.empty()&&d.source_path==guard.DataDir()/"blockchain"/"chaindb");
    RequireNoActivationHandoff(guard.DataDir());
    const std::string prefix="chaindb.orchard-handoff-";const auto name=d.candidate_path.filename().string();
    Require(name.starts_with(prefix));HandoffSelection record;record.nonce=name.substr(prefix.size());
    record.origin=d.origin;record.journal=receipt.CandidateJournal();record.wallet=receipt.WalletName();
    const auto root=HandoffStat(d.root.value),chain=HandoffStat(d.blockchain.value),
        original=HandoffStat(d.source_dir.value),copy=HandoffStat(d.candidate_dir.value);
    record.root_dev=root.st_dev;record.root_ino=root.st_ino;record.chain_dev=chain.st_dev;record.chain_ino=chain.st_ino;
    record.source_dev=original.st_dev;record.source_ino=original.st_ino;record.copy_dev=copy.st_dev;record.copy_ino=copy.st_ino;
    record.source_files=HandoffFilesDigest(d.source_files);record.copy_files=HandoffFilesDigest(d.candidate_files);
    record.copy_rows=d.candidate_digest;const auto control=record.Encode();
    Require(HandoffAbsent(d.blockchain.value,record.Original())&&(!cancelled||!cancelled()));
    receipt.Check(guard);CheckActivationHandoff(guard,*prepared);
    {
        std::unique_lock<std::mutex> selection(d.headers->mutex_);const auto* best=d.headers->best_header_;
        const auto* first=best?best->GetAncestor(d.proof->state.height):nullptr;
        Require(first&&first->hash==d.origin.first&&first->prev_hash==d.origin.parent&&
            uint64_t(best->height)>=uint64_t(first->height)+Params().state_commitment_burial_depth);
        HandoffPublish(d.blockchain.value,HandoffIntent,control);
    }
    const auto headers=d.headers;
    // Read-only handles own canonical-name process reservations. Release both
    // before reopening the exact recorded phase. The SAME root guard stays held.
    prepared.reset();return RecoverActivationHandoff(guard,receipt,headers,cancelled);
#endif
}

std::unique_ptr<PreparedOrchardActivationHandoff> OrchardSnapshotStore::RecoverActivationHandoff(
    const daemon::DatadirGuard& guard,const ClosedCoreReopenIntent& receipt,
    std::shared_ptr<const consensus::HeaderChainSelector> headers,const std::function<bool()>& cancelled){
#ifdef _WIN32
    (void)guard;(void)receipt;(void)headers;(void)cancelled;Require(false);return {};
#else
    receipt.Check(guard);Require(!cancelled||!cancelled());
    auto d=std::make_unique<PreparedOrchardActivationHandoff::Data>();d->root_path=guard.DataDir();
    d->headers=std::move(headers);d->cancelled=cancelled;d->root=HandoffFd(OpenHandoffRoot(guard));
    d->blockchain=HandoffFd(::openat(d->root.value,"blockchain",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto bytes=HandoffControl(d->blockchain.value,HandoffIntent);
    const auto done=HandoffControl(d->blockchain.value,HandoffDone);
    Require(!bytes.empty()||done.empty());if(bytes.empty())return {};
    const auto record=HandoffSelection::Decode(bytes);const auto root=HandoffStat(d->root.value),chain=HandoffStat(d->blockchain.value);
    Require(record.origin.magic==Params().magic&&record.origin.profile==ConsensusChecksum(Params())&&
        record.root_dev==uint64_t(root.st_dev)&&record.root_ino==uint64_t(root.st_ino)&&
        record.chain_dev==uint64_t(chain.st_dev)&&record.chain_ino==uint64_t(chain.st_ino)&&
        chain.st_uid==::geteuid()&&!(chain.st_mode&0022)&&
        record.journal==receipt.CandidateJournal()&&record.wallet==receipt.WalletName());
    const auto completed=HandoffSeal("DNODONE1"+storage::catalog::Hash(bytes).GetHex());
    Require(done.empty()||done==completed);
    const auto original=record.Original(),copy=record.Candidate();
    const bool live_absent=HandoffAbsent(d->blockchain.value,"chaindb"),copy_absent=HandoffAbsent(d->blockchain.value,copy),
        original_absent=HandoffAbsent(d->blockchain.value,original);
    unsigned phase=0;
    if(!live_absent&&!copy_absent&&original_absent)phase=0;
    else if(live_absent&&!copy_absent&&!original_absent)phase=1;
    else if(!live_absent&&copy_absent&&!original_absent)phase=2;
    else Require(false);
    Require(done.empty()||phase==2);
    d->source_path=d->root_path/"blockchain"/(phase==0?"chaindb":original);
    d->candidate_path=d->root_path/"blockchain"/(phase==2?"chaindb":copy);
    d->paired_sst=d->root_path/"blockchain"/"chaindb.legacy-v1";
    HandoffMaintenanceClear(d->root_path,d->root_path/"blockchain"/"chaindb");
    d->source_dir=HandoffFd(::openat(d->blockchain.value,d->source_path.filename().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    d->candidate_dir=HandoffFd(::openat(d->blockchain.value,d->candidate_path.filename().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto source_id=HandoffStat(d->source_dir.value),copy_id=HandoffStat(d->candidate_dir.value);
    Require(record.source_dev==uint64_t(source_id.st_dev)&&record.source_ino==uint64_t(source_id.st_ino)&&
        record.copy_dev==uint64_t(copy_id.st_dev)&&record.copy_ino==uint64_t(copy_id.st_ino)&&
        source_id.st_uid==::geteuid()&&!(source_id.st_mode&0022)&&copy_id.st_uid==::geteuid()&&!(copy_id.st_mode&0022));
    d->source_files=InspectHandoffFiles(d->source_path,d->paired_sst,cancelled);
    d->candidate_files=InspectHandoffFiles(d->candidate_path,{},cancelled);
    Require(record.source_files==HandoffFilesDigest(d->source_files)&&
        (!done.empty()||record.copy_files==HandoffFilesDigest(d->candidate_files)));
    // Before completion, exact unpublished candidate files are immutable.
    // A completed selected store may have been opened by the private writer;
    // reauthenticate its ENTIRE recorded logical baseline below, not its LOG
    // inode/mtime. Descendant rows still refuse this parent-only recovery path.
    d->source=std::make_unique<ChainDB>();Need(d->source->initReadOnlyForHandoff(d->source_path));
    d->candidate=std::make_unique<ChainDB>();Need(d->candidate->initReadOnlyForHandoff(d->candidate_path));
    // No state/owner is granted by completing this recorded filesystem choice.
    // A cold recovery without RAM selection revalidates the persisted as-of-
    // handoff header closure; startup must still merge/recheck current branches.
    if(!d->headers)d->headers=ReadActivationHeaders(*d->candidate,record.origin.first,cancelled);
    d->origin=record.origin;d->candidate_digest=record.copy_rows;d->selection=bytes;
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(d->candidate_mutex);
        auto proof=ReadActivationProof(d->candidate_mutex,*d->candidate,*d->headers,d->origin.first,cancelled);
        Require(proof&&HashOrchardSnapshotSection(*proof)==d->origin.proof);
        d->proof=std::make_shared<const consensus::OrchardSnapshotSection>(std::move(*proof));
    }
    auto result=std::unique_ptr<PreparedOrchardActivationHandoff>(new PreparedOrchardActivationHandoff(std::move(d)));
    auto& live=*result->data_;CheckActivationHandoff(guard,*result);receipt.Check(guard);
    if(phase==0){
        Require(HandoffAbsent(live.blockchain.value,original));
        HandoffRename(live.blockchain.value,"chaindb",original);live.source_path=live.root_path/"blockchain"/original;
        CheckActivationHandoff(guard,*result);receipt.Check(guard);phase=1;
    }
    if(phase==1){
        Require(HandoffAbsent(live.blockchain.value,"chaindb"));
        HandoffRename(live.blockchain.value,copy,"chaindb");live.candidate_path=live.root_path/"blockchain"/"chaindb";
        CheckActivationHandoff(guard,*result);receipt.Check(guard);
    }
    HandoffSync(live.source_dir.value);HandoffSync(live.candidate_dir.value);HandoffSync(live.blockchain.value);
    if(done.empty()){
        std::unique_lock<std::mutex> selection(live.headers->mutex_);const auto* best=live.headers->best_header_;
        const auto* first=best?best->GetAncestor(live.proof->state.height):nullptr;
        Require(first&&first->hash==live.origin.first&&first->prev_hash==live.origin.parent&&
            uint64_t(best->height)>=uint64_t(first->height)+Params().state_commitment_burial_depth);
        HandoffPublish(live.blockchain.value,HandoffDone,completed);
    }
    Require(HandoffControl(live.blockchain.value,HandoffIntent)==bytes&&HandoffControl(live.blockchain.value,HandoffDone)==completed);
    return result; // Both stores stay locked; no Init, writer, lifecycle or RPC publication.
#endif
}
OrchardSnapshotStore::ActivationPublication OrchardSnapshotStore::OpenActivationHandoff(
    const daemon::DatadirGuard& guard,const ClosedCoreReopenIntent& receipt,AnnotatedRecursiveMutex& mutex,
    const ChainWriteToken& token,const BlockStorage& files,
    std::shared_ptr<const consensus::HeaderChainSelector> current_headers,
    std::unique_ptr<PreparedOrchardActivationHandoff> prepared){
#ifdef _WIN32
    (void)guard;(void)receipt;(void)mutex;(void)token;(void)files;(void)current_headers;(void)prepared;Require(false);return {};
#else
    mutex.AssertHeld("open authenticated selected activation handoff");
    receipt.Check(guard);Require(prepared&&prepared->data_);auto& d=*prepared->data_;
    // Dedicated startup supplies its current branch-preserving HeaderStore
    // selector, never the cold as-of-intent data closure by implicit fallback.
    Require(bool(current_headers));VerifyCatalogTransferProof(*d.proof,*current_headers);
    d.headers=std::move(current_headers);
    CheckActivationHandoff(guard,*prepared);
    const auto audited=AuditActivationSuffix(mutex,*d.candidate,token,files,d.headers,d.proof,d.cancelled);
    Require(audited.Encode()==d.proof->parent_catalog.Encode());
    Require(d.candidate_path==guard.DataDir()/"blockchain"/"chaindb"&&!d.selection.empty()&&
        HandoffControl(d.blockchain.value,HandoffIntent)==d.selection&&
        HandoffControl(d.blockchain.value,HandoffDone)==HandoffSeal("DNODONE1"+storage::catalog::Hash(d.selection).GetHex()));
    const auto record=HandoffSelection::Decode(d.selection);
    Require(record.origin.Encode()==d.origin.Encode()&&record.copy_rows==d.candidate_digest&&
        record.journal==receipt.CandidateJournal()&&record.wallet==receipt.WalletName());
    Require(AuditActivationStore(mutex,*d.source,*d.candidate,token,files,record.copy_rows,
        d.headers,d.proof,d.cancelled).Encode()==audited.Encode());
    // Only this completed, wholly audited selection can request an existing-
    // layout writer. No create, lock retry, migration/schema append or generic
    // no-handoff fence bypass is exposed to callers.
    d.candidate->close();d.source->close();receipt.Check(guard);
    // The old source was opened before rename: both RocksDB's process lock
    // table and our directory reservation may still use its OLD live name.
    // Rebind the unchanged original under its actual retained backup name;
    // never bypass either lock or leave it writable.
    Need(d.source->initReadOnlyForHandoff(d.source_path));
    Require(HandoffSame(HandoffStat(d.candidate_dir.value),HandoffNamed(d.blockchain.value,"chaindb")));
    const ChainDB::SelectedHandoffOpen permit(d.candidate_path);
    d.candidate->dir_=d.candidate_path;
    Need(d.candidate->initAttempt(d.candidate_path,false,false,&permit));
    Require(HandoffSame(HandoffStat(d.candidate_dir.value),HandoffNamed(d.blockchain.value,"chaindb"))&&
        HandoffStoreDigest(*d.candidate,d.cancelled)==d.candidate_digest&&
        InspectHandoffFiles(d.source_path,d.paired_sst,d.cancelled)==d.source_files);
    receipt.Check(guard);VerifyCatalogTransferProof(*d.proof,*d.headers);
    const auto consistency=std::shared_ptr<const consensus::OrchardSelectedConsistency>(
        new consensus::OrchardSelectedConsistency(*d.candidate,mutex,d.origin));
    // CheckActivationHandoff authenticated every reachable parent node, exact
    // stump/header/work/tip, FULL legacy before-image and empty namespaces.
    // The original independent validated anchor remains untouched.
    ActivationPublication publication;publication.compact=std::make_unique<OrchardCompactChainstate>();
    publication.compact->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
        d.candidate.get(),&mutex,d.proof->parent_catalog,d.proof->retirement.record,consistency);
    {
        std::unique_lock<std::mutex> selected(d.headers->mutex_);const auto* best=d.headers->best_header_;
        const auto* first=best?best->GetAncestor(d.proof->state.height):nullptr;
        Require(first&&first->hash==d.origin.first&&first->prev_hash==d.origin.parent&&
            uint64_t(best->height)>=uint64_t(first->height)+Params().state_commitment_burial_depth);
    }
    receipt.Check(guard);Require(!d.cancelled||!d.cancelled());
    publication.database=std::move(d.candidate);return publication;
#endif
}
void OrchardSnapshotStore::CreateFreshStore(const std::filesystem::path& destination) {
    namespace fs=std::filesystem;
    Require(!fs::exists(destination)&&!fs::is_symlink(fs::symlink_status(destination))&&
        fs::is_directory(destination.parent_path()));
    auto temporary=destination;temporary+=".v6-init-"+util::hex(secure_random_bytes(16));bool owned=false;
    try {
        Require(fs::create_directory(temporary));owned=true;
        fs::permissions(temporary,fs::perms::owner_all,fs::perm_options::replace);
        {
            struct Store {
                std::unique_ptr<rocksdb::DB> db;std::vector<rocksdb::ColumnFamilyHandle*> handles;
                ~Store(){if(db)for(auto* handle:handles)db->DestroyColumnFamilyHandle(handle);}
            } store;
            rocksdb::Options options;options.create_if_missing=true;options.create_missing_column_families=true;
            std::vector<rocksdb::ColumnFamilyDescriptor> families;
            for(const auto* name:{"default","meta","blocks","headers","height","txindex","utxo","utreexo","prebase_coins","shielded_state_v1"}) {
                rocksdb::ColumnFamilyOptions family(options);
                if(std::string_view(name)=="shielded_state_v1")family=storage::ShieldedStateColumnFamilyOptions(std::move(family));
                families.emplace_back(name,std::move(family));
            }
            rocksdb::DB* opened=nullptr;const auto status=rocksdb::DB::Open(options,temporary.string(),families,&store.handles,&opened);
            store.db.reset(opened);Require(status.ok()&&store.db&&store.handles.size()==10);
            consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
            const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
            std::string marker;storage::catalog::Number(marker,0,4);
            const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
            marker.append(reinterpret_cast<const char*>(genesis.data),32);
            const auto root=tree.Root();marker.append(reinterpret_cast<const char*>(root.data()),root.size());
            storage::catalog::Number(marker,0,8);storage::catalog::Number(marker,0,8);
            std::string schema;storage::catalog::Number(schema,4,4);rocksdb::WriteBatch batch;
            batch.Put(storage::OrchardSnapshotFreshKey,"DNOV6-FRESH-1");
            batch.Put(store.handles[1],"schema_version",schema);
            batch.Put(store.handles[1],"storage_layout_v1","shielded-state-v1:READY");
            batch.Put(store.handles[1],"shielded_tip",marker);
            batch.Put(store.handles[9],"Mshielded_frontier",rocksdb::Slice(reinterpret_cast<const char*>(frontier.data()),frontier.size()));
            batch.Put(store.handles[9],"Mshielded_anchor_history",rocksdb::Slice(reinterpret_cast<const char*>(history.data()),history.size()));
            rocksdb::WriteOptions writes;writes.sync=true;Require(store.db->Write(writes,&batch).ok());
        }
        // Exercise the ordinary READY opening rules before publishing a name.
        {ChainDB check;Need(check.init(temporary));Require(check.hasSeparatedShieldedState()&&check.getTip().status()==Status::NotFound);check.close();}
        Require(!fs::exists(destination)&&!fs::is_symlink(fs::symlink_status(destination)));
        fs::rename(temporary,destination);owned=false;
#ifndef _WIN32
        const int descriptor=open(destination.parent_path().c_str(),O_RDONLY);Require(descriptor>=0);
        const int synced=fsync(descriptor);const int closed=close(descriptor);Require(synced==0&&closed==0);
#endif
    } catch(...) {
        // Only this exclusively created, still-unpublished directory belongs
        // to this operation. Interrupted processes leave it unselected.
        if(owned){std::error_code ignored;fs::remove_all(temporary,ignored);}throw;
    }
}
void OrchardSnapshotStore::SeedRetainedHeaders(const ChainDB& db,consensus::HeaderChainSelector& headers) {
    std::vector<std::pair<int32_t,uint256>> ordered;
    Need(db.forEachHeaderMetadata([&](const auto& hash,const ChainDB::PersistedHeaderMetadata& metadata){
        ordered.emplace_back(metadata.height,hash);return true;
    }));std::sort(ordered.begin(),ordered.end());
    for(const auto& [height,hash]:ordered) {
        const auto header=Need(db.getHeader(hash));
        // Only genuinely validated headers enter the selector. Invalid or
        // orphaned side records cannot rescue a missing selected ancestry.
        (void)headers.AddHeader(header);
    }
}
consensus::OrchardSnapshotSection OrchardSnapshotStore::Capture(const ChainDB& db,
    consensus::OrchardSnapshotProfile profile,const std::vector<uint8_t>& accumulator) {
    using namespace consensus;OrchardSnapshotSection s;s.profile=profile;s.accumulator=accumulator;
    s.state=Need(db.getOrchardState());s.retirement=Need(db.getLegacyRetirementState());
    s.base_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(s.state.block_hash)));
    if(profile==OrchardSnapshotProfile::Full) {
        // Live forests retain dispensable internal cache cells. Deserialization
        // rebuilds them, so normalize the trusted export view once; import
        // continues to require the resulting canonical fixed-point encoding.
        s.accumulator=CanonicalOrchardSnapshotForest(accumulator);
    }
    s.parent_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(s.base_catalog.parent)));
    s.base_body=ReadStoredOrchardBlock(db,s.state.block_hash,Params().enforce_witness_commitment&&
        s.state.height>=Params().witness_commitment_enforcement_height).WireBytes();
    Need(db.forEachOrchardNullifier([&](const auto& nf,const auto& owner){s.nullifiers.push_back(nf);return true;}));
    Need(db.forEachOrchardAnchor([&](const auto& anchor,uint64_t refs){s.anchors.push_back({anchor,refs});return true;}));
    const auto read=[&](const auto& id){return Need(db.getOrchardCatalogNode(id));};
    auto inventory=std::make_shared<OrchardSnapshotCatalog>(std::filesystem::path(db.db_->GetName()).parent_path());
    for(const auto* state:{&s.parent_catalog,&s.base_catalog}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count){
            storage::catalog::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto& bytes){
                inventory->Insert(id,bytes);return true;
            });
        };
        walk(storage::catalog::Kind::Transactions,state->transactions,state->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,state->legacy,state->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,state->nontransparent,state->nontransparent_count);
    }
    inventory->Seal();s.disk_nodes=std::move(inventory);ValidateOrchardSnapshotSection(s);return s;
}
consensus::OrchardSnapshotSection OrchardSnapshotStore::CaptureActivation(const ChainDB& db,
    const consensus::HeaderChainSelector& headers,const std::function<bool()>& cancelled) {
    using namespace consensus;
    const auto check_cancel=[&]{Require(!cancelled||!cancelled());};
    check_cancel();const auto& p=Params();
    Require(OrchardProfileConfigurationValid(p)&&p.orchard_activation_height&&
        p.orchard_activation_height!=UINT32_MAX&&
        p.orchard_catalog_commitment_activation_height==p.orchard_activation_height);
    HeaderIndexEntry base,parent;const auto best=headers.GetBestHeaderValue();
    Require(best&&headers.GetHeaderAtHeightCopy(p.orchard_activation_height,base)&&
        headers.GetHeaderAtHeightCopy(p.orchard_activation_height-1,parent)&&base.prev_hash==parent.hash&&
        base.chainwork>parent.chainwork);
    uint256 ancestor;uint32_t selected=0;
    Require(uint64_t(best->height)>=uint64_t(base.height)+p.state_commitment_burial_depth&&
        headers.GetAncestorHashByHash(best->hash,base.height,ancestor,selected)&&
        selected==best->height&&ancestor==base.hash);
    const auto context=SelectedOrchardBlockContext(base.header,base.height);Require(bool(context));
    OrchardSnapshotSection s;s.profile=OrchardSnapshotProfile::Compact;
    s.parent_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(parent.hash)));
    s.base_catalog=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(base.hash)));
    Require(s.parent_catalog.block==parent.hash&&s.parent_catalog.height==parent.height&&
        s.base_catalog.block==base.hash&&s.base_catalog.height==base.height);
    // Retirement contents are frozen; only its descendant marker changes.
    // Neither those contents nor the retained catalog are trusted until the
    // reconstructed first-state root matches the selected coinbase below.
    s.retirement=Need(db.getLegacyRetirementState());s.retirement.height=base.height;
    s.retirement.block_hash=base.hash;s.retirement.parent_hash=parent.hash;
    const bool witness=p.enforce_witness_commitment&&base.height>=p.witness_commitment_enforcement_height;
    const auto block=ReadStoredOrchardBlock(db,base.hash,witness);s.base_body=block.WireBytes();
    Require(s.base_body.size()<=ORCHARD_SNAPSHOT_MAX_BASE_BODY_BYTES);
    auto inventory=std::make_shared<OrchardSnapshotCatalog>(std::filesystem::path(db.db_->GetName()).parent_path());
    uint64_t visits=0;
    const auto read=[&](const uint256& id){check_cancel();return Need(db.getOrchardCatalogNode(id));};
    for(const auto* state:{&s.parent_catalog,&s.base_catalog}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count){
            // Six trees, two inventories at most; even duplicate paths charge
            // work. The disk inventory separately enforces its byte quota.
            Require(count<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES/49);
            storage::catalog::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto& bytes){
                Require(++visits<=2*(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES/49));
                inventory->Insert(id,bytes);return true;
            });
        };
        walk(storage::catalog::Kind::Transactions,state->transactions,state->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,state->legacy,state->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,state->nontransparent,state->nontransparent_count);
    }
    inventory->Seal();s.disk_nodes=std::move(inventory);check_cancel();
    const auto captured_read=[&](const uint256& id){check_cancel();return ReadOrchardSnapshotNode(s,id);};
    const auto view=CaptureOrchardCatalogCoins(block,*context,parent.header,s.parent_catalog,captured_read);
    const OrchardBranchMtpLookup mtp=[&](uint32_t height)->std::optional<uint64_t>{
        uint256 hash;uint32_t selected_height=0,found=0,time=0;
        if(height>parent.height||!headers.GetAncestorHashByHash(parent.hash,height,hash,selected_height)||
            selected_height!=parent.height||!headers.GetMedianTimePastByHash(hash,time,found)||found!=height)return {};
        return time;
    };
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(block,*context,view,mtp,witness);
    (void)CheckOrchardBlockFilter(block,coins);check_cancel();
    // There is no Orchard parent state at activation. Re-run real transparent
    // and Orchard authorizations, then derive commitments/frontier from empty;
    // current nullifiers/anchors/undo are neither copied nor rewound.
    const OrchardStateLookups empty{[](const uint256&)->StatusOr<bool>{return false;},
        [](const uint256&)->StatusOr<bool>{return false;}};
    const auto state=PrepareOrchardStateTransition(*context,std::nullopt,coins.Authorizations(),empty);
    s.state=state.Next();s.nullifiers=state.Nullifiers();
    std::sort(s.nullifiers.begin(),s.nullifiers.end(),OrchardSnapshotRawLess{});
    s.anchors.push_back({s.state.anchor,1});s.accumulator=s.base_catalog.stump;check_cancel();
    Require(OrchardSnapshotSectionEncodedSize(s)<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
    VerifyCatalogTransferProof(s,headers);check_cancel();return s;
}
std::unique_ptr<OrchardBoundaryReplay> OrchardSnapshotStore::ReplayActivation(
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof,
    std::shared_ptr<const consensus::HeaderChainSelector> headers,uint32_t height,const uint256& hash) {
    Require(proof&&headers);
    // Copy metadata/body/map before authenticating; an external mutable alias
    // cannot change the enrolled private origin. Disk nodes are already sealed.
    auto immutable=std::make_shared<const consensus::OrchardSnapshotSection>(*proof);
    VerifyCatalogTransferProof(*immutable,*headers);
    return std::unique_ptr<OrchardBoundaryReplay>(new OrchardBoundaryReplay(std::move(immutable),std::move(headers),height,hash));
}
void OrchardSnapshotStore::RetainActivationProof(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
    const consensus::HeaderChainSelector& headers,const consensus::OrchardSnapshotSection& untrusted,
    const std::function<bool()>& cancelled) {
    mutex.AssertHeld("retain authenticated activation proof");
    const auto check=[&]{Require(!cancelled||!cancelled());};check();
    Require(db.hasSeparatedShieldedState());
    const OrchardSnapshotSection proof=untrusted;VerifyCatalogTransferProof(proof,headers);check();
    crypto::CSHA256 hash;
    ActivationProofDescriptor d;d.first=proof.state.block_hash;d.parent=proof.parent_catalog.block;
    d.bytes=StreamOrchardSnapshotSection(proof,[&](auto bytes){check();hash.Write(bytes.data(),bytes.size());});
    hash.Finalize(d.digest.data);const auto encoded=d.Encode(),key=ActivationProofKey(d.first);
    std::string existing;const auto status=db.getRaw(key,existing);
    Require(status==Status::NotFound||(status==Status::Ok&&existing==encoded));
    const bool completed=status==Status::Ok;
    if(!completed) {
        const auto space=std::filesystem::space(std::filesystem::path(db.db_->GetName()).parent_path());
        Require(space.available>=2*d.bytes+(uint64_t(512)<<20));
    }
    // An interrupted, unselected prefix can resume only byte-identically.
    // A completed descriptor with missing chunks is corruption, not permission
    // to repair. All writes use the normal WAL/checksums and synced batch path.
    std::string chunk;chunk.reserve(storage::OrchardSnapshotProofChunkBytes);uint64_t part=0;
    const auto flush=[&]{
        check();const auto chunk_key=ActivationProofChunkKey(d.digest,part++);
        std::string old;const auto found=db.getRaw(chunk_key,old);
        Require(found==Status::NotFound? !completed : found==Status::Ok&&old==chunk);
        if(found==Status::NotFound) {
            rocksdb::WriteBatch batch;batch.Put(chunk_key,chunk);Need(db.writeBatch(token,std::move(batch),true));
        }
        chunk.clear();
    };
    Require(StreamOrchardSnapshotSection(proof,[&](std::span<const uint8_t> bytes){
        check();while(!bytes.empty()) {
            const auto n=std::min<size_t>(bytes.size(),storage::OrchardSnapshotProofChunkBytes-chunk.size());
            chunk.append(reinterpret_cast<const char*>(bytes.data()),n);bytes=bytes.subspan(n);
            if(chunk.size()==storage::OrchardSnapshotProofChunkBytes)flush();
        }
    })==d.bytes);if(!chunk.empty())flush();
    // Audit bytes actually read back from storage, not the original in-memory
    // proof. A descriptor never selects a merely partial or unaudited stream.
    auto reread=ReadActivationProofChunks(db,d,std::filesystem::path(db.db_->GetName()).parent_path(),cancelled);
    VerifyCatalogTransferProof(reread,headers);check();
    std::unique_lock<std::mutex> selection(headers.mutex_);const auto* best=headers.best_header_;
    const auto* selected=best?best->GetAncestor(proof.state.height):nullptr;
    Require(selected&&selected->hash==d.first&&selected->prev_hash==d.parent&&
        uint64_t(best->height)>=uint64_t(proof.state.height)+Params().state_commitment_burial_depth);
    if(!completed) {
        rocksdb::WriteBatch batch;batch.Put(key,encoded);Need(db.writeBatch(token,std::move(batch),true));
    }
}
std::optional<consensus::OrchardSnapshotSection> OrchardSnapshotStore::ReadActivationProof(
    AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers,
    const uint256& activation_block,const std::function<bool()>& cancelled) {
    mutex.AssertHeld("read authenticated activation proof");Require(!cancelled||!cancelled());
    Require(db.hasSeparatedShieldedState());std::string encoded;const auto status=db.getRaw(ActivationProofKey(activation_block),encoded);
    if(status==Status::NotFound)return {};Need(status);
    const auto d=ActivationProofDescriptor::Decode(encoded);Require(d.first==activation_block);
    auto proof=ReadActivationProofChunks(db,d,std::filesystem::path(db.db_->GetName()).parent_path(),cancelled);
    VerifyCatalogTransferProof(proof,headers);Require(!cancelled||!cancelled());return proof;
}
std::unique_ptr<OrchardCompactChainstate> OrchardSnapshotStore::RestoreActivationParent(
    AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers,
    const consensus::OrchardSnapshotSection& untrusted) {
    using namespace consensus;mutex.AssertHeld("restore authenticated activation parent");
    // Copy mutable metadata before verification; production node inventories
    // are sealed/shared, not reconstructed in a history-sized RAM container.
    const OrchardSnapshotSection proof=untrusted;VerifyCatalogTransferProof(proof,headers);
    const auto& parent=proof.parent_catalog;const auto& retired=proof.retirement.record;
    Require(db.hasSeparatedShieldedState()&&!storage::ReadOrchardSnapshotBase(db)&&
        uint64_t(parent.height)+1==proof.state.height&&parent.block==retired.boundary_parent);
    // Keep all existing before-image checks. In particular, the snapshot
    // shortcut for omitted legacy bytes is NOT available at this boundary.
    CheckPreparedLegacyRetirementUnderLock(db,retired);
    Require(storage::ReadOrchardCompactStorageBinding(db)==storage::EncodeOrchardCompactStorageBinding(parent)&&
        Need(db.getOrchardCatalogState(parent.block))==parent.Encode());
    const auto tip=Need(db.getTip());const auto header=Need(db.getHeader(parent.block));
    Require(tip.height==int32_t(parent.height)&&tip.hash==parent.block&&tip.work==parent.work);
    const auto context=SelectedOrchardBlockContext(OrchardBlockCandidate::DecodeExact(proof.base_body).Header(),proof.state.height);
    Require(bool(context));
    CheckOrchardCatalogStump(db,parent,*context,header,parent.height,UtreexoStump::deserialize(parent.stump));
    const auto forest=Need(db.getForestTipMarker());
    Require(forest.height==int32_t(parent.height)&&forest.block_hash==parent.block&&forest.forest_root==header.utreexo_root);
    // This also rejects untracked active/undo namespace rows, not just a
    // missing state head. No synthetic empty Orchard frontier is installed.
    Require(Need(db.previewOrchardCommitmentSets(std::nullopt,proof.state,proof.nullifiers))==ComputeOrchardSnapshotSets(proof));
    bool empty=true;Need(db.forEachUTXO([&](const auto&,uint32_t,const auto&){empty=false;return false;}));Require(empty);
    const auto read=[&](const uint256& id){
        const auto bytes=Need(db.getOrchardCatalogNode(id));Require(bytes==ReadOrchardSnapshotNode(proof,id));return bytes;
    };
    const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count){
        storage::catalog::Tree(kind,read).ForEachNode(root,count,[](const auto&,const auto&){return true;});
    };
    walk(storage::catalog::Kind::Transactions,parent.transactions,parent.transaction_count);
    walk(storage::catalog::Kind::LegacyCoins,parent.legacy,parent.legacy_count);
    walk(storage::catalog::Kind::NonTransparentCoins,parent.nontransparent,parent.nontransparent_count);
    auto owner=std::make_unique<OrchardCompactChainstate>();
    owner->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(&db,&mutex,parent,retired);
    // Header selection is separate from the selected writer mutex. Pin the
    // exact first-block authority and burial through private enrollment; no
    // header APIs/callbacks may run while this selector lock is held.
    std::unique_lock<std::mutex> selection(headers.mutex_);const auto* best=headers.best_header_;
    const auto* first=best?best->GetAncestor(proof.state.height):nullptr;
    const auto* previous=first?first->GetAncestor(parent.height):nullptr;
    Require(first&&first->hash==proof.state.block_hash&&previous&&previous->hash==parent.block&&
        uint64_t(best->height)>=uint64_t(proof.state.height)+Params().state_commitment_burial_depth);
    return owner;
}
consensus::OrchardBlockContext OrchardSnapshotStore::Authenticate(const consensus::OrchardSnapshotSection& s,
    const consensus::HeaderChainSelector& headers,uint32_t burial,bool require_best_ancestry) {
    using namespace consensus;
    Require(OrchardCatalogCommitmentActive(Params(),s.state.height)&&
        s.base_catalog.leaf_activation==GetUtreexoMaturityLeafActivationHeight());
    const auto genesis=CompiledGenesis();
    storage::catalog::Tree transactions(storage::catalog::Kind::Transactions,[&](const uint256& id){
        return ReadOrchardSnapshotNode(s,id);
    });
    Require(transactions.Find(s.parent_catalog.transactions,storage::catalog::TransactionKey(genesis.GetTxid())).has_value());
    const auto captured=headers.GetHeaderValue(s.state.block_hash);Require(captured&&captured->height==s.state.height);
    const auto base=*captured;
    if(require_best_ancestry) {
        HeaderIndexEntry selected;Require(headers.GetHeaderAtHeightCopy(s.state.height,selected)&&selected.hash==base.hash);
    } else Require(burial==0);
    const auto parent=headers.GetHeaderValue(base.prev_hash);const auto best=headers.GetBestHeaderValue();
    Require(parent&&best&&uint64_t(parent->height)+1==base.height);
    const auto context=SelectedOrchardBlockContext(base.header,base.height);Require(bool(context));
    const OrchardBranchMtpLookup mtp=[&](uint32_t height)->std::optional<uint64_t> {
        uint256 ancestor;uint32_t selected=0,found=0,time=0;
        if(height>parent->height||!headers.GetAncestorHashByHash(parent->hash,height,ancestor,selected)||
            selected!=parent->height||!headers.GetMedianTimePastByHash(ancestor,time,found)||found!=height)return {};
        return time;
    };
    VerifyOrchardSnapshotSection(s,s.profile,*context,parent->header,base.header,parent->chainwork,base.chainwork,
        base.hash,std::max(best->height,base.height),burial,mtp,Params().enforce_witness_commitment&&
        base.height>=Params().witness_commitment_enforcement_height);
    return *context;
}
void OrchardSnapshotStore::VerifyCatalogTransferProof(const consensus::OrchardSnapshotSection& proof,
    const consensus::HeaderChainSelector& headers) {
    Require(proof.profile==consensus::OrchardSnapshotProfile::Compact&&
        proof.state.height==Params().orchard_activation_height&&
        Params().orchard_catalog_commitment_activation_height==Params().orchard_activation_height);
    (void)Authenticate(proof,headers,Params().state_commitment_burial_depth);
}
OrchardSnapshotStore::Publication OrchardSnapshotStore::Install(AnnotatedRecursiveMutex& mutex,ChainDB& db,
    const ChainWriteToken& token,BlockStorage& files,const consensus::HeaderChainSelector& headers,
    const consensus::OrchardSnapshotFile& file) {
    using namespace consensus;mutex.AssertHeld("install authenticated Orchard snapshot");
    const auto& s=file.orchard;const auto context=Authenticate(s,headers,Params().state_commitment_burial_depth);
    VerifyOrchardSnapshotCoins(s,file.coins);
    Require(file.metadata.version==SNAPSHOT_VERSION_V6&&file.metadata.block_hash==s.state.block_hash&&
        file.metadata.block_height==s.state.height&&file.metadata.utxo_count==file.coins.size());
    Require(db.hasSeparatedShieldedState()&&!storage::ReadOrchardSnapshotBase(db)&&
        db.getOrchardState().status()==Status::NotFound&&db.getLegacyRetirementState().status()==Status::NotFound);
    const auto genesis_hash=uint256::FromHexUnsafe(Params().genesis_hash);
    const auto tip=db.getTip();Require(!tip.ok()?tip.status()==Status::NotFound:tip->height==0&&tip->hash==genesis_hash);
    const auto consistent=db.getValidatedTip();
    Require(consistent.status()==Status::NotFound||(consistent.ok()&&tip.ok()&&consistent->height==0&&consistent->hash==genesis_hash));
    // Header-only preparation may be retried, but an old canonical height map
    // is not a fresh store merely because its tip or coin table was lost.
    std::unique_ptr<rocksdb::Iterator> heights(db.db_->NewIterator(rocksdb::ReadOptions(),db.cf_[db.idx_height_].get()));
    Require(bool(heights));bool saw_genesis=false;
    for(heights->SeekToFirst();heights->Valid();heights->Next()) {
        Require(!saw_genesis&&heights->key().ToString()==db.makeHeightKey(0)&&
            Need(db.getBlockHashByHeight(0))==genesis_hash);saw_genesis=true;
    }
    Require(heights->status().ok());
    // No replacing an active full store, compact owner, sidecar migration or
    // implicit profile conversion. Fresh genesis rows are the only exception.
    Require(!storage::ReadOrchardCompactStorageBinding(db));
    const auto genesis=CompiledGenesis();
    const auto old_coins=ReadCoins(db);
    for(const auto& [point,coin]:old_coins) {
        Require(point.txid==genesis.GetTxid()&&point.vout<genesis.vout.size()&&coin.height==0&&coin.isCoinbase&&
            coin.value==genesis.vout[point.vout].value&&coin.scriptPubKey==genesis.vout[point.vout].scriptPubKey&&
            !coin.is_confidential&&coin.commitment.empty());
    }
    Publication publication;
    if(s.profile==OrchardSnapshotProfile::Full)publication.full=FullPublication(s,file.coins);
    else {
        publication.compact=std::make_unique<OrchardCompactChainstate>();
        publication.compact->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
            &db,&mutex,s.base_catalog,s.retirement.record);
    }
    crypto::CSHA256 proof_hash;
    const auto proof_size=StreamOrchardSnapshotSection(s,[&](auto bytes){proof_hash.Write(bytes.data(),bytes.size());});
    uint256 proof_digest;proof_hash.Finalize(proof_digest.data);
    storage::OrchardSnapshotBase base{uint8_t(s.profile),s.state.height,s.state.block_hash,
        proof_digest,storage::OrchardSnapshotRetirementFingerprint(s.retirement.record),proof_size};
    // Bounded immutable preparation. Until the final synced batch selects the
    // base, interrupted chunks/nodes are unreachable and confer no authority.
    rocksdb::WriteBatch pending;size_t count=0;
    const auto flush=[&]{if(pending.Count()){Need(db.writeBatch(token,std::move(pending),true));pending=rocksdb::WriteBatch();count=0;}};
    const auto best=headers.GetBestHeaderValue();Require(best&&best->height<=uint32_t(INT32_MAX));
    uint256 base_ancestor;uint32_t captured_height=0;
    Require(uint64_t(best->height)>=uint64_t(s.state.height)+Params().state_commitment_burial_depth&&
        headers.GetAncestorHashByHash(best->hash,s.state.height,base_ancestor,captured_height)&&
        captured_height==best->height&&base_ancestor==s.state.block_hash);
    // Follow one captured hash-linked branch, not successive best-at-height
    // queries which could mix branches during concurrent header selection.
    auto ancestor=best->hash;
    for(uint64_t remaining=uint64_t(best->height)+1;remaining;--remaining) {
        const auto captured=headers.GetHeaderValue(ancestor);Require(captured&&captured->height==remaining-1);
        const auto& header=*captured;const auto height=header.height;ancestor=header.prev_hash;
        const auto old=db.getHeader(header.hash);
        if(old.ok())Require(old->SerializeForHash()==header.header.SerializeForHash()&&
            Need(db.getBlockHeight(header.hash))==int(height)&&Need(db.getBlockWork(header.hash))==header.chainwork);
        else {
            Require(old.status()==Status::NotFound);
            Need(db.putHeader(token,header.hash,header.header,int(height),header.chainwork,&pending));
            ChainDB::PersistedHeaderMetadata m;m.parent_hash=header.prev_hash;m.height=int32_t(height);
            m.chainwork=header.chainwork;m.status_flags=BLOCK_VALID_HEADER;
            Need(db.putHeaderMetadata(token,header.hash,m,&pending));
        }
        if(++count==1024)flush();
    }
    Require(ancestor.IsNull());
    flush();
    ForEachOrchardSnapshotNode(s,[&](const auto& id,const auto& bytes){
        Need(db.stageOrchardCatalogNode(token,id,bytes,pending));if(++count==1024)flush();
    });flush();
    std::string proof_chunk;proof_chunk.reserve(storage::OrchardSnapshotProofChunkBytes);uint64_t part=0;
    const auto write_chunk=[&] {
        const auto key=storage::OrchardSnapshotProofKey(base.proof_digest,part++);
        std::string previous;const auto status=db.getRaw(key,previous);
        Require(status==Status::NotFound||(status==Status::Ok&&previous==proof_chunk));
        if(status==Status::NotFound){pending.Put(key,proof_chunk);flush();}
        proof_chunk.clear();
    };
    Require(StreamOrchardSnapshotSection(s,[&](std::span<const uint8_t> bytes) {
        while(!bytes.empty()) {
            const auto n=std::min<size_t>(bytes.size(),storage::OrchardSnapshotProofChunkBytes-proof_chunk.size());
            proof_chunk.append(reinterpret_cast<const char*>(bytes.data()),n);bytes=bytes.subspan(n);
            if(proof_chunk.size()==storage::OrchardSnapshotProofChunkBytes)write_chunk();
        }
    })==proof_size);
    if(!proof_chunk.empty())write_chunk();
    const auto body=OrchardBlockCandidate::DecodeExact(s.base_body);
    const auto position=Need(files.writeBlockBytes(s.state.block_hash,std::string(s.base_body.begin(),s.base_body.end())));
    Require(position.offset<=UINT32_MAX);
    HeaderIndexEntry selected;Require(headers.GetHeaderAtHeightCopy(s.state.height,selected)&&selected.hash==s.state.block_hash);
    rocksdb::WriteBatch batch;
    std::vector<std::pair<uint256,uint64_t>> anchors;anchors.reserve(s.anchors.size());
    for(const auto& anchor:s.anchors)anchors.emplace_back(anchor.anchor,anchor.references);
    Need(db.stageOrchardSnapshotState(token,s.state,s.nullifiers,anchors,batch));
    Need(db.stageLegacyRetirementSnapshotState(token,s.retirement,batch));
    Need(db.stageOrchardCatalogState(token,s.parent_catalog.block,s.parent_catalog.Encode(),batch));
    Need(db.stageOrchardCatalogState(token,s.base_catalog.block,s.base_catalog.Encode(),batch));
    Need(db.stageOrchardBlock(token,body,Params().enforce_witness_commitment&&
        s.state.height>=Params().witness_commitment_enforcement_height,batch));
    Need(db.putHeader(token,selected.hash,selected.header,int(selected.height),selected.chainwork,&batch));
    ChainDB::PersistedHeaderMetadata metadata;metadata.parent_hash=context.parent_hash;metadata.height=int32_t(context.height);
    metadata.chainwork=selected.chainwork;metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_VALID_TREE|BLOCK_HAVE_DATA;
    metadata.file_number=position.file_number;metadata.data_pos=uint32_t(position.offset);metadata.data_size=position.size;
    Need(db.putHeaderMetadata(token,selected.hash,metadata,&batch));
    for(const auto& [point,coin]:old_coins)Need(db.deleteCoin(token,point.txid.AsUint256(),point.vout,&batch));
    for(const auto& [point,coin]:file.coins) {
        Coin value;value.amount=coin.value.GetUna();value.script_pubkey=util::hex(coin.scriptPubKey);
        value.height=int(coin.height);value.coinbase=coin.isCoinbase;
        Need(db.putCoin(token,point.txid.AsUint256(),point.vout,value,&batch));
    }
    if(s.profile==OrchardSnapshotProfile::Compact)
        batch.Put(storage::OrchardCompactStorageKey,storage::EncodeOrchardCompactStorageBinding(s.base_catalog));
    else Need(db.putUtreexoCheckpointWithChecksum(token,int(s.state.height),s.accumulator,&batch));
    batch.Put(storage::OrchardSnapshotBaseKey,base.Encode());
    batch.Delete(storage::OrchardSnapshotFreshKey);
    StageOrchardSnapshotJournal(context,selected.header,selected.chainwork,s.state,
        UtreexoStump::deserialize(s.base_catalog.stump),batch);
    Need(db.putForestTipMarker(token,{int32_t(s.state.height),s.state.block_hash,selected.header.utreexo_root},&batch));
    Need(db.putHeightIndex(token,int(s.state.height),s.state.block_hash,&batch));
    Need(db.setTip(token,s.state.block_hash,int(s.state.height),selected.chainwork,&batch));
    // This marker means state/forest consistency, not pre-base script history.
    // Base metadata deliberately lacks CHAIN/SCRIPTS and has no fabricated undo.
    Need(db.setValidatedTip(token,s.state.block_hash,int(s.state.height),&batch));
    // Recheck selected authority immediately before the sole state publication.
    (void)Authenticate(s,headers,Params().state_commitment_burial_depth);
    {
        // AddHeader uses a separate mutex from the selected writer. Bind the
        // exact independently validated base AND burial across publication,
        // not merely immediately before it. No header API/callback under lock.
        std::unique_lock<std::mutex> selection(headers.mutex_);
        const auto* best=headers.best_header_;
        const auto* selected_base=best?best->GetAncestor(s.state.height):nullptr;
        Require(selected_base&&selected_base->hash==s.state.block_hash&&selected_base->chainwork==s.base_catalog.work&&
            selected_base->header.SerializeForHash()==body.Header().SerializeForHash()&&
            uint64_t(best->height)>=uint64_t(s.state.height)+Params().state_commitment_burial_depth);
        try {if(db.writeBatch(token,std::move(batch),true)!=Status::Ok)throw CommitUncertain();}
        catch(...){throw CommitUncertain();}
    }
    return publication;
}
std::unique_ptr<OrchardBranchReplay> OrchardSnapshotStore::ReplayFromBaseline(const ChainDB& db,
    const consensus::HeaderChainSelector& headers,uint32_t height,const uint256& hash,const arith_uint256& work) {
    const auto baseline=ReadBaseline(db);(void)Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    std::vector<BlockHeader> history(size_t(baseline.state.height)+1);auto previous=baseline.state.block_hash;
    for(size_t remaining=history.size();remaining;--remaining) {
        const auto row=headers.GetHeaderValue(previous);Require(row&&row->height==remaining-1);
        history[remaining-1]=row->header;previous=row->prev_hash;
    }
    Require(previous.IsNull());
    return std::unique_ptr<OrchardBranchReplay>(new OrchardBranchReplay(baseline,history,{height,hash,work}));
}
consensus::OrchardSnapshotSection OrchardSnapshotStore::ReadBaseline(const ChainDB& db) {
    const auto base=storage::ReadOrchardSnapshotBase(db);Require(bool(base));
    std::string chunk;size_t position=0;uint64_t loaded=0,part=0;crypto::CSHA256 proof_hash;
    auto s=consensus::ReadOrchardSnapshotSection(base->proof_bytes,[&](std::span<uint8_t> bytes) {
        while(!bytes.empty()) {
            if(position==chunk.size()) {
                Require(loaded<base->proof_bytes);
                Need(db.getRaw(storage::OrchardSnapshotProofKey(base->proof_digest,part++),chunk));
                Require(chunk.size()==std::min<uint64_t>(storage::OrchardSnapshotProofChunkBytes,base->proof_bytes-loaded));
                proof_hash.Write(chunk);loaded+=chunk.size();position=0;
            }
            const auto n=std::min(bytes.size(),chunk.size()-position);
            std::copy_n(reinterpret_cast<const uint8_t*>(chunk.data()+position),n,bytes.data());
            position+=n;bytes=bytes.subspan(n);
        }
    },std::filesystem::path(db.db_->GetName()).parent_path());
    uint256 digest;proof_hash.Finalize(digest.data);
    Require(loaded==base->proof_bytes&&position==chunk.size()&&digest==base->proof_digest);
    Require(uint8_t(s.profile)==base->profile&&s.state.height==base->height&&s.state.block_hash==base->block&&
        storage::OrchardSnapshotRetirementFingerprint(s.retirement.record)==base->retirement);return s;
}
bool OrchardSnapshotStore::RecoverBaseMarkers(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
    const consensus::HeaderChainSelector& headers) {
    using namespace consensus;mutex.AssertHeld("recover authenticated v6 base markers");
    const auto floor=storage::ReadOrchardSnapshotBase(db);Require(bool(floor));const auto tip=Need(db.getTip());
    if(tip.height>=int(floor->height))return false;
    const auto baseline=ReadBaseline(db);const auto context=Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    const auto consistent=Need(db.getValidatedTip());
    Require(tip.height>=0&&consistent.hash==tip.hash&&consistent.height==tip.height);
    uint256 old_ancestor;uint32_t base_height=0;
    Require(headers.GetAncestorHashByHash(floor->block,uint32_t(tip.height),old_ancestor,base_height)&&
        base_height==floor->height&&old_ancestor==tip.hash);
    Require(Need(db.getOrchardState())==baseline.state&&Need(db.getLegacyRetirementState())==baseline.retirement&&
        db.getBlockHashByHeight(int(floor->height)+1).status()==Status::NotFound);
    const auto forest=Need(db.getForestTipMarker());const auto legacy=Need(db.getShieldedTipMarker());
    const auto header=Need(db.getHeader(floor->block));
    Require(forest.height==int(floor->height)&&forest.block_hash==floor->block&&forest.forest_root==header.utreexo_root&&
        legacy.height==int(floor->height)&&legacy.block_hash==floor->block&&legacy.shielded_root==baseline.retirement.record.tree_root&&
        legacy.tree_size==baseline.retirement.record.tree_size&&legacy.nullifier_count==baseline.retirement.record.nullifier_count);
    const bool compact=baseline.profile==OrchardSnapshotProfile::Compact;
    const auto accumulator=compact?baseline.base_catalog.stump:Need(db.getUtreexoCheckpoint(int(floor->height)));
    const auto actual=Capture(db,baseline.profile,accumulator);
    Require(HashOrchardSnapshotSection(actual)==HashOrchardSnapshotSection(baseline));
    Need(db.forEachOrchardNullifier([&](const auto&,const auto& owner){return owner==floor->block;}));
    if(compact)Require(ReadCoins(db).empty()&&storage::ReadOrchardCompactStorageBinding(db)==
        storage::EncodeOrchardCompactStorageBinding(baseline.base_catalog));
    else {storage::RequireFullOrchardStorage(db);VerifyOrchardSnapshotCoins(actual,ReadCoins(db));}
    // Only an exact, fully authenticated imported base may bridge a marker-
    // above-history-tip gap. Never rewind a post-base state or repair a subset.
    rocksdb::WriteBatch batch;
    Need(db.putHeightIndex(token,int(floor->height),floor->block,&batch));
    Need(db.setTip(token,floor->block,int(floor->height),baseline.base_catalog.work,&batch));
    Need(db.setValidatedTip(token,floor->block,int(floor->height),&batch));
    StageOrchardSnapshotJournal(context,header,baseline.base_catalog.work,baseline.state,
        UtreexoStump::deserialize(baseline.base_catalog.stump),batch);
    (void)Authenticate(baseline,headers,Params().state_commitment_burial_depth);
    std::unique_lock<std::mutex> selection(headers.mutex_);const auto* best=headers.best_header_;
    const auto* selected_base=best?best->GetAncestor(floor->height):nullptr;
    Require(selected_base&&selected_base->hash==floor->block&&
        uint64_t(best->height)>=uint64_t(floor->height)+Params().state_commitment_burial_depth);
    Need(db.writeBatch(token,std::move(batch),true));return true;
}
OrchardSnapshotStore::Publication OrchardSnapshotStore::Restore(AnnotatedRecursiveMutex& mutex,const ChainDB& db,
    const consensus::HeaderChainSelector& headers) {
    using namespace consensus;mutex.AssertHeld("restore authenticated Orchard snapshot");
    const auto base=ReadBaseline(db);(void)Authenticate(base,headers,Params().state_commitment_burial_depth);
    const auto tip=Need(db.getTip()),consistent=Need(db.getValidatedTip());
    Require(tip.height>=int(base.state.height)&&tip.hash==consistent.hash&&tip.height==consistent.height);
    const auto selected=storage::catalog::State::Decode(Need(db.getOrchardCatalogState(tip.hash)));
    const bool compact=base.profile==OrchardSnapshotProfile::Compact;std::vector<uint8_t> accumulator;
    if(compact)accumulator=selected.stump;
    else {
        UtreexoForest restored;std::string error;
        Need(storage::RestoreHistoricalForest(db,uint32_t(tip.height),restored,error,[&](uint32_t height,uint256& hash){
            uint32_t selected_height=0;
            return headers.GetAncestorHashByHash(tip.hash,height,hash,selected_height)&&selected_height==uint32_t(tip.height);
        }));accumulator=restored.serializeCanonicalLeaves(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
    }
    const auto current=Capture(db,base.profile,accumulator);(void)Authenticate(current,headers,0,false);
    uint256 base_ancestor;uint32_t selected_height=0;
    Require(headers.GetAncestorHashByHash(tip.hash,base.state.height,base_ancestor,selected_height)&&
        selected_height==uint32_t(tip.height)&&base_ancestor==base.state.block_hash);
    Require(current.state.height==uint32_t(tip.height)&&current.state.block_hash==tip.hash&&current.base_catalog.work==tip.work&&
        current.retirement.record==base.retirement.record);
    const auto forest=Need(db.getForestTipMarker());const auto legacy=Need(db.getShieldedTipMarker());
    const auto header=Need(db.getHeader(tip.hash));
    Require(forest.height==tip.height&&forest.block_hash==tip.hash&&forest.forest_root==header.utreexo_root&&
        legacy.height==tip.height&&legacy.block_hash==tip.hash&&legacy.shielded_root==current.retirement.record.tree_root&&
        legacy.tree_size==current.retirement.record.tree_size&&legacy.nullifier_count==current.retirement.record.nullifier_count);
    Publication publication;
    if(compact) {
        Require(ReadCoins(db).empty()&&storage::ReadOrchardCompactStorageBinding(db)==
            storage::EncodeOrchardCompactStorageBinding(selected));
        publication.compact=std::make_unique<OrchardCompactChainstate>();
        publication.compact->selected_=std::make_shared<const OrchardCompactChainstate::Selected>(
            &db,&mutex,selected,current.retirement.record);
    } else {storage::RequireFullOrchardStorage(db);publication.full=FullPublication(current,ReadCoins(db));}
    return publication;
}
} // namespace dinero
