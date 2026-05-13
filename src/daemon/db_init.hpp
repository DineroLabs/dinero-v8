// src/daemon/db_init.hpp
// Rock-solid database initialization for multi-daemon architecture
// Ensures genesis block and meta keys are properly initialized
#pragma once
#include <sqlite3.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cctype>
#include "../../include/sqlite_txn.h"  // Added for RAII transaction helper

namespace dinero::db {

struct NetworkParams {
  // Human-readable network id ("mainnet" | "testnet" | "regtest")
  std::string network;

  // Genesis header (Bitcoin-style fields)
  int32_t  version;
  std::array<uint8_t, 32> prevhash;   // all zeros for genesis
  std::array<uint8_t, 32> merkle;
  uint32_t time;                       // unix epoch
  uint32_t bits;                       // compact target
  uint32_t nonce;

  // Canonical values computed by your consensus code
  std::array<uint8_t, 32> genesis_hash; // 32B, big-endian or raw hash bytes as you store in DB
  std::array<uint8_t, 32> chainwork;    // 32B big-endian chainwork for height 0
  int32_t  tx_count_genesis = 1;        // usually 1
  int      file_no = 0;                 // blk00000.dat
  int      file_pos = 0;                // offset, if you store raw blocks on disk; 0 is okay
};

// ---------- RAII helpers ----------
struct SqliteStmt {
  sqlite3_stmt* s = nullptr;
  SqliteStmt() = default;
  explicit SqliteStmt(sqlite3_stmt* ps) : s(ps) {}
  ~SqliteStmt() { if (s) sqlite3_finalize(s); }
  SqliteStmt(const SqliteStmt&) = delete;
  SqliteStmt& operator=(const SqliteStmt&) = delete;
  SqliteStmt(SqliteStmt&& o) noexcept : s(o.s) { o.s = nullptr; }
  SqliteStmt& operator=(SqliteStmt&& o) noexcept { if (this!=&o){ if(s) sqlite3_finalize(s); s=o.s; o.s=nullptr; } return *this; }
};

// Txn class removed - all code should use SqliteTxn for RAII transaction management
// Legacy transaction handling replaced by SqliteTxn RAII helper

// ---------- small utils ----------
inline void bindBlob(sqlite3_stmt* st, int idx, const std::array<uint8_t,32>& b) {
  if (sqlite3_bind_blob(st, idx, b.data(), (int)b.size(), SQLITE_STATIC) != SQLITE_OK)
    throw std::runtime_error("bind blob failed");
}
inline void bindZeroHash(sqlite3_stmt* st, int idx) {
  static const std::array<uint8_t,32> Z{};
  bindBlob(st, idx, Z);
}

inline bool getMetaBlob(sqlite3* db, const char* key, std::array<uint8_t,32>& out) {
  const char* sql = "SELECT value FROM meta WHERE key=? LIMIT 1;";
  SqliteStmt st;
  if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st.s);
  if (rc == SQLITE_ROW) {
    const void* p = sqlite3_column_blob(st.s, 0);
    int n = sqlite3_column_bytes(st.s, 0);
    if (p && n == 32) { std::memcpy(out.data(), p, 32); return true; }
  }
  return false;
}
inline bool getMetaText(sqlite3* db, const char* key, std::string& out) {
  const char* sql = "SELECT value FROM meta WHERE key=? LIMIT 1;";
  SqliteStmt st;
  if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st.s);
  if (rc == SQLITE_ROW) {
    const unsigned char* txt = sqlite3_column_text(st.s, 0);
    if (txt) { out.assign(reinterpret_cast<const char*>(txt)); return true; }
  }
  return false;
}
inline int64_t getScalarInt(sqlite3* db, const char* sql) {
  SqliteStmt st;
  if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) return -1;
  if (sqlite3_step(st.s) == SQLITE_ROW) return sqlite3_column_int64(st.s, 0);
  return -1;
}

