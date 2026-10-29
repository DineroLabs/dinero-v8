#include "daemon/reopen_candidate_journal.h"
#include "daemon/datadir_guard.h"
#include "daemon/services/mempool_service.h"
#include "daemon/utreexo_tx_reader.h"
#include "consensus/chainparams.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <openssl/rand.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace dinero::daemon {
namespace {
using Journal=ReopenCandidateJournal;using Bytes=std::vector<uint8_t>;
void Require(bool ok){if(!ok)throw std::runtime_error("Reopen candidate journal refused");}
constexpr const char* Prefix=".core-reopen-candidates-v1-";
constexpr const char* Sealed="candidates.sealed";
constexpr const char* Pending=".core-reopen-pending-v1";
constexpr const char* Partial=".core-reopen-pending-v1.partial";
#ifndef _WIN32
struct Fd {
    int value=-1;explicit Fd(int v):value(v){Require(v>=0);}
    ~Fd(){if(value>=0)::close(value);}
    Fd(Fd&& other)noexcept:value(std::exchange(other.value,-1)){}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
struct stat Stat(int fd){struct stat s{};Require(::fstat(fd,&s)==0);return s;}
struct stat Named(int fd,const char* name){struct stat s{};Require(::fstatat(fd,name,&s,AT_SYMLINK_NOFOLLOW)==0);return s;}
bool Same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool SameFile(const struct stat& a,const struct stat& b){
#ifdef __APPLE__
    const auto am=a.st_mtimespec,ac=a.st_ctimespec,bm=b.st_mtimespec,bc=b.st_ctimespec;
#else
    const auto am=a.st_mtim,ac=a.st_ctim,bm=b.st_mtim,bc=b.st_ctim;
#endif
    return Same(a,b)&&a.st_size==b.st_size&&a.st_mode==b.st_mode&&a.st_nlink==b.st_nlink&&
        a.st_uid==b.st_uid&&am.tv_sec==bm.tv_sec&&am.tv_nsec==bm.tv_nsec&&ac.tv_sec==bc.tv_sec&&ac.tv_nsec==bc.tv_nsec;
}
void PrivateDirectory(const struct stat& s){Require(S_ISDIR(s.st_mode)&&s.st_uid==::geteuid()&&(s.st_mode&0777)==0700);}
void SealedFile(const struct stat& s){Require(S_ISREG(s.st_mode)&&s.st_uid==::geteuid()&&s.st_nlink==1&&
    (s.st_mode&0777)==0400&&s.st_size>=121&&uint64_t(s.st_size)<=Journal::MaximumBytes);}
void OnlySealed(int fd){
    const int copied=::fcntl(fd,F_DUPFD_CLOEXEC,0);Require(copied>=0);
    DIR* dir=::fdopendir(copied);if(!dir){::close(copied);Require(false);}
    ::rewinddir(dir); // dup shares the directory offset with the retained fd.
    bool valid=true;unsigned files=0;errno=0;
    while(const auto entry=::readdir(dir)){
        const std::string name=entry->d_name;if(name=="."||name=="..")continue;
        if(name!=Sealed)valid=false;
        ++files;
    }
    const int error=errno;Require(::closedir(dir)==0&&error==0&&valid&&files==1);
}
void Space(int fd,uint64_t next){
    struct statvfs space{};Require(::fstatvfs(fd,&space)==0&&space.f_frsize);
    const auto needed=(uint64_t(512)<<20)+next;
    Require(uint64_t(space.f_bavail)>=(needed+space.f_frsize-1)/space.f_frsize);
}
std::array<uint8_t,32> Profile(){
    crypto::CSHA256 hash;hash.Write("DNRPCDOMAIN1").Write(ConsensusChecksum(Params())).Write(Params().name);
    uint8_t magic[4];for(unsigned i=0;i<4;++i)magic[i]=uint8_t(Params().magic>>(8*i));
    hash.Write(magic,4);std::array<uint8_t,32> out;hash.Finalize(out.data());return out;
}
void Put(Bytes& out,uint64_t n,unsigned width){for(unsigned i=0;i<width;++i)out.push_back(uint8_t(n>>(8*i)));}
Bytes Nonce(const std::string& name){
    const std::string prefix=Prefix;Require(name.size()==prefix.size()+32&&name.starts_with(prefix));Bytes out;
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';Require(c>='a'&&c<='f');return c-'a'+10;};
    for(size_t i=prefix.size();i<name.size();i+=2)out.push_back(uint8_t(digit(name[i])*16+digit(name[i+1])));
    return out;
}
Bytes Header(int root,const std::string& name){
    const auto st=Stat(root);const auto profile=Profile();const auto nonce=Nonce(name);
    Bytes bytes{'D','N','R','P','C','A','N',1};bytes.insert(bytes.end(),profile.begin(),profile.end());
    Put(bytes,uint64_t(st.st_dev),8);Put(bytes,uint64_t(st.st_ino),8);bytes.insert(bytes.end(),nonce.begin(),nonce.end());return bytes;
}
void CheckWire(const Journal::Candidate& candidate){
    Require(!candidate.body.empty()&&candidate.body.size()<=Journal::MaximumWireBytes&&candidate.proof.size()<=Journal::MaximumWireBytes);
    const auto body=DecodeRelayTransaction(candidate.body,RelayTransactionReadMode::AvailableFamilies);
    Require(body.Serialize()==candidate.body);
    if(!candidate.proof.empty()){
        const auto proof=UtreexoTransactionPayload::Decode(candidate.proof,RelayTransactionReadMode::AvailableFamilies);
        Require(proof.Body().Serialize()==candidate.body);
    }
}
struct Writer {
    int fd;uint64_t size=0;crypto::CSHA256 hash;
    void Write(std::span<const uint8_t> bytes,bool hashed=true){
        Require(bytes.size()<=Journal::MaximumBytes-size);size_t offset=0;
        while(offset<bytes.size()){
            const auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);
            if(n<0&&errno==EINTR)continue;Require(n>0);offset+=size_t(n);
        }
        size+=bytes.size();if(hashed)hash.Write(bytes.data(),bytes.size());
    }
    void Number(uint64_t n,unsigned width){Bytes bytes;Put(bytes,n,width);Write(bytes);}
};
struct Reader {
    int fd;uint64_t size=0;crypto::CSHA256 hash;
    Bytes Read(size_t count,bool hashed=true){
        Require(count<=Journal::MaximumBytes-size);Bytes bytes(count);size_t offset=0;
        while(offset<count){
            const auto n=::read(fd,bytes.data()+offset,count-offset);
            if(n<0&&errno==EINTR)continue;Require(n>0);offset+=size_t(n);
        }
        size+=count;if(hashed)hash.Write(bytes.data(),bytes.size());return bytes;
    }
    uint64_t Number(unsigned width){const auto b=Read(width);uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(b[i])<<(8*i);return n;}
};
bool Absent(int root,const char* name){
    struct stat st{};if(::fstatat(root,name,&st,AT_SYMLINK_NOFOLLOW)==0)return false;
    Require(errno==ENOENT);return true;
}
void WalletName(const std::string& name){
    Require(name.size()<=255&&name.find('\0')==std::string::npos&&
        name.find('/')==std::string::npos&&name.find('\\')==std::string::npos);
}
std::array<uint8_t,32> Digest(std::span<const uint8_t> bytes){
    std::array<uint8_t,32> digest;crypto::CSHA256().Write(bytes.data(),bytes.size()).Finalize(digest.data());return digest;
}
Bytes Control(int root){
    // A partial/dual-name publication is ambiguous even if the sealed name
    // exists. Do not delete it, follow it, or reinterpret it as no work.
    Require(Absent(root,Partial));
    if(Absent(root,Pending))return {};
    Fd file(::openat(root,Pending,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));const auto before=Stat(file.value);
    Require(S_ISREG(before.st_mode)&&before.st_uid==::geteuid()&&before.st_nlink==1&&
        (before.st_mode&0777)==0400&&before.st_size>=146&&before.st_size<=401);
    Reader read{file.value};auto bytes=read.Read(size_t(before.st_size),false);
    Require(SameFile(before,Stat(file.value))&&SameFile(before,Named(root,Pending)));
    Require(std::equal(bytes.begin(),bytes.begin()+8,std::string_view("DNRPEN01").begin()));
    const auto digest=Digest(std::span(bytes).first(bytes.size()-32));
    Require(std::equal(digest.begin(),digest.end(),bytes.end()-32));return bytes;
}
#endif
}

