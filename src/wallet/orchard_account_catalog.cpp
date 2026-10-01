#include "wallet/orchard_account_catalog.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <sqlite3.h>
namespace dinero::wallet {
namespace {
constexpr char kSetting[]="orchard_account_catalog_v1";
constexpr std::string_view kMagic="DNOAC01";
constexpr std::string_view kDomain="Dinero Orchard account catalog authentication v1";
constexpr size_t kHeader=7+32+1+8+4, kEntry=4+1+32+4+4, kTag=32;
constexpr size_t kMaxBytes=kHeader+OrchardAccountCatalog::kMaxAccounts*kEntry+kTag;
void Check(bool ok) { if(!ok) throw std::runtime_error("Orchard account catalog unavailable or invalid"); }
struct Statement {
    sqlite3_stmt* p=nullptr;
    Statement(sqlite3* db,const char* text) {
        const int rc=sqlite3_prepare_v2(db,text,-1,&p,nullptr);
        if(rc!=SQLITE_OK){sqlite3_finalize(p);p=nullptr;Check(false);}
    }
    ~Statement(){sqlite3_finalize(p);}
    Statement(const Statement&)=delete;
    void Text(int n,const std::string& s){Check(sqlite3_bind_text(p,n,s.data(),static_cast<int>(s.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
    void Done(){Check(sqlite3_step(p)==SQLITE_DONE);}
};
std::array<uint8_t,32> Identity(sqlite3* db){
    Statement q(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    Check(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(q.p,0)==32);
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.p,0));Check(bytes);
    std::array<uint8_t,32> id{};std::copy_n(bytes,id.size(),id.begin());q.Done();
    Check(std::any_of(id.begin(),id.end(),[](uint8_t b){return b!=0;}));return id;
}
std::array<uint8_t,32> Tag(std::span<const uint8_t> seed,std::span<const uint8_t> payload){
    Check(seed.size()==64);
    std::vector<uint8_t> material(kDomain.begin(),kDomain.end());material.push_back(0);
    material.insert(material.end(),payload.begin(),payload.end());
    std::array<uint8_t,32> tag{};unsigned size=0;
    Check(HMAC(EVP_sha256(),seed.data(),static_cast<int>(seed.size()),material.data(),material.size(),tag.data(),&size)&&size==tag.size());
    return tag;
}
std::vector<uint8_t> Decode(std::string_view text){
    Check(text.size()>=(kHeader+kTag)*2&&text.size()<=kMaxBytes*2&&text.size()%2==0);
    const auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;Check(false);return 0;};
    std::vector<uint8_t> bytes(text.size()/2);
    for(size_t i=0;i<bytes.size();++i)bytes[i]=(digit(text[i*2])<<4)|digit(text[i*2+1]);
    return bytes;
}
std::string Encode(std::span<const uint8_t> bytes){
    constexpr char digits[]="0123456789abcdef";std::string text(bytes.size()*2,'0');
    for(size_t i=0;i<bytes.size();++i){text[i*2]=digits[bytes[i]>>4];text[i*2+1]=digits[bytes[i]&15];}return text;
}
void Put(std::vector<uint8_t>& bytes,uint64_t value,size_t size){for(size_t i=0;i<size;++i)bytes.push_back(static_cast<uint8_t>(value>>(8*i)));}
uint64_t Get(std::span<const uint8_t> bytes,size_t& pos,size_t size){
    Check(pos<=bytes.size()&&size<=bytes.size()-pos);uint64_t value=0;
    for(size_t i=0;i<size;++i)value|=uint64_t(bytes[pos++])<<(8*i);return value;
}
void RequireFull(sqlite3* db){
    Statement s(db,"PRAGMA synchronous");Check(sqlite3_step(s.p)==SQLITE_ROW&&sqlite3_column_type(s.p,0)==SQLITE_INTEGER&&sqlite3_column_int(s.p,0)>=2);s.Done();
    Statement j(db,"PRAGMA journal_mode");Check(sqlite3_step(j.p)==SQLITE_ROW&&sqlite3_column_type(j.p,0)==SQLITE_TEXT);
    const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(j.p,0));Check(text);
    std::string mode(text,sqlite3_column_bytes(j.p,0));j.Done();
    Check(mode=="wal"||mode=="delete"||mode=="truncate"||mode=="persist");
}
} // namespace
std::optional<OrchardAccountCatalog::Snapshot> OrchardAccountCatalog::Read(sqlite3* db,std::span<const uint8_t> seed){
    Check(db&&!sqlite3_get_autocommit(db)&&seed.size()==64);
    Statement q(db,"SELECT value FROM settings WHERE key=?");q.Text(1,kSetting);
    const int rc=sqlite3_step(q.p);if(rc==SQLITE_DONE)return std::nullopt;
    Check(rc==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_TEXT);
    const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(q.p,0));const int size=sqlite3_column_bytes(q.p,0);
    Check(text&&size>=0&&static_cast<size_t>(size)<=kMaxBytes*2);
    const auto bytes=Decode(std::string_view(text,static_cast<size_t>(size)));q.Done();
    const std::span<const uint8_t> payload(bytes.data(),bytes.size()-kTag);
    const auto tag=Tag(seed,payload);Check(CRYPTO_memcmp(tag.data(),bytes.data()+payload.size(),kTag)==0);
    const auto id=Identity(db);Check(std::equal(kMagic.begin(),kMagic.end(),payload.begin()));
    Check(CRYPTO_memcmp(id.data(),payload.data()+kMagic.size(),id.size())==0);
    size_t pos=kMagic.size()+id.size();const uint64_t mode=Get(payload,pos,1);Check(mode<=1);
    Snapshot result{mode==1,Get(payload,pos,8),{}};const auto count=Get(payload,pos,4);
    Check(result.revision>0&&result.revision<=uint64_t(std::numeric_limits<int64_t>::max())&&count<=kMaxAccounts);
    Check(payload.size()==kHeader+count*kEntry&&(result.generated||count==0));
    result.accounts.reserve(static_cast<size_t>(count));
    for(size_t i=0;i<count;++i){
        Entry entry{};entry.account=static_cast<uint32_t>(Get(payload,pos,4));entry.network=static_cast<uint8_t>(Get(payload,pos,1));
        std::copy_n(payload.data()+pos,entry.genesis.size(),entry.genesis.begin());pos+=entry.genesis.size();
        entry.branch=static_cast<uint32_t>(Get(payload,pos,4));entry.activation=static_cast<uint32_t>(Get(payload,pos,4));
        Check(entry.account<0x80000000U&&entry.network<=2&&entry.branch&&entry.activation&&entry.activation!=UINT32_MAX);
        Check(std::any_of(entry.genesis.begin(),entry.genesis.end(),[](uint8_t b){return b!=0;}));
        Check(result.accounts.empty()||result.accounts.back().account<entry.account);result.accounts.push_back(entry);
    }
    Check(pos==payload.size());return result;
}
void OrchardAccountCatalog::InitializeForInitialSeed(sqlite3* db,std::span<const uint8_t> seed,bool generated){
    Check(db&&!sqlite3_get_autocommit(db)&&seed.size()==64);RequireFull(db);
    Statement empty(db,"SELECT 1 FROM hd_seeds");empty.Done();
    Statement absent(db,"SELECT 1 FROM settings WHERE key=?");absent.Text(1,kSetting);absent.Done();
    const auto id=Identity(db);std::vector<uint8_t> bytes(kMagic.begin(),kMagic.end());bytes.insert(bytes.end(),id.begin(),id.end());
    bytes.push_back(generated?1:0);Put(bytes,1,8);Put(bytes,0,4);const auto tag=Tag(seed,bytes);bytes.insert(bytes.end(),tag.begin(),tag.end());
    Statement insert(db,"INSERT INTO settings(key,value,updated_at) VALUES(?,?,strftime('%s','now'))");
    insert.Text(1,kSetting);insert.Text(2,Encode(bytes));insert.Done();Check(sqlite3_changes(db)==1);
}
uint64_t OrchardAccountCatalog::StageAppend(sqlite3* db,std::span<const uint8_t> seed,uint64_t expected,const Entry& entry){
    Check(db&&!sqlite3_get_autocommit(db)&&seed.size()==64);RequireFull(db);
    const auto current=Read(db,seed);Check(current&&current->generated&&current->revision==expected);
    Check(expected<uint64_t(std::numeric_limits<int64_t>::max())&&current->accounts.size()<kMaxAccounts);
    Check(entry.account<0x80000000U&&entry.network<=2&&entry.branch&&entry.activation&&entry.activation!=UINT32_MAX);
    Check(std::any_of(entry.genesis.begin(),entry.genesis.end(),[](uint8_t b){return b!=0;}));
    auto entries=current->accounts;
    auto where=std::lower_bound(entries.begin(),entries.end(),entry.account,[](const Entry& e,uint32_t n){return e.account<n;});
    Check(where==entries.end()||where->account!=entry.account);entries.insert(where,entry);
    const auto id=Identity(db);
    const auto seal=[&](const std::vector<Entry>& records,uint64_t revision){
        std::vector<uint8_t> bytes(kMagic.begin(),kMagic.end());bytes.insert(bytes.end(),id.begin(),id.end());
        bytes.push_back(1);Put(bytes,revision,8);Put(bytes,records.size(),4);
        for(const auto& e:records){Put(bytes,e.account,4);bytes.push_back(e.network);bytes.insert(bytes.end(),e.genesis.begin(),e.genesis.end());Put(bytes,e.branch,4);Put(bytes,e.activation,4);}
        const auto tag=Tag(seed,bytes);bytes.insert(bytes.end(),tag.begin(),tag.end());return Encode(bytes);
    };
    const auto before=seal(current->accounts,expected),after=seal(entries,expected+1);
    Statement write(db,"UPDATE settings SET value=?,updated_at=strftime('%s','now') WHERE key=? AND value=?");
    write.Text(1,after);write.Text(2,kSetting);write.Text(3,before);write.Done();Check(sqlite3_changes(db)==1);
    return expected+1;
}
} // namespace dinero::wallet
