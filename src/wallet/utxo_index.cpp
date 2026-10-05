#include "wallet/utxo_index.h"
#include "sqlite_open.h"
#include "wallet/hd_wallet.h"
#include "address/addr_codec.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <set>
#include <limits>

namespace dinero {

// =============================================================================
// WALLET INVARIANT: Derivation Path Validation
// =============================================================================
// Rule: Every wallet UTXO MUST have a valid derivation path.
// A valid path starts with "m/" and follows BIP32 format.
// Empty or invalid paths indicate ownership tracking failure.
// =============================================================================

/**
 * @brief Validate that a derivation path is valid for wallet UTXOs
 * @param path The derivation path string (e.g., "m/86'/1448'/0'/0/12")
 * @return true if path is valid BIP32 format, false otherwise
 *
 * Valid paths:
 *   - "m/86'/1448'/0'/0/0"  (BIP86 Taproot - PRIMARY)
 *   - "m/84'/1448'/0'/0/0"  (BIP84 P2WPKH - LEGACY)
 *   - "m/77'/1448'/..."     (Shielded key hierarchy)
 *
 * Invalid paths:
 *   - ""                    (empty - ownership unknown)
 *   - "unknown"             (placeholder - not derived)
 *   - anything not starting with "m/"
 */
static bool IsValidDerivationPath(const std::string& path) {
    // Must not be empty
    if (path.empty()) {
        return false;
    }

    // BIP-style derivation: "m/86'/...", "m/84'/...", etc.
    if (path.size() >= 4 && path[0] == 'm' && path[1] == '/') {
        return true;
    }

    // Descriptor-imported Taproot keys: "tr(...)"
    // These are valid ownership proofs — the wallet holds the private key.
    if (path.size() >= 4 && path.substr(0, 3) == "tr(") {
        return true;
    }

    return false;
}

/**
 * @brief Check if a path is explicitly marked as "external" (non-wallet)
 * @param path The path string
 * @return true if this is a known external/system path
 *
 * Some UTXOs may legitimately have non-wallet paths:
 *   - "genesis" - Genesis outputs
 *   - "coinbase" - Mining rewards (before wallet claims)
 */
static bool IsExternalPath(const std::string& path) {
    return path == "genesis" || path == "coinbase" || path == "system";
}

UTXOIndex::UTXOIndex(const std::string& db_path)
    : db_(nullptr), db_path_(db_path), stmt_add_utxo_(nullptr),
      stmt_spend_utxo_(nullptr), stmt_get_unspent_(nullptr),
      stmt_get_balance_(nullptr), stmt_is_spent_(nullptr), stmt_get_utxo_(nullptr),
      stmt_get_position_(nullptr) {  // Phase 11a: Utreexo position tracking
}

UTXOIndex::~UTXOIndex() {
    FinalizeStatements();
    if (db_) {
        sqlite3_close(db_);
    }
}

bool UTXOIndex::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_) return false;
    historical_scripts_.clear();
    // M.5.2: Guard against re-initialization (lifecycle safety)
    // If already initialized, clean up first to prevent memory leaks
    if (db_ != nullptr) {
        std::cerr << "WARNING: UTXOIndex::Initialize() called on already-initialized instance" << std::endl;
        std::cerr << "         Cleaning up previous state before re-initializing..." << std::endl;
        FinalizeStatements();
        sqlite3_close(db_);
        db_ = nullptr;
    }

    // CRITICAL FIX: Use unified SQLite opener with consistent PRAGMAs
    auto opened = open_sqlite(db_path_);
    if (opened.rc != SQLITE_OK) {
        std::cerr << "ERROR: Failed to open UTXO database: " << opened.errmsg << std::endl;
        return false;
    }
    db_ = opened.db;

    if (!CreateTables()) {
        std::cerr << "ERROR: Failed to create UTXO tables" << std::endl;
        return false;
    }

    if (!PrepareStatements()) {
        std::cerr << "ERROR: Failed to prepare UTXO statements" << std::endl;
        return false;
    }

    std::cout << "INFO: UTXO index initialized successfully" << std::endl;
    return true;
}

