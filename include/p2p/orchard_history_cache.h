#pragma once
#include "daemon/datadir_guard.h"
#include "consensus/orchard_validation_profile.h"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include "primitives/uint256.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1<<0)
#endif
#endif
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
        root_=OpenRoot();try{Check();}catch(...){::CloseHandle(root_);root_=INVALID_HANDLE_VALUE;throw;}
#endif
    }
    ~OrchardHistoryCache(){
#ifndef _WIN32
        if(root_>=0)::close(root_);
#else
        if(root_!=INVALID_HANDLE_VALUE)::CloseHandle(root_);
#endif
    }
    OrchardHistoryCache(const OrchardHistoryCache&)=delete;
    void Check()const{
#ifndef _WIN32
        Fd current(OpenRoot());Need(Same(Stat(root_),Stat(current.fd)));
#else
        WinHandle current(OpenRoot());Need(Same(Info(root_),Info(current.handle)));
#endif
    }
    bool Has(char kind,const uint256& id)const{
#ifndef _WIN32
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(false);if(dir.fd<0){Check();return false;}
        struct stat s{};const auto name=Name(kind,id);
        const int rc=::fstatat(dir.fd,name.c_str(),&s,AT_SYMLINK_NOFOLLOW);if(rc<0){Need(errno==ENOENT);DirectoryCheck(dir.fd);Check();return false;}
        File(s);DirectoryCheck(dir.fd);Check();return true;
#else
        return HasWindows(kind,id);
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
        ReadWindows(kind,id,maximum,consume);
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
        // Atomic no-replace publication. linkat is denied to Android app
        // domains (SELinux app_data_file neverallow, targetSdk>=29), and
        // bionic lacks a renameat2 wrapper before API 30, so Linux invokes
        // the syscall directly; Apple uses renameatx_np(RENAME_EXCL).
        Need(NoReplaceRename(dir.fd,partial,name)&&::fsync(dir.fd)==0&&::fsync(root_)==0);
        const auto final=Stat(file.fd);File(final);Need(SameFile(final,Named(dir.fd,name)));DirectoryCheck(dir.fd);Check();
#else
        PutWindows(kind,id,length,maximum,produce);
#endif
    }
private:
    const daemon::DatadirGuard& guard_;
#ifdef _WIN32
    HANDLE root_=INVALID_HANDLE_VALUE;
#else
    int root_=-1;
#endif
    mutable std::mutex mutex_;
    mutable std::optional<std::pair<uint64_t,uint64_t>> directory_identity_;
    static constexpr size_t HeaderBytes=97;
    [[noreturn]] static void Refuse(){throw std::runtime_error("Retained Orchard history DATA refused");}
    static void Need(bool value){if(!value)Refuse();}
