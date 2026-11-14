#include "wallet/wallet_index_v3_schema.h"

#include "crypto/secure_random.h"
#include "crypto/sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero {
namespace {
[[noreturn]] void Refuse(const char* why) { throw std::runtime_error(why); }

struct Database {
    sqlite3* db=nullptr;
    Database(const std::filesystem::path& path,int flags) {
        // sqlite3_open_v2 expects UTF-8 even when the native Windows path is
        // UTF-16. Canonicalize parent symlinks (macOS /var -> /private/var),
        // then refuse a symlink at the database filename itself.
        if(std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
            Refuse("Wallet index v3 database link refused");
        const auto parent=path.parent_path().empty()?std::filesystem::path("."):path.parent_path();
        const auto utf8=(std::filesystem::canonical(parent)/path.filename()).u8string();
        const std::string filename(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        if(sqlite3_open_v2(filename.c_str(),&db,flags|SQLITE_OPEN_NOFOLLOW,nullptr)!=SQLITE_OK) {
            if(db)sqlite3_close(db);
            db=nullptr;Refuse("Wallet index v3 database open failed");
        }
        sqlite3_busy_timeout(db,10000);
    }
    ~Database(){if(db)sqlite3_close(db);}
    Database(const Database&)=delete;
    Database& operator=(const Database&)=delete;
};
struct Statement {
    sqlite3_stmt* q=nullptr;
    Statement(sqlite3* db,const char* sql) {
        if(sqlite3_prepare_v2(db,sql,-1,&q,nullptr)!=SQLITE_OK) {
            sqlite3_finalize(q);q=nullptr;Refuse("Wallet index v3 query prepare failed");
        }
    }
    ~Statement(){sqlite3_finalize(q);}
};
void Exec(sqlite3* db,const char* sql) {
    if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)
        Refuse("Wallet index v3 transaction failed");
}
int64_t Integer(sqlite3* db,const char* sql) {
    Statement q(db,sql);
    if(sqlite3_step(q.q)!=SQLITE_ROW||sqlite3_column_type(q.q,0)!=SQLITE_INTEGER)
        Refuse("Wallet index v3 integer query failed");
    const auto result=sqlite3_column_int64(q.q,0);
    if(sqlite3_step(q.q)!=SQLITE_DONE)Refuse("Wallet index v3 integer query incomplete");
    return result;
}
void QuickCheck(sqlite3* db) {
    Statement q(db,"PRAGMA quick_check");
    if(sqlite3_step(q.q)!=SQLITE_ROW||sqlite3_column_type(q.q,0)!=SQLITE_TEXT ||
       sqlite3_column_bytes(q.q,0)!=2 ||
       std::memcmp(sqlite3_column_text(q.q,0),"ok",2)!=0 ||
       sqlite3_step(q.q)!=SQLITE_DONE)Refuse("Wallet index v3 SQLite integrity check failed");
}
void RequireIndependentBackup(const std::filesystem::path& source,
                              const std::filesystem::path& backup) {
    std::error_code error;
    const auto status=std::filesystem::symlink_status(backup,error);
    if(error==std::errc::no_such_file_or_directory ||
       (!error && status.type()==std::filesystem::file_type::not_found))
        Refuse("Wallet index v3 rollback backup missing");
    if(error || std::filesystem::is_symlink(status))
        Refuse("Wallet index v3 rollback backup link refused");
    if(!std::filesystem::is_regular_file(status))
        Refuse("Wallet index v3 rollback backup missing");
    const bool same=std::filesystem::equivalent(source,backup,error);
    if(error || same)
        Refuse("Wallet index v3 rollback backup is not independent from source");
    const auto links=std::filesystem::hard_link_count(backup,error);
    if(error || links!=1)
        Refuse("Wallet index v3 rollback backup is hardlinked outside this index");
}
constexpr const char* v2_body=R"((
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    value INTEGER NOT NULL,
    spk BLOB NOT NULL,
    path TEXT NOT NULL,
    height INTEGER NOT NULL,
    spend_height INTEGER,
    is_coinbase INTEGER NOT NULL DEFAULT 0,
    utreexo_position INTEGER,
    is_confidential INTEGER DEFAULT 0,
    commitment BLOB,
    range_proof BLOB,
    blinding_factor BLOB,
    nonce BLOB,
    owner_kind INTEGER NOT NULL DEFAULT 0 CHECK(typeof(owner_kind)='integer' AND owner_kind IN(0,1)),
    owner_reference TEXT NOT NULL DEFAULT '',
    CHECK(typeof(path)='text' AND typeof(owner_reference)='text' AND instr(path,char(0))=0 AND instr(owner_reference,char(0))=0 AND
        ((owner_kind=0 AND length(path)>=1 AND owner_reference='') OR
         (owner_kind=1 AND path='' AND length(owner_reference) BETWEEN 1 AND 128 AND is_confidential IS 0))),
    PRIMARY KEY(txid,vout)
))";
std::string V3Body() {
    std::string body=v2_body;
    body.insert(1,"\n    wallet_identity BLOB NOT NULL CHECK(typeof(wallet_identity)='blob' AND length(wallet_identity)=32 AND wallet_identity!=zeroblob(32)),");
    const auto key=body.find("PRIMARY KEY(txid,vout)");
    if(key==std::string::npos)Refuse("Wallet index v3 schema template changed");
    body.replace(key,std::strlen("PRIMARY KEY(txid,vout)"),"PRIMARY KEY(wallet_identity,txid,vout)");
    return body;
}
constexpr const char* receipt_body=R"((
            wallet_identity BLOB PRIMARY KEY NOT NULL CHECK(typeof(wallet_identity)='blob' AND length(wallet_identity)=32 AND wallet_identity!=zeroblob(32)),
            receipt BLOB,
            invalidated INTEGER NOT NULL DEFAULT 0 CHECK(invalidated IN(0,1))))";