namespace {
struct OwnershipSchemaStatement {
    sqlite3_stmt* value=nullptr;
    OwnershipSchemaStatement(sqlite3* db,const std::string& sql) {
        if(sqlite3_prepare_v2(db,sql.c_str(),-1,&value,nullptr)!=SQLITE_OK) {
            sqlite3_finalize(value);throw std::runtime_error("Wallet ownership schema prepare failed");
        }
    }
    ~OwnershipSchemaStatement(){sqlite3_finalize(value);}
    void Text(int position,const std::string& text) {
        if(sqlite3_bind_text(value,position,text.data(),int(text.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
            throw std::runtime_error("Wallet ownership schema bind failed");
    }
    void Done(){if(sqlite3_step(value)!=SQLITE_DONE)throw std::runtime_error("Wallet ownership schema incomplete");}
};
std::string OwnershipSchemaText(sqlite3_stmt* row,int column) {
    if(sqlite3_column_type(row,column)!=SQLITE_TEXT)throw std::runtime_error("Wallet ownership schema text type invalid");
    const auto* bytes=static_cast<const char*>(sqlite3_column_blob(row,column));const int size=sqlite3_column_bytes(row,column);
    if(!bytes||size<=0||std::memchr(bytes,0,size))throw std::runtime_error("Wallet ownership schema text invalid");
    return std::string(bytes,size);
}
void OwnershipSchemaExec(sqlite3* db,const std::string& sql) {
    if(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("Wallet ownership schema write failed");
}
int64_t OwnershipSchemaInteger(sqlite3* db,const std::string& sql) {
    OwnershipSchemaStatement row(db,sql);
    if(sqlite3_step(row.value)!=SQLITE_ROW||sqlite3_column_type(row.value,0)!=SQLITE_INTEGER)
        throw std::runtime_error("Wallet ownership schema integer invalid");
    const auto result=sqlite3_column_int64(row.value,0);row.Done();return result;
}
constexpr const char* ownership_table_body=R"((
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
void CheckOwnershipTable(sqlite3* db) {
    OwnershipSchemaStatement row(db,"SELECT sql FROM sqlite_schema WHERE type='table' AND name='wallet_utxos'");
    if(sqlite3_step(row.value)!=SQLITE_ROW)throw std::runtime_error("Wallet ownership table missing");
    const auto sql=OwnershipSchemaText(row.value,0);row.Done();const auto body=sql.find('(');
    if(body==std::string::npos||sql.substr(body)!=ownership_table_body)
        throw std::runtime_error("Wallet ownership table definition changed");
}
}

bool UTXOIndex::CreateTables() {
    // The migration owns one FULL SQLite transaction. It copies every existing
    // value without type coercion, recreates exact user indexes/triggers and
    // preserves metadata/receipts. Neither schema presence nor a new owner
    // column is authentication; historical rows still require a live key owner.
    if(!db_||sqlite3_db_readonly(db_,"main")!=0||!sqlite3_get_autocommit(db_))return false;
    bool owned=false;
    try {
        const auto synchronous=OwnershipSchemaInteger(db_,"PRAGMA synchronous");
        if(synchronous<2)OwnershipSchemaExec(db_,"PRAGMA synchronous=FULL");
        if(OwnershipSchemaInteger(db_,"PRAGMA synchronous")<2)
            throw std::runtime_error("Wallet ownership schema durability unavailable");
        { OwnershipSchemaStatement q(db_,"PRAGMA journal_mode");
          if(sqlite3_step(q.value)!=SQLITE_ROW)throw std::runtime_error("Wallet ownership journal unavailable");
          const auto mode=OwnershipSchemaText(q.value,0);q.Done();
          const auto* filename=sqlite3_db_filename(db_,"main");
          const bool memory=mode=="memory"&&(!filename||!*filename);
          if(mode!="wal"&&mode!="delete"&&mode!="truncate"&&mode!="persist"&&!memory)
              throw std::runtime_error("Wallet ownership journal unsupported"); }
        OwnershipSchemaExec(db_,"BEGIN IMMEDIATE");owned=true;
        const auto version=OwnershipSchemaInteger(db_,"PRAGMA user_version");
        if(version<0||version>2)throw std::runtime_error("Wallet ownership schema version unsupported");
        const bool present=OwnershipSchemaInteger(db_,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='wallet_utxos'")==1;
        if(version==2) {
            if(!present)throw std::runtime_error("Wallet ownership schema lacks its table");
            CheckOwnershipTable(db_);
        } else if(present) {
            const std::vector<std::string> columns={"txid","vout","value","spk","path","height","spend_height","is_coinbase",
                "utreexo_position","is_confidential","commitment","range_proof","blinding_factor","nonce"};
            std::set<std::string> observed;int rc;
            { OwnershipSchemaStatement q(db_,"PRAGMA table_xinfo(wallet_utxos)");
              while((rc=sqlite3_step(q.value))==SQLITE_ROW) {
                  const auto name=OwnershipSchemaText(q.value,1);
                  if(!observed.insert(name).second||std::find(columns.begin(),columns.end(),name)==columns.end()||
                     sqlite3_column_type(q.value,6)!=SQLITE_INTEGER||sqlite3_column_int64(q.value,6)!=0)
                      throw std::runtime_error("Wallet ownership legacy columns unsupported");
              }
              if(rc!=SQLITE_DONE)throw std::runtime_error("Wallet ownership legacy schema incomplete"); }
            for(const auto* n:{"txid","vout","value","spk","path","height"})
                if(!observed.count(n))throw std::runtime_error("Wallet ownership legacy column missing");
            // A table rebuild with a foreign-key relationship needs a separate
            // qualified migration. Refuse before mutation rather than cascade.
            { OwnershipSchemaStatement tables(db_,"SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%'");
              while((rc=sqlite3_step(tables.value))==SQLITE_ROW) {
                  const auto name=OwnershipSchemaText(tables.value,0);
                  OwnershipSchemaStatement foreign(db_,"SELECT \"table\" FROM pragma_foreign_key_list(?)");foreign.Text(1,name);int fr;
                  while((fr=sqlite3_step(foreign.value))==SQLITE_ROW)
                      if(name=="wallet_utxos"||OwnershipSchemaText(foreign.value,0)=="wallet_utxos")
                          throw std::runtime_error("Wallet ownership migration foreign key unsupported");
                  if(fr!=SQLITE_DONE)throw std::runtime_error("Wallet ownership foreign-key inventory incomplete");
              }
              if(rc!=SQLITE_DONE)throw std::runtime_error("Wallet ownership table inventory incomplete"); }
            std::vector<std::pair<std::string,std::string>> schema;
            { OwnershipSchemaStatement q(db_,"SELECT name,sql FROM sqlite_schema WHERE tbl_name='wallet_utxos' AND type IN('index','trigger') AND sql IS NOT NULL ORDER BY type,name");
              while((rc=sqlite3_step(q.value))==SQLITE_ROW)schema.emplace_back(OwnershipSchemaText(q.value,0),OwnershipSchemaText(q.value,1));
              if(rc!=SQLITE_DONE)throw std::runtime_error("Wallet ownership guard inventory incomplete"); }
            if(OwnershipSchemaInteger(db_,"SELECT count(*) FROM sqlite_schema WHERE name='wallet_utxos_ownership_v2'")!=0)
                throw std::runtime_error("Wallet ownership migration name conflict");
            OwnershipSchemaExec(db_,std::string("CREATE TABLE wallet_utxos_ownership_v2 ")+ownership_table_body);
            std::string destination,source,equal;
            for(const auto& column:columns) {
                if(!destination.empty()){destination+=',';source+=',';}
                destination+=column;
                source+=observed.count(column)?column:(column=="is_coinbase"||column=="is_confidential"?"0":"NULL");
                if(observed.count(column)) {
                    if(!equal.empty())equal+=" AND ";
                    equal+="typeof(a."+column+")=typeof(b."+column+") AND a."+column+" IS b."+column;
                }
            }
            const auto count=OwnershipSchemaInteger(db_,"SELECT count(*) FROM wallet_utxos");
            OwnershipSchemaExec(db_,"INSERT INTO wallet_utxos_ownership_v2("+destination+") SELECT "+source+" FROM wallet_utxos");
            if(OwnershipSchemaInteger(db_,"SELECT count(*) FROM wallet_utxos_ownership_v2")!=count||
               OwnershipSchemaInteger(db_,"SELECT count(*) FROM wallet_utxos a JOIN wallet_utxos_ownership_v2 b ON a.txid IS b.txid AND a.vout IS b.vout WHERE "+equal)!=count)
                throw std::runtime_error("Wallet ownership migration changed existing values");
            OwnershipSchemaExec(db_,"DROP TABLE wallet_utxos");
            OwnershipSchemaExec(db_,"ALTER TABLE wallet_utxos_ownership_v2 RENAME TO wallet_utxos");
            for(const auto& [name,sql]:schema) {
                OwnershipSchemaExec(db_,sql);
                OwnershipSchemaStatement q(db_,"SELECT sql FROM sqlite_schema WHERE name=? AND tbl_name='wallet_utxos'");q.Text(1,name);
                if(sqlite3_step(q.value)!=SQLITE_ROW||OwnershipSchemaText(q.value,0)!=sql)
                    throw std::runtime_error("Wallet ownership migration changed a guard");
                q.Done();
            }
            CheckOwnershipTable(db_);
        } else {
            if(version!=0)throw std::runtime_error("Wallet ownership legacy table missing");
            OwnershipSchemaExec(db_,std::string("CREATE TABLE wallet_utxos ")+ownership_table_body);
        }
        OwnershipSchemaExec(db_,R"(
            CREATE TABLE IF NOT EXISTS utxo_metadata(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_unspent ON wallet_utxos(spend_height) WHERE spend_height IS NULL;
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_path ON wallet_utxos(path);
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_height ON wallet_utxos(height);
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_spend_height ON wallet_utxos(spend_height) WHERE spend_height IS NOT NULL;
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_coinbase_maturity ON wallet_utxos(is_coinbase,height) WHERE spend_height IS NULL;
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_confidential ON wallet_utxos(is_confidential) WHERE is_confidential=1 AND spend_height IS NULL;
            CREATE INDEX IF NOT EXISTS idx_wallet_utxos_utreexo_position ON wallet_utxos(txid,vout,utreexo_position) WHERE utreexo_position IS NOT NULL;
        )");
        if(version!=2)OwnershipSchemaExec(db_,"PRAGMA user_version=2");
        if(OwnershipSchemaInteger(db_,"PRAGMA user_version")!=2)throw std::runtime_error("Wallet ownership schema version write failed");
        OwnershipSchemaExec(db_,"COMMIT");owned=false;
        return true;
    } catch(const std::exception& error) {
        if(owned&&!sqlite3_get_autocommit(db_)&&sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&!sqlite3_get_autocommit(db_))
            std::terminate();
        g_logger.error(std::string("[UTXOIndex] Ownership schema refused: ")+error.what());
        return false;
    }
}

bool UTXOIndex::PrepareStatements() {
    // Add UTXO statement (with confidential fields + Phase 11a: utreexo_position)
    //
    // UPSERT semantics for CT fields (blinding_factor, value):
    //   On conflict (re-scan of existing UTXO), preserve the existing
    //   blinding_factor and value when the incoming blinding_factor is NULL
    //   (CT rangeproof rewind failed — wallet was locked at scan time).
    //   This prevents losing recovered CT data across daemon restarts.
    // Creation replay cannot erase or replace a recorded spend. Only explicit
    // rollback may restore an existing spent row to unspent; an import may
    // still supply spend metadata when the row has none.
    const char* add_sql = R"(
        INSERT INTO wallet_utxos
        (txid, vout, value, spk, path, height, spend_height, is_coinbase,
         utreexo_position,
         is_confidential, commitment, range_proof, blinding_factor, nonce, owner_kind, owner_reference)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(txid, vout) DO UPDATE SET
          value            = CASE WHEN excluded.blinding_factor IS NOT NULL
                                  THEN excluded.value
                                  ELSE wallet_utxos.value END,
          spk              = excluded.spk,
          path             = excluded.path,
          height           = excluded.height,
          spend_height     = COALESCE(wallet_utxos.spend_height, excluded.spend_height),
          is_coinbase      = excluded.is_coinbase,
          utreexo_position = excluded.utreexo_position,
          is_confidential  = excluded.is_confidential,
          commitment       = excluded.commitment,
          range_proof      = excluded.range_proof,
          blinding_factor  = COALESCE(excluded.blinding_factor, wallet_utxos.blinding_factor),
          nonce            = COALESCE(excluded.nonce, wallet_utxos.nonce),
          owner_kind       = excluded.owner_kind,
          owner_reference  = excluded.owner_reference
        WHERE wallet_utxos.spk IS excluded.spk
          AND wallet_utxos.path IS excluded.path
          AND wallet_utxos.owner_kind IS excluded.owner_kind
          AND wallet_utxos.owner_reference IS excluded.owner_reference
    )";

    if (sqlite3_prepare_v2(db_, add_sql, -1, &stmt_add_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare add UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Spend UTXO statement
    const char* spend_sql = R"(
        UPDATE wallet_utxos SET spend_height = ? 
        WHERE txid = ? AND vout = ?
    )";
    
    if (sqlite3_prepare_v2(db_, spend_sql, -1, &stmt_spend_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare spend UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get unspent UTXOs statement
    const char* unspent_sql = R"(
        SELECT txid, vout, value, spk, path, height, spend_height, is_coinbase, is_confidential,
               commitment, range_proof, blinding_factor, nonce, utreexo_position, owner_kind, owner_reference
        FROM wallet_utxos 
        WHERE spend_height IS NULL 
        ORDER BY value DESC
    )";
    
    if (sqlite3_prepare_v2(db_, unspent_sql, -1, &stmt_get_unspent_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get unspent statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get balance statement
    const char* balance_sql = R"(
        SELECT COALESCE(SUM(value), 0) 
        FROM wallet_utxos 
        WHERE spend_height IS NULL
    )";
    
    if (sqlite3_prepare_v2(db_, balance_sql, -1, &stmt_get_balance_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get balance statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Check if UTXO is spent statement
    const char* is_spent_sql = R"(
        SELECT spend_height FROM wallet_utxos 
        WHERE txid = ? AND vout = ?
    )";
    
    if (sqlite3_prepare_v2(db_, is_spent_sql, -1, &stmt_is_spent_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare is spent statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get specific UTXO statement
    const char* get_utxo_sql = R"(
        SELECT txid, vout, value, spk, path, height, spend_height, is_coinbase, is_confidential,
               commitment, range_proof, blinding_factor, nonce, utreexo_position, owner_kind, owner_reference
        FROM wallet_utxos
        WHERE txid = ? AND vout = ?
    )";

    if (sqlite3_prepare_v2(db_, get_utxo_sql, -1, &stmt_get_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Phase 11a: Get Utreexo position statement
    const char* get_position_sql = R"(
        SELECT utreexo_position
        FROM wallet_utxos
        WHERE txid = ? AND vout = ?
    )";

    if (sqlite3_prepare_v2(db_, get_position_sql, -1, &stmt_get_position_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get position statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    return true;
}

void UTXOIndex::FinalizeStatements() {
    if (stmt_add_utxo_) { sqlite3_finalize(stmt_add_utxo_); stmt_add_utxo_ = nullptr; }
    if (stmt_spend_utxo_) { sqlite3_finalize(stmt_spend_utxo_); stmt_spend_utxo_ = nullptr; }
    if (stmt_get_unspent_) { sqlite3_finalize(stmt_get_unspent_); stmt_get_unspent_ = nullptr; }
    if (stmt_get_balance_) { sqlite3_finalize(stmt_get_balance_); stmt_get_balance_ = nullptr; }
    if (stmt_is_spent_) { sqlite3_finalize(stmt_is_spent_); stmt_is_spent_ = nullptr; }
    if (stmt_get_utxo_) { sqlite3_finalize(stmt_get_utxo_); stmt_get_utxo_ = nullptr; }
    if (stmt_get_position_) { sqlite3_finalize(stmt_get_position_); stmt_get_position_ = nullptr; }  // Phase 11a
}

bool UTXOIndex::AddUTXO(const WalletUTXO& utxo) {
    // A public provenance tag is not an authenticated inventory or chain proof.
    if (utxo.owner_kind != WalletOutputOwner::RecordedPath || !utxo.owner_reference.empty())
        return false;
    // ═══════════════════════════════════════════════════════════════════════════
    // WALLET INVARIANT: Every UTXO must have a valid derivation path
    // ═══════════════════════════════════════════════════════════════════════════
    // This ensures the wallet never credits "ghost UTXOs" without ownership proof.
    // Valid paths: "m/86'/..." (PRIMARY), "m/84'/..." (LEGACY), "m/77'/..." (CT)
    // External paths: "genesis", "coinbase", "system" (for system UTXOs)
    // ═══════════════════════════════════════════════════════════════════════════
    if (!IsValidDerivationPath(utxo.path) && !IsExternalPath(utxo.path)) {
        std::cerr << "ERROR [AddUTXO] INVARIANT VIOLATION: Invalid derivation path" << std::endl;
        std::cerr << "  txid: " << utxo.txid.AsUint256().GetHex() << std::endl;
        std::cerr << "  vout: " << utxo.vout << std::endl;
        std::cerr << "  path: \"" << utxo.path << "\"" << std::endl;
        std::cerr << "  This UTXO cannot be credited without ownership proof!" << std::endl;

        // In debug builds, crash immediately to catch the bug
        assert(false && "WALLET INVARIANT: AddUTXO called with invalid derivation path");

        // In release builds, refuse to add the UTXO (safe failure mode)
        return false;
    }

    return AddUTXORow(utxo);
}

bool UTXOIndex::AddUTXOForDelivery(const WalletUTXO& utxo,
        const std::map<std::vector<uint8_t>,std::string>& authenticated_historical) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (!db_ || !atomic_write_active_ || sqlite3_get_autocommit(db_)) return false;
    if (utxo.owner_kind == WalletOutputOwner::RecordedPath) return AddUTXO(utxo);
    if (utxo.owner_kind != WalletOutputOwner::HistoricalImport || !utxo.path.empty() ||
        utxo.is_confidential || utxo.owner_reference.empty() || utxo.owner_reference.size() > 128 ||
        utxo.owner_reference.find('\0') != std::string::npos) return false;
    const auto found = authenticated_historical.find(utxo.spk);
    if (found == authenticated_historical.end() || found->second != utxo.owner_reference) return false;
    return AddUTXORow(utxo);
}

bool UTXOIndex::AddUTXORow(const WalletUTXO& utxo) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (!db_ || !stmt_add_utxo_ || utxo.value.GetUna() > uint64_t(INT64_MAX) ||
        (utxo.utreexo_position && *utxo.utreexo_position > uint64_t(INT64_MAX)) ||
        utxo.path.find('\0') != std::string::npos) return false;
    auto* q=stmt_add_utxo_;sqlite3_reset(q);sqlite3_clear_bindings(q);
    struct Reset {sqlite3_stmt* q;~Reset(){sqlite3_reset(q);sqlite3_clear_bindings(q);}} reset{q};
    bool bound=true;
    const auto integer=[&](int n,int64_t value){bound &= sqlite3_bind_int64(q,n,value)==SQLITE_OK;};
    const auto text=[&](int n,const std::string& value){
        bound &= value.size()<=size_t(INT_MAX) && sqlite3_bind_text(q,n,value.data(),int(value.size()),SQLITE_TRANSIENT)==SQLITE_OK;
    };
    const auto blob=[&](int n,const std::vector<uint8_t>& value){
        bound &= value.size()<=size_t(INT_MAX) && sqlite3_bind_blob(q,n,value.data(),int(value.size()),SQLITE_TRANSIENT)==SQLITE_OK;
    };
    const auto null=[&](int n){bound &= sqlite3_bind_null(q,n)==SQLITE_OK;};
    text(1,utxo.txid.AsUint256().GetHex());integer(2,utxo.vout);integer(3,utxo.value.GetUna());
    blob(4,utxo.spk);text(5,utxo.path);integer(6,utxo.height);
    if(utxo.spend_height)integer(7,*utxo.spend_height);else null(7);
    integer(8,utxo.is_coinbase);if(utxo.utreexo_position)integer(9,*utxo.utreexo_position);else null(9);
    integer(10,utxo.is_confidential);
    for(const auto& item: {std::pair{11,&utxo.commitment},std::pair{12,&utxo.range_proof},
                          std::pair{13,&utxo.blinding_factor},std::pair{14,&utxo.nonce}}) {
        if(utxo.is_confidential && !item.second->empty())blob(item.first,*item.second);else null(item.first);
    }
    integer(15,static_cast<uint8_t>(utxo.owner_kind));text(16,utxo.owner_reference);
    // A conflict with different provenance must not silently replace an owner.
    return bound && sqlite3_step(q)==SQLITE_DONE && sqlite3_changes(db_)==1;
}

bool UTXOIndex::SpendUTXO(const TxId& txid, uint32_t vout, uint32_t height) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    sqlite3_reset(stmt_spend_utxo_);

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_int(stmt_spend_utxo_, 1, height);
    sqlite3_bind_text(stmt_spend_utxo_, 2, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_spend_utxo_, 3, vout);
    
    int rc = sqlite3_step(stmt_spend_utxo_);
    if (rc != SQLITE_DONE) {
        std::cerr << "ERROR: Failed to spend UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    return sqlite3_changes(db_) > 0;
}

bool UTXOIndex::DeleteUTXO(const TxId& txid, uint32_t vout) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    // Permanently delete UTXO from database (used during reorg to remove outputs from disconnected blocks)
    const char* delete_sql = "DELETE FROM wallet_utxos WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare delete UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_text(stmt, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, vout);

    int rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE);
    int changes = sqlite3_changes(db_);

    sqlite3_finalize(stmt);

    if (!success) {
        std::cerr << "ERROR: Failed to delete UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    return changes > 0;
}

bool UTXOIndex::IsUTXOSpent(const TxId& txid, uint32_t vout) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    sqlite3_reset(stmt_is_spent_);

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_text(stmt_is_spent_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_is_spent_, 2, vout);
    
    int rc = sqlite3_step(stmt_is_spent_);
    if (rc == SQLITE_ROW) {
        // Check if spend_height is NULL
        return sqlite3_column_type(stmt_is_spent_, 0) != SQLITE_NULL;
    }
    
    return false; // UTXO not found, consider unspent
}

WalletUTXO UTXOIndex::DecodeOwnedRow(sqlite3_stmt* q) const {
    const auto integer=[&](int n,int64_t low,int64_t high) {
        if(sqlite3_column_type(q,n)!=SQLITE_INTEGER)throw std::runtime_error("Wallet coin integer type invalid");
        const auto value=sqlite3_column_int64(q,n);
        if(value<low || value>high)throw std::runtime_error("Wallet coin integer out of range");
        return value;
    };
    const auto text=[&](int n) {
        if(sqlite3_column_type(q,n)!=SQLITE_TEXT)throw std::runtime_error("Wallet coin text type invalid");
        const auto* value=reinterpret_cast<const char*>(sqlite3_column_text(q,n));const int size=sqlite3_column_bytes(q,n);
        if(!value || size<0 || std::memchr(value,0,size))throw std::runtime_error("Wallet coin text malformed");
        return std::string(value,size);
    };
    const auto blob=[&](int n,bool optional) {
        if(optional && sqlite3_column_type(q,n)==SQLITE_NULL)return std::vector<uint8_t>{};
        if(sqlite3_column_type(q,n)!=SQLITE_BLOB)throw std::runtime_error("Wallet coin blob type invalid");
        const auto* value=static_cast<const uint8_t*>(sqlite3_column_blob(q,n));const int size=sqlite3_column_bytes(q,n);
        if(size<0 || (!value && size) || (!optional && !size))throw std::runtime_error("Wallet coin blob malformed");
        return size?std::vector<uint8_t>(value,value+size):std::vector<uint8_t>{};
    };
    WalletUTXO result;const auto id=text(0);
    if(id.size()!=64 || !std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
        throw std::runtime_error("Wallet coin transaction id malformed");
    result.txid=TxId(uint256::FromHexUnsafe(id));result.vout=uint32_t(integer(1,0,UINT32_MAX));
    result.value=AmountUna::Una(uint64_t(integer(2,0,INT64_MAX)));result.spk=blob(3,false);result.path=text(4);
    result.height=int(integer(5,INT_MIN,INT_MAX));
    if(sqlite3_column_type(q,6)!=SQLITE_NULL)result.spend_height=int(integer(6,INT_MIN,INT_MAX));
    result.is_coinbase=integer(7,0,1);result.is_confidential=integer(8,0,1);
    result.commitment=blob(9,true);result.range_proof=blob(10,true);
    result.blinding_factor=blob(11,true);result.nonce=blob(12,true);
    if(sqlite3_column_type(q,13)!=SQLITE_NULL)result.utreexo_position=uint64_t(integer(13,0,INT64_MAX));
    result.owner_kind=static_cast<WalletOutputOwner>(integer(14,0,1));result.owner_reference=text(15);
    if(result.owner_kind==WalletOutputOwner::RecordedPath) {
        if(!result.owner_reference.empty() || (!IsValidDerivationPath(result.path)&&!IsExternalPath(result.path)))
            throw std::runtime_error("Wallet coin recorded path invalid");
    } else {
        const auto owner=historical_scripts_.find(result.spk);
        if(!result.path.empty() || result.is_confidential || result.owner_reference.empty() || result.owner_reference.size()>128 ||
           owner==historical_scripts_.end() || owner->second!=result.owner_reference)
            throw std::runtime_error("Wallet historical coin has no authenticated live owner");
    }
    return result;
}

std::vector<WalletUTXO> UTXOIndex::GetUnspentUTXOs() const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if(!db_ || !stmt_get_unspent_)throw std::runtime_error("Wallet coin reader unavailable");
    auto* q=stmt_get_unspent_;sqlite3_reset(q);
    struct Reset {sqlite3_stmt* q;~Reset(){sqlite3_reset(q);}} reset{q};
    std::vector<WalletUTXO> result;int rc;
    while((rc=sqlite3_step(q))==SQLITE_ROW)result.push_back(DecodeOwnedRow(q));
    if(rc!=SQLITE_DONE)throw std::runtime_error("Wallet coin inventory incomplete");
    return result;
}

std::optional<WalletUTXO> UTXOIndex::GetUTXO(const TxId& txid,uint32_t vout) const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if(!db_ || !stmt_get_utxo_)throw std::runtime_error("Wallet coin reader unavailable");
    auto* q=stmt_get_utxo_;sqlite3_reset(q);sqlite3_clear_bindings(q);
    struct Reset {sqlite3_stmt* q;~Reset(){sqlite3_reset(q);sqlite3_clear_bindings(q);}} reset{q};
    const auto id=txid.AsUint256().GetHex();
    if(sqlite3_bind_text(q,1,id.data(),int(id.size()),SQLITE_TRANSIENT)!=SQLITE_OK ||
       sqlite3_bind_int64(q,2,vout)!=SQLITE_OK)throw std::runtime_error("Wallet coin lookup binding failed");
    const int rc=sqlite3_step(q);if(rc==SQLITE_DONE)return {};
    if(rc!=SQLITE_ROW)throw std::runtime_error("Wallet coin lookup failed");
    auto result=DecodeOwnedRow(q);
    if(result.txid!=txid || result.vout!=vout || sqlite3_step(q)!=SQLITE_DONE)
        throw std::runtime_error("Wallet coin lookup incomplete");
    return result;
}

bool UTXOIndex::GetUTXO(const TxId& txid, uint32_t vout, WalletUTXO& utxo) const {
    auto result = GetUTXO(txid, vout);
    if (result) {
        utxo = *result;
        return true;
    }
    return false;
}

// Phase M.6.2: Return AmountUna for type safety
AmountUna UTXOIndex::GetBalance() const {
    auto balance=AmountUna::Zero();
    for(const auto& coin:GetUnspentUTXOs()) {
        const auto next=balance.Add(coin.value);
        if(!next)throw std::runtime_error("Wallet balance overflow");balance=*next;
    }
    return balance;
}

AmountUna UTXOIndex::GetBalanceForPath(const std::string& path_prefix) const {
    // LIKE remains SQLite's existing path selection rule. Validate all rows in
    // the same implicit read snapshot; an unauthenticated owner is not zero.
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    auto balance=AmountUna::Zero();const std::string pattern=path_prefix+"%";
    for(const auto& coin:GetUnspentUTXOs()) {
        if(coin.owner_kind!=WalletOutputOwner::RecordedPath || sqlite3_strlike(pattern.c_str(),coin.path.c_str(),0)!=0)continue;
        const auto next=balance.Add(coin.value);
        if(!next)throw std::runtime_error("Wallet balance overflow");balance=*next;
    }
    return balance;
}

// Phase 44.1: UTXO count for AssumeUTXO verification
Result<uint64_t> UTXOIndex::GetUTXOCount() const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    // M.5.2 FIX: Correct table name (was "utxos", should be "wallet_utxos")
    const char* sql = "SELECT COUNT(*) FROM wallet_utxos WHERE spend_height IS NULL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string error = std::string("Failed to prepare count query: ") + sqlite3_errmsg(db_);
        return Result<uint64_t>::Err(error);
    }

    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return Result<uint64_t>::Ok(count);
}

BalanceDetail UTXOIndex::GetBalanceWithMaturity(int current_height) const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    const auto coins=GetUnspentUTXOs();BalanceDetail result;
    const auto add=[](AmountUna& target,AmountUna value) {
        const auto next=target.Add(value);if(!next)throw std::runtime_error("Wallet balance overflow");target=*next;
    };
    std::lock_guard<std::mutex> scripts(scripts_mutex_);
    for(const auto& coin:coins) {
        // Retain the existing index API maturity convention; spend selection
        // separately checks the candidate block height.
        if(!coin.is_coinbase || int64_t(current_height)-coin.height>=100)add(result.confirmed,coin.value);
        else add(result.immature,coin.value);
        if(coin.is_confidential && watched_scripts_.contains(coin.spk))add(result.confidential,coin.value);
    }
    result.total=result.confirmed;add(result.total,result.immature);
    result.total_with_conf=result.total;add(result.total_with_conf,result.confidential);
    return result;
}

std::optional<std::string> UTXOIndex::IsOurScript(const std::vector<uint8_t>& scriptPubKey) const {
    std::lock_guard<std::recursive_mutex> database(db_mutex_);
    if(historical_scripts_.contains(scriptPubKey))
        throw std::runtime_error("Historical script requires typed canonical wallet delivery");
    std::lock_guard<std::mutex> lock(scripts_mutex_);

    auto it = watched_scripts_.find(scriptPubKey);
    if (it != watched_scripts_.end()) {
        dinero::g_logger.info("[IsOurScript] MATCH FOUND! Path: " + it->second);
        return it->second;
    }

    return std::nullopt;
}

void UTXOIndex::RegisterAddress(const std::vector<uint8_t>& scriptPubKey, const std::string& derivation_path) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    watched_scripts_[scriptPubKey] = derivation_path;
}

void UTXOIndex::MergeRegisteredAddresses(const std::map<std::vector<uint8_t>, std::string>& scripts) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    auto merged = watched_scripts_;
    for (const auto& [script, path] : scripts) {
        if (script.empty() || path.empty())
            throw std::runtime_error("Invalid wallet script inventory");
        const auto [it, inserted] = merged.emplace(script, path);
        if (!inserted && it->second != path)
            throw std::runtime_error("Conflicting live wallet script path");
    }
    watched_scripts_.swap(merged);
}

void UTXOIndex::ClearRegisteredAddresses() {
    std::lock_guard<std::recursive_mutex> database(db_mutex_);
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    historical_scripts_.clear();
    watched_scripts_.clear();
}

void UTXOIndex::ProcessBlock(int height, const std::vector<std::string>& block_txs) {
    // ✅ LOCK: Protect entire SQLite transaction (must be atomic)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_)
        throw std::logic_error("Cannot replace owned wallet UTXO transaction");
    
    // Begin transaction for atomic block processing
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    
    try {
        for (const auto& tx_data : block_txs) {
            // Parse transaction and process outputs/inputs
            // This would integrate with your transaction parsing logic
            // For now, this is a placeholder
        }
        
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        std::cout << "INFO: Processed block " << height << " with " << block_txs.size() << " transactions" << std::endl;
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to process block " << height << ": " << e.what() << std::endl;
        throw;
    }
}

void UTXOIndex::RevertBlock(int height) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (!db_ || height < 0)
        throw std::invalid_argument("Invalid wallet UTXO rollback context");

    const auto exec = [&](const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Wallet UTXO rollback transaction failed");
    };
    // Acquire our own transaction before changing any rows. A failed BEGIN
    // (including an existing caller transaction) must not reach COMMIT/ROLLBACK.
    exec("BEGIN IMMEDIATE");
    try {
        const auto apply = [&](const char* sql) {
            sqlite3_stmt* raw = nullptr;
            const auto prepared = sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
            if (prepared != SQLITE_OK ||
                sqlite3_bind_int(stmt.get(), 1, height) != SQLITE_OK ||
                sqlite3_step(stmt.get()) != SQLITE_DONE)
                throw std::runtime_error("Wallet UTXO rollback statement failed");
        };
        apply("DELETE FROM wallet_utxos WHERE height = ?");
        apply("UPDATE wallet_utxos SET spend_height = NULL WHERE spend_height = ?");
        exec("COMMIT");
    } catch (...) {
        // Some SQLite errors already abort the transaction. Otherwise ensure
        // partial deletions/un-spends cannot survive into a subsequent write.
        if (!sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_))
            std::terminate();
        throw;
    }
}

// Priority 3 FIX: Validate wallet UTXOs against consensus
// Removes phantom UTXOs that exist in wallet but not in consensus
// This should be called after reorg completes to ensure consistency
size_t UTXOIndex::ValidateAgainstConsensus(std::function<bool(const TxId& txid, uint32_t vout)> consensus_has_utxo) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    size_t phantom_count = 0;
    std::vector<std::pair<std::string, uint32_t>> to_delete;

