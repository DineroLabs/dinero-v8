#pragma once
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace dinero::wallet::detail {
// Private, disposable replay material. Never a durable wallet owner or a
// validity certificate. No filename/import/reopen API: SQLite owns the temp
// file and removes it when the last view closes. Stored proof material must
// still pass the genuine authorization/state constructors before use.
// The configured pager cache is bounded; this alone is not an RSS guarantee.
class RuntimeReplaySpool {
public:
    using Bytes=std::vector<uint8_t>;
    static constexpr size_t MaximumRecordBytes=32*1024*1024;
    static constexpr size_t MaximumKeyBytes=64;
    static constexpr int PagerCacheKiB=1024;
    explicit RuntimeReplaySpool(const std::filesystem::path& work_directory={},bool cache_statements=false)
        :temporary_(work_directory),cache_statements_(cache_statements) {
        algorithm_.reset(EVP_MAC_fetch(nullptr,"HMAC",nullptr));Check(bool(algorithm_));
        mac_.reset(EVP_MAC_CTX_new(algorithm_.get()));Check(bool(mac_));
        char digest[]="SHA256";OSSL_PARAM parameters[]={OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,digest,0),OSSL_PARAM_construct_end()};
        Check(EVP_MAC_init(mac_.get(),secret_.bytes.data(),secret_.bytes.size(),parameters)==1);
        sqlite3* raw=nullptr;
        const int rc=sqlite3_open_v2(temporary_.database.c_str(),&raw,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|
                                   SQLITE_OPEN_FULLMUTEX|SQLITE_OPEN_PRIVATECACHE,nullptr);
        db_.reset(raw);
        Check(rc==SQLITE_OK);
        Check(!sqlite3_compileoption_used("TEMP_STORE=3"));
        Check(sqlite3_db_config(db_.get(),SQLITE_DBCONFIG_LOOKASIDE,nullptr,0,0)==SQLITE_OK);
        // No mmap or in-memory temp database; dirty pages may spill to the
        // SQLite-owned temporary file as a transaction grows.
        Exec("PRAGMA page_size=4096");
        Exec("PRAGMA cache_size=-1024");
        Exec("PRAGMA mmap_size=0");
        Exec("PRAGMA temp_store=FILE");
        Exec("PRAGMA cache_spill=ON");
        // Named app-workspace files are equally disposable. Never fsync a
        // scratch batch as though it were canonical durable state: interrupted
        // spools are discarded, and every value is MAC-checked before use.
        if(!work_directory.empty())Exec("PRAGMA synchronous=OFF");
        Exec("CREATE TABLE records(k BLOB PRIMARY KEY NOT NULL,v BLOB NOT NULL,tag BLOB NOT NULL) WITHOUT ROWID");
    }
    RuntimeReplaySpool(const RuntimeReplaySpool&)=delete;
    RuntimeReplaySpool& operator=(const RuntimeReplaySpool&)=delete;
    // One batch holds this private database's mutex. Call no external source
    // or service callbacks inside a batch. Internal Get/Insert are recursive.
    class Batch {
    public:
        explicit Batch(RuntimeReplaySpool& owner):owner_(owner),lock_(owner.mutex_) {
            Check(!owner_.frozen_&&sqlite3_get_autocommit(owner_.db_.get()));
            owner_.Exec("BEGIN IMMEDIATE");active_=true;
        }
        ~Batch(){if(active_)sqlite3_exec(owner_.db_.get(),"ROLLBACK",nullptr,nullptr,nullptr);}
        Batch(const Batch&)=delete;Batch& operator=(const Batch&)=delete;
        void Commit(){Check(active_);owner_.Exec("COMMIT");active_=false;}
    private:
        RuntimeReplaySpool& owner_;std::unique_lock<std::recursive_mutex> lock_;bool active_=false;
    };
    void Insert(std::span<const uint8_t> key,std::span<const uint8_t> value) {
        std::lock_guard guard(mutex_);Check(!frozen_&&!sqlite3_get_autocommit(db_.get()));
        Check(!key.empty()&&key.size()<=MaximumKeyBytes&&value.size()<=MaximumRecordBytes);
        const auto tag=Tag(key,value);
        Query stmt(db_.get(),insert_statement_,"INSERT INTO records(k,v,tag) VALUES(?1,?2,?3)",cache_statements_);
        stmt.Blob(1,key);stmt.Blob(2,value);stmt.Blob(3,tag);Check(sqlite3_step(stmt.get())==SQLITE_DONE);
        Check(sqlite3_changes(db_.get())==1);
    }
    std::optional<Bytes> Get(std::span<const uint8_t> key) const {
        std::lock_guard guard(mutex_);Check(!key.empty()&&key.size()<=MaximumKeyBytes);
        Query stmt(db_.get(),read_statement_,"SELECT v,tag FROM records WHERE k=?1",cache_statements_);stmt.Blob(1,key);
        const int rc=sqlite3_step(stmt.get());if(rc==SQLITE_DONE)return std::nullopt;
        Check(rc==SQLITE_ROW);auto result=Value(stmt.get(),0,key);
        Check(sqlite3_step(stmt.get())==SQLITE_DONE);return result;
    }
    // Indexed single-row walk for a namespace. No statement/cursor escapes
    // the method and no caller callback executes under this private mutex.
    std::optional<std::pair<Bytes,Bytes>> Next(uint8_t space,std::span<const uint8_t> after) const {
        std::lock_guard guard(mutex_);Check(space<255&&!after.empty()&&after[0]==space&&after.size()<=MaximumKeyBytes);
        Query stmt(db_.get(),next_statement_,"SELECT k,v,tag FROM records WHERE k>?1 AND k<?2 ORDER BY k LIMIT 1",cache_statements_);
        stmt.Blob(1,after);const Bytes upper{uint8_t(space+1)};stmt.Blob(2,upper);
        const int rc=sqlite3_step(stmt.get());if(rc==SQLITE_DONE)return std::nullopt;
        Check(rc==SQLITE_ROW&&sqlite3_column_type(stmt.get(),0)==SQLITE_BLOB);
        const int size=sqlite3_column_bytes(stmt.get(),0);Check(size>0&&size_t(size)<=MaximumKeyBytes);
        const auto* raw=static_cast<const uint8_t*>(sqlite3_column_blob(stmt.get(),0));Check(raw);
        Bytes key(raw,raw+size);Check(key[0]==space);auto value=Value(stmt.get(),1,key);
        Check(sqlite3_step(stmt.get())==SQLITE_DONE);return std::pair{std::move(key),std::move(value)};
    }
    void Freeze() {
        std::lock_guard guard(mutex_);Check(!frozen_&&sqlite3_get_autocommit(db_.get()));
        Exec("PRAGMA query_only=ON");frozen_=true;
    }
    struct Usage {int pager_bytes=0,schema_bytes=0,statement_bytes=0;};
    Usage UsageNow() const {
        std::lock_guard guard(mutex_);Usage result;int high=0;
        Check(sqlite3_db_status(db_.get(),SQLITE_DBSTATUS_CACHE_USED,&result.pager_bytes,&high,0)==SQLITE_OK);
        Check(sqlite3_db_status(db_.get(),SQLITE_DBSTATUS_SCHEMA_USED,&result.schema_bytes,&high,0)==SQLITE_OK);
        Check(sqlite3_db_status(db_.get(),SQLITE_DBSTATUS_STMT_USED,&result.statement_bytes,&high,0)==SQLITE_OK);
        return result;
    }
private:
    friend struct RuntimeReplaySpoolTestAccess;
    // App-owned workspaces avoid Android's unwritable system /tmp. Only this
    // exclusively created child belongs to the spool, never its caller path.
    struct Temporary {
        std::filesystem::path directory;std::string database;
        explicit Temporary(const std::filesystem::path& parent) {
            if(parent.empty())return;
            if(!std::filesystem::is_directory(parent))throw std::runtime_error("Spool workspace unavailable");
            std::array<uint8_t,16> nonce{};
            if(RAND_bytes(nonce.data(),int(nonce.size()))!=1)throw std::runtime_error("Spool nonce unavailable");
            std::string name="dno-spool-";
            for(auto b:nonce){name.push_back("0123456789abcdef"[b>>4]);name.push_back("0123456789abcdef"[b&15]);}
            const auto candidate=parent/name;
#ifdef _WIN32
            if(!std::filesystem::create_directory(candidate))throw std::runtime_error("Spool workspace collision");
#else
            if(::mkdir(candidate.c_str(),0700)!=0)throw std::runtime_error("Spool workspace creation failed");
#endif
            directory=candidate;database=(directory/"records.sqlite").string();
        }
        ~Temporary() {
            if(directory.empty())return;
            std::error_code ignored;
            for(const auto* name:{"records.sqlite","records.sqlite-journal","records.sqlite-wal","records.sqlite-shm"})
                std::filesystem::remove(directory/name,ignored);
            std::filesystem::remove(directory,ignored);
        }
    } temporary_;
    struct Close {void operator()(sqlite3* db)const noexcept {if(db)sqlite3_close_v2(db);}};
    struct Secret {
        std::array<uint8_t,32> bytes{};
        Secret(){if(RAND_bytes(bytes.data(),int(bytes.size()))!=1){OPENSSL_cleanse(bytes.data(),bytes.size());throw std::runtime_error("Replay spool integrity key unavailable");}}
        ~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}
        Secret(const Secret&)=delete;Secret& operator=(const Secret&)=delete;
    } secret_;
    // This process-local MAC detects spool alteration and binds the key and
    // value. It cannot confer consensus/proof validity or support reopening.
    std::array<uint8_t,32> Tag(std::span<const uint8_t> key,std::span<const uint8_t> value) const {
        const uint8_t length=uint8_t(key.size());std::array<uint8_t,32> result{};size_t size=0;
        // Every caller holds mutex_; keyed provider state is private to this
        // spool and is reset for each independent record MAC.
        Check(EVP_MAC_init(mac_.get(),nullptr,0,nullptr)==1);
        Check(EVP_MAC_update(mac_.get(),&length,1)==1&&EVP_MAC_update(mac_.get(),key.data(),key.size())==1);
        if(!value.empty())Check(EVP_MAC_update(mac_.get(),value.data(),value.size())==1);
        Check(EVP_MAC_final(mac_.get(),result.data(),&size,result.size())==1&&size==result.size());return result;
    }
    struct FreeMac {void operator()(EVP_MAC* p)const noexcept{EVP_MAC_free(p);}};
    struct FreeContext {void operator()(EVP_MAC_CTX* p)const noexcept{EVP_MAC_CTX_free(p);}};
    std::unique_ptr<EVP_MAC,FreeMac> algorithm_;
    mutable std::unique_ptr<EVP_MAC_CTX,FreeContext> mac_;
    Bytes Value(sqlite3_stmt* stmt,int column,std::span<const uint8_t> key) const {
        Check(sqlite3_column_type(stmt,column)==SQLITE_BLOB&&sqlite3_column_type(stmt,column+1)==SQLITE_BLOB);
        const int size=sqlite3_column_bytes(stmt,column);Check(size>=0&&size_t(size)<=MaximumRecordBytes);
        const auto* data=static_cast<const uint8_t*>(sqlite3_column_blob(stmt,column));Check(size==0||data);
        Check(sqlite3_column_bytes(stmt,column+1)==32);
        const auto* stored=sqlite3_column_blob(stmt,column+1);Check(stored);
        Bytes result;if(size)result.assign(data,data+size);const auto expected=Tag(key,result);
        Check(CRYPTO_memcmp(stored,expected.data(),expected.size())==0);return result;
    }
    std::unique_ptr<sqlite3,Close> db_;
    mutable std::recursive_mutex mutex_;
    bool frozen_=false;
    const bool cache_statements_;
    static void Check(bool ok){if(!ok)throw std::runtime_error("Runtime replay spool unavailable or inconsistent");}
    class Statement {
    public:
        Statement(sqlite3* db,const char* sql){sqlite3_stmt* raw=nullptr;const int rc=sqlite3_prepare_v2(db,sql,-1,&raw,nullptr);stmt_.reset(raw);Check(rc==SQLITE_OK);}
        sqlite3_stmt* get()const{return stmt_.get();}
        void Reset()noexcept{sqlite3_reset(get());sqlite3_clear_bindings(get());}
        void Blob(int index,std::span<const uint8_t> bytes) {
            static constexpr uint8_t empty=0;
            Check(bytes.size()<=size_t(std::numeric_limits<int>::max()));
            Check(sqlite3_bind_blob(get(),index,bytes.empty()?&empty:bytes.data(),int(bytes.size()),SQLITE_TRANSIENT)==SQLITE_OK);
        }
    private:
        struct Finalize {void operator()(sqlite3_stmt* s)const noexcept{if(s)sqlite3_finalize(s);}};
        std::unique_ptr<sqlite3_stmt,Finalize> stmt_;
    };
    // Optional fixed three-statement cache. Every lease resets bindings/rows
    // while the spool mutex is still held; no cursor or borrowed blob escapes.
    class Query {
    public:
        Query(sqlite3* db,std::unique_ptr<Statement>& cache,const char* sql,bool cached) {
            if(cached){if(!cache)cache=std::make_unique<Statement>(db,sql);statement_=cache.get();}
            else {owned_=std::make_unique<Statement>(db,sql);statement_=owned_.get();}
        }
        ~Query(){statement_->Reset();}
        sqlite3_stmt* get()const{return statement_->get();}
        void Blob(int index,std::span<const uint8_t> bytes){statement_->Blob(index,bytes);}
    private:
        std::unique_ptr<Statement> owned_;Statement* statement_=nullptr;
    };
    mutable std::unique_ptr<Statement> insert_statement_,read_statement_,next_statement_;
    void Exec(const char* sql){Check(sqlite3_exec(db_.get(),sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
};
} // namespace dinero::wallet::detail