struct GuardDefinition {
    const char* name;
    const char* type;
    const char* table;
    const char* sql;
};
constexpr std::array<GuardDefinition,6> v3_guards{{
    {"idx_wallet_utxos_v3_unspent","index","wallet_utxos",
     "CREATE INDEX idx_wallet_utxos_v3_unspent ON wallet_utxos(wallet_identity,spend_height) WHERE spend_height IS NULL"},
    {"idx_wallet_utxos_v3_height","index","wallet_utxos",
     "CREATE INDEX idx_wallet_utxos_v3_height ON wallet_utxos(wallet_identity,height)"},
    {"idx_wallet_utxos_v3_spent","index","wallet_utxos",
     "CREATE INDEX idx_wallet_utxos_v3_spent ON wallet_utxos(wallet_identity,spend_height) WHERE spend_height IS NOT NULL"},
    {"runtime_delivery_v3_insert","trigger","wallet_utxos",
     "CREATE TRIGGER runtime_delivery_v3_insert AFTER INSERT ON wallet_utxos BEGIN\n"
     "            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=NEW.wallet_identity;\n"
     "        END"},
    {"runtime_delivery_v3_update","trigger","wallet_utxos",
     "CREATE TRIGGER runtime_delivery_v3_update AFTER UPDATE ON wallet_utxos BEGIN\n"
     "            SELECT CASE WHEN NEW.wallet_identity IS NOT OLD.wallet_identity THEN RAISE(ABORT,'wallet identity change') END;\n"
     "            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=NEW.wallet_identity;\n"
     "        END"},
    {"runtime_delivery_v3_delete","trigger","wallet_utxos",
     "CREATE TRIGGER runtime_delivery_v3_delete AFTER DELETE ON wallet_utxos BEGIN\n"
     "            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=OLD.wallet_identity;\n"
     "        END"},
}};
void CheckTable(sqlite3* db,const char* name,const std::string& body) {
    Statement q(db,"SELECT sql FROM sqlite_schema WHERE type='table' AND name=?");
    if(sqlite3_bind_text(q.q,1,name,-1,SQLITE_STATIC)!=SQLITE_OK||sqlite3_step(q.q)!=SQLITE_ROW||
       sqlite3_column_type(q.q,0)!=SQLITE_TEXT)Refuse("Wallet index v3 table absent");
    const auto* raw=reinterpret_cast<const char*>(sqlite3_column_text(q.q,0));
    const std::string sql(raw,sqlite3_column_bytes(q.q,0));
    const auto begin=sql.find('(');
    if(begin==std::string::npos||sql.substr(begin)!=body||sqlite3_step(q.q)!=SQLITE_DONE)
        Refuse("Wallet index v3 table definition changed");
}
void CheckV3Guards(sqlite3* db) {
    CheckTable(db,"wallet_index_receipts_v3",receipt_body);
    if(Integer(db,"SELECT count(*) FROM sqlite_schema WHERE tbl_name IN ('wallet_utxos','wallet_index_receipts_v3') "
                  "AND type IN ('index','trigger') AND sql IS NOT NULL")!=int64_t(v3_guards.size()))
        Refuse("Wallet index v3 guard inventory changed");
    Statement q(db,"SELECT type,tbl_name,sql FROM sqlite_schema WHERE name=?");
    for(const auto& guard:v3_guards) {
        sqlite3_reset(q.q);sqlite3_clear_bindings(q.q);
        if(sqlite3_bind_text(q.q,1,guard.name,-1,SQLITE_STATIC)!=SQLITE_OK||sqlite3_step(q.q)!=SQLITE_ROW)
            Refuse("Wallet index v3 guard absent");
        for(const auto [column,expected]:{std::pair{0,guard.type},std::pair{1,guard.table},std::pair{2,guard.sql}}) {
            if(sqlite3_column_type(q.q,column)!=SQLITE_TEXT ||
               std::string(reinterpret_cast<const char*>(sqlite3_column_text(q.q,column)))!=expected)
                Refuse("Wallet index v3 guard changed");
        }
        if(sqlite3_step(q.q)!=SQLITE_DONE)Refuse("Wallet index v3 guard duplicated");
    }
}
std::vector<std::pair<std::string,std::string>> LegacyGuards(sqlite3* db,const char* table) {
    const std::string sql="SELECT type,name FROM sqlite_schema WHERE tbl_name='"+std::string(table)+
        "' AND type IN ('index','trigger') AND sql IS NOT NULL ORDER BY type,name";
    Statement q(db,sql.c_str());std::vector<std::pair<std::string,std::string>> result;int rc;
    while((rc=sqlite3_step(q.q))==SQLITE_ROW) {
        if(sqlite3_column_type(q.q,0)!=SQLITE_TEXT||sqlite3_column_type(q.q,1)!=SQLITE_TEXT)
            Refuse("Wallet index v3 legacy guard malformed");
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(q.q,0)),
                            reinterpret_cast<const char*>(sqlite3_column_text(q.q,1)));
    }
    if(rc!=SQLITE_DONE)Refuse("Wallet index v3 legacy guard inventory incomplete");
    return result;
}
void WriteNumber(crypto::CSHA256& hash,uint64_t value) {
    std::array<uint8_t,8> bytes{};
    for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(value>>(8*i));
    hash.Write(bytes.data(),bytes.size());
}
std::array<uint8_t,32> DigestQuery(sqlite3* db,const char* sql) {
    Statement q(db,sql);crypto::CSHA256 hash;
    const int columns=sqlite3_column_count(q.q);
    WriteNumber(hash,uint64_t(columns));int rc;
    while((rc=sqlite3_step(q.q))==SQLITE_ROW) {
        const uint8_t row=0x72;hash.Write(&row,1);
        for(int n=0;n<columns;++n) {
            const int type=sqlite3_column_type(q.q,n);
            const uint8_t tag=uint8_t(type);hash.Write(&tag,1);
            if(type==SQLITE_NULL){WriteNumber(hash,0);continue;}
            if(type==SQLITE_INTEGER){WriteNumber(hash,8);WriteNumber(hash,uint64_t(sqlite3_column_int64(q.q,n)));continue;}
            if(type==SQLITE_FLOAT){
                const double value=sqlite3_column_double(q.q,n);uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
                WriteNumber(hash,8);WriteNumber(hash,bits);continue;
            }
            if(type!=SQLITE_TEXT&&type!=SQLITE_BLOB)Refuse("Wallet index v3 value type invalid");
            const int size=sqlite3_column_bytes(q.q,n);
            const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.q,n));
            if(size<0||(size>0&&!bytes))Refuse("Wallet index v3 value malformed");
            WriteNumber(hash,uint64_t(size));if(size)hash.Write(bytes,size);
        }
    }
    if(rc!=SQLITE_DONE)Refuse("Wallet index v3 inventory incomplete");
    std::array<uint8_t,32> digest{};hash.Finalize(digest.data());return digest;
}
std::array<uint8_t,32> LegacyGuardDigest(sqlite3* db,const char* table) {
    const std::string sql="SELECT tbl_name,type,name,sql FROM sqlite_schema WHERE tbl_name IN ('"+
        std::string(table)+"','utxo_metadata') AND type IN ('index','trigger') "
        "AND sql IS NOT NULL ORDER BY tbl_name,type,name";
    return DigestQuery(db,sql.c_str());
}
struct Inventory {uint64_t rows=0;std::array<uint8_t,32> coins{},metadata{},guards{};};
std::string Hex(const std::array<uint8_t,32>& bytes) {
    static constexpr char alphabet[]="0123456789abcdef";
    std::string result;result.reserve(64);
    for(const auto value:bytes){result.push_back(alphabet[value>>4]);result.push_back(alphabet[value&15]);}
    return result;
}
std::string BackupDigest(const Inventory& inventory) {
    crypto::CSHA256 hash;
    hash.Write(inventory.coins.data(),inventory.coins.size());
    hash.Write(inventory.metadata.data(),inventory.metadata.size());
    hash.Write(inventory.guards.data(),inventory.guards.size());
    WriteNumber(hash,inventory.rows);
    std::array<uint8_t,32> digest{};hash.Finalize(digest.data());return Hex(digest);
}
std::filesystem::path BackupPath(const std::filesystem::path& index_file,const std::string& digest) {
    if(digest.size()!=64 || !std::all_of(digest.begin(),digest.end(),[](const char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
        Refuse("Wallet index v3 rollback backup marker malformed");
    auto path=index_file;path+=".v2-pre-v3-"+digest+".sqlite";return path;
}
std::string ReadBackupMarker(sqlite3* db) {
    Statement q(db,"SELECT value FROM utxo_metadata WHERE key='wallet_index_v3:rollback_backup_digest'");
    if(sqlite3_step(q.q)!=SQLITE_ROW||sqlite3_column_type(q.q,0)!=SQLITE_TEXT)
        Refuse("Wallet index v3 rollback backup marker missing");
    const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(q.q,0));
    const std::string digest(text,sqlite3_column_bytes(q.q,0));
    if(sqlite3_step(q.q)!=SQLITE_DONE)Refuse("Wallet index v3 rollback backup marker duplicated");
    (void)BackupPath({},digest);return digest;
}
void WriteBackupMarker(sqlite3* db,const std::string& digest) {
    Statement q(db,"INSERT INTO utxo_metadata(key,value) VALUES('wallet_index_v3:rollback_backup_digest',?)");
    if(sqlite3_bind_text(q.q,1,digest.c_str(),int(digest.size()),SQLITE_TRANSIENT)!=SQLITE_OK||
       sqlite3_step(q.q)!=SQLITE_DONE)
        Refuse("Wallet index v3 rollback backup marker write failed");
}
std::string ReadLegacyGuardMarker(sqlite3* db) {
    Statement q(db,"SELECT value FROM utxo_metadata WHERE key='wallet_index_v3:legacy_guard_digest'");
    if(sqlite3_step(q.q)!=SQLITE_ROW||sqlite3_column_type(q.q,0)!=SQLITE_TEXT)
        Refuse("Wallet index v3 legacy guard marker missing");
    const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(q.q,0));
    const std::string digest(text,sqlite3_column_bytes(q.q,0));
    if(sqlite3_step(q.q)!=SQLITE_DONE)Refuse("Wallet index v3 legacy guard marker duplicated");
    (void)BackupPath({},digest);return digest;
}
void WriteLegacyGuardMarker(sqlite3* db,const std::array<uint8_t,32>& digest) {
    const auto value=Hex(digest);
    Statement q(db,"INSERT INTO utxo_metadata(key,value) VALUES('wallet_index_v3:legacy_guard_digest',?)");
    if(sqlite3_bind_text(q.q,1,value.c_str(),int(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK||
       sqlite3_step(q.q)!=SQLITE_DONE)
        Refuse("Wallet index v3 legacy guard marker write failed");
}
Inventory InspectV2(sqlite3* db,const char* table) {
    if(Integer(db,"PRAGMA user_version")!=2)Refuse("Wallet index v3 requires version 2 source");
    CheckTable(db,table,v2_body);QuickCheck(db);
    if(Integer(db,"SELECT count(*) FROM utxo_metadata WHERE key LIKE 'wallet_index_v3:%'")!=0)
        Refuse("Wallet index v3 rollback marker exists in version 2 source");
    Inventory result;
    const std::string count="SELECT count(*) FROM "+std::string(table);
    const std::string rows="SELECT * FROM "+std::string(table)+" ORDER BY txid,vout";
    const auto n=Integer(db,count.c_str());if(n<0)Refuse("Wallet index v3 row count invalid");
    result.rows=uint64_t(n);result.coins=DigestQuery(db,rows.c_str());
    result.metadata=DigestQuery(db,"SELECT key,value FROM utxo_metadata ORDER BY key");
    result.guards=LegacyGuardDigest(db,table);
    return result;
}
void SyncAndPublish(const std::filesystem::path& temporary,const std::filesystem::path& published) {
#ifdef _WIN32
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)Refuse("Wallet index v3 backup file unavailable");
    const bool synced=FlushFileBuffers(file);CloseHandle(file);
    if(!synced||!MoveFileExW(temporary.c_str(),published.c_str(),MOVEFILE_WRITE_THROUGH))
        Refuse("Wallet index v3 backup publish failed");
#else
    const int file=open(temporary.c_str(),O_RDONLY);
    if(file<0)Refuse("Wallet index v3 backup file unavailable");
    const int synced=fsync(file),closed=close(file);
    if(synced!=0||closed!=0)Refuse("Wallet index v3 backup sync failed");
    // A preflight exists() check cannot protect the rollback artifact from a
    // concurrent publisher. link() is atomic and refuses an existing name.
    if(link(temporary.c_str(),published.c_str())!=0)
        Refuse("Wallet index v3 backup already exists or cannot be published");
    if(unlink(temporary.c_str())!=0)Refuse("Wallet index v3 staging backup cleanup failed");
    const auto parent=published.parent_path().empty()?std::filesystem::path("."):published.parent_path();
    const int directory=open(parent.c_str(),O_RDONLY);
    if(directory<0)Refuse("Wallet index v3 backup directory unavailable");
    const int dir_synced=fsync(directory),dir_closed=close(directory);
    if(dir_synced!=0||dir_closed!=0)Refuse("Wallet index v3 backup directory sync failed");
#endif
}
void MakePrivateEmptyFile(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)Refuse("Wallet index v3 temporary backup exists");CloseHandle(file);
#else
    const int file=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(file<0)Refuse("Wallet index v3 temporary backup exists");if(close(file)!=0)Refuse("Wallet index v3 temporary backup close failed");
#endif
}
void Backup(sqlite3* source,const std::filesystem::path& temporary) {
    MakePrivateEmptyFile(temporary);
    Database destination(temporary,SQLITE_OPEN_READWRITE);
    Exec(destination.db,"PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL");
    sqlite3_backup* backup=sqlite3_backup_init(destination.db,"main",source,"main");
    if(!backup)Refuse("Wallet index v3 backup start failed");
    const int copied=sqlite3_backup_step(backup,-1);
    const int finished=sqlite3_backup_finish(backup);
    if(copied!=SQLITE_DONE||finished!=SQLITE_OK)Refuse("Wallet index v3 backup copy failed");
}
void CreateV3(sqlite3* db) {
    const auto body=V3Body();
    Exec(db,"ALTER TABLE wallet_utxos RENAME TO wallet_utxos_legacy_v2");
    Exec(db,("CREATE TABLE wallet_utxos "+body).c_str());
    Exec(db,R"(
        CREATE TABLE wallet_index_receipts_v3(
            wallet_identity BLOB PRIMARY KEY NOT NULL CHECK(typeof(wallet_identity)='blob' AND length(wallet_identity)=32 AND wallet_identity!=zeroblob(32)),
            receipt BLOB,
            invalidated INTEGER NOT NULL DEFAULT 0 CHECK(invalidated IN(0,1)));
        CREATE INDEX idx_wallet_utxos_v3_unspent ON wallet_utxos(wallet_identity,spend_height) WHERE spend_height IS NULL;
        CREATE INDEX idx_wallet_utxos_v3_height ON wallet_utxos(wallet_identity,height);
        CREATE INDEX idx_wallet_utxos_v3_spent ON wallet_utxos(wallet_identity,spend_height) WHERE spend_height IS NOT NULL;
        CREATE TRIGGER runtime_delivery_v3_insert AFTER INSERT ON wallet_utxos BEGIN
            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=NEW.wallet_identity;
        END;
        CREATE TRIGGER runtime_delivery_v3_update AFTER UPDATE ON wallet_utxos BEGIN
            SELECT CASE WHEN NEW.wallet_identity IS NOT OLD.wallet_identity THEN RAISE(ABORT,'wallet identity change') END;
            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=NEW.wallet_identity;
        END;
        CREATE TRIGGER runtime_delivery_v3_delete AFTER DELETE ON wallet_utxos BEGIN
            UPDATE wallet_index_receipts_v3 SET receipt=NULL,invalidated=1 WHERE wallet_identity=OLD.wallet_identity;
        END;
        PRAGMA user_version=3;
    )");
}
WalletIndexV3SchemaResult InspectV3(sqlite3* db,const std::filesystem::path& index_file) {
    if(Integer(db,"PRAGMA user_version")!=3)Refuse("Wallet index v3 source version unavailable");
    const auto marker=ReadBackupMarker(db);
    const auto published=BackupPath(index_file,marker);
    RequireIndependentBackup(index_file,published);
    CheckTable(db,"wallet_utxos_legacy_v2",v2_body);
    CheckTable(db,"wallet_utxos",V3Body());CheckV3Guards(db);QuickCheck(db);
    const auto legacy_guard_marker=ReadLegacyGuardMarker(db);
    if(Hex(LegacyGuardDigest(db,"wallet_utxos_legacy_v2"))!=legacy_guard_marker)
        Refuse("Wallet index v3 legacy guard definitions changed");
    Database old(published,SQLITE_OPEN_READONLY);
    const auto backup=InspectV2(old.db,"wallet_utxos");
    if(BackupDigest(backup)!=marker)Refuse("Wallet index v3 rollback backup marker mismatch");
    const auto legacy=DigestQuery(db,"SELECT * FROM wallet_utxos_legacy_v2 ORDER BY txid,vout");
    if(backup.coins!=legacy || uint64_t(Integer(db,"SELECT count(*) FROM wallet_utxos_legacy_v2"))!=backup.rows)
        Refuse("Wallet index v3 legacy quarantine changed");
    if(LegacyGuards(old.db,"wallet_utxos")!=LegacyGuards(db,"wallet_utxos_legacy_v2"))
        Refuse("Wallet index v3 legacy guards changed");
    return {published,backup.rows,marker,legacy_guard_marker};
}
} // namespace

