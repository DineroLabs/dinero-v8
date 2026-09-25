#include "orchard_wallet_storage.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <algorithm>
#include <climits>
#include <limits>
#include <string_view>
#include <utility>
namespace dinero::orchard {
namespace {
[[noreturn]] void Fail(){throw std::runtime_error("Orchard wallet storage integrity, transaction or I/O failure");}
void Check(bool condition){if(!condition)Fail();}
struct Statement {
    sqlite3_stmt* p=nullptr;
    Statement(sqlite3* db,const char* sql){if(sqlite3_prepare_v2(db,sql,-1,&p,nullptr)!=SQLITE_OK){sqlite3_finalize(p);p=nullptr;Fail();}}
    ~Statement(){sqlite3_finalize(p);}
    Statement(const Statement&)=delete;
    void Blob(int index,std::span<const uint8_t> bytes){Check(bytes.size()<=size_t(INT_MAX));Check(sqlite3_bind_blob(p,index,bytes.data(),int(bytes.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
    void Int(int index,uint64_t value){Check(value<=uint64_t(INT64_MAX));Check(sqlite3_bind_int64(p,index,sqlite3_int64(value))==SQLITE_OK);}
};
void Exec(sqlite3* db,const char* sql){Check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
void Durable(sqlite3* db,bool writing){
    Check(db&&sqlite3_db_readonly(db,"main")==0);
    if(writing)Check(sqlite3_get_autocommit(db)==0);
    Statement sync(db,"PRAGMA main.synchronous");Check(sqlite3_step(sync.p)==SQLITE_ROW);Check(sqlite3_column_int(sync.p,0)>=2);
    Statement mode(db,"PRAGMA main.journal_mode");Check(sqlite3_step(mode.p)==SQLITE_ROW);
    const auto* name=sqlite3_column_text(mode.p,0);Check(name);
    const std::string_view text(reinterpret_cast<const char*>(name));
    Check(text=="wal"||text=="delete"||text=="truncate"||text=="persist");
}
void Schema(sqlite3* db){
    Statement stmt(db,"SELECT version FROM orchard_wallet_schema WHERE id=1");
    Check(sqlite3_step(stmt.p)==SQLITE_ROW&&sqlite3_column_type(stmt.p,0)==SQLITE_INTEGER&&sqlite3_column_int64(stmt.p,0)==1);
    Check(sqlite3_step(stmt.p)==SQLITE_DONE);
}
void U32(std::vector<uint8_t>& out,uint32_t x){for(unsigned i=0;i<4;++i)out.push_back(uint8_t(x>>(i*8)));}
void U64(std::vector<uint8_t>& out,uint64_t x){for(unsigned i=0;i<8;++i)out.push_back(uint8_t(x>>(i*8)));}
using Cipher=std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)>;
}
WalletStateBytes::WalletStateBytes(std::span<const uint8_t> bytes){Check(bytes.size()<=WalletSnapshotStore::kMaxStateBytes);bytes_.assign(bytes.begin(),bytes.end());}
WalletStateBytes::WalletStateBytes(size_t size){Check(size<=WalletSnapshotStore::kMaxStateBytes);bytes_.resize(size);}
void WalletStateBytes::Wipe()noexcept{if(!bytes_.empty())OPENSSL_cleanse(bytes_.data(),bytes_.size());}
WalletStateBytes::~WalletStateBytes(){Wipe();}
WalletStateBytes::WalletStateBytes(WalletStateBytes&& other)noexcept:bytes_(std::move(other.bytes_)){}
WalletStateBytes& WalletStateBytes::operator=(WalletStateBytes&& other)noexcept{if(this!=&other){Wipe();bytes_=std::move(other.bytes_);}return *this;}
void WalletSnapshotStore::InitializeSchemaUnderTransaction(sqlite3* db){
    Durable(db,true);
    Exec(db,"CREATE TABLE IF NOT EXISTS orchard_wallet_schema(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER NOT NULL);");
    Exec(db,"INSERT OR IGNORE INTO orchard_wallet_schema(id,version) VALUES(1,1);");Schema(db);
    Exec(db,"CREATE TABLE IF NOT EXISTS orchard_wallet_snapshots(wallet_id BLOB NOT NULL CHECK(typeof(wallet_id)='blob' AND length(wallet_id)=32),account INTEGER NOT NULL CHECK(account>=0 AND account<2147483648),revision INTEGER NOT NULL CHECK(revision>0),sealed BLOB NOT NULL CHECK(typeof(sealed)='blob' AND length(sealed)>=29 AND length(sealed)<=16777245),PRIMARY KEY(wallet_id,account)) WITHOUT ROWID;");
}
WalletSnapshotStore::WalletSnapshotStore(sqlite3* db,WalletStorageIdentity identity,std::span<const uint8_t> seed):db_(db),identity_(identity){
    Durable(db,false);Schema(db);
    Check(seed.size()>=32&&seed.size()<=252&&uint8_t(identity.network)<=2&&identity.account<0x80000000);
    Check(std::any_of(identity.genesis.begin(),identity.genesis.end(),[](auto b){return b!=0;}));
    Check(std::any_of(identity.wallet_id.begin(),identity.wallet_id.end(),[](auto b){return b!=0;}));
    constexpr std::string_view salt="DIN/orchard/wallet-store/kdf/v1";
    auto info=AssociatedData(0);
    std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr),EVP_PKEY_CTX_free);Check(bool(ctx));
    size_t size=key_.size();
    // Derive directly into owned key storage; constructor failure scrubs it.
    try{
        Check(EVP_PKEY_derive_init(ctx.get())==1&&EVP_PKEY_CTX_set_hkdf_md(ctx.get(),EVP_sha256())==1);
        Check(EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(),reinterpret_cast<const uint8_t*>(salt.data()),int(salt.size()))==1);
        Check(EVP_PKEY_CTX_set1_hkdf_key(ctx.get(),seed.data(),int(seed.size()))==1);
        Check(EVP_PKEY_CTX_add1_hkdf_info(ctx.get(),info.data(),int(info.size()))==1);
        Check(EVP_PKEY_derive(ctx.get(),key_.data(),&size)==1&&size==key_.size());
    }catch(...){OPENSSL_cleanse(key_.data(),key_.size());throw;}
}
WalletSnapshotStore::~WalletSnapshotStore(){OPENSSL_cleanse(key_.data(),key_.size());}
std::vector<uint8_t> WalletSnapshotStore::AssociatedData(uint64_t revision)const{
    constexpr std::string_view tag="DIN/orchard/wallet-snapshot/v1";
    std::vector<uint8_t> aad(tag.begin(),tag.end());aad.push_back(0);aad.push_back(uint8_t(identity_.network));
    aad.insert(aad.end(),identity_.genesis.begin(),identity_.genesis.end());aad.insert(aad.end(),identity_.wallet_id.begin(),identity_.wallet_id.end());
    U32(aad,identity_.account);U64(aad,revision);return aad;
}
std::vector<uint8_t> WalletSnapshotStore::Seal(uint64_t revision,std::span<const uint8_t> bytes)const{
    Check(bytes.size()<=kMaxStateBytes);
    std::vector<uint8_t> result(1+12+bytes.size()+16);result[0]=1;Check(RAND_bytes(result.data()+1,12)==1);
    Cipher ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);Check(bool(ctx));auto aad=AssociatedData(revision);int length=0,tail=0;
    Check(EVP_EncryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,key_.data(),result.data()+1)==1);
    Check(EVP_EncryptUpdate(ctx.get(),nullptr,&length,aad.data(),int(aad.size()))==1);
    Check(EVP_EncryptUpdate(ctx.get(),result.data()+13,&length,bytes.data(),int(bytes.size()))==1&&size_t(length)==bytes.size());
    Check(EVP_EncryptFinal_ex(ctx.get(),result.data()+13+length,&tail)==1&&tail==0);
    Check(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,result.data()+13+bytes.size())==1);return result;
}
WalletStateBytes WalletSnapshotStore::Open(uint64_t revision,std::span<const uint8_t> bytes)const{
    Check(bytes.size()>=29&&bytes.size()<=kMaxStateBytes+29&&bytes[0]==1);
    WalletStateBytes result(bytes.size()-29);Cipher ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);Check(bool(ctx));auto aad=AssociatedData(revision);int length=0,tail=0;
    Check(EVP_DecryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,key_.data(),bytes.data()+1)==1);
    Check(EVP_DecryptUpdate(ctx.get(),nullptr,&length,aad.data(),int(aad.size()))==1);
    // GCM may emit unauthenticated plaintext here; RAII wipes it on EVERY
    // exception. No reference or bytes escape before tag verification.
    Check(EVP_DecryptUpdate(ctx.get(),result.bytes_.data(),&length,bytes.data()+13,int(result.bytes_.size()))==1&&size_t(length)==result.bytes_.size());
    Check(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,const_cast<uint8_t*>(bytes.data()+bytes.size()-16))==1);
    uint8_t scratch[16]{};const auto status=EVP_DecryptFinal_ex(ctx.get(),scratch,&tail);OPENSSL_cleanse(scratch,sizeof(scratch));Check(status==1&&tail==0);return result;
}
std::optional<LoadedWalletState> WalletSnapshotStore::Read()const{
    Schema(db_);Statement stmt(db_,"SELECT revision,sealed FROM orchard_wallet_snapshots WHERE wallet_id=? AND account=?");stmt.Blob(1,identity_.wallet_id);stmt.Int(2,identity_.account);
    const auto code=sqlite3_step(stmt.p);if(code==SQLITE_DONE)return std::nullopt;Check(code==SQLITE_ROW);
    Check(sqlite3_column_type(stmt.p,0)==SQLITE_INTEGER&&sqlite3_column_int64(stmt.p,0)>0&&sqlite3_column_type(stmt.p,1)==SQLITE_BLOB);
    const auto revision=uint64_t(sqlite3_column_int64(stmt.p,0));const int size=sqlite3_column_bytes(stmt.p,1);Check(size>=29&&size<=int(kMaxStateBytes+29));
    const auto* blob=static_cast<const uint8_t*>(sqlite3_column_blob(stmt.p,1));Check(blob);
    auto state=Open(revision,{blob,size_t(size)});Check(sqlite3_step(stmt.p)==SQLITE_DONE);return LoadedWalletState{revision,std::move(state)};
}
uint64_t WalletSnapshotStore::StageReplace(uint64_t expected,const WalletStateBytes& state){
    Durable(db_,true);Check(expected<uint64_t(INT64_MAX)&&state.Bytes().size()<=kMaxStateBytes);
    // Authenticates the existing state before allowing replacement, including
    // for wrong-key attempts. CAS prevents stale concurrent writer publication.
    const auto previous=Read();Check(previous?previous->revision==expected:expected==0);
    const auto revision=expected+1;const auto sealed=Seal(revision,state.Bytes());
    Statement stmt(db_,expected==0?"INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) VALUES(?,?,?,?)":"UPDATE orchard_wallet_snapshots SET revision=?,sealed=? WHERE wallet_id=? AND account=? AND revision=?");
    if(expected==0){stmt.Blob(1,identity_.wallet_id);stmt.Int(2,identity_.account);stmt.Int(3,revision);stmt.Blob(4,sealed);}
    else{stmt.Int(1,revision);stmt.Blob(2,sealed);stmt.Blob(3,identity_.wallet_id);stmt.Int(4,identity_.account);stmt.Int(5,expected);}
    Check(sqlite3_step(stmt.p)==SQLITE_DONE&&sqlite3_changes(db_)==1);return revision;
}
} // namespace dinero::orchard