#ifndef _WIN32
int ReopenCandidateJournal::OpenRoot(const DatadirGuard& guard){
    Require(guard.held_&&guard.lock_fd_>=0);
    Fd root(::open(guard.datadir_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    const auto st=Stat(root.value);struct stat named{};
    Require(::lstat(guard.datadir_.c_str(),&named)==0&&Same(st,named)&&S_ISDIR(st.st_mode)&&st.st_uid==::geteuid()&&
        uint64_t(st.st_dev)==guard.root_device_&&uint64_t(st.st_ino)==guard.root_inode_);
    const auto lock=Stat(guard.lock_fd_),named_lock=Named(root.value,"dinerod.lock");
    Require(Same(lock,named_lock)&&S_ISREG(lock.st_mode)&&lock.st_nlink==1&&lock.st_uid==::geteuid());
    return std::exchange(root.value,-1);
}
struct ReopenCandidateJournal::Data {
    Fd root,dir,file;std::string name;Bytes header;struct stat directory,record;
    const std::thread::id thread=std::this_thread::get_id();mutable bool visiting=false;uint64_t count=0;bool captured_here=false;
    Data(Fd r,Fd d,Fd f,std::string n):root(std::move(r)),dir(std::move(d)),file(std::move(f)),name(std::move(n)),
        header(Header(root.value,name)),directory(Stat(dir.value)),record(Stat(file.value)){}
    void Check(const DatadirGuard& guard)const {
        Require(thread==std::this_thread::get_id());Fd current(ReopenCandidateJournal::OpenRoot(guard));
        Require(Same(Stat(root.value),Stat(current.value))&&Same(directory,Named(current.value,name.c_str())));
        PrivateDirectory(Stat(dir.value));SealedFile(Stat(file.value));
        Require(SameFile(record,Stat(file.value))&&SameFile(record,Named(dir.value,Sealed))&&header==Header(current.value,name));
        OnlySealed(dir.value);
    }
    uint64_t Scan(const std::function<void(const Candidate&)>& visitor={})const {
        Require(::lseek(file.value,0,SEEK_SET)==0);Reader reader{file.value};Require(reader.Read(header.size())==header);
        uint64_t rows=0;
        for(;;){
            const auto at=reader.size,tag=reader.Number(1);
            if(tag==0){
                Require(reader.Number(8)==rows&&reader.Number(8)==at);
                std::array<uint8_t,32> digest;reader.hash.Finalize(digest.data());
                const auto saved=reader.Read(32,false);Require(std::equal(saved.begin(),saved.end(),digest.begin()));
                uint8_t extra;ssize_t n;do{n=::read(file.value,&extra,1);}while(n<0&&errno==EINTR);
                Require(n==0&&reader.size==uint64_t(record.st_size));return rows;
            }
            Require(tag==1&&rows<MaximumCandidates);const auto body=reader.Number(4),proof=reader.Number(4);
            Require(body&&body<=MaximumWireBytes&&proof<=MaximumWireBytes);
            Candidate candidate{reader.Read(size_t(body)),reader.Read(size_t(proof))};CheckWire(candidate);
            ++rows;if(visitor)visitor(candidate);
        }
    }
    Bytes Intent(const DatadirGuard& guard,const std::string& wallet)const {
        WalletName(wallet);Check(guard);Require(!visiting&&Scan()==count);Check(guard);
        // The journal footer authenticates its complete canonical wire corpus.
        Bytes footer(32);Require(::pread(file.value,footer.data(),footer.size(),record.st_size-32)==32);
        Bytes bytes{'D','N','R','P','E','N','0','1'};
        bytes.insert(bytes.end(),header.begin(),header.end());bytes.insert(bytes.end(),footer.begin(),footer.end());
        Put(bytes,wallet.size(),2);bytes.insert(bytes.end(),wallet.begin(),wallet.end());
        const auto digest=Digest(bytes);bytes.insert(bytes.end(),digest.begin(),digest.end());Check(guard);return bytes;
    }
};
ReopenCandidateJournal::ReopenCandidateJournal(std::unique_ptr<Data> data):data_(std::move(data)){}
ReopenCandidateJournal::~ReopenCandidateJournal()=default;
const std::string& ReopenCandidateJournal::Name()const{Require(data_->thread==std::this_thread::get_id());return data_->name;}
uint64_t ReopenCandidateJournal::Count()const{Require(data_->thread==std::this_thread::get_id());return data_->count;}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::Open(const DatadirGuard& guard,const std::string& name){
    (void)Nonce(name);Fd root(OpenRoot(guard));
    Fd dir(::openat(root.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));PrivateDirectory(Stat(dir.value));OnlySealed(dir.value);
    Fd file(::openat(dir.value,Sealed,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));SealedFile(Stat(file.value));
    auto data=std::make_unique<Data>(std::move(root),std::move(dir),std::move(file),name);
    data->Check(guard);data->count=data->Scan();data->Check(guard);
    // Re-establish persistence if a prior caller saw an uncertain seal result.
    Require(::fsync(data->file.value)==0&&::fsync(data->dir.value)==0&&::fsync(data->root.value)==0);data->Check(guard);
    return std::unique_ptr<ReopenCandidateJournal>(new ReopenCandidateJournal(std::move(data)));
}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::Capture(const DatadirGuard& guard,
    const std::shared_ptr<MempoolService>& pool,const ReopenServiceFence& fence,const std::function<bool()>& cancelled){
    Require(bool(pool));Fd root(OpenRoot(guard));Space(root.value,0);std::string name;
    for(unsigned attempt=0;attempt<8;++attempt){
        std::array<uint8_t,16> nonce;Require(RAND_bytes(nonce.data(),int(nonce.size()))==1);name=Prefix;
        constexpr char hex[]="0123456789abcdef";for(auto b:nonce){name.push_back(hex[b>>4]);name.push_back(hex[b&15]);}
        if(::mkdirat(root.value,name.c_str(),0700)==0)break;
        Require(errno==EEXIST&&attempt<7);
    }
    Fd dir(::openat(root.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));PrivateDirectory(Stat(dir.value));
    Fd file(::openat(dir.value,"candidates.partial",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    Writer writer{file.value};writer.Write(Header(root.value,name));uint64_t rows=0;
    pool->ForEachReopenCandidate(fence,[&](const MempoolEntry& entry){
        Require(!cancelled||!cancelled());Require(rows<MaximumCandidates);
        const Candidate candidate{entry.tx.Serialize(),entry.cached_utxotx_payload};CheckWire(candidate);
        const auto next=uint64_t(candidate.body.size())+candidate.proof.size()+9;
        Require(next<=MaximumBytes-49-writer.size);Space(root.value,next);
        writer.Number(1,1);writer.Number(candidate.body.size(),4);writer.Number(candidate.proof.size(),4);
        writer.Write(candidate.body);writer.Write(candidate.proof);++rows;
    });
    Require(!cancelled||!cancelled());const auto payload=writer.size;
    writer.Number(0,1);writer.Number(rows,8);writer.Number(payload,8);
    std::array<uint8_t,32> digest;writer.hash.Finalize(digest.data());writer.Write(digest,false);
    const auto original=Stat(file.value);Require(S_ISREG(original.st_mode)&&original.st_nlink==1&&original.st_uid==::geteuid()&&
        Same(original,Named(dir.value,"candidates.partial"))&&uint64_t(original.st_size)==writer.size);
    Fd current(OpenRoot(guard));Require(Same(Stat(root.value),Stat(current.value))&&Same(Stat(dir.value),Named(root.value,name.c_str())));
    Require(::fchmod(file.value,0400)==0&&::fsync(file.value)==0);
    // linkat gives no-replace publication. A crash before unlink leaves BOTH
    // names/nlink2 and therefore refuses, never an assumed complete journal.
    Require(::linkat(dir.value,"candidates.partial",dir.value,Sealed,0)==0);
    Require(::unlinkat(dir.value,"candidates.partial",0)==0);
    Require(::fsync(dir.value)==0&&::fsync(root.value)==0);
    auto result=Open(guard,name);Require(result->Count()==rows);result->data_->captured_here=true;return result;
}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::OpenPending(const DatadirGuard& guard,std::string& wallet){
    Fd root(OpenRoot(guard));const auto bytes=Control(root.value);
    if(bytes.empty()){wallet.clear();return {};}
    // Header is 72 bytes (profile/root inode/nonce). Never choose a journal by
    // scanning directories or trusting a caller-supplied path in this record.
    constexpr size_t wallet_at=8+72+32;const auto size=uint64_t(bytes[wallet_at])|(uint64_t(bytes[wallet_at+1])<<8);
    Require(size<=255&&bytes.size()==wallet_at+2+size+32);
    const std::string name(bytes.begin()+wallet_at+2,bytes.end()-32);WalletName(name);
    std::string journal=Prefix;constexpr char hex[]="0123456789abcdef";
    for(size_t i=8+56;i<8+72;++i){journal.push_back(hex[bytes[i]>>4]);journal.push_back(hex[bytes[i]&15]);}
    auto opened=Open(guard,journal);Require(opened->data_->Intent(guard,name)==bytes);
    Require(Control(root.value)==bytes);wallet=name;return opened;
}
void ReopenCandidateJournal::SelectForReopen(const DatadirGuard& guard,const std::string& wallet,
    const std::function<void()>& before_publish){
    Require(data_->captured_here&&bool(before_publish));const auto bytes=data_->Intent(guard,wallet);
    Fd root(OpenRoot(guard));Require(Control(root.value).empty());Space(root.value,bytes.size());
    Fd file(::openat(root.value,Partial,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    Writer writer{file.value};writer.Write(bytes,false);
    const auto original=Stat(file.value);Require(S_ISREG(original.st_mode)&&original.st_nlink==1&&
        original.st_uid==::geteuid()&&Same(original,Named(root.value,Partial)));
    Require(::fchmod(file.value,0400)==0&&::fsync(file.value)==0);data_->Check(guard);
    before_publish(); // Any failure from here must leave transport CLOSED.
    data_->captured_here=false;
    Require(::linkat(root.value,Partial,root.value,Pending,0)==0);
    Require(::unlinkat(root.value,Partial,0)==0&&::fsync(root.value)==0);
    CheckPending(guard,wallet);
}
void ReopenCandidateJournal::CheckPending(const DatadirGuard& guard,const std::string& wallet)const {
    const auto expected=data_->Intent(guard,wallet);Fd root(OpenRoot(guard));Require(Control(root.value)==expected);
}
void ReopenCandidateJournal::ArchivePending(const DatadirGuard& guard,const std::string& wallet){
    CheckPending(guard,wallet);Fd root(OpenRoot(guard));
    const auto archive=std::string(Pending)+".completed-"+data_->name.substr(std::strlen(Prefix));
    // Preserve completed intent AND original journal. Dual-name interruption
    // has nlink2 and refuses; do not silently resolve an uncertain completion.
    Require(::linkat(root.value,Pending,root.value,archive.c_str(),0)==0);
    Require(::unlinkat(root.value,Pending,0)==0&&::fsync(root.value)==0);
    Require(Control(root.value).empty());data_->Check(guard);
}
void ReopenCandidateJournal::ForEach(const DatadirGuard& guard,const std::function<void(const Candidate&)>& visitor)const {
    data_->Check(guard);Require(bool(visitor)&&!data_->visiting);data_->visiting=true;
    struct Reset{bool& flag;~Reset(){flag=false;}} reset{data_->visiting};
    Require(data_->Scan()==data_->count);data_->Check(guard);
    Require(data_->Scan(visitor)==data_->count);data_->Check(guard);
}
#else
struct ReopenCandidateJournal::Data{};
ReopenCandidateJournal::ReopenCandidateJournal(std::unique_ptr<Data> data):data_(std::move(data)){}
ReopenCandidateJournal::~ReopenCandidateJournal()=default;
int ReopenCandidateJournal::OpenRoot(const DatadirGuard&){Require(false);return -1;}
const std::string& ReopenCandidateJournal::Name()const{throw std::runtime_error("Reopen journal unavailable on this platform");}
uint64_t ReopenCandidateJournal::Count()const{Require(false);return 0;}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::Open(const DatadirGuard&,const std::string&){Require(false);return {};}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::OpenPending(const DatadirGuard&,std::string&){Require(false);return {};}
void ReopenCandidateJournal::SelectForReopen(const DatadirGuard&,const std::string&,const std::function<void()>&){Require(false);}
void ReopenCandidateJournal::CheckPending(const DatadirGuard&,const std::string&)const{Require(false);}
void ReopenCandidateJournal::ArchivePending(const DatadirGuard&,const std::string&){Require(false);}
std::unique_ptr<ReopenCandidateJournal> ReopenCandidateJournal::Capture(const DatadirGuard&,const std::shared_ptr<MempoolService>&,
    const ReopenServiceFence&,const std::function<bool()>&){Require(false);return {};}
void ReopenCandidateJournal::ForEach(const DatadirGuard&,const std::function<void(const Candidate&)>&)const{Require(false);}
#endif
} // namespace dinero::daemon