std::optional<int64_t> ReadWalletIndexSchemaVersionReadOnly(
        const std::filesystem::path& index_file) {
    if(index_file.empty())Refuse("Wallet index schema probe source unavailable");
    std::error_code error;
    const auto status=std::filesystem::symlink_status(index_file,error);
    if(error==std::errc::no_such_file_or_directory ||
       (!error && status.type()==std::filesystem::file_type::not_found))
        return std::nullopt;
    if(error || (!std::filesystem::is_regular_file(status) &&
                 !std::filesystem::is_symlink(status)))
        Refuse("Wallet index schema probe source is not a regular file");
    // SQLITE_OPEN_READONLY can still create WAL coordination sidecars. Reject
    // a linked immediate parent before SQLite opens the index, so a failed
    // startup cannot create those files outside its locked blockchain root.
    // A directly symlinked v2 index remains supported below.
    error.clear();
    const auto parent=index_file.has_parent_path()?index_file.parent_path():std::filesystem::path{"."};
    const auto parent_status=std::filesystem::symlink_status(parent,error);
    if(error || std::filesystem::is_symlink(parent_status) ||
       !std::filesystem::is_directory(parent_status))
        Refuse("Wallet index schema probe source parent is linked or unavailable");
    // Legacy v2 accepts an explicitly symlinked index. Preserve that route:
    // follow the link only for this read-only version probe, then pass v2 to
    // its unchanged initializer. A v3 link still fails the authenticated
    // RequireFullArchiveWalletIndexV3 check before any legacy writer opens it.
    const auto source_path=std::filesystem::is_symlink(status)?
        std::filesystem::canonical(index_file,error):index_file;
    if(error || !std::filesystem::is_regular_file(source_path))
        Refuse("Wallet index schema probe source unavailable");
    Database source(source_path,SQLITE_OPEN_READONLY);
    return Integer(source.db,"PRAGMA user_version");
}