#ifndef _WIN32
    struct Fd{int fd=-1;explicit Fd(int f,bool optional=false):fd(f){Need(f>=0||optional);}~Fd(){if(fd>=0)::close(fd);}
        Fd(Fd&& o):fd(std::exchange(o.fd,-1)){}Fd(const Fd&)=delete;};
    static bool NoReplaceRename(int dir,const std::string& from,const std::string& to){
#if defined(__APPLE__)
        return ::renameatx_np(dir,from.c_str(),dir,to.c_str(),RENAME_EXCL)==0;
#elif defined(__linux__)
        return ::syscall(SYS_renameat2,dir,from.c_str(),dir,to.c_str(),RENAME_NOREPLACE)==0;
#else
#error no-replace rename primitive is required on this POSIX family
#endif
    }
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
#else
    struct WinHandle {
        HANDLE handle=INVALID_HANDLE_VALUE;
        explicit WinHandle(HANDLE value,bool optional=false):handle(value){Need(value!=INVALID_HANDLE_VALUE||optional);}
        ~WinHandle(){if(handle!=INVALID_HANDLE_VALUE)::CloseHandle(handle);}
        WinHandle(WinHandle&& other):handle(std::exchange(other.handle,INVALID_HANDLE_VALUE)){}
        WinHandle(const WinHandle&)=delete;
    };
    static BY_HANDLE_FILE_INFORMATION Info(HANDLE handle){BY_HANDLE_FILE_INFORMATION info{};
        Need(handle!=INVALID_HANDLE_VALUE&&::GetFileInformationByHandle(handle,&info));return info;}
    static uint64_t FileIndex(const BY_HANDLE_FILE_INFORMATION& info){
        return (uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;}
    static bool Same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
        return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&FileIndex(a)==FileIndex(b);}
    static bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
        return Same(a,b)&&a.nNumberOfLinks==b.nNumberOfLinks&&a.dwFileAttributes==b.dwFileAttributes&&
            a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&
            a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&
            a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;}
    static void CheckDirectoryInfo(const BY_HANDLE_FILE_INFORMATION& info){
        Need((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));}
    static void File(const BY_HANDLE_FILE_INFORMATION& info){
        Need(!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
            (info.dwFileAttributes&FILE_ATTRIBUTE_READONLY)&&info.nNumberOfLinks==1&&
            ((uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow)>=HeaderBytes+32);}
    static std::wstring NativePath(const std::filesystem::path& path){
        // The immutable cell name is long enough to cross MAX_PATH under an
        // ordinary Windows profile. Keep all cache API calls in the extended
        // namespace so publication and the subsequent identity read agree.
        std::wstring name=path.wstring();std::replace(name.begin(),name.end(),L'/',L'\\');
        if(name.rfind(L"\\\\?\\",0)==0)return name;
        if(name.rfind(L"\\\\",0)==0)return L"\\\\?\\UNC\\"+name.substr(2);
        Need(name.size()>2&&name[1]==L':'&&name[2]==L'\\');
        return L"\\\\?\\"+name;
    }
    static HANDLE OpenPath(const std::filesystem::path& path,bool directory,DWORD share=0){
        if(!share)share=directory?FILE_SHARE_READ|FILE_SHARE_WRITE:FILE_SHARE_READ;
        const auto native=NativePath(path);
        return ::CreateFileW(native.c_str(),GENERIC_READ,share,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|
                (directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);}
    HANDLE OpenRoot()const{
        Need(guard_.held_&&guard_.lock_handle_);
        WinHandle root(OpenPath(guard_.datadir_,true));CheckDirectoryInfo(Info(root.handle));
        return std::exchange(root.handle,INVALID_HANDLE_VALUE);
    }
    std::filesystem::path DirectoryPath()const{return guard_.datadir_/OrchardHistoryCache::Directory;}
    std::filesystem::path CellPath(char kind,const uint256& id)const{return DirectoryPath()/Name(kind,id);}
    void DirectoryCheck(HANDLE handle)const{
        const auto info=Info(handle);CheckDirectoryInfo(info);
        WinHandle named(OpenPath(DirectoryPath(),true));Need(Same(info,Info(named.handle)));
        const auto identity=std::pair{uint64_t(info.dwVolumeSerialNumber),FileIndex(info)};
        if(directory_identity_)Need(*directory_identity_==identity);else directory_identity_=identity;
        Check();
    }
    WinHandle OpenDirectory(bool create)const{
        const auto path=DirectoryPath();
        const auto native=NativePath(path);
        const auto attributes=::GetFileAttributesW(native.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            const auto reason=::GetLastError();Need((reason==ERROR_FILE_NOT_FOUND||reason==ERROR_PATH_NOT_FOUND)&&!directory_identity_);
            if(!create)return WinHandle(INVALID_HANDLE_VALUE,true);
            Need(::CreateDirectoryW(native.c_str(),nullptr));
        }
        WinHandle directory(OpenPath(path,true));DirectoryCheck(directory.handle);return directory;
    }
    static void ReadExact(HANDLE handle,std::span<uint8_t> bytes){
        while(!bytes.empty()){DWORD count=0;const auto n=DWORD(std::min<size_t>(bytes.size(),1<<20));
            Need(::ReadFile(handle,bytes.data(),n,&count,nullptr)&&count>0);bytes=bytes.subspan(count);}
    }
    static void WriteExact(HANDLE handle,std::span<const uint8_t> bytes){
        while(!bytes.empty()){DWORD count=0;const auto n=DWORD(std::min<size_t>(bytes.size(),1<<20));
            Need(::WriteFile(handle,bytes.data(),n,&count,nullptr)&&count>0);bytes=bytes.subspan(count);}
    }
    static uint64_t Number(std::span<const uint8_t> bytes,size_t at){Need(at+8<=bytes.size());uint64_t n=0;
        for(unsigned i=0;i<8;++i)n|=uint64_t(bytes[at+i])<<(8*i);return n;}
    Bytes Header(char kind,const uint256& id,uint64_t length)const{
        Need(kind=='p'||kind=='b');Bytes bytes{'D','N','O','H','C','0','1',0};std::array<uint8_t,32> profile{};
        crypto::CSHA256().Write(consensus::OrchardValidationProfile()).Finalize(profile.data());
        bytes.insert(bytes.end(),profile.begin(),profile.end());
        const auto root=Info(root_);for(auto n:{uint64_t(root.dwVolumeSerialNumber),FileIndex(root)})
            for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(n>>(8*i)));
        bytes.push_back(uint8_t(kind));bytes.insert(bytes.end(),id.begin(),id.end());
        for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(length>>(8*i)));
        Need(bytes.size()==HeaderBytes);return bytes;
    }
    static std::string Name(char kind,const uint256& id){Need(kind=='p'||kind=='b');std::array<uint8_t,32> profile{};
        crypto::CSHA256().Write(consensus::OrchardValidationProfile()).Finalize(profile.data());std::string name(1,kind);name+='-';
        for(auto b:profile){name.push_back("0123456789abcdef"[b>>4]);name.push_back("0123456789abcdef"[b&15]);}
        return name+'-'+id.GetHex()+".raw";}
    bool HasWindows(char kind,const uint256& id)const{
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(false);
        if(dir.handle==INVALID_HANDLE_VALUE){Check();return false;}
        const auto path=CellPath(kind,id);WinHandle file(OpenPath(path,false),true);
        if(file.handle==INVALID_HANDLE_VALUE){const auto reason=::GetLastError();Need(reason==ERROR_FILE_NOT_FOUND);
            DirectoryCheck(dir.handle);return false;}
        File(Info(file.handle));DirectoryCheck(dir.handle);return true;
    }
    void ReadWindows(char kind,const uint256& id,uint64_t maximum,
        const std::function<void(uint64_t,const Source&)>& consume)const{
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(false);Need(dir.handle!=INVALID_HANDLE_VALUE);
        const auto path=CellPath(kind,id);WinHandle file(OpenPath(path,false));const auto before=Info(file.handle);File(before);
        Bytes header(HeaderBytes);ReadExact(file.handle,header);const auto length=Number(header,HeaderBytes-8);
        Need(length<=maximum&&header==Header(kind,id,length)&&
            ((uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow)==HeaderBytes+length+32);
        crypto::CSHA256 hash;hash.Write(header.data(),header.size());uint64_t left=length;
        std::array<uint8_t,65536> chunk{};
        while(left){const auto n=size_t(std::min<uint64_t>(left,chunk.size()));ReadExact(file.handle,{chunk.data(),n});
            hash.Write(chunk.data(),n);left-=n;}
        std::array<uint8_t,32> digest{},saved{};hash.Finalize(digest.data());ReadExact(file.handle,saved);Need(saved==digest);
        WinHandle named(OpenPath(path,false));Need(SameFile(before,Info(file.handle))&&SameFile(before,Info(named.handle)));
        LARGE_INTEGER start{};start.QuadPart=HeaderBytes;Need(::SetFilePointerEx(file.handle,start,nullptr,FILE_BEGIN));left=length;
        consume(length,[&](std::span<uint8_t> bytes){Need(bytes.size()<=left);ReadExact(file.handle,bytes);left-=bytes.size();});
        Need(!left&&SameFile(before,Info(file.handle))&&SameFile(before,Info(named.handle)));
        DirectoryCheck(dir.handle);
    }
    void PutWindows(char kind,const uint256& id,uint64_t length,uint64_t maximum,
        const std::function<void(const Sink&)>& produce){
        Need(length<=maximum);const auto path=CellPath(kind,id);
        if(HasWindows(kind,id)){
            crypto::CSHA256 incoming;uint64_t count=0;
            produce([&](auto bytes){Need(bytes.size()<=length-count);incoming.Write(bytes.data(),bytes.size());count+=bytes.size();});
            Need(count==length);std::array<uint8_t,32> a{},b{};incoming.Finalize(a.data());
            ReadWindows(kind,id,maximum,[&](uint64_t size,const Source& read){Need(size==length);crypto::CSHA256 hash;
                std::array<uint8_t,65536> bytes{};while(size){const auto n=size_t(std::min<uint64_t>(size,bytes.size()));
                    read({bytes.data(),n});hash.Write(bytes.data(),n);size-=n;}hash.Finalize(b.data());});
            Need(a==b);return;
        }
        std::lock_guard lock(mutex_);Check();auto dir=OpenDirectory(true);
        ULARGE_INTEGER available{},total{},free{};
        const auto nativeDirectory=NativePath(DirectoryPath());
        Need(::GetDiskFreeSpaceExW(nativeDirectory.c_str(),&available,&total,&free)&&
            length<=UINT64_MAX-(uint64_t(512)<<20)-HeaderBytes-32&&
            available.QuadPart>=length+HeaderBytes+32+(uint64_t(512)<<20));
        const auto nonce=secure_random_bytes(16);std::string partial="partial-";
        for(auto b:nonce){partial.push_back("0123456789abcdef"[b>>4]);partial.push_back("0123456789abcdef"[b&15]);}
        const auto temporary=DirectoryPath()/partial;
        const auto nativeTemporary=NativePath(temporary);
        WinHandle file(::CreateFileW(nativeTemporary.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
            FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        const auto opened=Info(file.handle);Need(!(opened.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&opened.nNumberOfLinks==1);
        const auto header=Header(kind,id,length);crypto::CSHA256 hash;hash.Write(header.data(),header.size());
        WriteExact(file.handle,header);uint64_t count=0;
        produce([&](auto bytes){Need(bytes.size()<=length-count);WriteExact(file.handle,bytes);
            hash.Write(bytes.data(),bytes.size());count+=bytes.size();});
        Need(count==length);std::array<uint8_t,32> digest{};hash.Finalize(digest.data());WriteExact(file.handle,digest);
        FILE_BASIC_INFO basic{};Need(::GetFileInformationByHandleEx(file.handle,FileBasicInfo,&basic,sizeof(basic)));
        basic.FileAttributes=FILE_ATTRIBUTE_READONLY;
        Need(::SetFileInformationByHandle(file.handle,FileBasicInfo,&basic,sizeof(basic))&&::FlushFileBuffers(file.handle));
        Need(Same(opened,Info(file.handle)));DirectoryCheck(dir.handle);
        // Atomic publication from the open handle with ReplaceIfExists=false:
        // a competing final cell is never overwritten or silently adopted.
        const auto target=NativePath(path);
        const auto size=offsetof(FILE_RENAME_INFO,FileName)+(target.size()+1)*sizeof(wchar_t);
        auto* raw=static_cast<FILE_RENAME_INFO*>(std::malloc(size));Need(raw!=nullptr);
        std::unique_ptr<FILE_RENAME_INFO,decltype(&std::free)> rename(raw,&std::free);
        std::memset(raw,0,size);raw->ReplaceIfExists=FALSE;raw->RootDirectory=nullptr;
        raw->FileNameLength=DWORD(target.size()*sizeof(wchar_t));
        std::memcpy(raw->FileName,target.c_str(),raw->FileNameLength+sizeof(wchar_t));
        Need(::SetFileInformationByHandle(file.handle,FileRenameInfo,raw,DWORD(size)));
        WinHandle named(OpenPath(path,false,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE));
        const auto final=Info(file.handle);File(final);
        Need(SameFile(final,Info(named.handle)));DirectoryCheck(dir.handle);
    }
#endif
};
} // namespace dinero::p2p