    // Phase 1: Collect all unspent UTXOs from wallet
    const char* query_sql = R"(
        SELECT txid, vout FROM wallet_utxos WHERE spend_height IS NULL
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, query_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare validation query: " << sqlite3_errmsg(db_) << std::endl;
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* txid_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        uint32_t vout = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));

        if (!txid_hex) continue;

        // Convert hex string to TxId
        TxId txid(uint256::FromHexUnsafe(txid_hex));

        // Check if consensus has this UTXO
        if (!consensus_has_utxo(txid, vout)) {
            to_delete.emplace_back(txid_hex, vout);
        }
    }
    sqlite3_finalize(stmt);

    // Phase 2: Delete phantom UTXOs
    if (!to_delete.empty()) {
        sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

        const char* delete_sql = "DELETE FROM wallet_utxos WHERE txid = ? AND vout = ?";
        sqlite3_stmt* stmt_del;
        if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt_del, nullptr) == SQLITE_OK) {
            for (const auto& [txid_hex, vout] : to_delete) {
                sqlite3_reset(stmt_del);
                sqlite3_bind_text(stmt_del, 1, txid_hex.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(stmt_del, 2, vout);
                sqlite3_step(stmt_del);

                if (sqlite3_changes(db_) > 0) {
                    phantom_count++;
                    std::cerr << "WARNING: Removed phantom UTXO " << txid_hex.substr(0, 16) << "...:" << vout
                              << " (wallet had it, consensus didn't)" << std::endl;
                }
            }
            sqlite3_finalize(stmt_del);
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }

    if (phantom_count > 0) {
        std::cout << "INFO: ValidateAgainstConsensus removed " << phantom_count << " phantom UTXOs" << std::endl;
    }

    return phantom_count;
}