bool WalletIndexHasFullArchiveV3FootprintReadOnly(
        const std::filesystem::path& index_file,
        bool inspect_rollback_artifacts) {
    if(index_file.empty())Refuse("Wallet index v3 footprint source unavailable");
    const auto parent=index_file.parent_path();
    std::error_code error;
    const auto parent_status=std::filesystem::symlink_status(parent,error);
    if(error==std::errc::no_such_file_or_directory ||
       (!error && parent_status.type()==std::filesystem::file_type::not_found))
        return false;
    if(error || !std::filesystem::is_directory(parent_status))
        Refuse("Wallet index v3 footprint directory unavailable");
    // The published backup outlives the v3 index. Also fence an interrupted
    // publish, even if its index still reports v2. A false positive here only
    // asks the operator to inspect the source rather than deleting chain data.
    if(inspect_rollback_artifacts) {
        const std::string prefix=index_file.filename().string()+".v2-pre-v3-";
        std::filesystem::directory_iterator entry(parent,error),end;
        if(error)Refuse("Wallet index v3 footprint directory unreadable");
        for(;entry!=end;entry.increment(error)) {
            if(error)Refuse("Wallet index v3 footprint directory unreadable");
            if(entry->path().filename().string().starts_with(prefix))return true;
        }
        if(error)Refuse("Wallet index v3 footprint directory unreadable");
    }

    const auto status=std::filesystem::symlink_status(index_file,error);
    if(error==std::errc::no_such_file_or_directory ||
       (!error && status.type()==std::filesystem::file_type::not_found))
        return false;
    if(error || (!std::filesystem::is_regular_file(status) &&
                 !std::filesystem::is_symlink(status)))
        Refuse("Wallet index v3 footprint source unavailable");
    const auto source_path=std::filesystem::is_symlink(status)?
        std::filesystem::canonical(index_file,error):index_file;
    if(error || !std::filesystem::is_regular_file(source_path))
        Refuse("Wallet index v3 footprint source unavailable");
    Database source(source_path,SQLITE_OPEN_READONLY);
    if(Integer(source.db,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN ('wallet_utxos_legacy_v2','wallet_index_receipts_v3')")>0)
        return true;
    if(Integer(source.db,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='utxo_metadata'")==0)
        return false;
    Statement marker(source.db,"SELECT 1 FROM utxo_metadata WHERE key LIKE 'wallet_index_v3:%' LIMIT 1");
    const int step=sqlite3_step(marker.q);
    if(step!=SQLITE_ROW && step!=SQLITE_DONE)
        Refuse("Wallet index v3 footprint marker read failed");
    return step==SQLITE_ROW;
}

bool WalletIndexHasImportedSnapshotMarkerReadOnly(
        const std::filesystem::path& index_file) {
    if(index_file.empty()||!std::filesystem::is_regular_file(index_file))
        Refuse("Wallet index snapshot marker source unavailable");
    Database source(index_file,SQLITE_OPEN_READONLY);
    Statement q(source.db,R"(
        SELECT 1 FROM utxo_metadata WHERE
            key='wallet_snapshot_recovery_base_height' OR
            (key='assumeutxo_lifecycle_state' AND value='snapshot_loaded') OR
            (key='assumeutxo_active' AND value NOT IN ('0','false',''))
        LIMIT 1
    )");
    const int step=sqlite3_step(q.q);
    if(step!=SQLITE_ROW&&step!=SQLITE_DONE)
        Refuse("Wallet index snapshot marker read failed");
    return step==SQLITE_ROW;
}

WalletIndexV3SchemaResult RequireFullArchiveWalletIndexV3(const std::filesystem::path& index_file) {
    if(index_file.empty()||!std::filesystem::is_regular_file(index_file))
        Refuse("Wallet index v3 source file unavailable");
    Database source(index_file,SQLITE_OPEN_READONLY);
    return InspectV3(source.db,index_file);
}

WalletIndexV3SchemaResult RequireFullArchiveWalletIndexV3(
        sqlite3* source,const std::filesystem::path& index_file) {
    if(!source||index_file.empty()||!std::filesystem::is_regular_file(index_file)||
       std::filesystem::is_symlink(std::filesystem::symlink_status(index_file))||
       sqlite3_db_readonly(source,"main")<0)
        Refuse("Wallet index v3 source unavailable");
    const auto* opened=sqlite3_db_filename(source,"main");
    std::error_code error;
    if(!opened||!*opened||!std::filesystem::equivalent(std::filesystem::u8path(opened),index_file,error)||error)
        Refuse("Wallet index v3 connection source changed");
    return InspectV3(source,index_file);
}

void RequireFullArchiveWalletIndexV3ReadGuards(sqlite3* source,
        std::string_view verified_rollback_digest,
        std::string_view verified_legacy_guard_digest) {
    if(!source || Integer(source,"PRAGMA user_version")!=3)
        Refuse("Wallet index v3 read source unavailable");
    if(verified_rollback_digest.empty() || verified_legacy_guard_digest.empty())
        Refuse("Wallet index v3 verified markers unavailable");
    CheckTable(source,"wallet_utxos",V3Body());
    CheckTable(source,"wallet_utxos_legacy_v2",v2_body);
    CheckV3Guards(source);
    if(ReadBackupMarker(source)!=verified_rollback_digest)
        Refuse("Wallet index v3 rollback backup marker changed since open");
    const auto legacy_guard_marker=ReadLegacyGuardMarker(source);
    if(legacy_guard_marker!=verified_legacy_guard_digest ||
       Hex(LegacyGuardDigest(source,"wallet_utxos_legacy_v2"))!=legacy_guard_marker)
        Refuse("Wallet index v3 legacy guard definitions changed");
}

WalletIndexV3SchemaResult UpgradeFullArchiveWalletIndexSchemaToV3(const std::filesystem::path& index_file) {
    std::error_code source_error;
    const auto source_status=std::filesystem::symlink_status(index_file,source_error);
    if(index_file.empty()||source_error||!std::filesystem::is_regular_file(source_status))
        Refuse("Wallet index v3 source file unavailable or linked");
    Database source(index_file,SQLITE_OPEN_READWRITE);
    const auto version=Integer(source.db,"PRAGMA user_version");
    if(version==3) return InspectV3(source.db,index_file);
    if(version!=2)Refuse("Wallet index v3 requires initialized version 2 index");
    Exec(source.db,"PRAGMA synchronous=FULL");
    const auto original=InspectV2(source.db,"wallet_utxos");
    const auto digest=BackupDigest(original);
    const auto published=BackupPath(index_file,digest);
    const auto old_guards=LegacyGuards(source.db,"wallet_utxos");
    if(std::filesystem::exists(published)) {
        RequireIndependentBackup(index_file,published);
        Database old(published,SQLITE_OPEN_READONLY);
        const auto existing=InspectV2(old.db,"wallet_utxos");
        if(existing.rows!=original.rows||existing.coins!=original.coins||
           existing.metadata!=original.metadata||existing.guards!=original.guards)
            Refuse("Wallet index v3 existing backup differs from source");
    } else {
        const auto entropy=secure_random_bytes(16);std::string suffix;
        for(const auto n:entropy){suffix.push_back("0123456789abcdef"[n>>4]);suffix.push_back("0123456789abcdef"[n&15]);}
        auto temporary=published;temporary+=".staging-"+suffix;
        struct RemoveTemporary {std::filesystem::path file;~RemoveTemporary(){std::error_code ec;std::filesystem::remove(file,ec);} } cleanup{temporary};
        Backup(source.db,temporary);
        {
            Database candidate(temporary,SQLITE_OPEN_READONLY);
            const auto checked=InspectV2(candidate.db,"wallet_utxos");
            if(checked.rows!=original.rows||checked.coins!=original.coins||
               checked.metadata!=original.metadata||checked.guards!=original.guards)
                Refuse("Wallet index v3 backup differs from source");
        }
        SyncAndPublish(temporary,published);
    }
    RequireIndependentBackup(index_file,published);
    Exec(source.db,"BEGIN IMMEDIATE");bool owned=true;
    try {
        const auto now=InspectV2(source.db,"wallet_utxos");
        if(now.rows!=original.rows||now.coins!=original.coins||
           now.metadata!=original.metadata||now.guards!=original.guards)
            Refuse("Wallet index v3 source changed after backup");
        CreateV3(source.db);
        WriteBackupMarker(source.db,digest);
        WriteLegacyGuardMarker(source.db,LegacyGuardDigest(source.db,"wallet_utxos_legacy_v2"));
        CheckTable(source.db,"wallet_utxos_legacy_v2",v2_body);
        CheckTable(source.db,"wallet_utxos",V3Body());
        CheckV3Guards(source.db);
        if(DigestQuery(source.db,"SELECT * FROM wallet_utxos_legacy_v2 ORDER BY txid,vout")!=original.coins ||
           DigestQuery(source.db,"SELECT key,value FROM utxo_metadata WHERE key NOT LIKE 'wallet_index_v3:%' ORDER BY key")!=original.metadata ||
           ReadBackupMarker(source.db)!=digest ||
           Hex(LegacyGuardDigest(source.db,"wallet_utxos_legacy_v2"))!=ReadLegacyGuardMarker(source.db) ||
           LegacyGuards(source.db,"wallet_utxos_legacy_v2")!=old_guards ||
           Integer(source.db,"SELECT count(*) FROM wallet_utxos")!=0 ||
           Integer(source.db,"SELECT count(*) FROM wallet_index_receipts_v3")!=0 ||
           Integer(source.db,"PRAGMA user_version")!=3)
            Refuse("Wallet index v3 transaction changed preserved state");
        Exec(source.db,"COMMIT");owned=false;
    } catch(...) {
        if(owned)sqlite3_exec(source.db,"ROLLBACK",nullptr,nullptr,nullptr);
        throw;
    }
    QuickCheck(source.db);
    return {published,original.rows};
}

void ResetFullArchiveWalletIndexV3(const std::filesystem::path& index_file) {
    Database db(index_file,SQLITE_OPEN_READWRITE);
    if(Integer(db.db,"PRAGMA user_version")!=3)
        Refuse("Wallet index v3 reset requires version 3");
    const auto checked=UpgradeFullArchiveWalletIndexSchemaToV3(index_file);
    const auto marker=ReadBackupMarker(db.db);
    if(BackupPath(index_file,marker)!=checked.rollback_backup)
        Refuse("Wallet index v3 reset rollback backup changed");
    Exec(db.db,"PRAGMA synchronous=FULL");
    Exec(db.db,"BEGIN IMMEDIATE");bool owned=true;
    try {
        if(Integer(db.db,"PRAGMA user_version")!=3||ReadBackupMarker(db.db)!=marker)
            Refuse("Wallet index v3 reset source changed");
        CheckTable(db.db,"wallet_utxos",V3Body());CheckV3Guards(db.db);
        const auto quarantined=Integer(db.db,"SELECT count(*) FROM wallet_utxos_legacy_v2");
        Exec(db.db,"DELETE FROM wallet_utxos");
        // A namespace with no rows can still carry a receipt, so row-delete
        // triggers alone cannot invalidate a chainwide reset.
        Exec(db.db,"DELETE FROM wallet_index_receipts_v3");
        Exec(db.db,"DELETE FROM utxo_metadata WHERE key NOT IN ('wallet_index_v3:rollback_backup_digest','wallet_index_v3:legacy_guard_digest')");
        if(Integer(db.db,"SELECT count(*) FROM wallet_utxos")!=0 ||
           Integer(db.db,"SELECT count(*) FROM wallet_index_receipts_v3")!=0 ||
           Integer(db.db,"SELECT count(*) FROM wallet_utxos_legacy_v2")!=quarantined ||
           ReadBackupMarker(db.db)!=marker ||
           Hex(LegacyGuardDigest(db.db,"wallet_utxos_legacy_v2"))!=ReadLegacyGuardMarker(db.db))
            Refuse("Wallet index v3 reset incomplete");
        Exec(db.db,"COMMIT");owned=false;
    } catch(...) {
        if(owned)sqlite3_exec(db.db,"ROLLBACK",nullptr,nullptr,nullptr);
        throw;
    }
    QuickCheck(db.db);
}

} // namespace dinero