// INSERT OR REPLACE meta
inline void upsertMetaText(sqlite3* db, const char* key, const std::string& val) {
  const char* sql = "INSERT INTO meta(key,value) VALUES(?,?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
  SqliteStmt st;
  if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) throw std::runtime_error("prep upsertMetaText");
  sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st.s, 2, val.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st.s) != SQLITE_DONE) throw std::runtime_error("upsertMetaText step");
}
inline void upsertMetaBlob(sqlite3* db, const char* key, const std::array<uint8_t,32>& blob) {
  const char* sql = "INSERT INTO meta(key,value) VALUES(?,?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
  SqliteStmt st;
  if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) throw std::runtime_error("prep upsertMetaBlob");
  sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
  if (sqlite3_bind_blob(st.s, 2, blob.data(), 32, SQLITE_STATIC) != SQLITE_OK) throw std::runtime_error("bind meta blob");
  if (sqlite3_step(st.s) != SQLITE_DONE) throw std::runtime_error("upsertMetaBlob step");
}

// ---------- main entry ----------
struct EnsureResult {
  bool ok = false;
  std::string error;
  bool wrote_genesis = false;
  bool wrote_meta = false;
};

inline EnsureResult ensureGenesis(sqlite3* db, const NetworkParams& p) {
  EnsureResult R;

  // 0) Optional quick check: tables should exist (created by migrations)
  {
    int64_t n_meta = getScalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='meta';");
    int64_t n_hdrs = getScalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='headers';");
    int64_t n_bidx = getScalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='block_index';");
    int64_t n_utxo = getScalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='utxo';");
    if (n_meta == 0 || n_hdrs == 0 || n_bidx == 0 || n_utxo == 0) {
      R.error = "chainstate tables missing: run migrations before ensureGenesis()";
      return R;
    }
  }

  // 1) If meta.network exists, ensure it matches the selected network
  {
    std::string meta_net;
    if (getMetaText(db, "network", meta_net)) {
      if (meta_net != p.network) {
        R.error = "meta.network='" + meta_net + "' does not match process network='" + p.network + "'";
        return R;
      }
    }
  }

  // 2) If genesis_hash exists, verify it matches
  {
    std::array<uint8_t,32> meta_gen{};
    if (getMetaBlob(db, "genesis_hash", meta_gen)) {
      if (std::memcmp(meta_gen.data(), p.genesis_hash.data(), 32) != 0) {
        R.error = "meta.genesis_hash mismatch (DB vs compiled params)";
        return R;
      }
    }
  }

  // 3) Check if height 0 header exists
  bool have_genesis_header = false;
  {
    const char* sql = "SELECT 1 FROM headers WHERE height=0 LIMIT 1;";
    SqliteStmt st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st.s) == SQLITE_ROW) have_genesis_header = true;
    }
  }

  // 4) Write missing pieces in a single txn
  try {
    SqliteTxn txn(db);

    // 4a) Upsert meta.network/schema_version if missing
    upsertMetaText(db, "network", p.network);
    // Keep schema_version if you manage migrations elsewhere; default to "1"
    std::string sv;
    if (!getMetaText(db, "schema_version", sv)) upsertMetaText(db, "schema_version", "1");

    // 4b) Insert genesis header if absent
    if (!have_genesis_header) {
      const char* insH =
        "INSERT OR REPLACE INTO headers"
        "(hash,height,version,prevhash,merkle,time,bits,nonce,chainwork,status,file_no,file_pos,tx_count)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);";
      SqliteStmt sh;
      if (sqlite3_prepare_v2(db, insH, -1, &sh.s, nullptr) != SQLITE_OK) throw std::runtime_error("prep headers insert");

      int idx = 1;
      bindBlob(sh.s, idx++, p.genesis_hash);
      sqlite3_bind_int64(sh.s, idx++, 0);               // height
      sqlite3_bind_int(sh.s,   idx++, p.version);
      bindBlob(sh.s, idx++, p.prevhash);                 // should be zero hash
      bindBlob(sh.s, idx++, p.merkle);
      sqlite3_bind_int64(sh.s, idx++, static_cast<sqlite3_int64>(p.time));
      sqlite3_bind_int(sh.s,   idx++, static_cast<int>(p.bits));
      sqlite3_bind_int(sh.s,   idx++, static_cast<int>(p.nonce));
      bindBlob(sh.s, idx++, p.chainwork);
      // status flags: e.g., 1=VALID, 2=HAVE_DATA (adjust to your bitfield)
      sqlite3_bind_int(sh.s,   idx++, 1 | 2);
      sqlite3_bind_int(sh.s,   idx++, p.file_no);
      sqlite3_bind_int(sh.s,   idx++, p.file_pos);
      sqlite3_bind_int(sh.s,   idx++, p.tx_count_genesis);

      if (sqlite3_step(sh.s) != SQLITE_DONE) throw std::runtime_error("headers insert failed");
      R.wrote_genesis = true;

      // block_index height -> hash
      const char* insBI = "INSERT OR REPLACE INTO block_index(height,hash) VALUES(?,?);";
      SqliteStmt sbi;
      if (sqlite3_prepare_v2(db, insBI, -1, &sbi.s, nullptr) != SQLITE_OK) throw std::runtime_error("prep block_index");
      sqlite3_bind_int64(sbi.s, 1, 0);
      bindBlob(sbi.s, 2, p.genesis_hash);
      if (sqlite3_step(sbi.s) != SQLITE_DONE) throw std::runtime_error("block_index upsert failed");
    }

    // 4c) Upsert meta keys (besthash/height/chainwork/genesis_hash)
    std::array<uint8_t,32> tmp{};
    bool have_best = getMetaBlob(db, "besthash", tmp);
    bool have_gh   = getMetaBlob(db, "genesis_hash", tmp);
    bool have_cw   = getMetaBlob(db, "chainwork", tmp);
    std::string htxt;
    bool have_h    = getMetaText(db, "height", htxt);

    if (!have_gh)       upsertMetaBlob(db, "genesis_hash", p.genesis_hash), R.wrote_meta = true;
    if (!have_best)     upsertMetaBlob(db, "besthash",     p.genesis_hash), R.wrote_meta = true;
    if (!have_h)        upsertMetaText(db, "height",       "0"),           R.wrote_meta = true;
    if (!have_cw)       upsertMetaBlob(db, "chainwork",    p.chainwork),   R.wrote_meta = true;

    txn.commit();
    R.ok = true;
    return R;
  } catch (const std::exception& e) {
    R.error = e.what();
    return R;
  }
}

// Helper function to convert hex string to byte array (production-grade)
inline uint8_t _hexNibble(char c) {
  if (c>='0' && c<='9') return uint8_t(c - '0');
  c = std::tolower(static_cast<unsigned char>(c));
  if (c>='a' && c<='f') return uint8_t(10 + (c - 'a'));
  throw std::invalid_argument("hex32: invalid hex digit");
}

inline std::array<uint8_t,32> hex32(std::string_view s) {
  // allow optional "0x" prefix
  if (s.size() >= 2 && s[0]=='0' && (s[1]=='x' || s[1]=='X')) s.remove_prefix(2);
  if (s.size() != 64) throw std::invalid_argument("hex32: need 64 hex chars");
  std::array<uint8_t,32> out{};
  for (size_t i=0; i<32; ++i) {
    uint8_t hi = _hexNibble(s[2*i]);
    uint8_t lo = _hexNibble(s[2*i+1]);
    out[i] = uint8_t((hi<<4) | lo);
  }
  return out;
}

// Overload for C-style strings
inline std::array<uint8_t,32> hex32(const char* hex) {
  return hex32(std::string_view(hex));
}

} // namespace dinero::db