// Phase M.6.2: Output amounts now use AmountUna
void UTXOIndex::ScanBlockIdempotent(int height, const std::string& block_hash,
                                    const std::vector<std::tuple<std::string,
                                                                std::vector<std::pair<std::vector<uint8_t>, AmountUna>>,
                                                                std::vector<std::pair<std::string, uint32_t>>,
                                                                bool>>& transactions) {
    // ✅ LOCK: Protect entire SQLite transaction (must be atomic)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_)
        throw std::logic_error("Cannot replace owned wallet UTXO transaction");

    // This legacy path receives no authenticated wallet inventory or checked
    // canonical event. It cannot silently skip or relabel historical coins.
    if(!historical_scripts_.empty())
        throw std::runtime_error("Historical owners require canonical wallet delivery");

    // Begin transaction for atomic block processing
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

    try {
        int utxos_added = 0;
        int utxos_spent = 0;

        // Process each transaction in the block
        for (const auto& [txid, outputs, inputs, is_coinbase] : transactions) {

            // Phase 1: Add new UTXOs from outputs (idempotent with INSERT OR IGNORE)
            for (size_t vout = 0; vout < outputs.size(); ++vout) {
                const auto& [scriptPubKey, value] = outputs[vout];

                // Check if this output belongs to our wallet
                auto opt_path = IsOurScript(scriptPubKey);
                if (opt_path.has_value()) {
                    // Use INSERT OR IGNORE for idempotency - safe to call multiple times
                    const char* insert_sql = R"(
                        INSERT OR IGNORE INTO wallet_utxos
                        (txid, vout, value, spk, path, height, spend_height, is_coinbase)
                        VALUES (?, ?, ?, ?, ?, ?, NULL, ?)
                    )";

                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 2, static_cast<int>(vout));
                        // Phase M.6.2: Extract raw value for SQLite boundary
                        sqlite3_bind_int64(stmt, 3, value.GetInt64());
                        sqlite3_bind_blob(stmt, 4, scriptPubKey.data(), scriptPubKey.size(), SQLITE_STATIC);
                        sqlite3_bind_text(stmt, 5, opt_path.value().c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 6, height);
                        sqlite3_bind_int(stmt, 7, is_coinbase ? 1 : 0);

                        if (sqlite3_step(stmt) == SQLITE_DONE) {
                            if (sqlite3_changes(db_) > 0) {
                                utxos_added++;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }

            // Phase 2: Mark spent outputs from inputs (idempotent - only updates if spend_height is NULL)
            if (!is_coinbase) {
                for (const auto& [prev_txid, prev_vout] : inputs) {
                    // Only update if the UTXO exists and is currently unspent (spend_height IS NULL)
                    const char* spend_sql = R"(
                        UPDATE wallet_utxos
                        SET spend_height = ?
                        WHERE txid = ? AND vout = ? AND spend_height IS NULL
                    )";

                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db_, spend_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int(stmt, 1, height);
                        sqlite3_bind_text(stmt, 2, prev_txid.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 3, prev_vout);

                        if (sqlite3_step(stmt) == SQLITE_DONE) {
                            if (sqlite3_changes(db_) > 0) {
                                utxos_spent++;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

        if (utxos_added > 0 || utxos_spent > 0) {
            std::cout << "INFO: Scanned block " << height << " (" << block_hash.substr(0, 16) << "...): "
                      << "+" << utxos_added << " UTXOs, -" << utxos_spent << " spent" << std::endl;
        }

    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to scan block " << height << ": " << e.what() << std::endl;
        throw;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to scan block " << height << " (unknown error)" << std::endl;
        throw;
    }
}

// TransactionProcessor implementation
// Phase M.6.2: Output amounts now use AmountUna
void TransactionProcessor::ProcessTransaction(UTXOIndex& index, const std::string& txid,
                                            const std::vector<std::pair<std::vector<uint8_t>, AmountUna>>& outputs,
                                            const std::vector<std::pair<std::string, uint32_t>>& inputs,
                                            int height) {
    // Phase M.0: Convert txid string to uint256 for UTXO operations
    uint256 txid_uint256 = uint256::FromHexUnsafe(txid);

    // Process outputs (potential new UTXOs)
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& [scriptPubKey, value] = outputs[i];

        if (auto path = index.IsOurScript(scriptPubKey)) {
            // Phase M.6.2: value is already AmountUna from outputs vector
            WalletUTXO utxo(TxId(txid_uint256), static_cast<uint32_t>(i),
                           value,  // Already AmountUna
                           scriptPubKey, *path, height);
            index.AddUTXO(utxo);
        }
    }

    // Process inputs (spend existing UTXOs)
    for (const auto& [prev_txid, prev_vout] : inputs) {
        // Phase M.4.3-B Step 3: Convert hex string → uint256 → TxId
        TxId prev_txid_typed = TxId(uint256::FromHexUnsafe(prev_txid));
        if (index.IsUTXOSpent(prev_txid_typed, prev_vout)) {
            continue; // Already spent
        }
        index.SpendUTXO(prev_txid_typed, prev_vout, height);
    }
}

std::vector<uint8_t> TransactionProcessor::ParseScriptPubKey(const std::string& hex) {
    std::vector<uint8_t> script;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        script.push_back(byte);
    }
    return script;
}

bool TransactionProcessor::IsP2WPKH(const std::vector<uint8_t>& script) {
    // P2WPKH: OP_0 <20-byte-hash>
    return script.size() == 22 && script[0] == 0x00 && script[1] == 0x14;
}

bool TransactionProcessor::IsP2TR(const std::vector<uint8_t>& script) {
    // P2TR (BIP341): OP_1 <32-byte-pubkey>
    // scriptPubKey format: 0x51 0x20 <32 bytes>
    return script.size() == 34 && script[0] == 0x51 && script[1] == 0x20;
}

std::vector<uint8_t> TransactionProcessor::ExtractPubKeyHash(const std::vector<uint8_t>& script) {
    if (!IsP2WPKH(script)) {
        return {};
    }
    return std::vector<uint8_t>(script.begin() + 2, script.end());
}

std::vector<uint8_t> TransactionProcessor::ExtractTaprootPubkey(const std::vector<uint8_t>& script) {
    if (!IsP2TR(script)) {
        return {};
    }
    // Extract 32-byte x-only pubkey (skip OP_1 and push length)
    return std::vector<uint8_t>(script.begin() + 2, script.end());
}

// ============================================================================
// Phase F: Zero-Knowledge Privacy Methods
// ============================================================================

bool UTXOIndex::AddConfidentialUTXO(const ZKOutput& zk_output) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    // Use INSERT OR REPLACE for idempotency
    const char* sql = R"(
        INSERT OR REPLACE INTO wallet_utxos
        (txid, vout, value, spk, path, height, spend_height, is_coinbase,
         is_confidential, commitment, range_proof, blinding_factor, nonce)
        VALUES (?, ?, ?, '', NULL, ?, NULL, 0, 1, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare add confidential UTXO statement: "
                  << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Bind parameters
    // Phase M.4: Convert TxId to hex for SQLite storage
    std::string txid_hex = zk_output.txid.AsUint256().GetHex();
    sqlite3_bind_text(stmt, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, zk_output.vout);
    // Phase M.6.2: Extract raw value for SQLite boundary
    sqlite3_bind_int64(stmt, 3, zk_output.amount.GetInt64());  // Decrypted amount
    sqlite3_bind_int(stmt, 4, zk_output.block_height);
    sqlite3_bind_blob(stmt, 5, zk_output.commitment.data(), zk_output.commitment.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 6, zk_output.range_proof.data(), zk_output.range_proof.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, zk_output.blinding_factor.data(), zk_output.blinding_factor.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 8, zk_output.nonce.data(), zk_output.nonce.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "ERROR: Failed to add confidential UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Phase M.4: Convert TxId to hex for logging
    // Phase M.6.2: Extract raw value for logging
    std::cout << "INFO: Added confidential UTXO " << zk_output.txid.AsUint256().GetHex() << ":" << zk_output.vout
              << " with amount " << zk_output.amount.GetUna() << " una" << std::endl;

    return true;
}

std::vector<WalletUTXO> UTXOIndex::GetConfidentialUTXOs() const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    const auto coins=GetUnspentUTXOs();std::vector<WalletUTXO> result;
    std::lock_guard<std::mutex> scripts(scripts_mutex_);
    for(auto coin:coins) {
        if(!coin.is_confidential)continue;
        const auto owned=watched_scripts_.find(coin.spk);if(owned==watched_scripts_.end())continue;
        // Preserve the existing CT display-path selection from live watchers.
        coin.path=owned->second;result.push_back(std::move(coin));
    }
    return result;
}

AmountUna UTXOIndex::GetConfidentialBalance() const {
    auto balance=AmountUna::Zero();
    for(const auto& coin:GetConfidentialUTXOs()) {
        const auto next=balance.Add(coin.value);if(!next)throw std::runtime_error("Wallet balance overflow");balance=*next;
    }
    return balance;
}

AmountUna UTXOIndex::GetTotalBalance() const {
    // Derive both components from one checked inventory/snapshot, retaining
    // this API's existing inclusive base plus watched confidential convention.
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    const auto coins=GetUnspentUTXOs();auto balance=AmountUna::Zero();
    std::lock_guard<std::mutex> scripts(scripts_mutex_);
    for(const auto& coin:coins) {
        const auto add=[&](){const auto next=balance.Add(coin.value);if(!next)throw std::runtime_error("Wallet balance overflow");balance=*next;};
        add();if(coin.is_confidential && watched_scripts_.contains(coin.spk))add();
    }
    return balance;
}

std::vector<ZKOutput> UTXOIndex::ScanForNewConfidentialOutputs(
    int last_scanned_height,
    int current_height,
    const std::vector<uint8_t>& view_key) {

    // NOTE: This method is a placeholder for now.
    // The actual scanning is performed by ZKWalletSync, which:
    // 1. Queries ExplorerDB.getConfidentialOutputsInRange()
    // 2. Rewinds proofs with the view key
    // 3. Calls AddConfidentialUTXO() for discovered outputs
    //
    // This method exists for API completeness but is not currently used.
    // If needed in the future, it could query ExplorerDB directly.

    std::vector<ZKOutput> outputs;
    g_logger.warning("[UTXOIndex] ScanForNewConfidentialOutputs called but not implemented. "
                    "Use ZKWalletSync for background scanning.");
    return outputs;
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Control (Crash Safety - CRITICAL-002 fix)
// ═══════════════════════════════════════════════════════════════════════════

void UTXOIndex::ApplyAtomically(const std::function<void()>& writes) {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (!db_ || atomic_write_active_ || !sqlite3_get_autocommit(db_))
        throw std::runtime_error("Wallet UTXO write ownership unavailable");
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Wallet UTXO write begin failed");
    atomic_write_active_ = true;
    try {
        writes();
        if (sqlite3_get_autocommit(db_) ||
            sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Wallet UTXO write commit failed");
        atomic_write_active_ = false;
    } catch (...) {
        if (!sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_))
            std::terminate();
        atomic_write_active_ = false;
        throw;
    }
}

bool UTXOIndex::BeginTransaction() {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_) return false;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to begin transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool UTXOIndex::CommitTransaction() {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_) return false;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to commit transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool UTXOIndex::RollbackTransaction() {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_) return false;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to rollback transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Database Reset (AssumeUTXO Rollback)
// ═══════════════════════════════════════════════════════════════════════════

bool UTXOIndex::ClearAll() {
    // ⚠️ DANGER: This deletes ALL UTXOs and metadata
    // Only use for AssumeUTXO rollback on validation failure

    std::lock_guard<std::recursive_mutex> lock(db_mutex_);
    if (atomic_write_active_) return false;

    g_logger.warning("[UTXOIndex] ⚠️  ClearAll() called - deleting ALL UTXOs and metadata");

    // Begin atomic transaction
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to begin transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    // Delete all UTXOs
    // M.5.2 FIX: Correct table name (was "utxos", should be "wallet_utxos")
    // A reset invalidates even a tracked index with zero owned rows.
    rc = sqlite3_exec(db_,
        "INSERT OR REPLACE INTO utxo_metadata(key,value) SELECT 'runtime_delivery:v1:invalidated','1' "
        "WHERE EXISTS(SELECT 1 FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt');"
        "DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt';"
        "DELETE FROM wallet_utxos", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to delete UTXOs: " + error);
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    // Delete all metadata
    rc = sqlite3_exec(db_, "DELETE FROM utxo_metadata WHERE key NOT GLOB 'runtime_delivery:*'", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to delete metadata: " + error);
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    // Commit transaction
    rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to commit ClearAll: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    g_logger.info("[UTXOIndex] ✓ Successfully cleared all UTXOs and metadata");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Metadata Storage (Crash Safety - CRITICAL-003 fix)
// ═══════════════════════════════════════════════════════════════════════════

bool UTXOIndex::SetMetadata(const std::string& key, const std::string& value) {
    if (key.starts_with("runtime_delivery:")) return false;
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    const char* sql = "INSERT OR REPLACE INTO utxo_metadata (key, value) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare SetMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] Failed to set metadata '" + key + "': " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

std::optional<std::string> UTXOIndex::GetMetadata(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    const char* sql = "SELECT value FROM utxo_metadata WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare GetMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    std::optional<std::string> result;

    if (rc == SQLITE_ROW) {
        const unsigned char* value = sqlite3_column_text(stmt, 0);
        if (value) {
            result = std::string(reinterpret_cast<const char*>(value));
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool UTXOIndex::DeleteMetadata(const std::string& key) {
    if (key.starts_with("runtime_delivery:")) return false;
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    const char* sql = "DELETE FROM utxo_metadata WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare DeleteMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] Failed to delete metadata '" + key + "': " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

// Phase 11a: Get Utreexo position for proof generation
std::optional<uint64_t> UTXOIndex::getUtreexoPosition(const TxId& txid, uint32_t vout) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::recursive_mutex> lock(db_mutex_);

    if (!stmt_get_position_) {
        g_logger.error("[UTXOIndex] getUtreexoPosition: stmt_get_position_ not prepared");
        return std::nullopt;
    }

    sqlite3_reset(stmt_get_position_);

    // Bind parameters
    std::string txid_hex = txid.AsUint256().GetHex();
    sqlite3_bind_text(stmt_get_position_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_get_position_, 2, vout);

    int rc = sqlite3_step(stmt_get_position_);
    if (rc == SQLITE_ROW) {
        // Check if utreexo_position is NULL
        if (sqlite3_column_type(stmt_get_position_, 0) == SQLITE_NULL) {
            return std::nullopt;  // Position not tracked for this UTXO
        }

        uint64_t position = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_position_, 0));
        return position;
    }

    // UTXO not found or error
    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] getUtreexoPosition query failed: " +
                      std::string(sqlite3_errmsg(db_)));
    }

    return std::nullopt;
}

} // namespace dinero
