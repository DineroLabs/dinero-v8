#pragma once
#include "daemon/datadir_guard.h"
#include "consensus/orchard_validation_profile.h"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace dinero::p2p {
// Immutable PUBLIC bytes only. Checksums detect damage, never certify a proof,
// body, balance, cursor, catalog or owner. Every consumer must revalidate.
// Existing/partial/bad records are retained; no overwrite/repair/import API.
class OrchardHistoryCache final {
public:
    using Bytes=std::vector<uint8_t>;
    using Sink=std::function<void(std::span<const uint8_t>)>;
    using Source=std::function<void(std::span<uint8_t>)>;
    static constexpr const char* Directory=".orchard-phone-history-v1";
    explicit OrchardHistoryCache(const daemon::DatadirGuard& guard):guard_(guard){
#ifndef _WIN32
        root_=OpenRoot();try{Check();}catch(...){::close(root_);root_=-1;throw;}
#else
        Refuse();
#endif
    }
    ~OrchardHistoryCache(){
#ifndef _WIN32
        if(root_>=0)::close(root_);
#endif
    }
    OrchardHistoryCache(const OrchardHistoryCache&)=delete;
    void Check()const{
#ifndef _WIN32
        Fd current(OpenRoot());Need(Same(Stat(root_),Stat(current.fd)));
#else
        Refuse();
#endif
    }
    bool Has(char kind,const uint256& id)const{
#ifndef _WIN32
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(false);if(dir.fd<0){Check();return false;}
        struct stat s{};const auto name=Name(kind,id);
        const int rc=::fstatat(dir.fd,name.c_str(),&s,AT_SYMLINK_NOFOLLOW);if(rc<0){Need(errno==ENOENT);DirectoryCheck(dir.fd);Check();return false;}
        File(s);DirectoryCheck(dir.fd);Check();return true;
#else
        Refuse();
#endif
    }
    void Read(char kind,const uint256& id,uint64_t maximum,
        const std::function<void(uint64_t,const Source&)>& consume)const{
#ifndef _WIN32
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(false);Need(dir.fd>=0);
        const auto name=Name(kind,id);Fd file(::openat(dir.fd,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        const auto before=Stat(file.fd);File(before);Need(SameFile(before,Named(dir.fd,name)));
        Bytes header(HeaderBytes);ReadExact(file.fd,header);const auto length=Number(header,HeaderBytes-8);
        Need(length<=maximum&&header==Header(kind,id,length)&&uint64_t(before.st_size)==HeaderBytes+length+32);
        crypto::CSHA256 hash;hash.Write(header.data(),header.size());uint64_t left=length;
        std::array<uint8_t,65536> chunk{};
        while(left){const auto n=size_t(std::min<uint64_t>(left,chunk.size()));ReadExact(file.fd,{chunk.data(),n});hash.Write(chunk.data(),n);left-=n;}
        std::array<uint8_t,32> digest{},saved{};hash.Finalize(digest.data());ReadExact(file.fd,saved);Need(saved==digest);
        Need(SameFile(before,Stat(file.fd))&&SameFile(before,Named(dir.fd,name)));
        Need(::lseek(file.fd,HeaderBytes,SEEK_SET)==off_t(HeaderBytes));left=length;
        consume(length,[&](std::span<uint8_t> bytes){Need(bytes.size()<=left);ReadExact(file.fd,bytes);left-=bytes.size();});
        Need(!left&&SameFile(before,Stat(file.fd))&&SameFile(before,Named(dir.fd,name)));
        DirectoryCheck(dir.fd);Check();
#else
        Refuse();
#endif
    }
    void Put(char kind,const uint256& id,uint64_t length,uint64_t maximum,
        const std::function<void(const Sink&)>& produce){
#ifndef _WIN32
        Need(length<=maximum);const auto name=Name(kind,id);
        // Check existing bytes using the same strict reader, without nesting
        // this mutex or holding any selected/wallet/network lock.
        if(Has(kind,id)){
            crypto::CSHA256 incoming;uint64_t count=0;
            produce([&](auto bytes){Need(bytes.size()<=length-count);incoming.Write(bytes.data(),bytes.size());count+=bytes.size();});
            Need(count==length);std::array<uint8_t,32> a{},b{};incoming.Finalize(a.data());
            Read(kind,id,maximum,[&](uint64_t size,const Source& read){Need(size==length);crypto::CSHA256 hash;
                std::array<uint8_t,65536> bytes{};while(size){const auto n=size_t(std::min<uint64_t>(size,bytes.size()));read({bytes.data(),n});hash.Write(bytes.data(),n);size-=n;}hash.Finalize(b.data());});
            Need(a==b);return;
        }
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(true);
        struct statvfs space{};Need(::fstatvfs(dir.fd,&space)==0&&space.f_frsize&&
            length<=UINT64_MAX-(uint64_t(512)<<20)-HeaderBytes-32-space.f_frsize&&
            uint64_t(space.f_bavail)>=(length+HeaderBytes+32+(uint64_t(512)<<20)+space.f_frsize-1)/space.f_frsize);
        const auto nonce=secure_random_bytes(16);std::string partial="partial-";
        for(auto b:nonce){partial.push_back("0123456789abcdef"[b>>4]);partial.push_back("0123456789abcdef"[b&15]);}
        Fd file(::openat(dir.fd,partial.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        const auto opened=Stat(file.fd);Need(S_ISREG(opened.st_mode)&&opened.st_uid==::geteuid()&&opened.st_nlink==1);
        const auto header=Header(kind,id,length);crypto::CSHA256 hash;hash.Write(header.data(),header.size());WriteExact(file.fd,header);uint64_t count=0;
        produce([&](auto bytes){Need(bytes.size()<=length-count);WriteExact(file.fd,bytes);hash.Write(bytes.data(),bytes.size());count+=bytes.size();});
        Need(count==length);std::array<uint8_t,32> digest{};hash.Finalize(digest.data());WriteExact(file.fd,digest);
        Need(::fchmod(file.fd,0400)==0&&::fsync(file.fd)==0&&Same(Stat(file.fd),Named(dir.fd,partial)));
        DirectoryCheck(dir.fd);Check();
        // linkat is no-replace on both supported POSIX families. The only
        // removed name is this successful writer's own temporary hardlink.
        Need(::linkat(dir.fd,partial.c_str(),dir.fd,name.c_str(),0)==0&&::fsync(dir.fd)==0);
        Need(::unlinkat(dir.fd,partial.c_str(),0)==0&&::fsync(dir.fd)==0&&::fsync(root_)==0);
        const auto final=Stat(file.fd);File(final);Need(SameFile(final,Named(dir.fd,name)));DirectoryCheck(dir.fd);Check();
#else
        Refuse();
#endif
    }
private:
    const daemon::DatadirGuard& guard_;int root_=-1;mutable std::mutex mutex_;
    mutable std::optional<std::pair<uint64_t,uint64_t>> directory_identity_;
    static constexpr size_t HeaderBytes=97;
    [[noreturn]] static void Refuse(){throw std::runtime_error("Retained Orchard history DATA refused");}
    static void Need(bool value){if(!value)Refuse();}
#ifndef _WIN32
    struct Fd{int fd=-1;explicit Fd(int f,bool optional=false):fd(f){Need(f>=0||optional);}~Fd(){if(fd>=0)::close(fd);}
        Fd(Fd&& o):fd(std::exchange(o.fd,-1)){}Fd(const Fd&)=delete;};
    static struct stat Stat(int fd){struct stat s{};Need(::fstat(fd,&s)==0);return s;}
    static struct stat Named(int fd,const std::string& name){struct stat s{};Need(::fstatat(fd,name.c_str(),&s,AT_SYMLINK_NOFOLLOW)==0);return s;}
    static bool Same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
    static bool SameFile(const struct stat& a,const struct stat& b){
#ifdef __APPLE__
        const auto am=a.st_mtimespec,ac=a.st_ctimespec,bm=b.st_mtimespec,bc=b.st_ctimespec;
#else
        const auto am=a.st_mtim,ac=a.st_ctim,bm=b.st_mtim,bc=b.st_ctim;
#endif
        return Same(a,b)&&a.st_size==b.st_size&&a.st_mode==b.st_mode&&a.st_nlink==b.st_nlink&&a.st_uid==b.st_uid&&
            am.tv_sec==bm.tv_sec&&am.tv_nsec==bm.tv_nsec&&ac.tv_sec==bc.tv_sec&&ac.tv_nsec==bc.tv_nsec;
    }
    static void File(const struct stat& s){Need(S_ISREG(s.st_mode)&&s.st_uid==::geteuid()&&s.st_nlink==1&&(s.st_mode&0777)==0400&&s.st_size>=off_t(HeaderBytes+32));}
    int OpenRoot()const{
        Need(guard_.held_&&guard_.lock_fd_>=0);Fd fd(::open(guard_.datadir_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        const auto s=Stat(fd.fd);struct stat named{};Need(::lstat(guard_.datadir_.c_str(),&named)==0&&Same(s,named)&&
            S_ISDIR(s.st_mode)&&s.st_uid==::geteuid()&&uint64_t(s.st_dev)==guard_.root_device_&&uint64_t(s.st_ino)==guard_.root_inode_);
        const auto lock=Stat(guard_.lock_fd_);Need(Same(lock,Named(fd.fd,"dinerod.lock"))&&S_ISREG(lock.st_mode)&&lock.st_nlink==1&&lock.st_uid==::geteuid());
        return std::exchange(fd.fd,-1);
    }
    void DirectoryCheck(int fd)const{const auto s=Stat(fd);Need(S_ISDIR(s.st_mode)&&s.st_uid==::geteuid()&&(s.st_mode&0777)==0700&&Same(s,Named(root_,Directory)));
        const auto identity=std::pair{uint64_t(s.st_dev),uint64_t(s.st_ino)};if(directory_identity_)Need(*directory_identity_==identity);else directory_identity_=identity;}
    Fd OpenDirectory(bool create)const{
        struct stat s{};if(::fstatat(root_,Directory,&s,AT_SYMLINK_NOFOLLOW)<0){Need(errno==ENOENT&&!directory_identity_);if(!create)return Fd(-1,true);
            Need(::mkdirat(root_,Directory,0700)==0&&::fsync(root_)==0);}
        Fd dir(::openat(root_,Directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));DirectoryCheck(dir.fd);return dir;
    }
    static void ReadExact(int fd,std::span<uint8_t> bytes){while(!bytes.empty()){const auto n=::read(fd,bytes.data(),bytes.size());if(n<0&&errno==EINTR)continue;Need(n>0);bytes=bytes.subspan(size_t(n));}}
    static void WriteExact(int fd,std::span<const uint8_t> bytes){while(!bytes.empty()){const auto n=::write(fd,bytes.data(),bytes.size());if(n<0&&errno==EINTR)continue;Need(n>0);bytes=bytes.subspan(size_t(n));}}
    static uint64_t Number(std::span<const uint8_t> bytes,size_t at){Need(at+8<=bytes.size());uint64_t n=0;for(unsigned i=0;i<8;++i)n|=uint64_t(bytes[at+i])<<(8*i);return n;}
    Bytes Header(char kind,const uint256& id,uint64_t length)const{
        Need(kind=='p'||kind=='b');Bytes bytes{'D','N','O','H','C','0','1',0};std::array<uint8_t,32> profile{};
        crypto::CSHA256().Write(consensus::OrchardValidationProfile()).Finalize(profile.data());bytes.insert(bytes.end(),profile.begin(),profile.end());
        const auto s=Stat(root_);for(auto n:{uint64_t(s.st_dev),uint64_t(s.st_ino)})for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(n>>(8*i)));
        bytes.push_back(uint8_t(kind));bytes.insert(bytes.end(),id.begin(),id.end());for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(length>>(8*i)));Need(bytes.size()==HeaderBytes);return bytes;
    }
    static std::string Name(char kind,const uint256& id){Need(kind=='p'||kind=='b');std::array<uint8_t,32> profile{};
        crypto::CSHA256().Write(consensus::OrchardValidationProfile()).Finalize(profile.data());std::string name(1,kind);name+='-';
        for(auto b:profile){name.push_back("0123456789abcdef"[b>>4]);name.push_back("0123456789abcdef"[b&15]);}return name+'-'+id.GetHex()+".raw";}
#endif
};
} // namespace dinero::p2p
