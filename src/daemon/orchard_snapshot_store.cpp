#include "daemon/orchard_snapshot_store.h"
#include "daemon/datadir_guard.h"
#include "daemon/core_reopen.h"
#include "daemon/chainstate_recovery_marker.h"
#include "daemon/services/orchard_branch_replay.h"
#include "daemon/services/orchard_boundary_replay.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_header.h"
#include "consensus/orchard_catalog_validation.h"
#include "consensus/block_lifecycle.h"
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
#include <thread>
#include <utility>
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
    CreateFreshStore(d->candidate_path);
    d->candidate_dir=HandoffFd(::openat(d->blockchain.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    Require(HandoffSame(HandoffStat(d->candidate_dir.value),HandoffNamed(d->blockchain.value,name)));
    d->candidate=std::make_unique<ChainDB>();Need(d->candidate->init(d->candidate_path));auto& candidate=*d->candidate;
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
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(d->candidate_mutex);
        RetainActivationProof(d->candidate_mutex,candidate,token,*d->headers,proof,cancelled);
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
    auto& origin=d->origin;origin.magic=Params().magic;origin.profile=ConsensusChecksum(Params());
    origin.parent_height=parent.height;origin.validated_height=uint32_t(validated.height);
    origin.first=proof.state.block_hash;origin.parent=parent.block;origin.parent_catalog=storage::catalog::Hash(parent.Encode());
    origin.proof=HashOrchardSnapshotSection(proof);origin.retirement=storage::OrchardSnapshotRetirementFingerprint(proof.retirement.record);
    origin.source=before;origin.validated=validated.hash;
    Need(candidate.stageOrchardCatalogState(token,parent.block,parent.Encode(),batch));
    Require(batch.Put(storage::OrchardCompactStorageKey,storage::EncodeOrchardCompactStorageBinding(parent)).ok());
    Require(batch.Put(storage::OrchardActivationOriginKey,origin.Encode()).ok());
    Require(batch.Put(storage::OrchardActivationSelectionKey,origin.Selection()).ok());
    // Final source/selection checks BEFORE this candidate-only synced choice.
    VerifyCatalogTransferProof(proof,*d->headers);check_cancel();
    Require(HandoffStoreDigest(source,cancelled)==before&&InspectHandoffFiles(d->source_path,d->paired_sst,cancelled)==d->source_files);
    {HandoffFd root_check(OpenHandoffRoot(guard));Require(HandoffSame(HandoffStat(root_check.value),HandoffStat(d->root.value)));}
    flush();candidate.close();Need(candidate.initReadOnlyForHandoff(d->candidate_path));
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
    receipt.Check(guard);Require(headers&&(!cancelled||!cancelled()));
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
    Require(record.source_files==HandoffFilesDigest(d->source_files)&&record.copy_files==HandoffFilesDigest(d->candidate_files));
    d->source=std::make_unique<ChainDB>();Need(d->source->initReadOnlyForHandoff(d->source_path));
    d->candidate=std::make_unique<ChainDB>();Need(d->candidate->initReadOnlyForHandoff(d->candidate_path));
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
